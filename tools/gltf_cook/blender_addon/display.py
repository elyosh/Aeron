# Shows a model in the viewport the way the game shows it for a profile or
# a markings value. For a profile, the members of a component set are hidden.
# For a markings value, the materials of a variant are used.
#
# A set hides its members and everything under them, and flags each object
# it hid. When it shows them again, it only shows the flagged objects.
# A markings value puts the materials of its variant (variants.py) in the
# material slots. Blender's glTF exporter restores the original materials
# before it writes the file. The add-on's export does the same, and shows
# the variant again afterwards.

from types import SimpleNamespace

import bpy

from . import common, skins, variants

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


def markings_count(root):
    return len(root.aeron_flight.markings)


def previewing(root):
    """Whether the viewport shows a markings value, a variant or a skin of the model."""
    props = root.aeron_flight
    return props.preview_markings >= 0 or bool(props.preview_variant) or bool(props.preview_skin)


def apply_skin(root, markings=None):
    """Shows the materials of a markings value on the model's components.
    `markings` defaults to what the preview shows (a markings value, else the
    variant `preview_variant`, else the skin `preview_skin` alone), and -1
    shows the materials as modeled.
    A variant's layers show as preview copies of the materials they paint
    (skins.py)."""
    props = root.aeron_flight
    name = skin = ""
    if markings is None:
        markings = props.preview_markings
        if markings < 0:
            name = props.preview_variant
            skin = "" if name else props.preview_skin
    skins.restore(root)
    entries = props.markings
    if markings >= 0 and len(entries):
        name = entries[min(markings, len(entries) - 1)].variant
    entry = variants.variant(root, name) if name else None
    if skin:  # a variant with that skin alone
        entry = SimpleNamespace(base="", layers=[SimpleNamespace(skin=skin, opacity=100)])
    own = variants.scene_index(name) if name else None
    base = variants.scene_index(entry.base) if entry is not None and entry.base else None
    for component in common.components(root):
        for index, slot in enumerate(component.material_slots):
            modeled = variants.modeled_material(component, index)
            if modeled is None:
                continue
            layers = skins.chain(modeled, entry) if entry is not None else []
            if layers:
                material = skins.preview(modeled, layers)
            else:
                material = ((own is not None and variants.slot_material(component, index, own)) or
                            (base is not None and variants.slot_material(component, index, base)) or modeled)
            if slot.material != material:
                slot.material = material
    _show_variant_lights(root, name if previewing(root) else None)
    skins.remove_unused()


def _show_variant_lights(root, name):
    """Shows the lights that shine with variant `name` ("" for the materials
    as modeled) and hides the other lights that name variants. With None,
    every light shows."""
    for component in common.components(root):
        for child in component.children:
            if child.type != 'LIGHT' or not len(child.aeron_flight.light_variants):
                continue
            shown = name is None or name in {entry.variant for entry in child.aeron_flight.light_variants}
            try:
                child.hide_set(not shown)
            except RuntimeError:  # not in the view layer
                pass


def models_showing_skins():
    return [obj for obj in bpy.data.objects
            if common.role(obj) == 'model' and previewing(obj)]
