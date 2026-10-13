# Panels and operators of the skin layers (skins.py): the Skins panel of a
# material, and the Skin Variants panel of a model root.

import bpy
from bpy.props import EnumProperty
from bpy.types import Operator, Panel, UIList

from . import common, display, skins, ui


def _refresh(root):
    """Shows again the markings value on display, so that the preview follows an edit."""
    if root is not None and display.previewing(root):
        display.apply_skin(root)


def _model_of_material(context):
    obj = context.object
    return common.model_root(obj) if obj is not None else None


class AERON_UL_skins(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        layout.prop(item, "name", text="", emboss=False,
                    icon='IMAGE_DATA' if item.base_color is not None else 'ERROR')


class AERON_UL_skin_variants(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        row.prop(item, "name", text="", emboss=False, icon='MATERIAL')
        row.label(text="{} layer(s)".format(len(item.layers)))
        shown = data.preview_markings < 0 and data.preview_variant == item.name
        row.operator("aeron.show_skin", text="", icon='HIDE_OFF' if shown else 'HIDE_ON',
                     emboss=False).variant = item.name


class AERON_UL_skin_layers(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        row.prop(item, "skin", text="")
        row.prop(item, "opacity", text="")


class AERON_OT_skin_edit(Operator):
    """Add, remove or move skins, skin variants and their layers"""

    bl_idname = "aeron.skin_edit"
    bl_label = "Edit Skins"
    bl_options = {'REGISTER', 'UNDO'}

    target: EnumProperty(items=[('SKINS', "Skin", ""), ('VARIANTS', "Skin Variant", ""), ('LAYERS', "Layer", "")])
    action: EnumProperty(items=[('ADD', "Add", ""), ('REMOVE', "Remove", ""), ('UP', "Up", ""), ('DOWN', "Down", "")])

    def execute(self, context):
        if self.target == 'SKINS':
            material = skins.own_material(context.material)
            if material is None:
                return {'CANCELLED'}
            owner, items, active = material.aeron_flight, material.aeron_flight.skins, "active_skin"
            root = _model_of_material(context)
        else:
            root = common.model_root(context.object)
            if root is None:
                return {'CANCELLED'}
            props = root.aeron_flight
            if self.target == 'VARIANTS':
                owner, items, active = props, props.skin_variants, "active_skin_variant"
            else:
                if not 0 <= props.active_skin_variant < len(props.skin_variants):
                    return {'CANCELLED'}
                owner = props.skin_variants[props.active_skin_variant]
                items, active = owner.layers, "active_layer"
        index = getattr(owner, active)
        if self.action == 'ADD':
            item = items.add()
            if self.target == 'SKINS':
                item.name = "skin"
            elif self.target == 'VARIANTS':
                item.name = "Skin Variant"
            setattr(owner, active, len(items) - 1)
        elif not 0 <= index < len(items):
            return {'CANCELLED'}
        elif self.action == 'REMOVE':
            items.remove(index)
            setattr(owner, active, min(index, len(items) - 1))
        else:
            other = index - 1 if self.action == 'UP' else index + 1
            if not 0 <= other < len(items):
                return {'CANCELLED'}
            items.move(index, other)
            setattr(owner, active, other)
        _refresh(root)
        return {'FINISHED'}


def _buttons(layout, target, move=False):
    column = layout.column(align=True)
    actions = (('ADD', 'ADD'), ('REMOVE', 'REMOVE')) + ((('UP', 'TRIA_UP'), ('DOWN', 'TRIA_DOWN')) if move else ())
    for action, icon in actions:
        operator = column.operator("aeron.skin_edit", text="", icon=icon)
        operator.target, operator.action = target, action


class MATERIAL_PT_aeron_skins(Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'material'
    bl_label = "Aeron Skins"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return context.material is not None

    def draw(self, context):
        layout = self.layout
        material = skins.own_material(context.material)
        if material != context.material:
            layout.label(text="Showing a skin preview of " + material.name, icon='INFO')
        props = material.aeron_flight
        row = layout.row()
        row.template_list("AERON_UL_skins", "", props, "skins", props, "active_skin", rows=3)
        _buttons(row, 'SKINS')
        if not 0 <= props.active_skin < len(props.skins):
            layout.label(text="A skin is painted over the material by skin variants")
            return
        skin = props.skins[props.active_skin]
        for name in skins.CHANNELS:
            layout.label(text=skin.bl_rna.properties[name].name + ":")
            layout.template_ID(skin, name, new="image.new", open="image.open")
        row = layout.row(align=True)
        row.enabled = skin.emissive is not None
        row.prop(skin, "emissive_strength_given")
        sub = row.row(align=True)
        sub.enabled = skin.emissive_strength_given
        sub.prop(skin, "emissive_strength")


class OBJECT_PT_aeron_skin_variants(ui._RolePanel, Panel):
    bl_label = "Skin Variants"
    bl_options = {'DEFAULT_CLOSED'}
    role = 'model'

    def draw(self, context):
        props = context.object.aeron_flight
        layout = self.layout
        row = layout.row()
        row.template_list("AERON_UL_skin_variants", "", props, "skin_variants", props, "active_skin_variant",
                          rows=3)
        _buttons(row, 'VARIANTS', move=True)
        layout.operator("aeron.show_skin", text="Show As Modeled", depress=not display.previewing(context.object))
        if not 0 <= props.active_skin_variant < len(props.skin_variants):
            layout.label(text="A skin variant paints skins over the materials")
            return
        variant = props.skin_variants[props.active_skin_variant]
        layout.prop(variant, "base")
        layout.label(text="Layers, painted in order:")
        row = layout.row()
        row.template_list("AERON_UL_skin_layers", "", variant, "layers", variant, "active_layer", rows=3)
        _buttons(row, 'LAYERS', move=True)


classes = (AERON_UL_skins, AERON_UL_skin_variants, AERON_UL_skin_layers, AERON_OT_skin_edit,
           MATERIAL_PT_aeron_skins, OBJECT_PT_aeron_skin_variants)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
