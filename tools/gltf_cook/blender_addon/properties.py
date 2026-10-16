# The add-on's data on objects, materials and the scene: object roles and
# their fields, and the model root's articulations, settings and component sets.

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

from . import animation, common, display, flipbook_preview, overlays, preview, skins, variants


def _component_poll(self, obj):
    """Accept only the components of the model that owns the property."""
    return common.role(obj) == 'component' and obj.parent == self.id_data


class AeronComponentRef:
    """A reference to a component. It is the object when the component is in
    this model. Otherwise it is an ordinal, for example when a cockpit's
    articulations turn components of its craft."""
    component: PointerProperty(
        name="Component", type=bpy.types.Object, poll=_component_poll,
        description="A component of this model. Leave empty to give a component number instead")
    component_index: IntProperty(
        name="Number", min=0, max=common.MAX_COMPONENT_ORDINAL,
        description="Number of the component when none is picked, such as a craft component that its "
                    "cockpit model turns")


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
    group: StringProperty(name="Group", description="Group defined by the game, written as a tag", update=_group_update)
    angle: IntProperty(name="Angle", min=-128, max=255, description="Angle when open, in 1/256 turn",
                       update=_preview_update)
    speed: IntProperty(name="Speed", min=0, max=255, description="Turning speed, in 1/256 turn per game step")
    has_speed_back: BoolProperty(name="Own Closing Speed", default=False)
    speed_back: IntProperty(name="Closing Speed", min=0, max=255,
                            description="Speed when closing, in 1/256 turn per game step")
    behavior: StringProperty(name="Behavior", description="Optional behavior defined by the game, written as a tag")


class AeronSetting(PropertyGroup):
    value: IntProperty(name="Value")


class AeronSetMember(AeronComponentRef, PropertyGroup):
    pass


class AeronPreviewGroup(PropertyGroup):
    amount: FloatProperty(name="Amount", min=0.0, max=1.0, subtype='FACTOR', update=_preview_update,
                          description="0 shows the members as modeled, 1 turns each one to its articulation's angle")


def _set_preview_update(self, context):
    """Only one set is hidden at a time, because the game applies one profile."""
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
        description="Hide the set's members and their children in the viewport, as the game does for "
                    "the set's profile")


def _markings_update(self, context):
    root = self.id_data
    if root.aeron_flight.preview_markings >= 0:
        display.apply_skin(root)


def _search_variants(self, context, edit_text):
    names = variants.names(self.id_data) if common.role(self.id_data) == 'model' else []
    return [name for name in names if edit_text.lower() in name.lower()]


class AeronMarkingsEntry(PropertyGroup):
    variant: StringProperty(
        name="Variant", update=_markings_update, search=_search_variants,
        description="Variant shown for this markings value. Empty shows the materials as modeled")


def _role_update(self, context):
    obj = self.id_data
    if self.role in {'component', 'hardpoint', 'engineGlow'} and obj.parent is not None:
        siblings = [child for child in obj.parent.children
                    if child != obj and common.role(child) == self.role]
        self.order = common.next_order(siblings)


def _explosion_flag(bit):
    """A checkbox for one bit of the explosion flags. The other bits are kept."""
    def get(self):
        return bool(self.explosion_flags_bits & bit)

    def set(self, value):
        self.explosion_flags_bits = self.explosion_flags_bits | bit if value else self.explosion_flags_bits & ~bit

    return get, set


class AeronFlightProperties(PropertyGroup):
    role: EnumProperty(
        name="Role",
        items=[
            ('none', "None", "Not part of a flight model"),
            ('model', "Model", "Root of a flight model"),
            ('component', "Component", "A part of the flight model"),
            ('hardpoint', "Hardpoint", "A weapon or attachment point on its component"),
            ('engineGlow', "Engine Glow", "An engine glow on its component"),
            ('collision', "Collision", "The collision mesh of its component, never rendered"),
        ],
        default='none',
        update=_role_update,
    )
    # Position among siblings with the same role, set by the order lists.
    order: IntProperty(name="Order", default=0, min=0)

    # Component.
    mesh_type: EnumProperty(name="Mesh Type", items=common.enum_items(common.MESH_TYPE_NAMES), default='0')
    explosion_flags_bits: IntProperty()
    # The explosion flags of the OPT format, which OPT editors call Type 1 to 3.
    # The tooltips give their meaning in X-Wing Alliance.
    explosion_type1: BoolProperty(
        name="Hit Through Shields", get=_explosion_flag(1)[0], set=_explosion_flag(1)[1],
        description="Explosion type 1. In XWA, the component takes damage even while the shields are up, "
                    "except on the hardest difficulty")
    explosion_type2: BoolProperty(
        name="Destructible", get=_explosion_flag(2)[0], set=_explosion_flag(2)[1],
        description="Explosion type 2. In XWA, the component has hit points, shows its health when "
                    "targeted, and breaks off when destroyed")
    explosion_type3: BoolProperty(
        name="Type 3", get=_explosion_flag(4)[0], set=_explosion_flag(4)[1],
        description="Explosion type 3. Not used by XWA")
    tags: StringProperty(
        name="Tags",
        description="Tags defined by the game, separated by commas, such as hangarcrane.arm. "
                    "Use lower-case letters, digits, '.' and '_'")
    target_id: IntProperty(
        name="Target ID", default=0,
        description="Components with the same nonzero ID and mesh type are targeted as one, through the "
                    "first of them. A nonzero ID also makes the game use the target point")
    target: FloatVectorProperty(
        name="Target", size=3, subtype='TRANSLATION', unit='LENGTH',
        description="Where the game puts the targeting box, and the point the component turns around when "
                    "it breaks off. Without a target ID, the game uses the center of the bounds")
    has_descriptor_geometry: BoolProperty(
        name="Custom Bounds", default=False,
        description="Export the bounds below instead of letting the game compute them from the collision mesh "
                    "(or else the render mesh)")
    descriptor_span: FloatVectorProperty(name="Span", size=3, subtype='XYZ', unit='LENGTH')
    descriptor_center: FloatVectorProperty(name="Center", size=3, subtype='TRANSLATION', unit='LENGTH')
    descriptor_bounds_min: FloatVectorProperty(name="Bounds Min", size=3, subtype='TRANSLATION', unit='LENGTH')
    descriptor_bounds_max: FloatVectorProperty(name="Bounds Max", size=3, subtype='TRANSLATION', unit='LENGTH')
    has_rotation: BoolProperty(name="Hinge", default=False, update=_preview_update,
                               description="The game can turn this component around a hinge")
    pivot: FloatVectorProperty(name="Pivot", size=3, subtype='TRANSLATION', unit='LENGTH', update=_preview_update)
    rotation_axis: FloatVectorProperty(name="Rotation Axis", size=3, subtype='XYZ', default=(0.0, 1.0, 0.0),
                                       update=_preview_update)
    direction_axis: FloatVectorProperty(name="Direction Axis", size=3, subtype='XYZ', default=(0.0, 0.0, 1.0))
    up_axis: FloatVectorProperty(name="Up Axis", size=3, subtype='XYZ', default=(1.0, 0.0, 0.0))
    active_child: IntProperty()
    # Articulation preview (preview.py): the component's own preview angle,
    # the turn the preview applied, and its transform when the preview started.
    preview_angle: IntProperty(
        name="Preview Angle", min=-128, max=255, update=_preview_update,
        description="Angle of the component in the preview, in 1/256 turn. When not 0, it overrides the "
                    "articulation groups")
    preview_matrix: FloatVectorProperty(size=16, default=preview.IDENTITY)
    preview_rest: FloatVectorProperty(size=17)  # its transform channels when the preview started
    # Hidden by the Hide Members option of a component set (display.py).
    preview_hidden: BoolProperty()

    # Hardpoint.
    hardpoint_type: EnumProperty(
        name="Type", items=common.enum_items(common.HARDPOINT_TYPE_NAMES), default='0')

    # Engine glow.
    glow_enabled: BoolProperty(name="Enabled", default=True)
    # sRGB, as the file format stores them.
    core_color: FloatVectorProperty(name="Core Color", size=4, subtype='COLOR_GAMMA', min=0.0, max=1.0,
                                    default=(1.0, 0.8, 0.3, 1.0))
    outer_color: FloatVectorProperty(name="Outer Color", size=4, subtype='COLOR_GAMMA', min=0.0, max=1.0,
                                     default=(1.0, 0.2, 0.0, 0.0))

    # Light (a Blender light under a component).
    light_exclude: BoolProperty(name="Exclude from Model", default=False,
                                description="Leave this light out of the exported model")
    light_tube: BoolProperty(name="Tube", default=False,
                             description="Export this area light as a tube. Its X size is the length "
                                         "and its Y size the thickness")
    light_one_sided: BoolProperty(name="One-Sided", default=False,
                                  description="The tube emits only toward the light's -Z, like a strip")

    # Model root.
    articulations: CollectionProperty(type=AeronArticulation)
    active_articulation: IntProperty()
    articulation_settings: CollectionProperty(type=AeronSetting)
    active_setting: IntProperty()
    component_sets: CollectionProperty(type=AeronComponentSet)
    active_set: IntProperty()
    # The model's variants (variants.py).
    variants: CollectionProperty(type=variants.AeronVariant)
    active_variant: IntProperty()
    # The material variant each markings value shows. Values past the end
    # show the last entry's variant.
    markings: CollectionProperty(type=AeronMarkingsEntry)
    active_markings: IntProperty()
    preview_active: BoolProperty()
    preview_groups: CollectionProperty(type=AeronPreviewGroup)
    # The markings value shown in the viewport, or -1 for the materials as
    # modelled (display.py).
    preview_markings: IntProperty(default=-1, min=-1)
    # The variant shown in the viewport when no markings value is (Variants
    # panel), or "".
    preview_variant: StringProperty()
    # The skin shown alone when no markings value or variant is (Aeron Skins
    # panel), or "".
    preview_skin: StringProperty()
    active_component: IntProperty()
    # Kept from the import (sources.py): the glTF file, its asset extras
    # (JSON), and the names of the extensions and extras it had (JSON).
    source_path: StringProperty(
        name="Model File", subtype='FILE_PATH',
        description="The .gltf the model was imported from or last saved to")
    asset_extras: StringProperty()
    source_names: StringProperty()
    # The model's energy default, which is `energy` in its AERON_flight_model
    # extension. Materials without their own energy effects use it (animation.py).
    energy_default_given: BoolProperty(
        name="Default Energy Effects", description="Energy effects for every material that has none of its own")
    energy_default: PointerProperty(type=animation.AeronEnergy)


class AeronMaterialProperties(animation.AeronAnimation, PropertyGroup):
    """The AERON_flight_model data of a material (animation.py), and its skins (skins.py)."""
    skins: CollectionProperty(type=skins.AeronSkinLayer)
    active_skin: IntProperty()
    # A skin preview copy (skins.py): the material it shows, and the layers.
    preview_of: PointerProperty(type=bpy.types.Material)
    preview_key: StringProperty()


class AeronProblem(PropertyGroup):
    message: StringProperty()
    object: PointerProperty(type=bpy.types.Object)
    warning: BoolProperty()


def _game_preview_update(self, context):
    # Soft Falloff stays off when the preview ends: the game has none.
    for obj in context.scene.objects:
        if common.is_model_light(obj):
            obj.data.use_shadow = not self.game_preview
            if self.game_preview and obj.data.type == 'POINT':
                obj.data.use_soft_falloff = False


class AeronSceneProperties(PropertyGroup):
    problems: CollectionProperty(type=AeronProblem)
    active_problem: IntProperty()
    opt_project: StringProperty(
        name="OPT Project", subtype='DIR_PATH',
        description="Project that Import XWA OPT uses by default in this file")
    game_preview: BoolProperty(
        name="Game-Like Lights", default=False, update=_game_preview_update,
        description="Turn off the shadows and the Soft Falloff of model lights, as the game draws them "
                    "without either")
    # The game states active in the flipbook preview (flipbook_preview.py).
    flipbook_states: PointerProperty(type=flipbook_preview.AeronFlipbookStates)

    # Viewport overlays (overlays.py).
    overlays: BoolProperty(name="Overlays", default=True, update=overlays.tag_redraw,
                           description="Draw the game data of the active object's flight model in the viewport")
    overlay_glows: BoolProperty(name="Engine Glows", default=True, update=overlays.tag_redraw,
                                description="Draw engine glows as the game draws them from this view")
    overlay_throttle: FloatProperty(
        name="Throttle", default=1.0, min=0.0, max=1.0, subtype='FACTOR', update=overlays.tag_redraw,
        description="Throttle for the engine glows, which sets their size and length")
    overlay_hardpoints: BoolProperty(name="Hardpoints", default=True, update=overlays.tag_redraw,
                                     description="Draw hardpoints, colored by family")
    overlay_projectiles: BoolProperty(
        name="Projectiles", default=True, update=overlays.tag_redraw,
        description="Draw the projectile that each weapon hardpoint fires: the project's model, or else one "
                    "converted from the game folder set in the preferences (XWA Upgrade Folder)")
    overlay_hardpoint_labels: BoolProperty(name="Hardpoint Labels", default=False, update=overlays.tag_redraw,
                                           description="Show each hardpoint's number and type")
    overlay_descriptors: BoolProperty(
        name="Bounds", default=False, update=overlays.tag_redraw,
        description="Draw the bounds of the components. Bounds that the game computes from the mesh are faint")
    overlay_targets: BoolProperty(name="Targets", default=True, update=overlays.tag_redraw,
                                  description="Draw target points, with a line to their component")
    overlay_hinges: BoolProperty(name="Hinges", default=True, update=overlays.tag_redraw,
                                 description="Draw hinge pivots and axes, and the open angle of each articulation")
    overlay_lights: BoolProperty(name="Light Shapes", default=True, update=overlays.tag_redraw,
                                 description="Draw the shape the game gives each model light")
    overlay_light_ranges: BoolProperty(name="Light Ranges", default=False, update=overlays.tag_redraw,
                                       description="Draw the range of model lights that set one with Custom "
                                                   "Distance")
    overlay_component_labels: BoolProperty(name="Component Labels", default=False, update=overlays.tag_redraw,
                                           description="Show each component's number and name")
    overlay_in_front: BoolProperty(name="In Front", default=False, update=overlays.tag_redraw,
                                   description="Draw the markers on top of the model, even where it hides them")


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
