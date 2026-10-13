# Panels and lists: the Object properties panels for each role, and the
# Aeron tab of the 3D view. The panels for material animations and effect
# libraries are in animation_ui.py.

import os

import bpy
from bpy.types import Panel, UIList

from . import common, display, opt_import, overlays, preview

# Labels of the objects shown in the lists, keyed by list kind and then by
# object name. filter_items fills it, and it runs before draw_item.
_labels = {}


def _select_button(layout, obj):
    layout.operator("aeron.select", text="", icon='RESTRICT_SELECT_OFF', emboss=False).name = obj.name


def _glow_base(component):
    """Returns the number of engine glows in the components before this one."""
    root = component.parent
    count = 0
    for other in common.components(root):
        if other == component:
            break
        count += sum(1 for child in other.children if common.role(child) == 'engineGlow')
    return count


def _child_labels(component):
    labels, hardpoint, glow = {}, 0, _glow_base(component)
    for child in common.ordered_children(component):
        if common.role(child) == 'hardpoint':
            labels[child.name] = "Hardpoint {}: {}".format(
                hardpoint, common.HARDPOINT_TYPE_NAMES[int(child.aeron_flight.hardpoint_type)])
            hardpoint += 1
        else:
            labels[child.name] = "Engine Glow {}".format(glow)
            glow += 1
    return labels


class _ObjectList(UIList):
    """Lists the scene objects but shows only `members(context)`, in their order."""

    kind = ""

    def members(self, context):
        return []

    def labels(self, members):
        return {obj.name: "{}  {}".format(index, obj.name) for index, obj in enumerate(members)}

    def filter_items(self, context, data, propname):
        objects = getattr(data, propname)
        members = self.members(context)
        position = {obj.name: index for index, obj in enumerate(members)}
        _labels[self.kind] = self.labels(members)
        flags, order, rest = [], [], len(members)
        for obj in objects:
            if obj.name in position:
                flags.append(self.bitflag_filter_item)
                order.append(position[obj.name])
            else:
                flags.append(0)
                order.append(rest)
                rest += 1
        return flags, order


class AERON_UL_components(_ObjectList):
    kind = "components"

    def members(self, context):
        root = common.model_root(context.object)
        return common.components(root) if root else []

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        row.label(text=_labels.get(self.kind, {}).get(item.name, item.name), icon='MESH_DATA')
        row.label(text=common.MESH_TYPE_NAMES[int(item.aeron_flight.mesh_type)])
        _select_button(row, item)


class AERON_UL_children(_ObjectList):
    kind = "children"

    def members(self, context):
        return common.ordered_children(context.object) if common.role(context.object) == 'component' else []

    def labels(self, members):
        return _child_labels(members[0].parent) if members else {}

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        hardpoint = common.role(item) == 'hardpoint'
        row.label(text=_labels.get(self.kind, {}).get(item.name, item.name),
                  icon='EMPTY_AXIS' if hardpoint else 'LIGHT_SUN')
        row.label(text=item.name)
        _select_button(row, item)


class AERON_UL_references(UIList):
    """Articulations and set members. Shows the component, or the ordinal when no component is set."""

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        name = item.component.name if item.component else "component {}".format(item.component_index)
        if hasattr(item, "group"):
            row.label(text=item.group or "(no group)", icon='CON_ROTLIKE')
        row.label(text=name, icon='MESH_DATA' if item.component else 'LINENUMBERS_ON')


class AERON_UL_named(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        row.prop(item, "name", text="", emboss=False)
        if hasattr(item, "value"):
            row.prop(item, "value", text="")
        elif hasattr(item, "members"):
            row.label(text="{} component(s)".format(len(item.members)))
            row.prop(item, "preview", text="", icon='HIDE_ON' if item.preview else 'HIDE_OFF', emboss=False)


class AERON_UL_markings(UIList):
    """The markings list. Each row gives the variant of a markings value, with a button to show it."""

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        split = layout.split(factor=0.12)
        split.label(text=str(index))
        row = split.row(align=True)
        row.prop(item, "variant", text="")  # searches glTF and skin variants
        shown = data.preview_markings == index
        row.operator("aeron.show_skin", text="", icon='HIDE_OFF' if shown else 'HIDE_ON',
                     emboss=False).markings = index


class AERON_UL_problems(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        text = "{}: {}".format(item.object.name, item.message) if item.object else item.message
        row.label(text=text, icon='INFO' if item.warning else 'ERROR')
        if item.object:
            _select_button(row, item.object)


def _list_buttons(layout, target):
    column = layout.column(align=True)
    for action, button in (('ADD', 'ADD'), ('REMOVE', 'REMOVE')):
        operator = column.operator("aeron.list_edit", text="", icon=button)
        operator.target, operator.action = target, action


def _move_buttons(layout, target):
    column = layout.column(align=True)
    for direction, button in (('UP', 'TRIA_UP'), ('DOWN', 'TRIA_DOWN')):
        operator = column.operator("aeron.move", text="", icon=button)
        operator.target, operator.direction = target, direction


def _reference(layout, item):
    layout.prop(item, "component")
    if item.component is None:
        layout.prop(item, "component_index")


# ---- Object properties -------------------------------------------------------

class _ObjectPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'object'


class _RolePanel(_ObjectPanel):
    bl_parent_id = "OBJECT_PT_aeron"
    role = ''

    @classmethod
    def poll(cls, context):
        return common.role(context.object) == cls.role


def _status(obj):
    """Returns where the object is in its model, or why it is not exported."""
    role = common.role(obj)
    root = common.model_root(obj)
    if role == 'model':
        return "Model root with {} component(s)".format(len(common.components(obj))), 'INFO'
    if role == 'component':
        if common.role(obj.parent) != 'model':
            return "Not under a model root: not exported", 'ERROR'
        return "Component {} of {}".format(common.components(obj.parent).index(obj), root.name), 'INFO'
    if role in {'hardpoint', 'engineGlow', 'collision'} and common.role(obj.parent) != 'component':
        return "Not under a component: not exported", 'ERROR'
    if role in {'hardpoint', 'engineGlow'}:
        return "{} of {}".format(_child_labels(obj.parent).get(obj.name, ""), obj.parent.name), 'INFO'
    if role == 'collision':
        return "Collision mesh of {} (never rendered)".format(obj.parent.name), 'INFO'
    return None, None


class OBJECT_PT_aeron(_ObjectPanel, Panel):
    bl_label = "Aeron Flight Model"

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.type in {'EMPTY', 'MESH', 'LIGHT'}

    def draw(self, context):
        obj = context.object
        if obj.type == 'LIGHT':
            draw_light(self.layout, obj)
            return
        props = obj.aeron_flight
        layout = self.layout
        layout.use_property_split = True
        layout.prop(props, "role")
        text, icon = _status(obj)
        if text:
            layout.label(text=text, icon=icon)
        if props.role == 'hardpoint':
            layout.prop(props, "hardpoint_type")
        elif props.role == 'engineGlow':
            layout.prop(props, "glow_enabled")
            layout.prop(props, "core_color")
            layout.prop(props, "outer_color")
            column = layout.column(align=True)
            column.label(text="Glows toward -Y")
            column.label(text="X and Z scale set its width")
            column.label(text="Y scale sets its length")


def draw_light(layout, obj):
    props = obj.aeron_flight
    light = obj.data
    if common.role(obj.parent) != 'component':
        layout.label(text="Parent to a component to export it", icon='INFO')
        return
    layout.prop(props, "light_exclude")
    if props.light_exclude:
        return
    if light.type not in {'POINT', 'AREA'}:
        layout.label(text="{} lights are not supported".format(light.type.capitalize()), icon='ERROR')
        return
    if light.type == 'AREA':
        row = layout.row()
        row.prop(props, "light_tube")
        if props.light_tube:
            row.prop(props, "light_one_sided")
    column = layout.column(align=True)
    column.label(text="In Game: " + common.light_game_shape(obj))
    column.label(text="Intensity: {:.5g} cd".format(common.light_punctual(obj, obj.matrix_world)["intensity"]))
    if light.use_custom_distance:
        column.label(text="Range: {:.4g} m, fading out".format(light.cutoff_distance))
    else:
        column.label(text="Range: auto (set Custom Distance)")
    layout.label(text="Set power, color, size in Light tab", icon='LIGHT')


class OBJECT_PT_aeron_components(_RolePanel, Panel):
    bl_label = "Components"
    role = 'model'

    def draw(self, context):
        obj = context.object
        row = self.layout.row()
        row.template_list("AERON_UL_components", "", context.scene, "objects", obj.aeron_flight,
                          "active_component", rows=8)
        _move_buttons(row, 'components')
        self.layout.label(text="List order is the component number")


class OBJECT_PT_aeron_model_file(_RolePanel, Panel):
    bl_label = "Model File"
    bl_options = {'DEFAULT_CLOSED'}
    role = 'model'

    def draw(self, context):
        props = context.object.aeron_flight
        if not props.source_path:
            self.layout.label(text="Not saved")
            return
        row = self.layout.row()
        row.enabled = False
        row.prop(props, "source_path", text="")


class OBJECT_PT_aeron_material_animations(_RolePanel, Panel):
    bl_label = "Material Animations"
    bl_options = {'DEFAULT_CLOSED'}
    role = 'model'

    def draw(self, context):
        props = context.object.aeron_flight
        layout = self.layout
        # The model's default energy state (animation.py).
        layout.prop(props, "energy_default_given")
        row = layout.row(align=True)
        row.enabled = props.energy_default_given
        for name in ("shields", "tractor", "jamming"):
            row.prop(props.energy_default, name, toggle=True)
        layout.operator("aeron.reload_animations", icon='FILE_REFRESH')


class OBJECT_PT_aeron_markings(_RolePanel, Panel):
    bl_label = "Markings"
    role = 'model'

    @classmethod
    def poll(cls, context):
        return super().poll(context) and (display.markings_count(context.object) > 0 or
                                          len(context.object.aeron_flight.variants))

    def draw(self, context):
        root = context.object
        props = root.aeron_flight
        layout = self.layout
        layout.label(text="Variant shown per markings value:")
        row = layout.row()
        row.template_list("AERON_UL_markings", "", props, "markings", props, "active_markings", rows=3)
        _list_buttons(row, 'markings')
        column = layout.column(align=True)
        column.label(text="Empty list: materials as modeled")
        column.label(text="Higher values use the last row")
        layout.operator("aeron.show_skin", text="Show As Modeled",
                        depress=not display.previewing(root)).markings = -1


class OBJECT_PT_aeron_articulations(_RolePanel, Panel):
    bl_label = "Articulations"
    bl_options = {'DEFAULT_CLOSED'}
    role = 'model'

    def draw(self, context):
        props = context.object.aeron_flight
        layout = self.layout
        row = layout.row()
        row.template_list("AERON_UL_references", "articulations", props, "articulations", props,
                          "active_articulation", rows=3)
        _list_buttons(row, 'articulations')
        if 0 <= props.active_articulation < len(props.articulations):
            item = props.articulations[props.active_articulation]
            column = layout.column()
            column.use_property_split = True
            _reference(column, item)
            column.prop(item, "group")
            column.prop(item, "angle")
            column.prop(item, "speed")
            column.prop(item, "has_speed_back")
            if item.has_speed_back:
                column.prop(item, "speed_back")
            column.prop(item, "behavior")


class OBJECT_PT_aeron_preview(_RolePanel, Panel):
    """One slider per articulation group, from 0 (as built) to 1 (each member at its articulation angle)"""
    bl_label = "Articulation Preview"
    role = 'model'

    def draw(self, context):
        props = context.object.aeron_flight
        layout = self.layout
        column = layout.column(align=True)
        for group in props.preview_groups:
            column.prop(group, "amount", text=group.name, slider=True)
        if not len(props.preview_groups):
            column.label(text="No articulation groups yet")
        row = layout.row(align=True)
        playing = preview.playing(context.object)
        row.operator("aeron.articulation_play", text="Pause" if playing else "Play",
                     icon='PAUSE' if playing else 'PLAY')
        if props.preview_active:
            row.operator("aeron.articulation_reset", icon='LOOP_BACK')


class OBJECT_PT_aeron_settings(_RolePanel, Panel):
    bl_label = "Articulation Settings"
    bl_options = {'DEFAULT_CLOSED'}
    role = 'model'

    def draw(self, context):
        props = context.object.aeron_flight
        row = self.layout.row()
        row.template_list("AERON_UL_named", "settings", props, "articulation_settings", props,
                          "active_setting", rows=3)
        _list_buttons(row, 'articulation_settings')


class OBJECT_PT_aeron_sets(_RolePanel, Panel):
    bl_label = "Component Sets"
    bl_options = {'DEFAULT_CLOSED'}
    role = 'model'

    def draw(self, context):
        props = context.object.aeron_flight
        layout = self.layout
        row = layout.row()
        row.template_list("AERON_UL_named", "sets", props, "component_sets", props, "active_set", rows=3)
        _list_buttons(row, 'component_sets')
        if not 0 <= props.active_set < len(props.component_sets):
            return
        component_set = props.component_sets[props.active_set]
        layout.prop(component_set, "preview")
        profile = display.profile_name(component_set.name)
        if profile is not None:
            layout.label(text="Hidden by the profile " + profile, icon='INFO')
        layout.label(text="Members of " + component_set.name)
        row = layout.row()
        row.template_list("AERON_UL_references", "members", component_set, "members", component_set,
                          "active_member", rows=3)
        _list_buttons(row, 'members')
        operator = layout.operator("aeron.list_edit", text="Add Selected Components")
        operator.target, operator.action = 'members', 'ADD_SELECTED'
        if 0 <= component_set.active_member < len(component_set.members):
            column = layout.column()
            column.use_property_split = True
            _reference(column, component_set.members[component_set.active_member])


class OBJECT_PT_aeron_component(_RolePanel, Panel):
    bl_label = "Component"
    role = 'component'

    def draw(self, context):
        props = context.object.aeron_flight
        layout = self.layout
        layout.use_property_split = True
        layout.prop(props, "mesh_type")
        column = layout.column(heading="Explosion", align=True)
        column.prop(props, "explosion_type1")
        column.prop(props, "explosion_type2")
        column.prop(props, "explosion_type3")
        other = props.explosion_flags_bits & 0xFFFFFFF8
        if other:
            column.label(text="Other flags: 0x{:X}".format(other))
        layout.prop(props, "tags")
        layout.prop(props, "target_id")
        row = layout.row(align=True)
        row.active = props.target_id != 0
        row.prop(props, "target")
        row.operator("aeron.from_cursor", text="", icon='PIVOT_CURSOR').field = 'target'


class OBJECT_PT_aeron_children(_RolePanel, Panel):
    bl_label = "Hardpoints and Engine Glows"
    role = 'component'

    def draw(self, context):
        obj = context.object
        layout = self.layout
        row = layout.row()
        row.template_list("AERON_UL_children", "", context.scene, "objects", obj.aeron_flight,
                          "active_child", rows=4)
        _move_buttons(row, 'children')
        layout.label(text="Engine glows are numbered model-wide")
        draw_add_buttons(layout)


class OBJECT_PT_aeron_descriptor(_RolePanel, Panel):
    bl_label = "Bounds"
    bl_options = {'DEFAULT_CLOSED'}
    role = 'component'

    def draw_header(self, context):
        self.layout.prop(context.object.aeron_flight, "has_descriptor_geometry", text="")

    def draw(self, context):
        props = context.object.aeron_flight
        layout = self.layout
        if not props.has_descriptor_geometry:
            layout.label(text="The game computes it from the mesh")
            layout.operator("aeron.descriptor_from_bounds", text="Set From Mesh Bounds")
            return
        layout.label(text="In the component's local space")
        column = layout.column()
        column.use_property_split = True
        column.prop(props, "descriptor_span")
        column.prop(props, "descriptor_center")
        column.prop(props, "descriptor_bounds_min")
        column.prop(props, "descriptor_bounds_max")
        layout.operator("aeron.descriptor_from_bounds")


class OBJECT_PT_aeron_component_articulations(_RolePanel, Panel):
    """The articulations of the model root that turn this component"""
    bl_label = "Articulations"
    role = 'component'

    def draw(self, context):
        obj = context.object
        layout = self.layout
        root = obj.parent if common.role(obj.parent) == 'model' else None
        if root is None:
            layout.label(text="Not under a model root", icon='ERROR')
            return
        entries = [(index, item) for index, item in enumerate(root.aeron_flight.articulations)
                   if item.component == obj]
        if entries and not obj.aeron_flight.has_rotation:
            layout.label(text="Enable Hinge so it can turn", icon='ERROR')
        if not entries:
            layout.label(text="No articulation turns it")
        for index, item in entries:
            box = layout.box()
            row = box.row(align=True)
            row.prop(item, "group", text="", icon='CON_ROTLIKE')
            operator = row.operator("aeron.component_articulation", text="", icon='X')
            operator.action, operator.index = 'REMOVE', index
            column = box.column(align=True)
            column.use_property_split = True
            row = column.row(align=True)
            row.prop(item, "angle")
            row.label(text="{:.1f}\u00b0".format(item.angle * 360.0 / 256.0))
            column.prop(item, "speed")
            row = column.row(align=True, heading="Closing Speed")
            row.prop(item, "has_speed_back", text="")
            sub = row.row(align=True)
            sub.active = item.has_speed_back
            sub.prop(item, "speed_back", text="")
            column.prop(item, "behavior")
        operator = layout.operator("aeron.component_articulation", text="Add Articulation", icon='ADD')
        operator.action = 'ADD'


class OBJECT_PT_aeron_hinge(_RolePanel, Panel):
    bl_label = "Hinge"
    bl_options = {'DEFAULT_CLOSED'}
    role = 'component'

    def draw_header(self, context):
        self.layout.prop(context.object.aeron_flight, "has_rotation", text="")

    def draw(self, context):
        props = context.object.aeron_flight
        layout = self.layout
        layout.active = props.has_rotation
        layout.label(text="In the component's local space")
        column = layout.column()
        column.use_property_split = True
        row = column.row(align=True)
        row.prop(props, "pivot")
        row.operator("aeron.from_cursor", text="", icon='PIVOT_CURSOR').field = 'pivot'
        column.prop(props, "rotation_axis")
        column.prop(props, "direction_axis")
        column.prop(props, "up_axis")
        layout.operator("aeron.from_cursor", text="Axes From 3D Cursor", icon='ORIENTATION_CURSOR').field = 'axes'
        row = layout.row()
        row.prop(props, "preview_angle", slider=True)
        row.label(text="{:.1f}\u00b0".format(props.preview_angle * 360.0 / 256.0))
        root = common.model_root(context.object)
        if root is not None and root.aeron_flight.preview_active:
            layout.operator("aeron.articulation_reset", icon='LOOP_BACK')


# ---- 3D view -------------------------------------------------------------------

def draw_add_buttons(layout):
    grid = layout.grid_flow(columns=2, align=True)
    for kind, text, icon in (('hardpoint', "Hardpoint", 'EMPTY_AXIS'), ('engineGlow', "Engine Glow", 'LIGHT_SUN'),
                             ('collision', "Collision", 'MOD_PHYSICS'), ('point', "Point", 'LIGHT_POINT'),
                             ('panel', "Panel", 'LIGHT_AREA'), ('tube', "Tube", 'MESH_CYLINDER')):
        grid.operator("aeron.add", text=text, icon=icon).kind = kind


class VIEW3D_PT_aeron(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Aeron"
    bl_label = "Flight Model"

    def draw(self, context):
        layout = self.layout
        root = common.model_root(context.active_object)
        if root is not None:
            parts = common.components(root)
            children = [child for part in parts for child in part.children]
            box = layout.box()
            box.label(text=root.name, icon='EMPTY_DATA')
            box.label(text="{} components, {} lights".format(
                len(parts), sum(1 for c in children if common.is_model_light(c))))
            box.label(text="{} hardpoints, {} engine glows".format(
                sum(1 for c in children if common.role(c) == 'hardpoint'),
                sum(1 for c in children if common.role(c) == 'engineGlow')))
            layout.operator("aeron.view_pilot_eye", icon='HIDE_OFF')
        layout.operator("aeron.make_flight_model", icon='OUTLINER_OB_EMPTY')
        layout.label(text="Add to active component at cursor:")
        draw_add_buttons(layout)
        layout.prop(context.scene.aeron_flight, "game_preview")
        row = layout.row(align=True)
        row.operator("import_scene.aeron_flight_model", text="Import", icon='IMPORT')
        row.operator("import_scene.xwa_opt", text="Import OPT", icon='IMPORT')
        known = opt_import.projects(context)
        layout.operator("aeron.opt_project", text="OPT Project: " + (os.path.basename(known[0]) if known else "None"),
                        icon='FILE_FOLDER')
        row = layout.row(align=True)
        row.operator("export_scene.aeron_flight_model_project", text="Save to Project", icon='FILE_TICK')
        row.operator("export_scene.aeron_flight_model", text="Export for Game", icon='EXPORT')


class VIEW3D_PT_aeron_overlays(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Aeron"
    bl_label = "Overlays"
    bl_parent_id = "VIEW3D_PT_aeron"

    def draw_header(self, context):
        self.layout.prop(context.scene.aeron_flight, "overlays", text="")

    def draw(self, context):
        settings = context.scene.aeron_flight
        layout = self.layout
        layout.active = settings.overlays
        column = layout.column(align=True)
        column.prop(settings, "overlay_glows")
        row = column.row()
        row.active = settings.overlay_glows
        row.prop(settings, "overlay_throttle", slider=True)
        column.separator()
        column.prop(settings, "overlay_hardpoints")
        row = column.row()
        row.active = settings.overlay_hardpoints
        row.prop(settings, "overlay_hardpoint_labels", text="Labels")
        row.prop(settings, "overlay_projectiles")
        root = common.model_root(context.active_object)
        if settings.overlay_hardpoints and root is not None:
            counts = [0] * len(overlays.HARDPOINT_FAMILIES)
            for component in common.components(root):
                for child in component.children:
                    if common.role(child) == 'hardpoint':
                        counts[overlays.hardpoint_family(int(child.aeron_flight.hardpoint_type))] += 1
            legend = column.column(align=True)
            if not any(counts):
                legend.label(text="No hardpoints in " + root.name, icon='INFO')
            for index, family in enumerate(overlays.HARDPOINT_FAMILIES):
                if counts[index]:
                    legend.label(text="{}: {}".format(family[0], counts[index]), icon=overlays.family_icon(index))
        column.separator()
        for name in ("overlay_descriptors", "overlay_targets", "overlay_hinges", "overlay_lights",
                     "overlay_light_ranges", "overlay_component_labels", "overlay_in_front"):
            column.prop(settings, name)


class VIEW3D_PT_aeron_problems(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Aeron"
    bl_label = "Problems"
    bl_parent_id = "VIEW3D_PT_aeron"

    def draw(self, context):
        layout = self.layout
        layout.operator("aeron.validate", icon='CHECKMARK')
        scene_props = context.scene.aeron_flight
        if len(scene_props.problems):
            layout.template_list("AERON_UL_problems", "", scene_props, "problems", scene_props,
                                 "active_problem", rows=4)
        else:
            layout.label(text="Run Check Model to list problems")


classes = (
    AERON_UL_components,
    AERON_UL_children,
    AERON_UL_references,
    AERON_UL_named,
    AERON_UL_markings,
    AERON_UL_problems,
    OBJECT_PT_aeron,
    OBJECT_PT_aeron_components,
    OBJECT_PT_aeron_model_file,
    OBJECT_PT_aeron_material_animations,
    OBJECT_PT_aeron_markings,
    OBJECT_PT_aeron_articulations,
    OBJECT_PT_aeron_preview,
    OBJECT_PT_aeron_settings,
    OBJECT_PT_aeron_sets,
    OBJECT_PT_aeron_component,
    OBJECT_PT_aeron_children,
    OBJECT_PT_aeron_descriptor,
    OBJECT_PT_aeron_hinge,
    OBJECT_PT_aeron_component_articulations,
    VIEW3D_PT_aeron,
    VIEW3D_PT_aeron_overlays,
    VIEW3D_PT_aeron_problems,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
