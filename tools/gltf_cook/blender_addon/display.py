# What the viewport shows of a model as the game would for a profile or a
# markings value: a component set's members hidden, a skin's materials.
#
# A set hides its members and everything under them, flagging what it hid,
# and shows back only what it flagged. A skin swaps material slots between
# the meshes' glTF variant materials; Blender's glTF exporter puts the
# original materials back before writing, and so does the add-on's export,
# which shows the skin again afterwards.

import json

import bpy

from . import common

PROFILE_PREFIX = "hidden_by_profile."


def profile_name(set_name):
    """The profile whose set `set_name` is, as the game names it (lower
    case, '.' and two hex digits for other bytes), or None."""
    if not set_name.startswith(PROFILE_PREFIX):
        return None
    encoded, name, index = set_name[len(PROFILE_PREFIX):], "", 0
    while index < len(encoded):
        try:
            if encoded[index] == "." and len(encoded) >= index + 3:
                name += chr(int(encoded[index + 1:index + 3], 16))
                index += 3
                continue
        except ValueError:
            pass
        name += encoded[index]
        index += 1
    return name


# ---- component sets ----------------------------------------------------------------


def apply_sets(root):
    """Hides the members of the model's sets shown hidden, and shows back
    what a set hid and no longer does."""
    hidden = set()
    for component_set in root.aeron_flight.component_sets:
        if component_set.preview:
            for member in component_set.members:
                component = member.component
                if component is not None and component.parent == root:
                    hidden.add(component)
                    hidden.update(component.children_recursive)
    for obj in root.children_recursive:
        props = obj.aeron_flight
        try:
            if obj in hidden and not props.preview_hidden and not obj.hide_get():
                obj.hide_set(True)
                props.preview_hidden = True
            elif obj not in hidden and props.preview_hidden:
                obj.hide_set(False)
                props.preview_hidden = False
        except RuntimeError:  # not in the view layer
            pass


# ---- skins -------------------------------------------------------------------------


def _variants():
    # Blender's glTF add-on registers its variants only once a file had some.
    return getattr(bpy.data.scenes[0], "gltf2_KHR_materials_variants_variants", ())


def skin_extras(root):
    """The asset extras naming the model's skins, kept from the import."""
    try:
        return json.loads(root.aeron_flight.asset_extras or "{}")
    except ValueError:
        return {}


def markings_count(root):
    """The markings values with a skin of their own: the named ones and the
    model's paint states; 0 without variants."""
    variants = len(_variants())
    if not variants:
        return 0
    extras = skin_extras(root)
    return max(len(extras.get("aeronSkinMarkings") or ()), extras.get("aeronAssetVariants", variants))


def markings_variant(root, markings):
    """The variant index the game shows for `markings`, as the renderer
    picks it: the skin named for it, otherwise the paint state
    min(markings, paint states - 1). None without variants."""
    variants = _variants()
    if not len(variants):
        return None
    extras = skin_extras(root)
    names = extras.get("aeronSkinMarkings") or ()
    if markings < len(names) and names[markings] is not None:
        for variant in variants:
            if variant.name == names[markings]:
                return variant.variant_idx
        return None
    return min(markings, extras.get("aeronAssetVariants", len(variants)) - 1)


def apply_skin(root, markings=None):
    """Shows the skin of the model's chosen markings value (`markings`
    when given; -1 for the original materials) on its components."""
    if markings is None:
        markings = root.aeron_flight.preview_markings
    variant = markings_variant(root, markings) if markings >= 0 else None
    for component in common.components(root):
        mesh = component.data
        defaults = {item.material_slot_index: item.default_material
                    for item in getattr(mesh, "gltf2_variant_default_materials", ())}
        mapped = {}
        if variant is not None:
            for item in getattr(mesh, "gltf2_variant_mesh_data", ()):
                if any(pointer.variant.variant_idx == variant for pointer in item.variants):
                    mapped[item.material_slot_index] = item.material
        for index, slot in enumerate(component.material_slots):
            if mapped.get(index) is not None:
                material = mapped[index]
            elif index in defaults:
                material = defaults[index]
            else:
                continue
            if slot.material != material:
                slot.material = material


def models_showing_skins():
    return [obj for obj in bpy.data.objects
            if common.role(obj) == 'model' and obj.aeron_flight.preview_markings >= 0]
