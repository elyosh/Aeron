# Import: Blender's glTF importer reads the model, then the hook below (active
# only during this add-on's import) restores the AERON_flight_model data on
# the objects it created: roles and fields, the ordinals as list order, the
# model root's articulations, settings and sets, and the model lights as
# Blender point and area lights. The model's animation file, asset extras
# and the names of its extensions and extras are kept too (sources.py).

import json
import os

import bpy
from bpy.props import StringProperty
from bpy.types import Operator
from bpy_extras.io_utils import ImportHelper

from . import animation, common, sources

# The running import's results (None outside this add-on's imports).
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
        for name in extension.get("markings", []):
            props.markings.add().variant = name or ""


class glTF2ImportUserExtension:
    """Blender's glTF importer hooks (found on the add-on's module)."""

    def gather_import_scene_after_nodes_hook(self, gltf_scene, blender_scene, gltf):
        if _state is None:
            return
        _state["cooked"] = "KHR_texture_basisu" in (gltf.data.extensions_used or [])
        _state["materials"] = {material.name: set(getattr(material, "blender_material", {}).values())
                               for material in gltf.data.materials or [] if material.name}
        try:
            restore(gltf)
        except Exception as error:  # the importer would only log it
            _state["error"] = "{}: {}".format(type(error).__name__, error)


def _preview_transmission(roots):
    """EEVEE shows what is behind transmissive (glass) materials only with
    raytraced transmission; a preview setting that is not exported."""
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


class IMPORT_OT_aeron_flight_model(Operator, ImportHelper):
    """Import a flight model (.gltf or .glb) with its Aeron flight data"""

    bl_idname = "import_scene.aeron_flight_model"
    bl_label = "Import Aeron Flight Model"
    bl_options = {'REGISTER', 'UNDO'}

    filter_glob: StringProperty(default="*.gltf;*.glb", options={'HIDDEN'})

    def execute(self, context):
        from . import preferences

        global _state
        try:
            document = sources.read_document(self.filepath)
        except (OSError, ValueError) as error:
            self.report({'ERROR'}, "cannot read {}: {}".format(self.filepath, error))
            return {'CANCELLED'}
        animations, energy = {}, None
        animation_file = sources.animation_path(self.filepath)
        if os.path.isfile(animation_file):
            cooker = preferences.cooker_path(context)
            if not cooker:
                self.report({'ERROR'}, "reading the model's animation file needs aeron_gltf_cook: see the "
                                       "add-on's preferences")
                return {'CANCELLED'}
            try:
                animations, energy = sources.read_animations(cooker, animation_file)
            except (OSError, RuntimeError) as error:
                self.report({'ERROR'}, "cannot read {}: {}".format(animation_file, error))
                return {'CANCELLED'}
        _state = {"roots": [], "components": 0, "lights": 0, "cooked": False, "error": None, "materials": {}}
        try:
            result = bpy.ops.import_scene.gltf(
                filepath=self.filepath, merge_vertices=False, import_shading='NORMALS',
                export_import_convert_lighting_mode='SPEC')
            state = _state
        finally:
            _state = None
        if 'FINISHED' not in result:
            self.report({'ERROR'}, "Blender's glTF import did not finish")
            return {'CANCELLED'}
        if state["error"]:
            self.report({'ERROR'}, "the flight data was not restored: " + state["error"])
            return {'CANCELLED'}
        unmatched = []
        for name, entry in animations.items():
            matched = [bpy.data.materials.get(blender_name) for blender_name in state["materials"].get(name, ())]
            matched = [material for material in matched if material is not None]
            for material in matched:
                animation.set_entry(material, entry)
            if not matched:
                unmatched.append(name)
        if not state["roots"]:
            self.report({'WARNING'}, "no Aeron flight model in this file: imported as plain glTF")
            return {'FINISHED'}
        root = state["roots"][0]
        root.aeron_flight.source_path = os.path.abspath(self.filepath)
        animation.set_energy_default(root, energy)
        asset_extras = (document.get("asset") or {}).get("extras")
        root.aeron_flight.asset_extras = json.dumps(asset_extras) if isinstance(asset_extras, dict) else ""
        root.aeron_flight.source_names = json.dumps(sources.names(document))
        _preview_transmission(state["roots"])
        context.view_layer.objects.active = root
        message = "Imported {} component(s), {} light(s)".format(state["components"], state["lights"])
        if animations:
            message += ", {} animated material(s)".format(len(animations) - len(unmatched))
        if unmatched:
            self.report({'WARNING'}, "the animation file names material(s) the model does not have, "
                                     "dropped: " + ", ".join(unmatched))
        if state["cooked"]:
            self.report({'WARNING'}, message + "; a cooked .glb has textures Blender cannot read: "
                                               "import the uncooked .gltf instead")
        else:
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
