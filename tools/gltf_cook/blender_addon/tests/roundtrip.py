"""Round trip of source tree models through Blender and this add-on.

    python3 roundtrip.py --blender BLENDER --cooker AERON_GLTF_COOK
                         [--effects-dir DIR] [--keep] MODEL_DIR...

Each model directory (`<key>/<key>.gltf`, its buffers, textures and
optional `<key>.anim.yaml`) is copied to a scratch tree, imported with the
add-on in a headless Blender, and exported back over the copy with the source
export. The result must keep:
- the flight data: the same nodes per role, in the loader's order, with the
  same AERON_flight_model objects, lights and world transforms;
- every material a primitive or skin uses, with its extras and the same
  texture file in each slot;
- the asset extras, and every extension and extras name (the add-on's guard);
- the animation file, byte for byte (nothing was edited), except for the
  entries of materials nothing uses, which Blender drops with them;
- the textures: none added, none removed.
Both the original and the result are cooked (with their animations), and
must use the same materials and skins.
"""

import argparse
import importlib.util
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

ADDON_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULE = "bl_ext.user_default.aeron_flight_model"
EXTENSION = "AERON_flight_model"
TOLERANCE = 1.0e-4
# Fields at their default (AERON_flight_model's README), which a writer may
# leave out.
DEFAULTS = {
    "model": {"articulations": [], "articulationSettings": {}, "componentSets": {}},
    "component": {"explosionFlags": 0, "targetId": 0, "target": [0.0, 0.0, 0.0], "tags": []},
    "engineGlow": {"enabled": True},
    "light": {"shape": "point", "radius": 0.1, "oneSided": False},
}


# ---- inside Blender ------------------------------------------------------------

def blender_main(argv):
    import addon_utils
    import bpy

    cooker, gltf = argv
    addon_utils.enable(MODULE, default_set=True, handle_error=None)
    bpy.context.preferences.addons[MODULE].preferences.cooker = cooker
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    result = bpy.ops.import_scene.aeron_flight_model(filepath=gltf)
    print("ROUNDTRIP import", result)
    if 'FINISHED' not in result:
        sys.exit(1)
    result = bpy.ops.export_scene.aeron_flight_model_source(filepath=gltf)
    print("ROUNDTRIP export", result)
    for problem in bpy.context.scene.aeron_flight.problems:
        print("ROUNDTRIP problem", problem.object.name if problem.object else "-", problem.message)
    sys.exit(0 if 'FINISHED' in result else 1)


# ---- comparisons -----------------------------------------------------------------

def _sources():
    spec = importlib.util.spec_from_file_location("aeron_sources", os.path.join(ADDON_DIR, "sources.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _close(a, b, path, failures):
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                failures.append("{}.{}: only in the {}".format(path, key, "original" if key in a else "result"))
            else:
                _close(a[key], b[key], "{}.{}".format(path, key), failures)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            failures.append("{}: {} items, now {}".format(path, len(a), len(b)))
        for index, (x, y) in enumerate(zip(a, b)):
            _close(x, y, "{}[{}]".format(path, index), failures)
    elif isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool):
        if abs(a - b) > TOLERANCE * max(1.0, abs(a), abs(b)):
            failures.append("{}: {} -> {}".format(path, a, b))
    elif a != b:
        failures.append("{}: {!r} -> {!r}".format(path, a, b))


def _multiply(a, b):
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4)]


def _local(node):
    if "matrix" in node:
        return node["matrix"]
    x, y, z, w = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    sx, sy, sz = node.get("scale", [1.0, 1.0, 1.0])
    tx, ty, tz = node.get("translation", [0.0, 0.0, 0.0])
    return [(1 - 2 * (y * y + z * z)) * sx, 2 * (x * y + z * w) * sx, 2 * (x * z - y * w) * sx, 0.0,
            2 * (x * y - z * w) * sy, (1 - 2 * (x * x + z * z)) * sy, 2 * (y * z + x * w) * sy, 0.0,
            2 * (x * z + y * w) * sz, 2 * (y * z - x * w) * sz, (1 - 2 * (x * x + y * y)) * sz, 0.0,
            tx, ty, tz, 1.0]


def _flight(document):
    """The flight model's nodes as the loader orders them: (label, node,
    world matrix), lights sorted by position (their order is not data)."""
    nodes = document.get("nodes", [])
    lights = document.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", [])
    parents = {child: index for index, node in enumerate(nodes) for child in node.get("children", [])}
    worlds = {}

    def world(index):
        if index not in worlds:
            local = _local(nodes[index])
            worlds[index] = _multiply(world(parents[index]), local) if index in parents else local
        return worlds[index]

    def role(index):
        return (nodes[index].get("extensions") or {}).get(EXTENSION, {}).get("role")

    items = []
    for root in (index for index in range(len(nodes)) if role(index) == "model"):
        items.append(("model", nodes[root], world(root)))
        components = [c for c in nodes[root].get("children", []) if role(c) == "component"]
        glows = []
        for ordinal, component in enumerate(components):
            items.append(("component {}".format(ordinal), nodes[component], world(component)))
            children = nodes[component].get("children", [])
            hardpoints = [c for c in children if role(c) == "hardpoint"]
            for number, child in enumerate(hardpoints):
                items.append(("component {} hardpoint {}".format(ordinal, number), nodes[child], world(child)))
            glows += [c for c in children if role(c) == "engineGlow"]
            for child in (c for c in children if role(c) == "collision"):
                items.append(("component {} collision".format(ordinal), nodes[child], world(child)))
            found = []
            for child in (c for c in children if role(c) == "light"):
                light = lights[nodes[child]["extensions"]["KHR_lights_punctual"]["light"]]
                light = dict({"color": [1.0, 1.0, 1.0], "intensity": 1.0}, **light)
                light.pop("name", None)
                found.append((world(child)[12:15], child, light))
            for number, (_, child, light) in enumerate(sorted(found)):
                node = dict(nodes[child], light=light)
                items.append(("component {} light {}".format(ordinal, number), node, world(child)))
        for number, child in enumerate(glows):
            items.append(("engine glow {}".format(number), nodes[child], world(child)))
    return items


def _explicit(node):
    """The node's AERON_flight_model object without fields at their default."""
    extension = node.get("extensions", {}).get(EXTENSION, {})
    defaults = DEFAULTS.get(extension.get("role"), {})
    explicit = {}
    for key, value in extension.items():
        failures = []
        if key in defaults:
            _close(defaults[key], value, key, failures)
        if key not in defaults or failures:
            explicit[key] = value
    return explicit


def compare_flight(original, result, failures):
    a, b = _flight(original), _flight(result)
    labels_a, labels_b = [item[0] for item in a], [item[0] for item in b]
    if labels_a != labels_b:
        failures.append("flight nodes: {} -> {} ({})".format(
            len(a), len(b), sorted(set(labels_a) ^ set(labels_b))[:6]))
        return
    size = max([1.0] + [abs(v) for item in a for v in item[2][12:15]])
    for (label, node_a, world_a), (_, node_b, world_b) in zip(a, b):
        _close(_explicit(node_a), _explicit(node_b), label, failures)
        if "light" in node_a:
            _close(node_a["light"], node_b["light"], label + " light", failures)
        for index, (x, y) in enumerate(zip(world_a, world_b)):
            if abs(x - y) > TOLERANCE * (size if index >= 12 else 1.0):
                failures.append("{}: world matrix [{}] {} -> {}".format(label, index, x, y))
                break


def _texture_files(document, directory, material):
    files = {}

    def walk(path, value):
        if isinstance(value, dict):
            if "index" in value and path.endswith("Texture"):
                source = document["textures"][value["index"]].get("source")
                uri = document["images"][source]["uri"] if source is not None else None
                files[path] = os.path.realpath(os.path.join(directory, uri)) if uri else None
                return
            for key, item in value.items():
                walk(path + "/" + key, item)

    walk("", material)
    return files


def _used_materials(document):
    used = set()
    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            if "material" in primitive:
                used.add(primitive["material"])
            mappings = primitive.get("extensions", {}).get("KHR_materials_variants", {}).get("mappings", [])
            used.update(mapping["material"] for mapping in mappings)
    return {document["materials"][index].get("name") for index in used}


def compare_materials(original, result, directory, sources, failures):
    by_name = {material.get("name"): material for material in result.get("materials", [])}
    for material in original.get("materials", []):
        name = material.get("name")
        if name not in by_name:
            if name in _used_materials(original):
                failures.append("material {}: lost".format(name))
            continue
        extras_a = {k: v for k, v in (material.get("extras") or {}).items() if k not in sources.OBSOLETE_EXTRAS}
        _close(extras_a, by_name[name].get("extras") or {}, "material {} extras".format(name), failures)
        files_a = _texture_files(original, directory, material)
        files_b = _texture_files(result, directory, by_name[name])
        if files_a != files_b:
            failures.append("material {}: textures {} -> {}".format(
                name, sorted(files_a.items()), sorted(files_b.items())))
    _close((original.get("asset") or {}).get("extras"), (result.get("asset") or {}).get("extras"),
           "asset extras", failures)
    for item in sources.lost(sources.names(original), result):
        failures.append("lost " + item)


def _cooked(cooker, gltf, effects_dir, out):
    argv = [cooker, gltf, "--out", out, "--quality", "fast"]
    animations = os.path.splitext(gltf)[0] + ".anim.yaml"
    if os.path.exists(animations):
        argv += ["--animations", animations]
        if effects_dir:
            argv += ["--effects-dir", effects_dir]
    completed = subprocess.run(argv, capture_output=True, text=True)
    if completed.returncode != 0:
        return None, (completed.stderr or completed.stdout).strip().splitlines()[-1:]
    document = _sources().read_document(out)
    variants = document.get("extensions", {}).get("KHR_materials_variants", {}).get("variants", [])
    return (sorted(_used_materials(document)), [variant.get("name") for variant in variants]), None


def _listing(directory):
    return sorted(os.path.relpath(os.path.join(base, name), directory)
                  for base, _, names in os.walk(directory) for name in names if name != ".DS_Store")


def roundtrip(args, model_dir, scratch, notes):
    key = os.path.basename(os.path.normpath(model_dir))
    copy = os.path.join(scratch, "models", key)
    shutil.copytree(model_dir, copy)
    gltf = os.path.join(copy, key + ".gltf")
    animations = os.path.join(copy, key + ".anim.yaml")
    original = _sources().read_document(gltf)
    textures_before = [path for path in _listing(copy) if path.startswith("textures" + os.sep)]
    animation_bytes = open(animations, "rb").read() if os.path.exists(animations) else None
    if animation_bytes is not None:
        shutil.copyfile(animations, os.path.join(scratch, key + ".anim.yaml"))
    cooked_before, error = _cooked(args.cooker, gltf, args.effects_dir, os.path.join(scratch, key + ".before.glb"))
    if error:
        return ["the original does not cook: {}".format(error)]

    env = dict(os.environ, BLENDER_USER_EXTENSIONS=os.path.join(scratch, "ext"),
               BLENDER_USER_CONFIG=os.path.join(scratch, "config"),
               BLENDER_USER_SCRIPTS=os.path.join(scratch, "scripts"))
    completed = subprocess.run([args.blender, "-b", "--factory-startup", "--python-exit-code", "1",
                                "--python", os.path.abspath(__file__),
                                "--", "--inside", args.cooker, gltf], env=env, capture_output=True, text=True)
    lines = [line for line in completed.stdout.splitlines() if line.startswith("ROUNDTRIP")]
    if completed.returncode != 0:
        return ["Blender failed: " + " | ".join(lines + completed.stderr.strip().splitlines()[-3:])]

    failures = []
    result = _sources().read_document(gltf)
    sources = _sources()
    compare_flight(original, result, failures)
    compare_materials(original, result, copy, sources, failures)
    textures_after = [path for path in _listing(copy) if path.startswith("textures" + os.sep)]
    if textures_after != textures_before:
        failures.append("textures: added {}, removed {}".format(
            sorted(set(textures_after) - set(textures_before))[:5],
            sorted(set(textures_before) - set(textures_after))[:5]))
    after = open(animations, "rb").read() if os.path.exists(animations) else None
    if after != animation_bytes:
        read = lambda path: _sources().read_animations(args.cooker, path)
        before = read(os.path.join(scratch, key + ".anim.yaml"))
        used = _used_materials(original)
        if read(animations) != {name: entry for name, entry in before.items() if name in used}:
            failures.append("the animation file changed")
        else:
            notes.append("animations of unused materials dropped: " + ", ".join(sorted(set(before) - used)))
    cooked_after, error = _cooked(args.cooker, gltf, args.effects_dir, os.path.join(scratch, key + ".after.glb"))
    if error:
        failures.append("the result does not cook: {}".format(error))
    elif cooked_after[1] != cooked_before[1]:
        failures.append("cooked skins {} -> {}".format(cooked_before[1], cooked_after[1]))
    elif cooked_after[0] != cooked_before[0]:
        failures.append("cooked materials: lost {}, added {}".format(
            sorted(set(cooked_before[0]) - set(cooked_after[0]))[:5],
            sorted(set(cooked_after[0]) - set(cooked_before[0]))[:5]))
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--blender", required=True)
    parser.add_argument("--cooker", required=True)
    parser.add_argument("--effects-dir")
    parser.add_argument("--keep", action="store_true", help="keep the scratch tree")
    parser.add_argument("models", nargs="+")
    args = parser.parse_args()
    args.cooker = os.path.abspath(args.cooker)
    scratch = tempfile.mkdtemp(prefix="aeron_roundtrip_")
    os.makedirs(os.path.join(scratch, "ext", "user_default"))
    os.symlink(ADDON_DIR, os.path.join(scratch, "ext", "user_default", "aeron_flight_model"))
    failed = 0
    for model_dir in args.models:
        notes = []
        failures = roundtrip(args, model_dir, scratch, notes)
        print("{}: {}".format(os.path.basename(os.path.normpath(model_dir)),
                              "ok" if not failures else "{} problem(s)".format(len(failures))))
        for failure in failures[:20]:
            print("  " + failure)
        for note in notes:
            print("  note: " + note)
        failed += bool(failures)
    if args.keep:
        print("scratch:", scratch)
    else:
        shutil.rmtree(scratch, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    if "--inside" in sys.argv:
        blender_main(sys.argv[sys.argv.index("--inside") + 1:])
    else:
        sys.exit(main())
