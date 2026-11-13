"""Writes the lod-ships preset: small craft at chosen sizes on screen, in
front of the lit hull of a Super Star Destroyer, to check how levels of
detail look against a bright background.

    python3 lod_ships_scene.py ASSET_ROOT OUT.yaml [key=value ...]

ASSET_ROOT holds mod/flight/models with the TFTC-R models, as for
shadow_casters_scene.py. Each row is one craft; from left to right its
radius on screen is each of SIZES_PX, in pixels of the image. Capital ships
don't fit: to be a few pixels across they would have to be farther than the
hull behind them.
"""
import math
import os
import sys
import textwrap

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shadow_casters_scene import CELL, MODELS, SSD, gltf_to_opt, hull_heightmap, rotation  # noqa: E402

CRAFT = ['tiefighter', 'tieinterceptor', 'xwing', 'ywing', 'awing', 'assaultgunboat']
SIZES_PX = [60, 30, 15, 8, 4, 2.5]
OPTIONS = dict(w=2560, h=1440, msaa=4, hull_distance=7000.0, sun='0.35,-0.45,-0.82', out='captures/lod-ships',
               defaults='aeron/config/scene3d_defaults.yaml', prepass_min=2.5, draw_min=2.0, shadow_min=4.0)


def model_radius(path):
    """Half the diagonal of the bounds of the nodes without a transform: the
    radius that the sizes on screen are measured with."""
    import json
    import struct
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 12)[0]
    j = json.loads(d[20:20 + n])
    lo, hi = [1e30] * 3, [-1e30] * 3
    for nd in j['nodes']:
        if 'mesh' not in nd or any(k in nd for k in ('matrix', 'rotation', 'scale', 'translation')):
            continue
        for p in j['meshes'][nd['mesh']]['primitives']:
            a = j['accessors'][p['attributes']['POSITION']]
            lo = [min(x, y) for x, y in zip(lo, a['min'])]
            hi = [max(x, y) for x, y in zip(hi, a['max'])]
    return 0.5 * math.sqrt(sum((h - l) ** 2 for l, h in zip(lo, hi)))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    asset_root, out_path = sys.argv[1], sys.argv[2]
    opt = dict(OPTIONS)
    for a in sys.argv[3:]:
        k, v = a.split('=', 1)
        if k not in opt:
            sys.exit(f'unknown option {k}; options: {", ".join(opt)}')
        opt[k] = type(opt[k])(v)

    # scene_capture's 60 degree vertical field of view.
    tan_v = math.tan(math.radians(30))
    tan_h = tan_v * opt['w'] / opt['h']
    focal = opt['h'] / (2 * tan_v)

    # The SSD's top faces the camera, its length across the view, and its
    # wide stern on the right, behind the smallest sizes.
    up_target, length_target = np.array([0, 0, -1.0]), np.array([-1.0, 0, 0])
    best = None
    for yaw in (0, 90, 180, 270):
        for pitch in (0, 90, 180, 270):
            for roll in (0, 90, 180, 270):
                r = rotation(yaw, pitch, roll)
                score = (r @ gltf_to_opt(np.array([0, 1.0, 0]))) @ up_target + \
                    (r @ gltf_to_opt(np.array([0, 0, 1.0]))) @ length_target
                if best is None or score > best[0] + 1e-6:
                    best = (score, (yaw, pitch, roll), r)
    _, angles, r_ssd = best
    # Its hull at the middle of its length, hull_distance ahead.
    h, x0, z0 = hull_heightmap(f'{asset_root}/{SSD}')
    ix, iz = int((0 - x0) // CELL), int((0 - z0) // CELL)
    top = np.nanmax(h[ix - 1:ix + 2, iz - 1:iz + 2])
    anchor = r_ssd @ gltf_to_opt(np.array([0.0, top, 0.0]))
    ssd_pos = np.array([0, 0, opt['hull_distance']]) - anchor

    lines = [f'    - {{ model: {SSD}, position: [{ssd_pos[0]:.2f}, {ssd_pos[1]:.2f}, {ssd_pos[2]:.2f}], '
             f'rotation_degrees: [{angles[0]}, {angles[1]}, {angles[2]}] }}']
    rows, cols = len(CRAFT), len(SIZES_PX)
    for row, craft in enumerate(CRAFT):
        radius = model_radius(f'{asset_root}/{MODELS}{craft}.glb')
        for col, size in enumerate(SIZES_PX):
            # Cell centres in the middle 60% of the view's height, where the hull is.
            ndc_x = -0.85 + 1.7 * (col + 0.5) / cols
            ndc_y = -0.55 + 1.1 * (row + 0.5) / rows
            ray = np.array([ndc_x * tan_h, ndc_y * tan_v, 1.0])
            p = ray / np.linalg.norm(ray) * radius * focal / size
            if p[2] >= opt['hull_distance'] - 500:
                sys.exit(f'{craft} at {size} px would be behind the hull; raise hull_distance')
            lines.append(f'    - {{ model: {MODELS}{craft}.glb, position: [{p[0]:.2f}, {p[1]:.2f}, {p[2]:.2f}], '
                         f'rotation_degrees: [35, 20, 0] }}')

    models = [SSD] + [MODELS + c + '.glb' for c in CRAFT]
    args = ' '.join(sys.argv[3:])
    header = (f'Levels of detail test scene, written by lod_ships_scene.py{" with " + args if args else ""}. '
              f'Rows from the top: {", ".join(CRAFT)}. From left to right, each craft\'s radius on screen is '
              f'{", ".join(f"{s:g}" for s in SIZES_PX)} pixels. Behind them, the lit hull of a Super Star '
              f'Destroyer {opt["hull_distance"] / 1000:g} km away. Set asset_root to the folder holding '
              f'mod/flight/models with the TFTC-R models.')
    open(out_path, 'w').write(''.join(f'# {line}\n' for line in textwrap.wrap(header, 76)) + f'''width: {opt['w']}
height: {opt['h']}
frames: 1
fps: 60
pan_degrees_per_second: 0
pan_direction_degrees: 0
shutter: 0.5
quality: 0
reference_samples: 0
msaa_samples: {opt['msaa']}
cull: back
bloom:
  intensity: 0.15
  kernel: 4
tonemap: agx
output_dir: {opt['out']}
asset_root: .
models:
''' + ''.join(f'  - {m}\n' for m in models) + f'''game_scene:
  defaults: {opt['defaults']}
  sun_direction: [{opt['sun']}]
  sun_color: [2.513, 2.513, 2.513]
  ambient: 0.08
  small_objects:
    prepass_min_size: {opt['prepass_min']}
    draw_min_size: {opt['draw_min']}
    shadow_min_size: {opt['shadow_min']}
  instances:
''' + '\n'.join(lines) + '\n')
    print(len(lines), 'instances; SSD rotation', angles)


if __name__ == '__main__':
    main()
