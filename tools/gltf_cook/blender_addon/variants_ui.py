# The Variants panel of a model root (variants.py). It lists the variants,
# with the markings values that show each one. Below the list are what the
# selected one changes: its base, replaced materials and layers.

import bpy
from bpy.props import EnumProperty, StringProperty
from bpy.types import Operator, Panel, UIList

from . import common, display, skins, skins_ui, ui, variants

_SPLIT = 0.72


def _refresh(root):
    if display.previewing(root):
        display.apply_skin(root)


class AERON_UL_variants(UIList):
    """The model's variants: name, markings values and an eye to show it."""

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        root = data.id_data
        split = layout.split(factor=_SPLIT)
        split.prop(item, "name", text="", emboss=False)
        row = split.row(align=True)
        row.label(text=variants.markings_of(root, item.name) or "-")
        shown = data.preview_markings < 0 and data.preview_variant == item.name
        row.operator("aeron.show_skin", text="", icon='HIDE_OFF' if shown else 'HIDE_ON',
                     emboss=False).variant = item.name


class AERON_UL_skin_layers(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        row.prop(item, "skin", text="")
        row.prop(item, "opacity", text="")


def _active(root):
    props = root.aeron_flight
    return props.variants[props.active_variant] if 0 <= props.active_variant < len(props.variants) else None


class AERON_OT_variant_edit(Operator):
    """Add, remove or move variants and their layers"""

    bl_idname = "aeron.variant_edit"
    bl_label = "Edit Variants"
    bl_options = {'REGISTER', 'UNDO'}

    target: EnumProperty(items=[('VARIANTS', "Variant", ""), ('LAYERS', "Layer", "")])
    action: EnumProperty(items=[('ADD', "Add", ""), ('REMOVE', "Remove", ""), ('UP', "Up", ""), ('DOWN', "Down", "")])

    @classmethod
    def poll(cls, context):
        return common.role(context.object) == 'model'

    def execute(self, context):
        root = context.object
        props = root.aeron_flight
        variants.sync(root)
        if self.target == 'VARIANTS':
            if self.action == 'REMOVE':
                if not 0 <= props.active_variant < len(props.variants):
                    return {'CANCELLED'}
                variants.remove(root, props.active_variant)
                props.active_variant = min(props.active_variant, len(props.variants) - 1)
            elif not skins_ui.edit_list(props, props.variants, "active_variant", self.action,
                                        lambda item: setattr(item, "name", "Variant")):
                return {'CANCELLED'}
        else:
            entry = _active(root)
            if entry is None or not skins_ui.edit_list(entry, entry.layers, "active_layer", self.action,
                                                       lambda item: None):
                return {'CANCELLED'}
        _refresh(root)
        return {'FINISHED'}


def _search_modeled(self, context, edit_text):
    root = context.object
    names = [material.name for material in skins.model_materials(root)] if common.role(root) == 'model' else []
    return [name for name in names if edit_text.lower() in name.lower()]


def _search_materials(self, context, edit_text):
    return [material.name for material in bpy.data.materials
            if not material.get(skins.PREVIEW_TAG) and edit_text.lower() in material.name.lower()]


class AERON_OT_variant_replace(Operator):
    """Show another material instead of one of the model's materials in this variant"""

    bl_idname = "aeron.variant_replace"
    bl_label = "Replace Material"
    bl_options = {'REGISTER', 'UNDO'}

    material: StringProperty(name="Material", search=_search_modeled, options={'SKIP_SAVE'},
                             description="A material of the model")
    replacement: StringProperty(name="With", search=_search_materials, options={'SKIP_SAVE'},
                                description="The material the variant shows instead, wherever the model has the "
                                            "first one")

    @classmethod
    def poll(cls, context):
        return common.role(context.object) == 'model' and _active(context.object) is not None

    def invoke(self, context, event):
        if not self.material:
            for obj in context.selected_objects:
                if common.model_root(obj) == context.object and obj.active_material is not None:
                    self.material = skins.own_material(obj.active_material).name
                    break
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        root = context.object
        modeled = bpy.data.materials.get(self.material)
        replacement = bpy.data.materials.get(self.replacement)
        if modeled is None or replacement is None:
            self.report({'ERROR'}, "Choose a material of the model and the one to show instead")
            return {'CANCELLED'}
        variants.replace(root, _active(root).name, modeled, replacement)
        _refresh(root)
        return {'FINISHED'}


class AERON_OT_variant_unreplace(Operator):
    """Show the modeled material again in this variant"""

    bl_idname = "aeron.variant_unreplace"
    bl_label = "Remove Replacement"
    bl_options = {'REGISTER', 'UNDO'}

    material: StringProperty()

    @classmethod
    def poll(cls, context):
        return common.role(context.object) == 'model' and _active(context.object) is not None

    def execute(self, context):
        root = context.object
        modeled = bpy.data.materials.get(self.material)
        if modeled is None:
            return {'CANCELLED'}
        variants.replace(root, _active(root).name, modeled, modeled)
        _refresh(root)
        return {'FINISHED'}


class OBJECT_PT_aeron_variants(ui._RolePanel, Panel):
    bl_label = "Variants"
    role = 'model'

    def draw(self, context):
        root = context.object
        props = root.aeron_flight
        layout = self.layout
        header = layout.row()
        split = header.split(factor=_SPLIT)
        split.label(text="Name")
        split.label(text="Markings")
        header.label(text="", icon='BLANK1')
        row = layout.row()
        row.template_list("AERON_UL_variants", "", props, "variants", props, "active_variant", rows=4)
        skins_ui.list_buttons(row, "aeron.variant_edit", move=True, target='VARIANTS')
        layout.operator("aeron.show_skin", text="Show As Modeled", depress=not display.previewing(root)).markings = -1
        entry = _active(root)
        if entry is None:
            layout.label(text="A variant replaces materials and paints skins")
            return
        layout.prop(entry, "name")
        layout.prop(entry, "base")
        row = layout.row()
        row.label(text="Replaced materials:")
        row.operator("aeron.variant_replace", text="", icon='ADD')
        column = layout.column(align=True)
        for modeled, replacement in variants.replacements(root, entry.name):
            line = column.row(align=True)
            line.label(text="{} → {}".format(modeled.name if modeled else "?", replacement.name))
            if modeled is not None:
                line.operator("aeron.variant_unreplace", text="", icon='X', emboss=False).material = modeled.name
        layout.label(text="Layers, painted in order:")
        row = layout.row()
        row.template_list("AERON_UL_skin_layers", "", entry, "layers", entry, "active_layer", rows=3)
        skins_ui.list_buttons(row, "aeron.variant_edit", move=True, target='LAYERS')


classes = (AERON_UL_variants, AERON_UL_skin_layers, AERON_OT_variant_edit, AERON_OT_variant_replace,
           AERON_OT_variant_unreplace, OBJECT_PT_aeron_variants)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
