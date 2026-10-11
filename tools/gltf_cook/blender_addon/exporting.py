# Export: Blender's glTF exporter writes the model's objects (filtered,
# ordered and given their AERON_flight_model data by the hooks below, which
# act only during this add-on's export), then aeron_gltf_cook cooks the
# .gltf into the runtime .glb.

import json
import os
import shutil
import subprocess
import tempfile

import bpy
from bpy.props import BoolProperty, StringProperty
from bpy.types import Operator
from bpy_extras.io_utils import ExportHelper
from mathutils import Matrix

from . import common, validation

# Blender (x, y, z) -> glTF (x, z, -y).
_TO_GLTF = Matrix(((1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0), (0.0, -1.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)))

# The running export (None outside this add-on's exports).
_state = None


def _ordinal(ordinals, ref):
    return ordinals[ref.component.name] if ref.component is not None else ref.component_index


def _model_extension(root, ordinals):
    props = root.aeron_flight
    extension = {"role": "model"}
    articulations = []
    for articulation in props.articulations:
        item = {"component": _ordinal(ordinals, articulation), "group": articulation.group,
                "angle": articulation.angle, "speed": articulation.speed}
        if articulation.has_speed_back:
            item["speedBack"] = articulation.speed_back
        if articulation.behavior:
            item["behavior"] = articulation.behavior
        articulations.append(item)
    if articulations:
        extension["articulations"] = articulations
    if len(props.articulation_settings):
        extension["articulationSettings"] = {s.name: s.value for s in props.articulation_settings}
    if len(props.component_sets):
        extension["componentSets"] = {
            s.name: [_ordinal(ordinals, member) for member in s.members] for s in props.component_sets}
    return extension


def _component_extension(obj):
    props = obj.aeron_flight
    extension = {"role": "component", "meshType": int(props.mesh_type)}
    flags = props.explosion_flags_bits & 0xFFFFFFFF
    if flags:
        extension["explosionFlags"] = flags
    tags = common.parse_tags(props.tags)
    if tags:
        extension["tags"] = tags
    if props.target_id:
        extension["targetId"] = props.target_id
    if props.target_id or any(props.target):
        extension["target"] = common.to_gltf(props.target)
    if props.has_descriptor_geometry:
        bounds_min, bounds_max = common.bounds_to_gltf(props.descriptor_bounds_min, props.descriptor_bounds_max)
        extension["span"] = common.extent_to_gltf(props.descriptor_span)
        extension["center"] = common.to_gltf(props.descriptor_center)
        extension["boundsMin"] = bounds_min
        extension["boundsMax"] = bounds_max
    if props.has_rotation:
        extension["rotation"] = {
            "pivot": common.to_gltf(props.pivot),
            "rotationAxis": common.to_gltf(props.rotation_axis),
            "directionAxis": common.to_gltf(props.direction_axis),
            "upAxis": common.to_gltf(props.up_axis),
        }
    return extension


def _glow_trs(obj):
    """An engine glow's glTF TRS: X/Y scale positive (its size), Z signed
    (its depth), which a plain decomposition does not keep."""
    matrix = _TO_GLTF @ common.local_matrix(obj) @ _TO_GLTF.inverted()
    basis = matrix.to_3x3()
    columns = [basis.col[axis].copy() for axis in range(3)]
    scale = [column.length for column in columns]
    if basis.determinant() < 0.0:
        scale[2] = -scale[2]
    rotation = Matrix([column / s for column, s in zip(columns, scale)]).transposed().to_quaternion()
    translation = matrix.translation
    return ([float(v) for v in translation],
            [float(rotation.x), float(rotation.y), float(rotation.z), float(rotation.w)],
            [float(v) for v in scale])


class _ExportState:
    """What the hooks write, prepared from the validated model."""

    def __init__(self, root):
        self.names = {root.name}
        self.extensions = {}
        self.lights = {}
        self.glows = {}
        self.rank = {}
        self.nodes = {}
        parts = common.components(root)
        ordinals = {obj.name: index for index, obj in enumerate(parts)}
        self.extensions[root.name] = _model_extension(root, ordinals)
        for ordinal, component in enumerate(parts):
            self.names.add(component.name)
            self.rank[component.name] = (0, ordinal)
            self.extensions[component.name] = _component_extension(component)
            for index, child in enumerate(common.ordered_children(component)):
                self.names.add(child.name)
                self.rank[child.name] = (0, index)
                if common.role(child) == 'hardpoint':
                    self.extensions[child.name] = {"role": "hardpoint",
                                                   "type": int(child.aeron_flight.hardpoint_type)}
                else:
                    props = child.aeron_flight
                    self.extensions[child.name] = {"role": "engineGlow", "enabled": props.glow_enabled,
                                                   "coreColor": list(props.core_color),
                                                   "outerColor": list(props.outer_color)}
                    self.glows[child.name] = _glow_trs(child)
            for child in component.children:
                if common.role(child) == 'collision':
                    self.names.add(child.name)
                    self.rank[child.name] = (1, 0)
                    self.extensions[child.name] = {"role": "collision"}
                elif common.is_model_light(child):
                    self.names.add(child.name)
                    self.rank[child.name] = (2, child.name)
                    extension = {"role": "light"}
                    extension.update(common.light_shape(child, common.local_matrix(child).to_scale()))
                    self.extensions[child.name] = extension
                    self.lights[child.name] = common.light_punctual(child, child.matrix_world)

    def filter_tree(self, vtree):
        for node in vtree.nodes.values():
            obj = node.blender_object
            node.keep_tag = obj is not None and node.blender_type != node.COLLECTION and obj.name in self.names

    def node(self, gltf_node, obj):
        from io_scene_gltf2.io.com.gltf2_io_extensions import ChildOfRootExtension, Extension

        if obj is None or obj.name not in self.names:
            return
        self.nodes[id(gltf_node)] = obj.name
        if isinstance(gltf_node.extras, dict):
            gltf_node.extras.pop("aeron_flight", None)
            gltf_node.extras = gltf_node.extras or None
        extension = self.extensions[obj.name]
        gltf_node.extensions = gltf_node.extensions or {}
        gltf_node.extensions[common.EXTENSION_NAME] = Extension(
            name=common.EXTENSION_NAME, extension=extension, required=False)
        if obj.name in self.glows:
            gltf_node.translation, gltf_node.rotation, gltf_node.scale = self.glows[obj.name]
        if obj.name in self.lights:
            # The sizes carry the light's scale; Blender exports area lights
            # without a light, and point lights in its own way.
            gltf_node.scale = None
            light = ChildOfRootExtension(name=common.LIGHTS_EXTENSION_NAME, path=["lights"],
                                         extension=self.lights[obj.name])
            gltf_node.extensions[common.LIGHTS_EXTENSION_NAME] = Extension(
                name=common.LIGHTS_EXTENSION_NAME, extension={"light": light}, required=False)
        if extension["role"] == 'collision' and gltf_node.mesh is not None:
            # Never rendered: no materials, so the cooker atlases no textures.
            for primitive in gltf_node.mesh.primitives:
                primitive.material = None
        if gltf_node.children:
            gltf_node.children.sort(key=lambda child: self.rank.get(self.nodes.get(id(child), ""), (3, "")))


class glTF2ExportUserExtension:
    """Blender's glTF exporter hooks (found on the add-on's module)."""

    def gather_tree_filter_tag_hook(self, vtree, export_settings):
        if _state is not None:
            _state.filter_tree(vtree)

    def gather_node_hook(self, gltf_node, blender_object, export_settings):
        if _state is not None:
            _state.node(gltf_node, blender_object)

    def gather_scene_hook(self, gltf_scene, blender_scene, export_settings):
        if _state is not None and isinstance(gltf_scene.extras, dict):
            gltf_scene.extras.pop("aeron_flight", None)
            gltf_scene.extras = gltf_scene.extras or None


def _png_images(gltf_path):
    """The cooker reads PNG only: convert the exported JPEG and WebP images."""
    with open(gltf_path, "r", encoding="utf-8") as stream:
        document = json.load(stream)
    directory = os.path.dirname(gltf_path)
    changed = False
    for image in document.get("images", []):
        uri = image.get("uri", "")
        stem, extension = os.path.splitext(uri)
        if extension.lower() not in {".jpg", ".jpeg", ".webp"}:
            continue
        source = os.path.join(directory, uri)
        loaded = bpy.data.images.load(source)
        try:
            loaded.pixels[0]  # loads the image before its path changes
            loaded.file_format = 'PNG'
            loaded.filepath_raw = os.path.join(directory, stem + ".png")
            loaded.save()
        finally:
            bpy.data.images.remove(loaded)
        image["uri"] = stem + ".png"
        image["mimeType"] = "image/png"
        changed = True
    if changed:
        with open(gltf_path, "w", encoding="utf-8") as stream:
            json.dump(document, stream, separators=(",", ":"))


def export_gltf(root, filepath, apply_modifiers=True):
    """Write `root`'s model as an uncooked .gltf (with its .bin and images)."""
    global _state
    context = bpy.context
    active = context.view_layer.objects.active
    mode = active.mode if active is not None else 'OBJECT'
    _state = _ExportState(root)
    try:
        # Tangents (MikkTSpace, as glTF expects): the game's normal maps need them.
        result = bpy.ops.export_scene.gltf(
            filepath=filepath, export_format='GLTF_SEPARATE', export_image_format='AUTO',
            export_yup=True, export_apply=apply_modifiers, export_tangents=True, export_animations=False,
            export_lights=True, export_cameras=False, export_extras=True, export_current_frame=True,
            export_import_convert_lighting_mode='SPEC', export_skins=False, export_morph=False,
            export_gpu_instances=False, export_draco_mesh_compression_enable=False,
            export_hierarchy_flatten_objs=False, use_selection=False, use_visible=False,
            use_renderable=False, use_active_collection=False, will_save_settings=False)
    finally:
        _state = None
        # The exporter leaves edit and other modes for object mode.
        if active is not None and mode != 'OBJECT' and context.view_layer.objects.active == active:
            bpy.ops.object.mode_set(mode=mode)
    if 'FINISHED' not in result:
        raise RuntimeError("Blender's glTF export did not finish ({})".format(result))
    _png_images(filepath)


def _tail(text, count=20):
    return "\n".join((text or "").rstrip().splitlines()[-count:])


class EXPORT_OT_aeron_flight_model(Operator, ExportHelper):
    """Export the selected object's flight model to an Aeron runtime .glb"""

    bl_idname = "export_scene.aeron_flight_model"
    bl_label = "Export Aeron Flight Model"
    bl_options = {'PRESET'}

    filename_ext = ".glb"
    filter_glob: StringProperty(default="*.glb", options={'HIDDEN'})
    apply_modifiers: BoolProperty(name="Apply Modifiers", default=True,
                                  description="Export meshes with their modifiers applied")
    keep_intermediate: BoolProperty(
        name="Keep Intermediate .gltf", default=False,
        description="Keep the uncooked .gltf, .bin and images next to the .glb "
                    "(a failed cook always keeps them)")

    def execute(self, context):
        from . import preferences

        cooker = preferences.cooker_path(context)
        if not cooker:
            self.report({'ERROR'}, "aeron_gltf_cook is not set: see the add-on's preferences")
            return {'CANCELLED'}
        root = common.model_root(context.active_object) or _single_root(context.scene)
        problems = validation.validate(root)
        validation.store(context.scene, problems)
        if problems.errors:
            self.report({'ERROR'}, "{} problem(s), see Aeron > Problems: {}".format(
                len(problems.errors), problems.errors[0][1]))
            return {'CANCELLED'}

        out_glb = bpy.path.abspath(self.filepath)
        out_dir = os.path.dirname(out_glb)
        if out_dir and not os.path.isdir(out_dir):
            self.report({'ERROR'}, "no such directory: " + out_dir)
            return {'CANCELLED'}
        scratch = tempfile.mkdtemp(prefix="aeron_flight_model_")
        scratch_gltf = os.path.join(scratch, os.path.splitext(os.path.basename(out_glb))[0] + ".gltf")
        try:
            export_gltf(root, scratch_gltf, self.apply_modifiers)
        except Exception as error:
            self.report({'ERROR'}, "glTF export failed: {}. Scratch: {}".format(error, scratch))
            return {'CANCELLED'}

        prefs = preferences.get(context)
        argv = [cooker, scratch_gltf, "--out", out_glb,
                "--atlas-page", str(prefs.atlas_page), "--max-texture", str(prefs.max_texture),
                "--mip-min-subrect", prefs.mip_min_subrect, "--quality", prefs.bc7_quality]
        if not prefs.zstd:
            argv.append("--no-zstd")
        try:
            completed = subprocess.run(argv, capture_output=True, text=True)
        except OSError as error:
            self.report({'ERROR'}, "cannot run {}: {}. Scratch: {}".format(cooker, error, scratch))
            return {'CANCELLED'}
        if completed.returncode != 0:
            self.report({'ERROR'}, "aeron_gltf_cook failed (exit {}). Scratch: {}\n{}".format(
                completed.returncode, scratch, _tail(completed.stderr or completed.stdout)))
            return {'CANCELLED'}
        if self.keep_intermediate:
            kept = os.path.splitext(out_glb)[0] + "_gltf"
            shutil.rmtree(kept, ignore_errors=True)
            shutil.move(scratch, kept)
        else:
            shutil.rmtree(scratch, ignore_errors=True)
        warnings = len(problems.items) - len(problems.errors)
        self.report({'INFO'}, "Wrote {}{}".format(
            out_glb, " ({} warning(s), see Aeron > Problems)".format(warnings) if warnings else ""))
        return {'FINISHED'}


def _single_root(scene):
    roots = [obj for obj in scene.objects if common.role(obj) == 'model']
    return roots[0] if len(roots) == 1 else None


def menu_func(self, context):
    self.layout.operator(EXPORT_OT_aeron_flight_model.bl_idname, text="Aeron Flight Model (.glb)")


classes = (EXPORT_OT_aeron_flight_model,)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_export.append(menu_func)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
