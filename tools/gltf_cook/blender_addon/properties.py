# The add-on's data on objects (roles and their fields, the model root's
# articulations, settings and component sets) and on the scene.

import bpy
from bpy.props import (
    BoolProperty,
    CollectionProperty,
    EnumProperty,
    FloatProperty,
    FloatVectorProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import PropertyGroup

from . import animation, common, display, flipbook_preview, overlays, preview


def _component_poll(self, obj):
    """Components of the model that owns the property."""
    return common.role(obj) == 'component' and obj.parent == self.id_data


class AeronComponentRef:
    """A component ordinal: the object when it is in this model, otherwise a
    raw ordinal (a cockpit's articulations name its craft's components)."""
    component: PointerProperty(
        name="Component", type=bpy.types.Object, poll=_component_poll,
        description="A component of this model; empty for a raw ordinal")
    component_index: IntProperty(
        name="Ordinal", min=0, max=common.MAX_COMPONENT_ORDINAL,
        description="Component ordinal when no component is picked, such as a craft's "
                    "component that its cockpit model turns")


def _preview_update(self, context):
    root = common.model_root(self.id_data)
    if root is not None:
        preview.apply(root)


def _group_update(self, context):
    root = common.model_root(self.id_data)
    if root is not None:
        preview.sync_groups(root)
        preview.apply(root)


class AeronArticulation(AeronComponentRef, PropertyGroup):
    group: StringProperty(name="Group", description="Game-defined group (tag syntax)", update=_group_update)
    angle: IntProperty(name="Angle", min=-128, max=255, description="Opened angle, in 1/256 turn",
                       update=_preview_update)
    speed: IntProperty(name="Speed", min=0, max=255, description="1/256 turn per game step")
    has_speed_back: BoolProperty(name="Own closing speed", default=False)
    speed_back: IntProperty(name="Closing speed", min=0, max=255, description="1/256 turn per game step")
    behavior: StringProperty(name="Behavior", description="Optional game-defined behavior (tag syntax)")


class AeronSetting(PropertyGroup):
    value: IntProperty(name="Value")


class AeronSetMember(AeronComponentRef, PropertyGroup):
    pass


class AeronPreviewGroup(PropertyGroup):
    amount: FloatProperty(name="Amount", min=0.0, max=1.0, subtype='FACTOR', update=_preview_update,
                          description="0: as modelled; 1: each member at its articulation's angle")


def _set_preview_update(self, context):
    """One set hidden at a time, as the game applies one profile."""
    root = self.id_data
    if self.preview:
        for other in root.aeron_flight.component_sets:
            if other != self and other.preview:
                other.preview = False
    display.apply_sets(root)


class AeronComponentSet(PropertyGroup):
    members: CollectionProperty(type=AeronSetMember)
    active_member: IntProperty()
    preview: BoolProperty(
        name="Hide Members", update=_set_preview_update,
        description="Hide the set's members (and what is under them) in the viewport, as the game hides "
                    "a profile's set")


def _markings_update(self, context):
    root = self.id_data
    if root.aeron_flight.preview_markings >= 0:
        display.apply_skin(root)


class AeronMarkingsEntry(PropertyGroup):
    variant: StringProperty(
        name="Variant", update=_markings_update,
        description="The material variant this markings value shows; empty: the materials as modelled")


def _role_update(self, context):
    obj = self.id_data
    if self.role in {'component', 'hardpoint', 'engineGlow'} and obj.parent is not None:
        siblings = [child for child in obj.parent.children
                    if child != obj and common.role(child) == self.role]
        self.order = common.next_order(siblings)


def _get_explosion_flags(self):
    return str(self.explosion_flags_bits & 0xFFFFFFFF)


def _set_explosion_flags(self, text):
    try:
        value = int(text.strip(), 0)
    except ValueError:
        return
    if 0 <= value <= 0xFFFFFFFF:
        self.explosion_flags_bits = value - (1 << 32) if value >= (1 << 31) else value


class AeronFlightProperties(PropertyGroup):
    role: EnumProperty(
        name="Role",
        items=[
            ('none', "None", "Not part of a flight model"),
            ('model', "Model", "Root of one flight model"),
            ('component', "Component", "A part of the model, in the model's component order"),
            ('hardpoint', "Hardpoint", "A weapon or attachment point of its component"),
            ('engineGlow', "Engine glow", "An engine glow of its component"),
            ('collision', "Collision", "Collision mesh of its component (not rendered)"),
        ],
        default='none',
        update=_role_update,
    )
    # Ordinal order among siblings; set by the order lists, not typed.
    order: IntProperty(name="Order", default=0, min=0)

    # Component.
    mesh_type: EnumProperty(name="Mesh type", items=common.enum_items(common.MESH_TYPE_NAMES), default='0')
    explosion_flags_bits: IntProperty()
    explosion_flags: StringProperty(
        name="Explosion flags", get=_get_explosion_flags, set=_set_explosion_flags,
        description="Unsigned 32-bit value (decimal, or 0x hexadecimal)")
    tags: StringProperty(
        name="Tags",
        description="Comma-separated game-defined tags (lower-case letters, digits, "
                    "'.' and '_'), e.g. hangarcrane.arm")
    target_id: IntProperty(
        name="Target ID", default=0,
        description="Components sharing a nonzero ID and their mesh type are targeted as one, by the first; "
                    "a nonzero ID also makes the game use the target point")
    target: FloatVectorProperty(
        name="Target", size=3, subtype='TRANSLATION', unit='LENGTH',
        description="Where the game aims the component's targeting box and turns it about when it breaks "
                    "off, in the component's local space; used with a target ID (otherwise the "
                    "descriptor's center)")
    has_descriptor_geometry: BoolProperty(
        name="Authored descriptor", default=False,
        description="Store the descriptor below instead of the game computing it from the "
                    "collision (or render) mesh's bounds")
    descriptor_span: FloatVectorProperty(name="Span", size=3, subtype='XYZ', unit='LENGTH')
    descriptor_center: FloatVectorProperty(name="Center", size=3, subtype='TRANSLATION', unit='LENGTH')
    descriptor_bounds_min: FloatVectorProperty(name="Bounds min", size=3, subtype='TRANSLATION', unit='LENGTH')
    descriptor_bounds_max: FloatVectorProperty(name="Bounds max", size=3, subtype='TRANSLATION', unit='LENGTH')
    has_rotation: BoolProperty(name="Hinge", default=False, update=_preview_update,
                               description="The game can turn this component about a hinge")
    pivot: FloatVectorProperty(name="Pivot", size=3, subtype='TRANSLATION', unit='LENGTH', update=_preview_update)
    rotation_axis: FloatVectorProperty(name="Rotation axis", size=3, subtype='XYZ', default=(0.0, 1.0, 0.0),
                                       update=_preview_update)
    direction_axis: FloatVectorProperty(name="Direction axis", size=3, subtype='XYZ', default=(0.0, 0.0, 1.0))
    up_axis: FloatVectorProperty(name="Up axis", size=3, subtype='XYZ', default=(1.0, 0.0, 0.0))
    active_child: IntProperty()
    # Articulation preview (preview.py): the component's own angle, the turn
    # the preview gave it, and its transform when the preview started.
    preview_angle: IntProperty(
        name="Try angle", min=-128, max=255, update=_preview_update,
        description="Turn the component to this angle (1/256 turn) in the preview, whatever its groups")
    preview_matrix: FloatVectorProperty(size=16, default=preview.IDENTITY)
    preview_rest: FloatVectorProperty(size=17)  # its transform channels at the start
    # Hidden by a component set's preview (display.py).
    preview_hidden: BoolProperty()

    # Hardpoint.
    hardpoint_type: EnumProperty(
        name="Type", items=common.enum_items(common.HARDPOINT_TYPE_NAMES), default='0')

    # Engine glow.
    glow_enabled: BoolProperty(name="Enabled", default=True)
    # sRGB, as the format stores them.
    core_color: FloatVectorProperty(name="Core color", size=4, subtype='COLOR_GAMMA', min=0.0, max=1.0,
                                    default=(1.0, 0.8, 0.3, 1.0))
    outer_color: FloatVectorProperty(name="Outer color", size=4, subtype='COLOR_GAMMA', min=0.0, max=1.0,
                                     default=(1.0, 0.2, 0.0, 0.0))

    # Light (a Blender light under a component).
    light_exclude: BoolProperty(name="Not a model light", default=False,
                                description="Leave this light out of the exported model")
    light_tube: BoolProperty(name="Tube", default=False,
                             description="Export this area light as a tube along its X size, "
                                         "its Y size the tube's thickness")
    light_one_sided: BoolProperty(name="One-sided", default=False,
                                  description="The tube emits only toward the light's -Z, as a strip")

    # Model root.
    articulations: CollectionProperty(type=AeronArticulation)
    active_articulation: IntProperty()
    articulation_settings: CollectionProperty(type=AeronSetting)
    active_setting: IntProperty()
    component_sets: CollectionProperty(type=AeronComponentSet)
    active_set: IntProperty()
    # The material variant each markings value shows; past the end, the
    # last entry's.
    markings: CollectionProperty(type=AeronMarkingsEntry)
    active_markings: IntProperty()
    preview_active: BoolProperty()
    preview_groups: CollectionProperty(type=AeronPreviewGroup)
    # The markings value whose look the components show; -1: their own
    # materials (display.py).
    preview_markings: IntProperty(default=-1, min=-1)
    active_component: IntProperty()
    # Kept from the import (sources.py): the glTF file, its asset extras
    # (JSON), and the extensions and extras names it had (JSON).
    source_path: StringProperty(
        name="Model File", subtype='FILE_PATH',
        description="The .gltf the model was imported from or last saved to")
    asset_extras: StringProperty()
    source_names: StringProperty()
    # The model's energy default (its AERON_flight_model `energy`): the
    # energy effects of every material without its own (animation.py).
    energy_default_given: BoolProperty(
        name="Energy Default", description="The energy effects of every material that has none of its own")
    energy_default: PointerProperty(type=animation.AeronEnergy)


class AeronMaterialProperties(animation.AeronAnimation, PropertyGroup):
    """The material's AERON_flight_model object (animation.py)."""


class AeronProblem(PropertyGroup):
    message: StringProperty()
    object: PointerProperty(type=bpy.types.Object)
    warning: BoolProperty()


def _game_preview_update(self, context):
    for obj in context.scene.objects:
        if common.is_model_light(obj):
            obj.data.use_shadow = not self.game_preview


class AeronSceneProperties(PropertyGroup):
    problems: CollectionProperty(type=AeronProblem)
    active_problem: IntProperty()
    game_preview: BoolProperty(
        name="Game-like light preview", default=False, update=_game_preview_update,
        description="Turn off the model lights' shadows, as the game draws them "
                    "(turning it off again turns their shadows on)")
    # The game states active in the flipbook preview (flipbook_preview.py).
    flipbook_states: PointerProperty(type=flipbook_preview.AeronFlipbookStates)

    # Viewport overlays (overlays.py).
    overlays: BoolProperty(name="Overlays", default=True, update=overlays.tag_redraw,
                           description="Draw the active object's flight model as the game uses it")
    overlay_glows: BoolProperty(name="Engine glows", default=True, update=overlays.tag_redraw,
                                description="Engine glows as the game draws them, seen from the view")
    overlay_throttle: FloatProperty(
        name="Throttle", default=1.0, min=0.0, max=1.0, subtype='FACTOR', update=overlays.tag_redraw,
        description="The engine glows' size and length follow the throttle")
    overlay_hardpoints: BoolProperty(name="Hardpoints", default=True, update=overlays.tag_redraw,
                                     description="Hardpoints, coloured by family")
    overlay_hardpoint_labels: BoolProperty(name="Hardpoint labels", default=False, update=overlays.tag_redraw,
                                           description="Each hardpoint's ordinal and type")
    overlay_descriptors: BoolProperty(
        name="Descriptors", default=False, update=overlays.tag_redraw,
        description="Descriptor boxes: authored, or faint where the game computes them from the mesh")
    overlay_targets: BoolProperty(name="Targets", default=True, update=overlays.tag_redraw,
                                  description="Target points, joined to their component")
    overlay_hinges: BoolProperty(name="Hinges", default=True, update=overlays.tag_redraw,
                                 description="Hinge pivots and axes, and each articulation's opened angle")
    overlay_lights: BoolProperty(name="Light shapes", default=True, update=overlays.tag_redraw,
                                 description="The shape the game gives each model light")
    overlay_light_ranges: BoolProperty(name="Light ranges", default=False, update=overlays.tag_redraw,
                                       description="The range of model lights that have one (Custom Distance)")
    overlay_component_labels: BoolProperty(name="Component labels", default=False, update=overlays.tag_redraw,
                                           description="Each component's ordinal and name")
    overlay_in_front: BoolProperty(name="In front", default=False, update=overlays.tag_redraw,
                                   description="Draw the markers over the model instead of hidden by it")


classes = (
    AeronArticulation,
    AeronSetting,
    AeronSetMember,
    AeronPreviewGroup,
    AeronComponentSet,
    AeronMarkingsEntry,
    AeronFlightProperties,
    AeronMaterialProperties,
    AeronProblem,
    AeronSceneProperties,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Object.aeron_flight = PointerProperty(type=AeronFlightProperties)
    bpy.types.Material.aeron_flight = PointerProperty(type=AeronMaterialProperties)
    bpy.types.Scene.aeron_flight = PointerProperty(type=AeronSceneProperties)


def unregister():
    del bpy.types.Scene.aeron_flight
    del bpy.types.Material.aeron_flight
    del bpy.types.Object.aeron_flight
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
