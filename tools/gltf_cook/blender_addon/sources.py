# What a model's .gltf holds besides what Blender keeps of a glTF: the asset
# extras, kept on the model root, and the names of the extensions, extras and
# material AERON_flight_model members the file had, so that an export losing
# one stops. Blender is imported only where needed: tests/roundtrip.py reads
# files with it outside Blender.

import json
import os
import re
import struct

EXTENSION_NAME = "AERON_flight_model"

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
_EXTRAS_LEVELS = ("asset", "scenes", "nodes", "meshes", "materials", "textures", "images")


def material_extensions(document):
    """The AERON_flight_model objects of a glTF document's materials, by name."""
    return {material.get("name"): material["extensions"][EXTENSION_NAME]
            for material in document.get("materials", [])
            if EXTENSION_NAME in (material.get("extensions") or {})}


def model_extension(document):
    """The AERON_flight_model object of a glTF document's model root, or {}."""
    scenes = document.get("scenes") or [{}]
    roots = scenes[document.get("scene", 0)].get("nodes") or []
    nodes = document.get("nodes", [])
    for index in roots:
        extension = (nodes[index].get("extensions") or {}).get(EXTENSION_NAME) or {}
        if extension.get("role") == "model":
            return extension
    return {}


def project_effects(model_path):
    """The effects folder of the project holding the model file `model_path`
    (<project>/models/<key>/<key>.gltf): <project>/effects, or "" for a
    model outside a project."""
    models = os.path.dirname(os.path.dirname(model_path)) if model_path else ""
    effects = os.path.join(os.path.dirname(models), "effects")
    return effects if os.path.basename(models) == "models" and os.path.isdir(effects) else ""


def built_libraries(messages):
    """The effect libraries a cook's messages say it built."""
    return re.findall(r"^\[aeron_gltf_cook\] library: ([a-z0-9_-]+): built", messages or "", re.MULTILINE)


def library_changes(messages):
    """What a cook's library rebuilds removed: [(library, key, what)], `what`
    "removed" or "changed kind from <old> to <new>"."""
    changes = []
    for library, rest in re.findall(r"^\[aeron_gltf_cook\] library: ([a-z0-9_-]+): (\".*)$", messages or "",
                                    re.MULTILINE):
        key, end = json.JSONDecoder().raw_decode(rest)
        changes.append((library, key, rest[end:].strip()))
    for library, rest in re.findall(r"^\[aeron_gltf_cook\] library: ([a-z0-9_-]+): removed (\".*)$",
                                    messages or "", re.MULTILINE):
        changes.append((library, json.loads(rest), "removed"))
    return changes


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
    """The extensions and extras keys of a glTF document, per level, and the
    members of its materials' AERON_flight_model objects."""
    extras = {}
    for level in _EXTRAS_LEVELS:
        items = [document.get("asset")] if level == "asset" else document.get(level) or []
        keys = set()
        for item in items:
            if isinstance(item, dict) and isinstance(item.get("extras"), dict):
                keys.update(item["extras"])
        if keys:
            extras[level] = sorted(keys)
    members = set()
    for extension in material_extensions(document).values():
        members.update(extension)
    return {"extensions": sorted(set(document.get("extensionsUsed", []))), "extras": extras,
            "materialMembers": sorted(members)}


def lost(recorded, document):
    """What of `recorded` (names()) the exported `document` lacks."""
    now = names(document)
    missing = ["extension " + name for name in recorded.get("extensions", [])
               if name not in now["extensions"] and name not in OPTIONAL_EXTENSIONS]
    for level, keys in recorded.get("extras", {}).items():
        missing += ["{} extras {}".format(level, key) for key in keys if key not in now["extras"].get(level, [])]
    missing += ["material {} {}".format(EXTENSION_NAME, member) for member in recorded.get("materialMembers", [])
                if member not in now["materialMembers"]]
    return missing


def check_markings(problems, root):
    """The markings list names variants that the scene has and that some part
    of the model maps (Blender's glTF exporter writes only those)."""
    import bpy

    try:
        json.loads(root.aeron_flight.asset_extras or "{}")
    except ValueError:
        problems.error(root, "the kept asset extras are not JSON")
        return
    # Blender's glTF add-on registers its variants only once a file had some.
    variants = {variant.name: variant.variant_idx
                for variant in getattr(bpy.data.scenes[0], "gltf2_KHR_materials_variants_variants", ())}
    mapped = set()
    for obj in root.children_recursive:
        if obj.type == 'MESH':
            for item in getattr(obj.data, "gltf2_variant_mesh_data", ()):
                mapped.update(pointer.variant.variant_idx for pointer in item.variants)
    for markings, entry in enumerate(root.aeron_flight.markings):
        if not entry.variant:
            continue
        if entry.variant not in variants:
            problems.error(root, "markings value {} shows the material variant {!r}, which does not "
                                 "exist".format(markings, entry.variant))
        elif variants[entry.variant] not in mapped:
            problems.error(root, "markings value {} shows the material variant {!r}, which no part of the "
                                 "model maps: the export would leave it out".format(markings, entry.variant))


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
