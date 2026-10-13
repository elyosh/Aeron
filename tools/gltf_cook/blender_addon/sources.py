# Data in a model's .gltf that Blender does not keep when it imports a glTF.
# The asset extras are kept on the model root. The names of the file's
# extensions, extras and material AERON_flight_model members are recorded
# too, so that an export that would lose one of them stops. Blender is
# imported only where needed, because tests/roundtrip.py uses this module
# outside Blender.

import json
import os
import re
import struct

EXTENSION_NAME = "AERON_flight_model"

# Extensions that an export may drop without losing data. Blender leaves
# them out when they hold only defaults, such as a default specular or
# emissive strength or an identity texture transform, or when the scene does
# not use them (lights, skins).
OPTIONAL_EXTENSIONS = {
    "KHR_lights_punctual",
    "KHR_materials_emissive_strength",
    "KHR_materials_specular",
    "KHR_materials_variants",
    "KHR_texture_transform",
}
_EXTRAS_LEVELS = ("asset", "scenes", "nodes", "meshes", "materials", "textures", "images")


def material_extensions(document):
    """The AERON_flight_model objects of the materials in a glTF document, by material name."""
    return {material.get("name"): material["extensions"][EXTENSION_NAME]
            for material in document.get("materials", [])
            if EXTENSION_NAME in (material.get("extensions") or {})}


def model_extension(document):
    """The AERON_flight_model object of the model root in a glTF document, or {}."""
    scenes = document.get("scenes") or [{}]
    roots = scenes[document.get("scene", 0)].get("nodes") or []
    nodes = document.get("nodes", [])
    for index in roots:
        extension = (nodes[index].get("extensions") or {}).get(EXTENSION_NAME) or {}
        if extension.get("role") == "model":
            return extension
    return {}


def model_project(model_path):
    """The project that holds the model file `model_path`
    (<project>/models/<key>/<key>.gltf), or "" for a model outside a
    project."""
    models = os.path.dirname(os.path.dirname(model_path)) if model_path else ""
    return os.path.dirname(models) if os.path.basename(models) == "models" else ""


def project_effects(model_path):
    """<project>/effects for a model in a project, or "" when there is
    none."""
    project = model_project(model_path)
    effects = os.path.join(project, "effects") if project else ""
    return effects if effects and os.path.isdir(effects) else ""


def built_libraries(messages):
    """The effect libraries that a cook built, read from its messages."""
    return re.findall(r"^\[aeron_gltf_cook\] library: ([a-z0-9_-]+): built", messages or "", re.MULTILINE)


def library_changes(messages):
    """The keys that a cook removed or changed when it rebuilt the libraries,
    as [(library, key, what)]. `what` is "removed" or "changed kind from
    <old> to <new>"."""
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
    """Reads the JSON of a .gltf, or the JSON chunk of a .glb."""
    with open(path, "rb") as stream:
        data = stream.read()
    if data[:4] == b"glTF":
        length, kind = struct.unpack_from("<II", data, 12)
        if kind != 0x4E4F534A:
            raise ValueError("no JSON chunk")
        data = data[20:20 + length]
    return json.loads(data.decode("utf-8"))


def names(document):
    """The extensions of a glTF document, its extras keys at each level, and
    the members of the AERON_flight_model objects of its materials."""
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
    """The names in `recorded` (from names()) that the exported `document` lacks."""
    now = names(document)
    missing = ["extension " + name for name in recorded.get("extensions", [])
               if name not in now["extensions"] and name not in OPTIONAL_EXTENSIONS]
    for level, keys in recorded.get("extras", {}).items():
        missing += ["{} extras {}".format(level, key) for key in keys if key not in now["extras"].get(level, [])]
    missing += ["material {} {}".format(EXTENSION_NAME, member) for member in recorded.get("materialMembers", [])
                if member not in now["materialMembers"]]
    return missing


def check_markings(problems, root):
    """Checks that each markings value names a variant of the model."""
    from . import variants

    try:
        json.loads(root.aeron_flight.asset_extras or "{}")
    except ValueError:
        problems.error(root, "the asset extras kept from the import are not valid JSON")
        return
    names = set(variants.names(root))
    for markings, entry in enumerate(root.aeron_flight.markings):
        if entry.variant and entry.variant not in names:
            problems.error(root, "markings value {}: variant {!r} does not exist".format(markings, entry.variant))


def model_materials(root):
    """The materials that the model's meshes use, including those of their variants."""
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
    """The images of the model's materials and of their skins."""
    from . import skins

    images, seen = set(), set()
    for material in model_materials(root):
        if material.aeron_flight.preview_of is None:
            images.update(_tree_images(material.node_tree, seen))
    images.update(skins.skin_images(root))
    return images
