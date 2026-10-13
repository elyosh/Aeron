# Shows a model in the viewport the way the game shows it for a profile or
# a markings value. For a profile, the members of a component set are hidden.
# For a markings value, the materials of a variant are used.
#
# A set hides its members and everything under them, and flags each object
# it hid. When it shows them again, it only shows the flagged objects.
# A markings value puts the meshes' glTF variant materials in their material
# slots. Blender's glTF exporter restores the original materials before it
# writes the file. The add-on's export does the same, and shows the variant
# again afterwards.

import bpy

from . import common, skins

PROFILE_PREFIX = "hidden_by_profile."


def profile_name(set_name):
    """Returns the name of the profile that the set `set_name` belongs to, or None if it is not a
    profile set. The name is decoded as the game encodes it: lower case, with '.' and two hex digits
    for any other byte."""
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
    """Hides the members of the model's sets whose preview is on. Shows again
    the objects that a set hid but no longer hides."""
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


# ---- markings -------------------------------------------------------------------------


def _variants():
    # The variants property exists only once Blender's glTF add-on has
    # registered it, which it does when a file has variants.
    return getattr(bpy.data.scenes[0], "gltf2_KHR_materials_variants_variants", ())


def markings_count(root):
    return len(root.aeron_flight.markings)


def _variant_index(name):
    for variant in _variants():
        if variant.name == name:
            return variant.variant_idx
    return None


def _slot_variant_material(component, index, variant):
    """The material that glTF variant `variant` puts in slot `index`, or None."""
    for item in getattr(component.data, "gltf2_variant_mesh_data", ()):
        if item.material_slot_index == index and any(p.variant.variant_idx == variant for p in item.variants):
            return item.material
    return None


def previewing(root):
    """Whether the viewport shows a markings value or a skin variant of the model."""
    return root.aeron_flight.preview_markings >= 0 or bool(root.aeron_flight.preview_variant)


def apply_skin(root, markings=None):
    """Shows the materials of a markings value on the model's components.
    `markings` defaults to what the preview shows (a markings value, or else
    the skin variant `preview_variant`), and -1 shows the original
    materials. A skin variant shows preview copies of the materials it
    changes (skins.py)."""
    props = root.aeron_flight
    name = ""
    if markings is None:
        markings = props.preview_markings
        if markings < 0:
            name = props.preview_variant
    skins.restore(root)
    entries = props.markings
    if markings >= 0 and len(entries):
        name = entries[min(markings, len(entries) - 1)].variant
    skin_variant = skins.skin_variant(root, name) if name else None
    if skin_variant is not None:
        base = _variant_index(skin_variant.base) if skin_variant.base else None
        skins.show(root, skin_variant,
                   lambda component, index: _slot_variant_material(component, index, base) if base is not None
                   else None)
        skins.remove_unused()
        return
    variant = _variant_index(name) if name else None
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
    skins.remove_unused()


def models_showing_skins():
    return [obj for obj in bpy.data.objects
            if common.role(obj) == 'model' and previewing(obj)]
