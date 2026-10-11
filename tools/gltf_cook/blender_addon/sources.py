# What a model's source holds besides what Blender keeps of a glTF: the model
# animation file (`<model>.anim.yaml`, tools/effect_pack/README.md), kept per
# material; the asset extras (the skins' aeronSkinMarkings and
# aeronAssetVariants), kept on the model root; and the names of the
# extensions and extras the file had, so that an export losing one stops.
# Blender is imported only where needed: tests/roundtrip.py reads files with
# it outside Blender.

import json
import os
import struct
import subprocess

ANIMATION_SUFFIX = ".anim.yaml"

# Extensions an export may lack without losing data: Blender writes these
# only while they hold something (a specular or emissive strength at its
# default, an identity transform), or the scene holds them (lights, skins).
OPTIONAL_EXTENSIONS = {
    "KHR_lights_punctual",
    "KHR_materials_emissive_strength",
    "KHR_materials_specular",
    "KHR_materials_variants",
    "KHR_texture_transform",
}
# Extras the cooker derives, or that are no longer written.
OBSOLETE_EXTRAS = {"aeronAnimBaseColor", "aeronVariants"}
_EXTRAS_LEVELS = ("asset", "scenes", "nodes", "meshes", "materials", "textures", "images")


def animation_path(gltf_path):
    return os.path.splitext(gltf_path)[0] + ANIMATION_SUFFIX


def read_animations(cooker, path):
    """The materials of a model animation file, {name: entry}, as the cooker
    reads and checks them. Raises RuntimeError with the cooker's message."""
    completed = subprocess.run([cooker, "--animations-json", path], capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError((completed.stderr or "").strip() or "aeron_gltf_cook exit {}".format(
            completed.returncode))
    return json.loads(completed.stdout)["materials"]


def write_animations(path, materials):
    """Writes a model animation file, one material per line."""
    lines = ["version: 1", "materials:" if materials else "materials: {}"]
    for name, entry in materials.items():
        lines.append("  {}: {}".format(json.dumps(name), json.dumps(entry, separators=(",", ":"))))
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        stream.write("\n".join(lines) + "\n")


def material_animations(names):
    """The animation entries of the Blender materials `names` that have one."""
    import bpy

    entries = {}
    for name in names:
        material = bpy.data.materials.get(name)
        if material is not None and material.aeron_flight.animation:
            entries[name] = json.loads(material.aeron_flight.animation)
    return entries


def read_document(path):
    """The JSON of a .gltf, or of a .glb's JSON chunk."""
    with open(path, "rb") as stream:
        data = stream.read()
    if data[:4] == b"glTF":
        length, kind = struct.unpack_from("<II", data, 12)
        if kind != 0x4E4F534A:
            raise ValueError("no JSON chunk")
        data = data[20:20 + length]
    return json.loads(data.decode("utf-8"))


def names(document):
    """The extensions and extras keys of a glTF document, per level."""
    extras = {}
    for level in _EXTRAS_LEVELS:
        items = [document.get("asset")] if level == "asset" else document.get(level) or []
        keys = set()
        for item in items:
            if isinstance(item, dict) and isinstance(item.get("extras"), dict):
                keys.update(item["extras"])
        if keys:
            extras[level] = sorted(keys)
    return {"extensions": sorted(set(document.get("extensionsUsed", []))), "extras": extras}


def lost(recorded, document):
    """What of `recorded` (names()) the exported `document` lacks."""
    now = names(document)
    missing = ["extension " + name for name in recorded.get("extensions", [])
               if name not in now["extensions"] and name not in OPTIONAL_EXTENSIONS]
    for level, keys in recorded.get("extras", {}).items():
        missing += ["{} extras {}".format(level, key) for key in keys
                    if key not in now["extras"].get(level, []) and key not in OBSOLETE_EXTRAS]
    return missing


def check_asset_extras(problems, root):
    """The skins' asset extras name variants that the scene still has."""
    import bpy

    try:
        extras = json.loads(root.aeron_flight.asset_extras or "{}")
    except ValueError:
        problems.error(root, "the kept asset extras are not JSON")
        return
    # Blender's glTF add-on registers its variants only once a file had some.
    variants = [variant.name for variant in getattr(bpy.data.scenes[0], "gltf2_KHR_materials_variants_variants", ())]
    for name in extras.get("aeronSkinMarkings", []):
        if name is not None and name not in variants:
            problems.error(root, "skin markings name the material variant {!r}, which no longer "
                                 "exists".format(name))
    count = extras.get("aeronAssetVariants")
    if count is not None and not 1 <= count <= len(variants):
        problems.error(root, "the model's own paint states are its first {} material variant(s), but "
                             "there are {}".format(count, len(variants)))


def model_materials(root):
    """The materials the model's meshes use, their skin variants included."""
    materials = set()
    for obj in [root] + list(root.children_recursive):
        materials.update(slot.material for slot in obj.material_slots if slot.material is not None)
        if obj.type == 'MESH':
            materials.update(item.material for item in getattr(obj.data, "gltf2_variant_mesh_data", ())
                             if item.material is not None)
    return materials


def _tree_images(tree, seen):
    images = []
    if tree is None or tree in seen:
        return images
    seen.add(tree)
    for node in tree.nodes:
        if node.type == 'TEX_IMAGE' and node.image is not None:
            images.append(node.image)
        elif node.type == 'GROUP':
            images += _tree_images(node.node_tree, seen)
    return images


def model_images(root):
    images, seen = set(), set()
    for material in model_materials(root):
        images.update(_tree_images(material.node_tree, seen))
    return images
