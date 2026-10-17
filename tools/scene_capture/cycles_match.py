"""Writes scene_capture game scenes matching a Blender scene, for Cycles references.

    blender -b SCENE.blend --python cycles_match.py -- --out DIR
        --asset-root ROOT --model OBJECT=GLB [--model OBJECT=GLB ...] --hangar OBJECT
        --probes BAKE_DIR --defaults SCENE3D_DEFAULTS [--camera NAME] [--width 1920]
        [--height 1080] [--probe-occlusion] [--planar-reflection]

SCENE.blend holds each model under its own root object (as the glTF importer
leaves it) and a camera; it is opened read-only and never saved. OBJECT names
a model's root, GLB its cooked file relative to ROOT. The hangar model owns
the probes of BAKE_DIR, a probes bake folder (bake.json, probes.ktx2,
grid.ktx2), in place of its GLB's.

Writes, under DIR, which is scene_capture's working directory for these runs:
- full.yaml, diffuse.yaml, reflect.yaml: the scene from the .blend's camera,
  then with only the probes' diffuse light, then with only their reflections
  (emission stays in both). Each writes to captures/<name>.
- assets/: links to the models; a model whose parts are posed differently in
  the .blend (an articulation such as landing gear) is a copy with those
  parts posed as there, without its cooked BVH, which the loader then builds.
- assets/probes_full, probes_diffuse, probes_reflect: links to BAKE_DIR's
  images, with its bake.json as is, then with the specular strength at 0,
  then with the diffuse strength at 0.
- scene.json: the camera and the other models' footprint on the floor, for
  cycles_render.py and cycles_compare.py.

With --probe-occlusion and --planar-reflection, the configs turn on
scene_capture's probe_occlusion and planar_reflection; the latter needs the
bake's reflectionPlane.

scene_capture's camera has a 60 degree vertical field of view; the .blend's
camera gives the position and direction only."""

import argparse
import json
import math
import os
import re
import struct
import sys

import bpy
import mathutils
import numpy as np
from mathutils.bvhtree import BVHTree

FOOTPRINT_CELL = 0.25  # metres

# glTF (Y up) to Blender (Z up), as the importer converts each node.
GLTF_TO_BLENDER = mathutils.Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))
# The model frame is the game's: from Blender, (-x, y, z) relative to the root.
MODEL_TO_BLENDER = mathutils.Matrix(((-1, 0, 0), (0, 1, 0), (0, 0, 1)))
# scene_capture's place_ship: model axes onto camera axes before yaw, pitch and roll.
CAPTURE_BASE = mathutils.Matrix(((1, 0, 0), (0, 0, -1), (0, -1, 0)))


def parse_args():
    parser = argparse.ArgumentParser(prog="cycles_match.py")
    parser.add_argument("--out", required=True)
    parser.add_argument("--asset-root", required=True)
    parser.add_argument("--model", action="append", required=True, metavar="OBJECT=GLB")
    parser.add_argument("--hangar", required=True)
    parser.add_argument("--probes", required=True)
    parser.add_argument("--defaults", required=True)
    parser.add_argument("--camera")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--probe-occlusion", action="store_true")
    parser.add_argument("--planar-reflection", action="store_true")
    return parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])


def link(target, path):
    if os.path.lexists(path):
        os.remove(path)
    os.symlink(os.path.abspath(target), path)


def base_name(name):
    return re.sub(r"\.\d{3}$", "", name)


def read_glb(path):
    data = open(path, "rb").read()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    json_length, json_type = struct.unpack_from("<II", data, 12)
    return magic, version, json_type, json.loads(data[20:20 + json_length]), data[20 + json_length:]


def write_glb(path, magic, version, json_type, document, rest):
    text = json.dumps(document, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    with open(path, "wb") as stream:
        stream.write(struct.pack("<III", magic, version, 12 + 8 + len(text) + len(rest)))
        stream.write(struct.pack("<II", len(text), json_type) + text + rest)


def node_matrix(node):
    if "matrix" in node:
        return mathutils.Matrix([node["matrix"][i::4] for i in range(4)])
    x, y, z, w = node.get("rotation", (0, 0, 0, 1))
    return mathutils.Matrix.LocRotScale(mathutils.Vector(node.get("translation", (0, 0, 0))),
                                        mathutils.Quaternion((w, x, y, z)),
                                        mathutils.Vector(node.get("scale", (1, 1, 1))))


def posed_model(root, glb_path, out_path):
    """Copies the GLB with its nodes posed as the .blend's objects of the same
    names under `root`; returns the posed node names (none: no copy)."""
    magic, version, json_type, document, rest = read_glb(glb_path)
    counts = {}
    for node in document.get("nodes", []):
        counts[node.get("name")] = counts.get(node.get("name"), 0) + 1
    objects = {}
    for obj in root.children_recursive:
        objects.setdefault(base_name(obj.name), []).append(obj)
    posed = []
    for node in document.get("nodes", []):
        name = node.get("name")
        if counts.get(name) != 1 or len(objects.get(name, [])) != 1:
            continue
        local = GLTF_TO_BLENDER.inverted() @ objects[name][0].matrix_local @ GLTF_TO_BLENDER
        rest_pose = node_matrix(node)
        if max(abs(local[i][j] - rest_pose[i][j]) for i in range(4) for j in range(4)) < 1e-4:
            continue
        t, q, s = local.decompose()
        node.pop("matrix", None)
        node["translation"], node["rotation"], node["scale"] = list(t), [q.x, q.y, q.z, q.w], list(s)
        posed.append(name)
    if not posed:
        return posed
    for node in document["nodes"]:
        extensions = node.get("extensions", {})
        if extensions.get("AERON_flight_model", {}).get("role") == "model":
            # The cooked BVH hashes the rest pose.
            extensions.get("AERON_flight_model_cooked", {}).pop("renderData", None)
    write_glb(out_path, magic, version, json_type, document, rest)
    return posed


def placement(root, camera_rows, eye):
    """scene_capture's position (metres, camera axes) and [yaw, pitch, roll] of a root."""
    scale = root.matrix_world.to_scale()
    if max(abs(s - 1) for s in scale) > 1e-4:
        sys.exit(f"cycles_match.py: {root.name} is scaled {tuple(scale)}; scene_capture places models unscaled")
    rotation = camera_rows @ root.matrix_world.to_3x3() @ MODEL_TO_BLENDER
    q = rotation @ CAPTURE_BASE.inverted()  # yaw * pitch * roll
    pitch = math.asin(max(-1.0, min(1.0, -q[1][2])))
    roll = math.atan2(q[1][0], q[1][1])
    yaw = math.atan2(q[0][2], q[2][2])
    position = camera_rows @ (root.matrix_world.translation - eye)
    return list(position), [math.degrees(a) for a in (yaw, pitch, roll)], rotation


def render_meshes(roots):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    layer_objects = bpy.context.view_layer.objects
    verts, polys = [], []
    for root in roots:
        for obj in root.children_recursive:
            if obj.type != 'MESH' or obj.hide_render or obj.name not in layer_objects:
                continue
            evaluated = obj.evaluated_get(depsgraph)
            mesh = evaluated.to_mesh()
            mesh.calc_loop_triangles()
            base = len(verts)
            verts.extend(obj.matrix_world @ v.co for v in mesh.vertices)
            polys.extend(tuple(base + i for i in tri.vertices) for tri in mesh.loop_triangles)
            evaluated.to_mesh_clear()
    return verts, polys


def footprint(roots, out_path):
    """Which 0.25 m floor cells the models cover, seen from above."""
    verts, polys = render_meshes(roots)
    if not polys:
        return None
    points = np.array([tuple(v) for v in verts])
    lo, hi = points.min(0), points.max(0)
    nx = int((hi[0] - lo[0]) / FOOTPRINT_CELL) + 1
    ny = int((hi[1] - lo[1]) / FOOTPRINT_CELL) + 1
    bvh = BVHTree.FromPolygons(verts, polys)
    down = mathutils.Vector((0, 0, -1))
    covered = np.zeros((ny, nx), np.uint8)
    for j in range(ny):
        for i in range(nx):
            origin = mathutils.Vector((lo[0] + (i + 0.5) * FOOTPRINT_CELL, lo[1] + (j + 0.5) * FOOTPRINT_CELL, hi[2] + 1))
            covered[j, i] = bvh.ray_cast(origin, down)[0] is not None
    np.save(out_path, covered)
    return {"file": os.path.basename(out_path), "origin": [float(lo[0]), float(lo[1])], "cell": FOOTPRINT_CELL}


def write_probe_variants(bake_dir, assets):
    """The bake's variants under `assets`: as is, without reflections, without diffuse light. Returns its
    key light shadow."""
    with open(os.path.join(bake_dir, "bake.json")) as stream:
        layout = json.load(stream)
    for name, changes in (("full", {}), ("diffuse", {"specularStrength": 0}), ("reflect", {"diffuseStrength": 0})):
        folder = os.path.join(assets, "probes_" + name)
        os.makedirs(folder, exist_ok=True)
        for image in ("probes.ktx2", "grid.ktx2"):
            link(os.path.join(bake_dir, image), os.path.join(folder, image))
        with open(os.path.join(folder, "bake.json"), "w") as stream:
            json.dump(dict(layout, **changes), stream, indent=1)
    return layout.get("keyLightShadow", 0.7)  # the engine's default


def capture_config(name, width, height, models, instances, sun_direction, key_probe_shadow, probe_occlusion,
                   planar_reflection):
    lines = [
        f"# {name}: written by cycles_match.py; run scene_capture from this folder.",
        f"width: {width}", f"height: {height}", "frames: 1", "fps: 60",
        "pan_degrees_per_second: 0", "pan_direction_degrees: 0", "shutter: 0.5", "quality: 0",
        "reference_samples: 0", "msaa_samples: 4", "cull: back",
        "bloom:", "  intensity: 0.2", "  kernel: 4", "tonemap: agx",
        f"output_dir: captures/{name}", "asset_root: assets", "models:",
        *[f"  - {m}" for m in models],
        "game_scene:",
        "  defaults: scene3d_defaults.yaml",
        "  # Lit by the probes only, as the .blend's lights are in their bake.",
        "  model_light_intensity: 0",
        "  # The hangar key light: up from the hangar floor, intensity 0.8.",
        "  sun_color: [2.0, 2.0, 2.0]",
        "  sun_direction: [%.6f, %.6f, %.6f]" % tuple(sun_direction),
        "  ambient: 0.02",
        f"  key_probe_shadow: {key_probe_shadow:g}",
        f"  probe_occlusion: {'true' if probe_occlusion else 'false'}",
        f"  planar_reflection: {'true' if planar_reflection else 'false'}",
        "  instances:",
        *[f"    - {i.replace('{PROBES}', 'probes_' + name)}" for i in instances],
    ]
    return "\n".join(lines) + "\n"


def main():
    args = parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(os.path.join(out, "assets"), exist_ok=True)
    camera = bpy.data.objects[args.camera] if args.camera else bpy.context.scene.camera
    if camera is None or camera.type != 'CAMERA':
        sys.exit("cycles_match.py: no camera; name one with --camera")
    models = []
    for item in args.model:
        obj, _, glb = item.partition("=")
        if obj not in bpy.data.objects or not glb:
            sys.exit(f"cycles_match.py: bad --model {item}")
        models.append((bpy.data.objects[obj], glb))
    if args.hangar not in [root.name for root, _ in models]:
        sys.exit("cycles_match.py: --hangar must name one of the --model objects")

    key_probe_shadow = write_probe_variants(args.probes, os.path.join(out, "assets"))
    link(args.defaults, os.path.join(out, "scene3d_defaults.yaml"))

    world = camera.matrix_world
    camera_rows = mathutils.Matrix((world.col[0].xyz.normalized(), -world.col[1].xyz.normalized(),
                                    -world.col[2].xyz.normalized()))
    eye = world.translation
    model_paths, instances, sun_direction = [], [], None
    for root, glb in models:
        source = os.path.join(args.asset_root, glb)
        stem, extension = os.path.splitext(glb)
        target = os.path.join(out, "assets", stem + "_posed" + extension)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        posed = posed_model(root, source, target)
        if posed:
            print(f"{root.name}: posed as in the .blend: {', '.join(posed)}")
            path = stem + "_posed" + extension
        else:
            link(source, os.path.join(out, "assets", glb))
            path = glb
        model_paths.append(path)
        position, rotation, matrix = placement(root, camera_rows, eye)
        fields = (f"model: {path}, " + ("role: hangar, " if root.name == args.hangar else "") +
                  "position: [%.6f, %.6f, %.6f], rotation_degrees: [%.6f, %.6f, %.6f]" % (*position, *rotation))
        if root.name == args.hangar:
            fields += ", probes: {PROBES}"
            sun_direction = list(matrix @ mathutils.Vector((0, 0, 1)))
        instances.append("{ " + fields + " }")
    for name in ("full", "diffuse", "reflect"):
        with open(os.path.join(out, f"{name}.yaml"), "w") as stream:
            stream.write(capture_config(name, args.width, args.height, model_paths, instances, sun_direction,
                                        key_probe_shadow, args.probe_occlusion, args.planar_reflection))

    others = [root for root, _ in models if root.name != args.hangar]
    scene = {"camera": camera.name, "width": args.width, "height": args.height, "vertical_fov_degrees": 60.0,
             "hangar": args.hangar, "models": [root.name for root in others],
             "footprint": footprint(others, os.path.join(out, "footprint.npy"))}
    with open(os.path.join(out, "scene.json"), "w") as stream:
        json.dump(scene, stream, indent=1)
    print(f"cycles_match.py: wrote {out}")


main()
