# Imports a flight model. Blender's glTF importer reads the file, and the hooks
# below restore the AERON_flight_model data on the objects it creates. The
# hooks act only during this add-on's imports. They restore the roles and
# fields, the ordinals as list order, the articulations, articulation
# settings, component sets, markings and energy default of the model root,
# the model lights as Blender point and area lights, and the animation and
# screen of each material. The asset extras and the names of the file's
# extensions and extras are kept too (sources.py).

import json
import os

import bpy
from bpy.props import StringProperty
from bpy.types import Operator
from bpy_extras.io_utils import ImportHelper
from mathutils import Vector

from . import animation, common, operators, probes, skins, sources, variants

# Results of the running import, or None outside this add-on's imports.
_state = None


def _extension(node):
    return (node.extensions or {}).get(common.EXTENSION_NAME) or {}


def _set_reference(ref, ordinal, parts):
    if 0 <= ordinal < len(parts):
        ref.component = parts[ordinal]
    else:
        ref.component_index = ordinal


def _restore_component(obj, extension):
    props = obj.aeron_flight
    props.mesh_type = str(extension.get("meshType", 0))
    flags = extension.get("explosionFlags", 0)
    props.explosion_flags_bits = flags - (1 << 32) if flags >= (1 << 31) else flags
    props.tags = ", ".join(extension.get("tags", []))
    props.target_id = extension.get("targetId", 0)
    if "target" in extension:
        props.target = common.from_gltf(extension["target"])
    if "span" in extension:
        props.has_descriptor_geometry = True
        props.descriptor_span = common.extent_from_gltf(extension["span"])
        props.descriptor_center = common.from_gltf(extension["center"])
        props.descriptor_bounds_min, props.descriptor_bounds_max = common.bounds_from_gltf(
            extension["boundsMin"], extension["boundsMax"])
    rotation = extension.get("rotation")
    if rotation:
        props.has_rotation = True
        props.pivot = common.from_gltf(rotation["pivot"])
        props.rotation_axis = common.from_gltf(rotation["rotationAxis"])
        props.direction_axis = common.from_gltf(rotation["directionAxis"])
        props.up_axis = common.from_gltf(rotation["upAxis"])


def _restore_light(obj, extension, punctual):
    shape = extension.get("shape", "point")
    one_sided = shape == "panel" or extension.get("oneSided", False)
    obj.data.type = 'POINT' if shape == "point" else 'AREA'
    light = obj.data
    if shape == "point":
        light.shadow_soft_size = extension.get("radius", common.DEFAULT_LIGHT_RADIUS)
        light.use_soft_falloff = False
    else:
        light.shape = 'RECTANGLE'
        if shape == "tube":
            obj.aeron_flight.light_tube = True
            obj.aeron_flight.light_one_sided = one_sided
            light.size = extension["length"]
            light.size_y = 2.0 * extension.get("radius", common.DEFAULT_LIGHT_RADIUS)
        else:
            light.size, light.size_y = extension["size"]
    light.color = punctual.get("color", (1.0, 1.0, 1.0))
    light.energy = common.watts(punctual.get("intensity", 1.0), one_sided)
    light.exposure = 0.0
    light.normalize = True
    light.use_temperature = False
    light.use_custom_distance = "range" in punctual
    if "range" in punctual:
        light.cutoff_distance = punctual["range"]


def _set_role(obj, role, order=0):
    obj.aeron_flight.role = role
    obj.aeron_flight.order = order


def restore(gltf):
    nodes = gltf.data.nodes or []
    lights = ((gltf.data.extensions or {}).get(common.LIGHTS_EXTENSION_NAME) or {}).get("lights", [])

    def blender_object(index):
        vnode = gltf.vnodes.get(index)
        return vnode.blender_object if vnode is not None else None

    def role(index):
        return _extension(nodes[index]).get("role")

    for root_index, root_node in enumerate(nodes):
        if role(root_index) != "model" or blender_object(root_index) is None:
            continue
        root = blender_object(root_index)
        _set_role(root, 'model')
        _state["roots"].append(root)
        parts = []
        for component_index in root_node.children or []:
            component = blender_object(component_index)
            if role(component_index) != "component" or component is None:
                continue
            _set_role(component, 'component', len(parts))
            _restore_component(component, _extension(nodes[component_index]))
            parts.append(component)
            ordinal = 0
            for child_index in nodes[component_index].children or []:
                child = blender_object(child_index)
                child_role = role(child_index)
                extension = _extension(nodes[child_index])
                if child is None:
                    continue
                if child_role == "hardpoint":
                    _set_role(child, 'hardpoint', ordinal)
                    child.aeron_flight.hardpoint_type = str(extension.get("type", 0))
                    ordinal += 1
                elif child_role == "engineGlow":
                    _set_role(child, 'engineGlow', ordinal)
                    child.aeron_flight.glow_enabled = extension.get("enabled", True)
                    child.aeron_flight.core_color = extension.get("coreColor", (1.0, 1.0, 1.0, 1.0))
                    child.aeron_flight.outer_color = extension.get("outerColor", (1.0, 1.0, 1.0, 0.0))
                    child.empty_display_type = 'ARROWS'
                    ordinal += 1
                elif child_role == "collision":
                    _set_role(child, 'collision')
                    child.display_type = 'WIRE'
                    child.hide_render = True
                elif child_role == "light" and child.type == 'LIGHT':
                    light_index = (nodes[child_index].extensions or {}).get(
                        common.LIGHTS_EXTENSION_NAME, {}).get("light")
                    if light_index is not None and light_index < len(lights):
                        _restore_light(child, extension, lights[light_index])
                        _state["lights"] += 1
        _state["components"] += len(parts)

        props = root.aeron_flight
        extension = _extension(root_node)
        for item in extension.get("articulations", []):
            articulation = props.articulations.add()
            _set_reference(articulation, item["component"], parts)
            articulation.group = item["group"]
            articulation.angle = item["angle"]
            articulation.speed = item["speed"]
            articulation.has_speed_back = "speedBack" in item
            articulation.speed_back = item.get("speedBack", item["speed"])
            articulation.behavior = item.get("behavior", "")
        for name, value in extension.get("articulationSettings", {}).items():
            setting = props.articulation_settings.add()
            setting.name = name
            setting.value = value
        for name, members in extension.get("componentSets", {}).items():
            component_set = props.component_sets.add()
            component_set.name = name
            for ordinal in members:
                _set_reference(component_set.members.add(), ordinal, parts)
        variants.restore(root, extension)
        for name in extension.get("markings", []):
            props.markings.add().variant = name or ""
        animation.set_energy_default(root, extension.get("energy"))
        probes.restore(root, extension)


class glTF2ImportUserExtension:
    """Hooks for Blender's glTF importer, which finds them on the add-on's module."""

    def gather_import_material_after_hook(self, gltf_material, vertex_color, blender_material, gltf):
        if _state is None:
            return
        extension = (gltf_material.extensions or {}).get(common.EXTENSION_NAME) or {}
        try:
            animation.set_entry(blender_material, extension)
            screen = extension.get("screen")
            blender_material.aeron_flight.screen = json.dumps(screen) if screen is not None else ""
            skins.restore_material(blender_material, extension, gltf)
        except Exception as error:  # the importer would only log it
            _state["error"] = "material {}: {}: {}".format(blender_material.name, type(error).__name__, error)
            return
        if animation.has_animation(blender_material):
            _state["animated"].add(gltf_material.name)

    def gather_import_scene_after_nodes_hook(self, gltf_scene, blender_scene, gltf):
        if _state is None:
            return
        try:
            restore(gltf)
        except Exception as error:  # the importer would only log it
            _state["error"] = "{}: {}".format(type(error).__name__, error)


def _preview_transmission(roots):
    """Turns on raytraced transmission for transmissive (glass) materials.
    EEVEE needs it to show what is behind them. The setting is not
    exported."""
    for root in roots:
        for obj in root.children_recursive:
            for slot in obj.material_slots:
                material = slot.material
                if material is None or material.node_tree is None:
                    continue
                for node in material.node_tree.nodes:
                    if node.type == 'BSDF_PRINCIPLED':
                        weight = node.inputs["Transmission Weight"]
                        if weight.is_linked or weight.default_value > 0.0:
                            material.use_raytrace_refraction = True


def _frame(context, root):
    """Shows the whole model in every 3D view of the window, as View Selected
    does: the glTF import selects what it imported. A large model gets a clip
    end far enough to show it."""
    window = context.window
    if window is None:
        return
    corners = [obj.matrix_world @ Vector(corner) for obj in [root] + root.children_recursive
               if obj.type == 'MESH' for corner in obj.bound_box]
    size = (Vector(map(max, *corners)) - Vector(map(min, *corners))).length if corners else 0.0
    for area in window.screen.areas:
        if area.type != 'VIEW_3D':
            continue
        space = area.spaces.active
        if space.clip_end < 10.0 * size:
            space.clip_end = 10.0 * size
        region = next((region for region in area.regions if region.type == 'WINDOW'), None)
        with context.temp_override(window=window, area=area, region=region):
            bpy.ops.view3d.view_selected()


def _variant_list():
    return getattr(bpy.data.scenes[0], "gltf2_KHR_materials_variants_variants", None)


def _merge_variants(before, roots):
    """Blender's glTF importer replaces the scene's variant list with the
    file's. Meshes refer to variants by position in that list, so the meshes
    imported before would show the wrong variants. This puts back the list as
    it was, appends the file's new names, and moves the new meshes to the
    merged positions. A name in both is one variant."""
    variants = _variant_list()
    if variants is None or not before:
        return
    imported = [variant.name for variant in variants]
    if imported == before[:len(imported)] and len(imported) <= len(before):
        merged = list(before)
    else:
        merged = list(before) + [name for name in imported if name not in before]
    position = {name: index for index, name in enumerate(merged)}
    for root in roots:
        for obj in [root] + list(root.children_recursive):
            if obj.type != 'MESH':
                continue
            for item in getattr(obj.data, "gltf2_variant_mesh_data", ()):
                for pointer in item.variants:
                    index = pointer.variant.variant_idx
                    if 0 <= index < len(imported):
                        pointer.variant.variant_idx = position[imported[index]]
    variants.clear()
    for index, name in enumerate(merged):
        variant = variants.add()
        variant.variant_idx = index  # set before the name: its setter uses the index
        variant.name = name


class IMPORT_OT_aeron_flight_model(Operator, ImportHelper):
    """Import a flight model (.gltf or .glb) with its Aeron flight data"""

    bl_idname = "import_scene.aeron_flight_model"
    bl_label = "Import Aeron Flight Model"
    bl_options = {'REGISTER', 'UNDO'}

    filter_glob: StringProperty(default="*.gltf;*.glb", options={'HIDDEN'})

    def execute(self, context):
        global _state
        try:
            document = sources.read_document(self.filepath)
        except (OSError, ValueError) as error:
            self.report({'ERROR'}, "Cannot read {}: {}".format(self.filepath, error))
            return {'CANCELLED'}
        if common.COOKED_EXTENSION_NAME in (document.get("extensionsRequired") or []):
            self.report({'ERROR'}, "This .glb is cooked for the game: Blender cannot read its textures, and "
                                   "the cooker dropped its source data. Import the model's .gltf instead")
            return {'CANCELLED'}
        _state = {"roots": [], "components": 0, "lights": 0, "error": None, "animated": set()}
        variants_before = [variant.name for variant in _variant_list() or ()]
        try:
            result = bpy.ops.import_scene.gltf(
                filepath=self.filepath, merge_vertices=False, import_shading='NORMALS',
                export_import_convert_lighting_mode='SPEC')
            state = _state
        finally:
            _state = None
        if 'FINISHED' not in result:
            self.report({'ERROR'}, "Blender's glTF import failed")
            return {'CANCELLED'}
        _merge_variants(variants_before, state["roots"])
        for root in state["roots"]:
            variants.sync(root, first=True)
        if state["error"]:
            self.report({'ERROR'}, "Cannot restore the flight data: " + state["error"])
            return {'CANCELLED'}
        if not state["roots"]:
            self.report({'WARNING'}, "No flight model in this file. Imported it as plain glTF")
            return {'FINISHED'}
        root = state["roots"][0]
        root.aeron_flight.source_path = os.path.abspath(self.filepath)
        asset_extras = (document.get("asset") or {}).get("extras")
        root.aeron_flight.asset_extras = json.dumps(asset_extras) if isinstance(asset_extras, dict) else ""
        root.aeron_flight.source_names = json.dumps(sources.names(document))
        _preview_transmission(state["roots"])
        context.view_layer.objects.active = root
        _frame(context, root)
        # The camera at the pilot eye, ready for Numpad 0. It becomes the scene
        # camera only when the scene has none.
        eye = operators.pilot_eye(root)
        if eye is not None:
            camera = operators.pilot_eye_camera(context, root, eye)
            if context.scene.camera is None:
                context.scene.camera = camera
        message = "Imported {} component(s), {} light(s)".format(state["components"], state["lights"])
        if state["animated"]:
            message += ", {} animated material(s)".format(len(state["animated"]))
        self.report({'INFO'}, message)
        return {'FINISHED'}


def menu_func(self, context):
    self.layout.operator(IMPORT_OT_aeron_flight_model.bl_idname, text="Aeron Flight Model (.gltf/.glb)")


classes = (IMPORT_OT_aeron_flight_model,)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(menu_func)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
