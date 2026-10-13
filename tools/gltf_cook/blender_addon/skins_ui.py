# The Skins panel of a material (skins.py). The variants that paint the
# skins are in the Variants panel (variants_ui.py).

import bpy
from bpy.props import EnumProperty
from bpy.types import Operator, Panel, UIList

from . import common, display, skins, variants


def _refresh(root):
    """Shows again the markings value on display, so that the preview follows an edit."""
    if root is not None and display.previewing(root):
        display.apply_skin(root)


def _model_of_material(context):
    obj = context.object
    return common.model_root(obj) if obj is not None else None


class AERON_UL_skins(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        row.prop(item, "name", text="", emboss=False, icon='IMAGE_DATA' if item.base_color is not None else 'ERROR')
        root = _model_of_material(context)
        if root is not None:
            props = root.aeron_flight
            shown = props.preview_markings < 0 and not props.preview_variant and props.preview_skin == item.name
            row.operator("aeron.show_skin", text="", icon='HIDE_OFF' if shown else 'HIDE_ON', emboss=False).skin = \
                item.name


def edit_list(owner, items, active, action, add):
    """Adds (with `add(item)`), removes or moves the active item of a list."""
    index = getattr(owner, active)
    if action == 'ADD':
        add(items.add())
        setattr(owner, active, len(items) - 1)
    elif not 0 <= index < len(items):
        return False
    elif action == 'REMOVE':
        items.remove(index)
        setattr(owner, active, min(index, len(items) - 1))
    else:
        other = index - 1 if action == 'UP' else index + 1
        if not 0 <= other < len(items):
            return False
        items.move(index, other)
        setattr(owner, active, other)
    return True


def list_buttons(layout, operator, move=False, **properties):
    column = layout.column(align=True)
    actions = (('ADD', 'ADD'), ('REMOVE', 'REMOVE')) + ((('UP', 'TRIA_UP'), ('DOWN', 'TRIA_DOWN')) if move else ())
    for action, icon in actions:
        button = column.operator(operator, text="", icon=icon)
        button.action = action
        for name, value in properties.items():
            setattr(button, name, value)


class AERON_OT_skin_edit(Operator):
    """Add or remove skins of the material"""

    bl_idname = "aeron.skin_edit"
    bl_label = "Edit Skins"
    bl_options = {'REGISTER', 'UNDO'}

    action: EnumProperty(items=[('ADD', "Add", ""), ('REMOVE', "Remove", ""), ('UP', "Up", ""), ('DOWN', "Down", "")])

    def execute(self, context):
        material = skins.own_material(context.material)
        if material is None:
            return {'CANCELLED'}
        props = material.aeron_flight
        if not edit_list(props, props.skins, "active_skin", self.action, lambda item: setattr(item, "name", "skin")):
            return {'CANCELLED'}
        _refresh(_model_of_material(context))
        return {'FINISHED'}


class AERON_OT_skin_new_image(Operator):
    """Create an image for the skin that changes nothing until you paint it, in the model's textures/skins/ folder"""

    bl_idname = "aeron.skin_new_image"
    bl_label = "New Skin Image"
    bl_options = {'REGISTER', 'UNDO'}

    channel: EnumProperty(items=[(channel, channel, "") for channel in skins.CHANNELS])

    def execute(self, context):
        material = skins.own_material(context.material)
        if material is None or not 0 <= material.aeron_flight.active_skin < len(material.aeron_flight.skins):
            return {'CANCELLED'}
        skin = material.aeron_flight.skins[material.aeron_flight.active_skin]
        image = skins.new_image(_model_of_material(context), material, skin, self.channel)
        setattr(skin, self.channel, image)
        if image.filepath:
            self.report({'INFO'}, "Created " + bpy.path.abspath(image.filepath))
        return {'FINISHED'}


def _painting(context, image):
    paint = context.scene.tool_settings.image_paint
    return (image is not None and context.object is not None and context.object.mode == 'TEXTURE_PAINT' and
            paint.mode == 'IMAGE' and paint.canvas == image)


def _uv_map(material):
    """The UV map that the material's base color texture uses, or ""."""
    from .flipbook_preview import _bsdf, _texture
    bsdf = _bsdf(material.node_tree) if material.node_tree is not None else None
    socket = bsdf.inputs["Base Color"] if bsdf is not None else None
    node = _texture(socket) if socket is not None and socket.is_linked else None
    vector = node.inputs["Vector"] if node is not None else None
    while vector is not None and vector.is_linked:
        source = vector.links[0].from_node
        if source.type == 'UVMAP':
            return source.uv_map
        vector = source.inputs.get("Vector")  # through a Mapping node
    return ""


def _view3d(context):
    return next((area for area in context.screen.areas if area.type == 'VIEW_3D'), None) if context.screen else None


def _local_view(context, view, on):
    """Turns the local view of a 3D view on (framing the selected objects) or off."""
    if view is None or bool(view.spaces[0].local_view) == on:
        return
    region = next(region for region in view.regions if region.type == 'WINDOW')
    with context.temp_override(area=view, region=region):
        bpy.ops.view3d.localview(frame_selected=True)


class AERON_OT_skin_paint(Operator):
    """Paint this image on the model: show the skin alone, isolate the parts that have the material and switch to \
Texture Paint. Click again to stop"""

    bl_idname = "aeron.skin_paint"
    bl_label = "Paint Skin Image"
    bl_options = {'REGISTER', 'UNDO'}

    channel: EnumProperty(items=[(channel, channel, "") for channel in skins.CHANNELS])

    @classmethod
    def poll(cls, context):
        return context.material is not None and _model_of_material(context) is not None

    def execute(self, context):
        material = skins.own_material(context.material)
        props = material.aeron_flight
        if not 0 <= props.active_skin < len(props.skins):
            return {'CANCELLED'}
        skin = props.skins[props.active_skin]
        image = getattr(skin, self.channel)
        root = _model_of_material(context)
        view = _view3d(context)
        if _painting(context, image):  # stop
            bpy.ops.object.mode_set(mode='OBJECT')
            _local_view(context, view, False)
            root.aeron_flight.preview_skin = ""
            display.apply_skin(root)
            return {'FINISHED'}
        if image is None:
            self.report({'ERROR'}, "The skin has no image here yet. Create one with +")
            return {'CANCELLED'}
        parts = [(component, index) for component in common.components(root)
                 for index in range(len(component.material_slots))
                 if variants.modeled_material(component, index) == material]
        if not parts:
            self.report({'ERROR'}, "No part of the model uses " + material.name)
            return {'CANCELLED'}
        target, slot = next(((c, i) for c, i in parts if c == context.object), parts[0])
        if context.object is not None and context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        _local_view(context, view, False)

        root.aeron_flight.preview_markings = -1
        root.aeron_flight.preview_variant = ""
        root.aeron_flight.preview_skin = skin.name
        display.apply_skin(root)

        for obj in context.view_layer.objects:
            obj.select_set(False)
        for component, _ in parts:
            component.select_set(True)
        context.view_layer.objects.active = target
        target.active_material_index = slot
        uv_map = _uv_map(material)
        if uv_map in target.data.uv_layers:
            target.data.uv_layers.active = target.data.uv_layers[uv_map]
        if view is not None and view.spaces[0].shading.type in {'SOLID', 'WIREFRAME'}:
            view.spaces[0].shading.type = 'MATERIAL'
        _local_view(context, view, True)
        for area in (context.screen.areas if context.screen else ()):
            if area.type == 'IMAGE_EDITOR':
                area.spaces[0].image = image
        with context.temp_override(object=target, active_object=target):
            bpy.ops.object.mode_set(mode='TEXTURE_PAINT')
        paint = context.scene.tool_settings.image_paint
        paint.mode = 'IMAGE'
        paint.canvas = image
        self.report({'INFO'}, "Painting {} on {}. Save it with Image > Save".format(image.name, target.name))
        return {'FINISHED'}


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
        list_buttons(row, "aeron.skin_edit")
        if not 0 <= props.active_skin < len(props.skins):
            layout.label(text="Variants paint skins over the material")
            return
        skin = props.skins[props.active_skin]
        for name in skins.CHANNELS:
            layout.label(text=skin.bl_rna.properties[name].name + ":")
            row = layout.row(align=True)
            row.template_ID(skin, name, open="image.open")
            row.operator("aeron.skin_new_image", text="", icon='ADD').channel = name
            image = getattr(skin, name)
            paint = row.row(align=True)
            paint.enabled = image is not None
            paint.operator("aeron.skin_paint", text="", icon='BRUSH_DATA',
                           depress=_painting(context, image)).channel = name
        row = layout.row(align=True)
        row.enabled = skin.emissive is not None
        row.prop(skin, "emissive_strength_given")
        sub = row.row(align=True)
        sub.enabled = skin.emissive_strength_given
        sub.prop(skin, "emissive_strength")


classes = (AERON_UL_skins, AERON_OT_skin_edit, AERON_OT_skin_new_image, AERON_OT_skin_paint,
           MATERIAL_PT_aeron_skins)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
