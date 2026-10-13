# Material properties > Aeron Animation: the material animation editor
# (animation.py) and the project's effect libraries (libraries.py).

import bpy
from bpy.types import Panel, UIList

from . import animation, common, flipbook_preview, libraries

_STATE_NAMES = {identifier: name for identifier, name, _ in animation.STATES}


def _side_buttons(layout, *buttons):
    """A list's side column: (operator, icon, {property: value}) or None for a gap."""
    column = layout.column(align=True)
    for button in buttons:
        if button is None:
            column.separator()
            continue
        operator, icon, values = button
        properties = column.operator(operator, text="", icon=icon)
        for name, value in values.items():
            setattr(properties, name, value)


def _list_buttons(layout, prefix):
    _side_buttons(layout, ("aeron.{}_add".format(prefix), 'ADD', {}),
                  ("aeron.{}_remove".format(prefix), 'REMOVE', {}), None,
                  ("aeron.{}_move".format(prefix), 'TRIA_UP', {"direction": -1}),
                  ("aeron.{}_move".format(prefix), 'TRIA_DOWN', {"direction": 1}))


def _thumbnail(layout, context, library, key):
    icon_id = libraries.icon(context, library, key)
    if icon_id:
        layout.label(text="", icon_value=icon_id)
    else:
        layout.label(text="", icon='IMAGE_DATA')


class AERON_UL_flipbooks(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row()
        row.label(text=_STATE_NAMES.get(item.state, item.state),
                  icon='LIGHT' if item.layer == "emissive" else 'IMAGE_RGB')
        row.label(text="{} frames".format(len(item.frames)))


class AERON_UL_frames(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        _thumbnail(row, context, data.library, item.key)
        row.prop(item, "key", text="", emboss=False)
        row.prop(item, "seconds", text="")


class AERON_UL_damage_alternatives(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        stages = [stage or "-" for stage in (item.stage_75, item.stage_50, item.stage_25)]
        layout.label(text=" / ".join(stages), icon='MOD_EXPLODE')


class AERON_UL_libraries(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row()
        row.label(text=item.name, icon='ASSET_MANAGER')
        counts = row.row(align=True)
        counts.alignment = 'RIGHT'
        counts.label(text=str(item.images), icon='IMAGE_DATA')
        counts.label(text=str(item.models), icon='OBJECT_DATA')


class AERON_UL_library_images(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        _thumbnail(row, context, libraries.selected_library(context), item.name)
        row.label(text=item.name)
        kind = row.row()
        kind.alignment = 'RIGHT'
        kind.label(text=item.kind)


class _MaterialPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'material'

    @classmethod
    def poll(cls, context):
        return context.material is not None


class MATERIAL_PT_aeron_animation(_MaterialPanel, Panel):
    bl_label = "Aeron Animation"

    def draw(self, context):
        layout = self.layout
        props = context.material.aeron_flight
        layout.prop(props, "energy_override")
        root = common.model_root(context.object) if context.object else None
        # Not overridden: the model's default, greyed.
        source = props.energy if props.energy_override or root is None else root.aeron_flight.energy_default
        row = layout.row(align=True)
        row.enabled = props.energy_override
        for name in animation.ENERGIES:
            row.prop(source, name, toggle=True)
        layout.operator("aeron.clear_animation", icon='X')


class MATERIAL_PT_aeron_flipbooks(_MaterialPanel, Panel):
    bl_label = "Flipbooks"
    bl_parent_id = "MATERIAL_PT_aeron_animation"

    def draw(self, context):
        layout = self.layout
        props = context.material.aeron_flight
        row = layout.row()
        row.template_list("AERON_UL_flipbooks", "", props, "flipbooks", props, "active_flipbook", rows=3)
        _list_buttons(row, "flipbook")
        if not len(props.flipbooks):
            return
        book = props.flipbooks[props.active_flipbook]
        column = layout.column()
        column.use_property_split = True
        column.prop(book, "state")
        column.prop(book, "layer")
        column.prop(book, "mode")
        column.prop(book, "library")
        column.prop(book, "emissive_strength" if book.layer == "emissive" else "unlit")
        layout.label(text="Frames")
        row = layout.row()
        row.template_list("AERON_UL_frames", "", book, "frames", book, "active_frame", rows=4)
        _list_buttons(row, "frame")
        row = layout.row(align=True)
        row.operator("aeron.frames_add_prefix", text="By Prefix…")
        row.operator("aeron.frames_add_added", text="From Added…")
        row.operator("aeron.frames_set_duration", text="Set Duration…")


class MATERIAL_PT_aeron_preview(_MaterialPanel, Panel):
    bl_label = "Preview"
    bl_parent_id = "MATERIAL_PT_aeron_animation"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, context):
        self.layout.prop(context.window_manager, "aeron_flipbook_preview", text="")

    def draw(self, context):
        layout = self.layout
        playing = context.screen is not None and context.screen.is_animation_playing
        layout.operator("screen.animation_play", text="Pause" if playing else "Play Timeline",
                        icon='PAUSE' if playing else 'PLAY')
        if context.window_manager.aeron_flipbook_preview:
            if flipbook_preview.message:
                layout.label(text=flipbook_preview.message, icon='ERROR')
            reason = flipbook_preview.skipped.get(context.material.name)
            if reason:
                layout.label(text="Not shown: " + reason, icon='INFO')
        layout.label(text="Active States")
        states = context.scene.aeron_flight.flipbook_states
        flow = layout.grid_flow(columns=2, even_columns=True, align=True)
        for state, _, _ in animation.STATES[1:]:
            flow.prop(states, flipbook_preview.state_property(state))


class MATERIAL_PT_aeron_damage(_MaterialPanel, Panel):
    bl_label = "Hull Damage"
    bl_parent_id = "MATERIAL_PT_aeron_animation"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        props = context.material.aeron_flight
        if not props.has_damage:
            layout.operator("aeron.damage_add", icon='ADD')
            return
        damage = props.damage
        column = layout.column()
        column.use_property_split = True
        column.prop(damage, "library")
        column.prop(damage, "choice")
        column.prop(damage, "placement")
        layout.label(text="Alternatives")
        row = layout.row()
        row.template_list("AERON_UL_damage_alternatives", "", damage, "alternatives", damage, "active_alternative",
                          rows=2)
        _side_buttons(row, ("aeron.damage_alternative_add", 'ADD', {}),
                      ("aeron.damage_alternative_remove", 'REMOVE', {}))
        if 0 <= damage.active_alternative < len(damage.alternatives):
            alternative = damage.alternatives[damage.active_alternative]
            column = layout.column()
            column.use_property_split = True
            for stage in ("stage_75", "stage_50", "stage_25"):
                column.prop(alternative, stage)
            column.prop(alternative, "has_scale")
            if alternative.has_scale:
                column.prop(alternative, "scale")
            column.prop(alternative, "offset_mode")
            if alternative.offset_mode == 'RANGE':
                column.prop(alternative, "offset_min")
                column.prop(alternative, "offset_max")
        layout.operator("aeron.damage_remove", icon='X')


class MATERIAL_PT_aeron_libraries(_MaterialPanel, Panel):
    bl_label = "Effect Libraries"
    bl_parent_id = "MATERIAL_PT_aeron_animation"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return super().poll(context) and bool(libraries.effects_dir(context))

    def draw(self, context):
        layout = self.layout
        if libraries.project(context) is None:
            layout.operator("aeron.libraries_refresh", text="Load Libraries", icon='FILE_REFRESH')
            return
        current = libraries.state(context)
        row = layout.row()
        row.template_list("AERON_UL_libraries", "", current, "libraries", current, "active_library", rows=4)
        _side_buttons(row, ("aeron.library_add_images", 'NEWFOLDER', {"create": True}),
                      ("aeron.library_add_images", 'FILE_IMAGE', {"create": False}),
                      ("aeron.library_add_blender_image", 'IMAGE_DATA', {}), None,
                      ("aeron.libraries_refresh", 'FILE_REFRESH', {}))
        row = layout.row()
        row.template_list("AERON_UL_library_images", "", current, "images", current, "active_image", rows=5)
        _side_buttons(row, ("aeron.library_edit_image", 'GREASEPENCIL', {}),
                      ("aeron.library_rename_image", 'SORTALPHA', {}),
                      ("aeron.library_remove_image", 'REMOVE', {}))


classes = (
    AERON_UL_flipbooks,
    AERON_UL_frames,
    AERON_UL_damage_alternatives,
    AERON_UL_libraries,
    AERON_UL_library_images,
    MATERIAL_PT_aeron_animation,
    MATERIAL_PT_aeron_flipbooks,
    MATERIAL_PT_aeron_preview,
    MATERIAL_PT_aeron_damage,
    MATERIAL_PT_aeron_libraries,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
