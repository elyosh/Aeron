"""Writes the shadow-casters preset: a battle above a Super Star Destroyer,
to test which shadow casters the cascades can leave out.

    python3 shadow_casters_scene.py ASSET_ROOT OUT.yaml [key=value ...]

ASSET_ROOT holds mod/flight/models with the TFTC-R models (the Super Star
Destroyer, TIE fighters, interceptors and bombers, the modified corvette and
the Imperial Star Destroyer). The preset's model paths are relative to it,
like scene_capture's asset_root.

The camera is 1.5 km above the bow and looks toward the stern. The ship is
tilted so its hull fills the middle of the view from 2 to 17 km, which
gives receivers in the two far cascades. Above the hull fly:

- fighters, 10 to 300 m above the visible hull, so 2 to 17 km from the
  camera; some are moved just above the top of the view, so they are off
  screen but their shadows are not;
- fighters near the camera, 60 m to 1 km away, whose shadows on the hull
  far below are tiny;
- corvettes and Star Destroyers, as larger casters.

Without key=value options, it writes the preset. key=value changes one of
the OPTIONS. The same options always give the same scene.
"""
import json
import math
import random
import struct
import sys

import numpy as np

OPTIONS = dict(
    w=2560, h=1440, msaa=4,
    # The Super Star Destroyer's yaw and pitch in degrees. The camera is
    # cam_height metres above its hull at (cam_gx, cam_gz), in glTF metres.
    yaw=180.0, tilt=-14.0, cam_height=1500.0, cam_gx=0.0, cam_gz=8000.0,
    n_small=500, n_near=40, n_medium=12, n_large=3,
    # Share of the fighters above the hull that are moved just above the top
    # of the view.
    outside=0.15,
    seed=1, sun='0.25,-0.9,-0.35', out='captures/shadow-casters', bench=0,
    defaults='aeron/config/scene3d_defaults.yaml',
    # The game's small_objects settings.
    prepass_min=2.5, draw_min=0.6, shadow_min=4.0,
)
MODELS = 'mod/flight/models/'
SSD = MODELS + 'superstardestroyertftc.glb'
SMALL = [MODELS + 'tiefighter.glb', MODELS + 'tieinterceptor.glb', MODELS + 'tiebomber.glb']
MEDIUM = [MODELS + 'modcorvettetftc.glb']
LARGE = [MODELS + 'imperialstardestroyertftc.glb']
CELL = 50.0  # height map cell, metres


def hull_heightmap(path):
    """The height of a glb model's top: the highest point (glTF +Y) of its
    triangles in each CELL square over X and Z, or NaN where there is none.
    Only the nodes without a transform are read, which is enough for the
    hull."""
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 12)[0]
    j = json.loads(d[20:20 + n])
    b0 = 20 + n + 8

    def accessor(i, comps, dtype):
        a = j['accessors'][i]
        bv = j['bufferViews'][a['bufferView']]
        size = np.dtype(dtype).itemsize * comps
        stride = bv.get('byteStride', size)
        off = b0 + bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        raw = np.frombuffer(d, np.uint8, count=stride * (a['count'] - 1) + size, offset=off)
        idx = np.arange(a['count'])[:, None] * stride + np.arange(size)
        return raw[idx].copy().view(dtype).reshape(a['count'], comps)

    plain = [nd['mesh'] for nd in j['nodes'] if 'mesh' in nd and
             not any(k in nd for k in ('matrix', 'rotation', 'scale', 'translation'))]
    samples = []
    for mi in plain:
        for p in j['meshes'][mi]['primitives']:
            v = accessor(p['attributes']['POSITION'], 3, np.float32)
            index_type = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32}[
                j['accessors'][p['indices']]['componentType']]
            t = v[accessor(p['indices'], 1, index_type).reshape(-1, 3).astype(np.int64)]
            # Enough points on each triangle that they are under half a cell
            # apart.
            edge = np.max(np.linalg.norm(t - np.roll(t, 1, axis=1), axis=2), axis=1)
            steps = np.ceil(np.clip(edge / (CELL / 2), 1, 64)).astype(int)
            for k in np.unique(steps):
                tk = t[steps == k]
                u, w = np.meshgrid(np.arange(k + 1), np.arange(k + 1))
                inside = u + w <= k
                u, w = u[inside] / k, w[inside] / k
                pts = (tk[:, None, 0] * (1 - u - w)[None, :, None] + tk[:, None, 1] * u[None, :, None] +
                       tk[:, None, 2] * w[None, :, None])
                samples.append(pts.reshape(-1, 3))
    p = np.concatenate(samples)
    xi = np.floor(p[:, 0] / CELL).astype(int)
    zi = np.floor(p[:, 2] / CELL).astype(int)
    x0, z0 = xi.min(), zi.min()
    h = np.full((xi.max() - x0 + 1, zi.max() - z0 + 1), np.nan)
    np.fmax.at(h, (xi - x0, zi - z0), p[:, 1])
    return h, x0 * CELL, z0 * CELL


def rotation(yaw, pitch, roll):
    """scene_capture's place_ship rotation, for a model in OPT axes."""
    r = math.radians
    cy, sy = math.cos(r(yaw)), math.sin(r(yaw))
    cp, sp = math.cos(r(pitch)), math.sin(r(pitch))
    cr, sr = math.cos(r(roll)), math.sin(r(roll))
    base = np.array([[1, 0, 0], [0, 0, -1], [0, -1, 0]], float)
    roll_m = np.array([[cr, -sr, 0], [sr, cr, 0], [0, 0, 1]])
    pitch_m = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
    yaw_m = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    return yaw_m @ pitch_m @ roll_m @ base


def gltf_to_opt(g):
    # The loader undoes opt2gltf's axis swap: glTF = (-x, z, -y) of OPT.
    return np.array([-g[0], -g[2], g[1]])


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
    rng = random.Random(opt['seed'])

    H, x0, z0 = hull_heightmap(f'{asset_root}/{SSD}')
    R_ssd = rotation(opt['yaw'], opt['tilt'], 0)
    # Place the ship so that the camera is cam_height above its hull at
    # (cam_gx, cam_gz).
    ix, iz = int((opt['cam_gx'] - x0) // CELL), int((opt['cam_gz'] - z0) // CELL)
    top = np.nanmax(H[ix - 1:ix + 2, iz])
    anchor = R_ssd @ gltf_to_opt(np.array([opt['cam_gx'], top, opt['cam_gz']]))
    up = R_ssd @ gltf_to_opt(np.array([0.0, 1.0, 0.0]))
    ssd_pos = -anchor - up * opt['cam_height']

    # Every hull cell, in camera axes (+X right, +Y down, +Z forward).
    gx, gz = np.meshgrid(x0 + (np.arange(H.shape[0]) + 0.5) * CELL, z0 + (np.arange(H.shape[1]) + 0.5) * CELL,
                         indexing='ij')
    ok = ~np.isnan(H)
    g = np.stack([gx[ok], H[ok], gz[ok]], 1)
    with np.errstate(all='ignore'):  # this numpy build's matmul warns on these finite values
        cam = (R_ssd @ np.stack([-g[:, 0], -g[:, 2], g[:, 1]])).T + ssd_pos
    dist = np.linalg.norm(cam, axis=1)
    # scene_capture's 60 degree vertical field of view.
    th = math.tan(math.radians(30))
    tw = th * opt['w'] / opt['h']
    visible = (cam[:, 2] > 1) & (np.abs(cam[:, 0]) < tw * cam[:, 2]) & (np.abs(cam[:, 1]) < th * cam[:, 2])
    near_hull, far_hull = dist[visible].min(), dist[visible].max()
    print(f'hull in view from {near_hull:.0f} to {far_hull:.0f} m')

    def in_view(p):
        return p[2] > 1 and abs(p[0]) < tw * p[2] and abs(p[1]) < th * p[2]

    def pick(lo, hi):
        """A visible hull cell at a log-uniform distance from lo to hi,
        limited to the distances of the visible hull."""
        lo, hi = max(lo, near_hull), min(hi, far_hull)
        for _ in range(1000):
            d = math.exp(rng.uniform(math.log(lo), math.log(hi)))
            cand = np.nonzero(visible & (np.abs(np.log(dist / d)) < 0.08))[0]
            if len(cand):
                return cam[rng.choice(cand)]
        sys.exit(f'no hull cell near {d:.0f} m')

    def place(lo, hi, h_lo, h_hi, outside):
        """A point above the hull. An outside one is raised just above the
        top of the view: off screen, with its shadow in view."""
        while True:
            base = pick(lo, hi)
            if not outside:
                return base + up * math.exp(rng.uniform(math.log(h_lo), math.log(h_hi)))
            t = h_lo
            while t < 3000 and in_view(base + up * t):
                t *= 1.1
            if t < 3000:
                return base + up * t * rng.uniform(1.05, 1.3)

    def instance(model, p):
        return (f'    - {{ model: {model}, position: [{p[0]:.2f}, {p[1]:.2f}, {p[2]:.2f}], '
                f'rotation_degrees: [{rng.uniform(-180, 180):.1f}, {rng.uniform(-60, 60):.1f}, '
                f'{rng.uniform(-180, 180):.1f}] }}')

    instances = [f'    - {{ model: {SSD}, position: [{ssd_pos[0]:.2f}, {ssd_pos[1]:.2f}, {ssd_pos[2]:.2f}], '
                 f'rotation_degrees: [{opt["yaw"]}, {opt["tilt"]}, 0] }}']

    def add(models, count, lo, hi, h_lo, h_hi):
        for i in range(count):
            instances.append(instance(models[i % len(models)],
                                      place(lo, hi, h_lo, h_hi, rng.random() < opt['outside'])))

    add(SMALL, opt['n_small'], 30, 20000, 10, 300)
    for i in range(opt['n_near']):
        d = math.exp(rng.uniform(math.log(60), math.log(1000)))
        v = np.array([rng.uniform(-tw, tw) * 0.9, rng.uniform(-th, th) * 0.9, 1.0])
        instances.append(instance(SMALL[i % len(SMALL)], v / np.linalg.norm(v) * d))
    add(MEDIUM, opt['n_medium'], 300, 20000, 100, 600)
    add(LARGE, opt['n_large'], 6000, 20000, 400, 1000)

    bench = '''benchmark:
  warmup_seconds: 1
  sample_seconds: 1
  samples: 5
  min_frames: 60
  frames_in_flight: 2
''' if opt['bench'] else ''
    models = [SSD] + SMALL + MEDIUM + LARGE
    args = ' '.join(sys.argv[3:])
    open(out_path, 'w').write(f'''# Shadow casters test scene, written by shadow_casters_scene.py{" with " + args if args else ""}.
# A battle above a Super Star Destroyer: fighters and larger ships cast
# shadows on its hull, 2 to 17 km from the camera. Set asset_root to the
# folder holding mod/flight/models with the TFTC-R models.
width: {opt['w']}
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
''' + '\n'.join(instances) + '\n' + bench)
    print(len(instances), 'instances')


if __name__ == '__main__':
    main()
