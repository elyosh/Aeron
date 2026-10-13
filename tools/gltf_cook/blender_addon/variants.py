# Variants are the looks of a model. Markings values and missions choose them
# by name. A variant can replace materials, paint skins (skins.py) over
# materials, and start from another variant (its base). The model root lists
# its variants. Replaced materials are stored as glTF material variants, in
# the data of Blender's glTF add-on: a list of names on the scene, and
# mappings on the meshes. Layers and the base are stored as a skin variant
# with the same name (tools/extensions/AERON_flight_model, "Skins").
#
# For a variant, a material slot shows:
# - the modeled material with the layers painted over it, if that material
#   has any of the layers' skins;
# - else the variant's replacement;
# - else the base's replacement;
# - else the modeled material.
#
# All models share the scene's name list, and a name is the same variant in
# every model (importing.py). So renaming or removing a variant only changes
# this model's mappings, and leaves the scene list alone.

import bpy
from bpy.app.handlers import persistent
from bpy.props import CollectionProperty, IntProperty, StringProperty
from bpy.types import PropertyGroup

from . import common, skins


# ---- Blender's glTF variant data ----------------------------------------------------

def scene_variants():
    """The scene's glTF variant list, or None. Blender's glTF add-on adds it
    when its Material Variants option is on."""
    return getattr(bpy.data.scenes[0], "gltf2_KHR_materials_variants_variants", None)


def scene_index(name):
    for variant in scene_variants() or ():
        if variant.name == name:
            return variant.variant_idx
    return None


def enable_gltf_variants():
    """Turns on the Material Variants option of Blender's glTF add-on. The
    option adds the variant data, and the exporter writes variants only
    when it is on."""
    addon = bpy.context.preferences.addons.get("io_scene_gltf2")
    if addon is not None and not addon.preferences.KHR_materials_variants_ui:
        addon.preferences.KHR_materials_variants_ui = True


def _add_scene_variant(name):
    variants = scene_variants()
    variant = variants.add()
    variant.variant_idx = len(variants) - 1  # set before the name: its setter uses the index
    variant.name = name
    return variant.variant_idx


def _slots(root):
    """The model's (component, slot index) pairs."""
    for component in common.components(root):
        for index in range(len(component.material_slots)):
            yield component, index


def slot_material(component, index, variant):
    """The material that glTF variant `variant` (an index) shows in the slot, or None."""
    for item in getattr(component.data, "gltf2_variant_mesh_data", ()):
        if item.material_slot_index == index and any(p.variant.variant_idx == variant for p in item.variants):
            return item.material
    return None


def modeled_material(component, index):
    """The slot's modeled material."""
    for item in getattr(component.data, "gltf2_variant_default_materials", ()):
        if item.material_slot_index == index and item.default_material is not None:
            return item.default_material
    return skins.own_material(component.material_slots[index].material)


def _mapped(root):
    """The indices of the glTF variants that the model's meshes map."""
    out = set()
    for component in common.components(root):
        for item in getattr(component.data, "gltf2_variant_mesh_data", ()):
            out.update(pointer.variant.variant_idx for pointer in item.variants)
    return out


def replacements(root, name):
    """The materials that variant `name` replaces on the model, as a list of
    (modeled material, replacement) without repeats."""
    index = scene_index(name)
    out = []
    if index is None:
        return out
    for component, slot in _slots(root):
        replacement = slot_material(component, slot, index)
        if replacement is not None:
            pair = (modeled_material(component, slot), replacement)
            if pair not in out:
                out.append(pair)
    return out


def _unmap(mesh, slot, index):
    """Removes glTF variant `index` from the mappings of a mesh slot."""
    data = mesh.gltf2_variant_mesh_data
    for position in reversed(range(len(data))):
        item = data[position]
        if item.material_slot_index != slot:
            continue
        for pointer in reversed(range(len(item.variants))):
            if item.variants[pointer].variant.variant_idx == index:
                item.variants.remove(pointer)
        if not len(item.variants):
            data.remove(position)


def replace(root, name, modeled, replacement):
    """Makes variant `name` show `replacement` in every slot of the model
    whose modeled material is `modeled`. Replacing a material with itself
    removes the replacement."""
    enable_gltf_variants()
    index = scene_index(name)
    if index is None:
        if replacement == modeled:
            return
        index = _add_scene_variant(name)
    for component, slot in _slots(root):
        if modeled_material(component, slot) != modeled:
            continue
        mesh = component.data
        if not any(item.material_slot_index == slot for item in mesh.gltf2_variant_default_materials):
            default = mesh.gltf2_variant_default_materials.add()
            default.material_slot_index = slot
            default.default_material = modeled
        _unmap(mesh, slot, index)
        if replacement == modeled:
            continue
        item = next((item for item in mesh.gltf2_variant_mesh_data
                     if item.material_slot_index == slot and item.material == replacement), None)
        if item is None:
            item = mesh.gltf2_variant_mesh_data.add()
            item.material_slot_index = slot
            item.material = replacement
        item.variants.add().variant.variant_idx = index


def _move_mappings(root, old, new):
    """Moves the model's mappings of glTF variant `old` to `new` (indices)."""
    for component in common.components(root):
        for item in getattr(component.data, "gltf2_variant_mesh_data", ()):
            for pointer in item.variants:
                if pointer.variant.variant_idx == old:
                    pointer.variant.variant_idx = new


def _rename(root, old, new):
    """Renames the variant's replacements, and the markings, bases and preview that use the old name."""
    props = root.aeron_flight
    index = scene_index(old)
    if index is not None and index in _mapped(root):
        enable_gltf_variants()
        target = scene_index(new)
        _move_mappings(root, index, _add_scene_variant(new) if target is None else target)
    for entry in props.markings:
        if entry.variant == old:
            entry.variant = new
    for variant in props.variants:
        if variant.base == old:
            variant.base = new
    if props.preview_variant == old:
        props.preview_variant = new


def remove(root, position):
    """Removes the model's variant at `position`, with its replacements."""
    props = root.aeron_flight
    name = props.variants[position].name
    index = scene_index(name)
    if index is not None:
        for component, slot in _slots(root):
            if hasattr(component.data, "gltf2_variant_mesh_data"):
                _unmap(component.data, slot, index)
    props.variants.remove(position)
    if props.preview_variant == name:
        props.preview_variant = ""


def sync(root, first=False):
    """Adds the glTF variants that the model's meshes use and that are
    missing from its list. They go at the end, or at the start with
    `first`."""
    props = root.aeron_flight
    if any(component.data is not None and component.data.get("gltf2_variant_mesh_data")
           for component in common.components(root)):
        enable_gltf_variants()
    names = {variant.name for variant in props.variants}
    mapped = _mapped(root)
    position = 0
    for variant in scene_variants() or ():
        if variant.variant_idx in mapped and variant.name not in names:
            props.variants.add().name = variant.name
            names.add(variant.name)
            if first:
                props.variants.move(len(props.variants) - 1, position)
                position += 1


@persistent
def _on_load(*_args):
    for obj in bpy.data.objects:
        if common.role(obj) == 'model':
            sync(obj)


# ---- the model's list ------------------------------------------------------------------

def _root(owner):
    return owner if common.role(owner) == 'model' else None


def _unique(name, taken):
    if name not in taken:
        return name
    number = 1
    while "{}.{:03d}".format(name, number) in taken:
        number += 1
    return "{}.{:03d}".format(name, number)


def _name_update(self, context):
    old, new = self.stored_name, self.name
    if old == new:
        return
    root = _root(self.id_data)
    if root is not None and new:
        taken = {v.name for v in root.aeron_flight.variants if v.as_pointer() != self.as_pointer()}
        if new in taken:
            self.name = _unique(new, taken)  # runs this again with the unique name
            return
        if old:
            _rename(root, old, new)
    self.stored_name = self.name


def _search_bases(self, context, edit_text):
    root = _root(self.id_data)
    names = [v.name for v in root.aeron_flight.variants if v.name != self.name] if root else []
    return [name for name in names if edit_text.lower() in name.lower()]


class AeronVariant(PropertyGroup):
    """A look of the model: replaced materials and skin layers."""
    name: StringProperty(
        name="Name", update=_name_update,
        description="Markings values and missions use this name. Renaming updates the model's markings and "
                    "bases, but not missions")
    stored_name: StringProperty(options={'HIDDEN'})  # the name before a rename
    base: StringProperty(
        name="Base", search=_search_bases, update=skins.preview_update,
        description="Another variant to start from. Where this variant doesn't replace or paint a material, "
                    "the base's replacement shows. Its layers are not used")
    layers: CollectionProperty(type=skins.AeronSkinVariantLayer)
    active_layer: IntProperty()


def variant(root, name):
    for item in root.aeron_flight.variants:
        if item.name == name:
            return item
    return None


def names(root):
    return [item.name for item in root.aeron_flight.variants]


def markings_of(root, name):
    """The markings values that show variant `name`, as text: "1, 3+", or ""."""
    entries = root.aeron_flight.markings
    values = [str(m) + ("+" if m == len(entries) - 1 else "") for m, entry in enumerate(entries)
              if entry.variant == name]
    return ", ".join(values)


# ---- import and export ------------------------------------------------------------------

def restore(root, extension):
    """Reads the model's skin variants. After the import, sync() adds the glTF ones."""
    variants = root.aeron_flight.variants
    variants.clear()
    for item in extension.get("skinVariants", []):
        entry = variants.add()
        entry.name = item.get("name", "")
        entry.base = item.get("base", "")
        for layer in item.get("layers", []):
            added = entry.layers.add()
            added.skin = layer.get("skin", "")
            added.opacity = layer.get("opacity", 100)


def skin_variants_json(root):
    """The model's `skinVariants`: the variants with layers or a base."""
    out = []
    for entry in root.aeron_flight.variants:
        if not len(entry.layers) and not entry.base:
            continue
        item = {"name": entry.name}
        if entry.base:
            item["base"] = entry.base
        item["layers"] = [dict({"skin": layer.skin}, **({"opacity": layer.opacity} if layer.opacity != 100 else {}))
                          for layer in entry.layers]
        out.append(item)
    return out


# ---- checks ------------------------------------------------------------------------------

def check(problems, root):
    skin_names = set(skins.skin_names(root))
    seen = set()
    used = set()
    all_names = set(names(root))
    for entry in root.aeron_flight.variants:
        label = "variant {!r}".format(entry.name)
        if not entry.name:
            problems.error(root, "a variant has no name")
        elif entry.name in seen:
            problems.error(root, "{}: another variant has this name".format(label))
        seen.add(entry.name)
        if entry.base:
            if entry.base == entry.name or entry.base not in all_names:
                problems.error(root, "{}: base {!r} is not another variant of the model".format(label, entry.base))
            elif not replacements(root, entry.base):
                problems.error(root, "{}: base {!r} replaces no materials, and a base gives only its replaced "
                                     "materials".format(label, entry.base))
        elif not len(entry.layers) and not replacements(root, entry.name):
            problems.error(root, "{}: it replaces no materials and has no layers".format(label))
        for layer in entry.layers:
            used.add(layer.skin)
            if layer.skin not in skin_names:
                problems.error(root, "{}: no material has skin {!r}".format(label, layer.skin))
    for name in sorted(skin_names - used):
        problems.warning(root, "no variant uses skin {!r}".format(name))


classes = (AeronVariant,)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.app.handlers.load_post.append(_on_load)


def unregister():
    if _on_load in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
