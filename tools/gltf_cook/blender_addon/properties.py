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

from . import common, overlays


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


class AeronArticulation(AeronComponentRef, PropertyGroup):
    group: StringProperty(name="Group", description="Game-defined group (tag syntax)")
    angle: IntProperty(name="Angle", min=-128, max=255, description="Opened angle, in 1/256 turn")
    speed: IntProperty(name="Speed", min=0, max=255, description="1/256 turn per game step")
    has_speed_back: BoolProperty(name="Own closing speed", default=False)
    speed_back: IntProperty(name="Closing speed", min=0, max=255, description="1/256 turn per game step")
    behavior: StringProperty(name="Behavior", description="Optional game-defined behavior (tag syntax)")


class AeronSetting(PropertyGroup):
    value: IntProperty(name="Value")


class AeronSetMember(AeronComponentRef, PropertyGroup):
    pass


class AeronComponentSet(PropertyGroup):
    members: CollectionProperty(type=AeronSetMember)
    active_member: IntProperty()


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
    target_id: IntProperty(name="Target ID", default=0)
    target: FloatVectorProperty(name="Target", size=3, subtype='TRANSLATION', unit='LENGTH',
                                description="Target point, in the component's local space")
    has_descriptor_geometry: BoolProperty(
        name="Authored descriptor", default=False,
        description="Store the descriptor below instead of the game computing it from the "
                    "collision (or render) mesh's bounds")
    descriptor_span: FloatVectorProperty(name="Span", size=3, subtype='XYZ', unit='LENGTH')
    descriptor_center: FloatVectorProperty(name="Center", size=3, subtype='TRANSLATION', unit='LENGTH')
    descriptor_bounds_min: FloatVectorProperty(name="Bounds min", size=3, subtype='TRANSLATION', unit='LENGTH')
    descriptor_bounds_max: FloatVectorProperty(name="Bounds max", size=3, subtype='TRANSLATION', unit='LENGTH')
    has_rotation: BoolProperty(name="Hinge", default=False,
                               description="The game can turn this component about a hinge")
    pivot: FloatVectorProperty(name="Pivot", size=3, subtype='TRANSLATION', unit='LENGTH')
    rotation_axis: FloatVectorProperty(name="Rotation axis", size=3, subtype='XYZ', default=(0.0, 1.0, 0.0))
    direction_axis: FloatVectorProperty(name="Direction axis", size=3, subtype='XYZ', default=(0.0, 0.0, 1.0))
    up_axis: FloatVectorProperty(name="Up axis", size=3, subtype='XYZ', default=(1.0, 0.0, 0.0))
    active_child: IntProperty()

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
    active_component: IntProperty()
    # Kept from the import (sources.py): the glTF file, its asset extras
    # (JSON), and the extensions and extras names it had (JSON).
    source_path: StringProperty(subtype='FILE_PATH')
    asset_extras: StringProperty()
    source_names: StringProperty()


class AeronMaterialProperties(PropertyGroup):
    # The material's entry of the model animation file, as JSON (sources.py).
    animation: StringProperty()


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
    AeronComponentSet,
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
