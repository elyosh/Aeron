bl_info = {
    "name": "Aeron GLB Exporter",
    "author": "Aeron",
    "version": (1, 0, 0),
    "blender": (3, 6, 0),
    "location": "File > Export > Aeron GLB (.glb)",
    "description": (
        "Export the scene to an Aeron runtime .glb: runs the built-in glTF "
        "exporter into a scratch dir, then post-processes via the "
        "aeron_gltf_cook CLI (per-channel BC7/KTX2 atlases + "
        "KHR_texture_transform)."
    ),
    "category": "Import-Export",
}

import os
import json
import shutil
import subprocess
import tempfile

import bpy
from bpy.props import (
    BoolProperty,
    EnumProperty,
    FloatVectorProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import AddonPreferences, Operator, Panel, PropertyGroup
from bpy_extras.io_utils import ExportHelper

ADDON_ID = __name__
EXTENSION_NAME = "AERON_flight_model"

MESH_TYPE_NAMES = (
    "Default", "Main hull", "Wing", "Fuselage", "Gun turret", "Small gun",
    "Engine", "Bridge", "Shield generator", "Energy generator", "Launcher",
    "Communication system", "Beam system", "Command beam", "Docking platform",
    "Landing platform", "Hangar", "Cargo pod", "Miscellaneous hull", "Antenna",
    "Rotary wing", "Rotary gun turret", "Rotary launcher",
    "Rotary communication system", "Rotary beam system", "Rotary command beam",
    "Hatch", "Custom", "Weapon system 1", "Weapon system 2", "Power regenerator",
    "Reactor",
)

HARDPOINT_TYPE_NAMES = (
    "None", "Laser cannon / Rebel laser", "Ion cannon / Turbo rebel laser",
    "Turbo laser / Empire laser", "Ion turbo laser / Turbo empire laser",
    "Cluster missile / Ion cannon", "Torpedo mag pulse / Turbo ion cannon",
    "Concussion missile / Torpedo", "Proton torpedo / Missile",
    "Advanced concussion / Super rebel laser",
    "Advanced proton torpedo / Super empire laser",
    "Advanced torpedo mag pulse / Super ion cannon", "Bomb / Super torpedo",
    "Beam weapon / Super missile", "Dumb bomb", "Fire rocket / Fired bomb",
    "Mag pulse", "Turbo mag pulse", "Warhead / Super mag pulse", "Gunner",
    "Cockpit sparks", "Docking point", "Towing", "Acceleration start",
    "Acceleration end", "Cockpit / inside hangar", "Engine / outside hangar",
    "Passive heavy dock / dock from big", "Passive light dock / dock from small",
    "Active heavy dock / dock to big", "Active light dock / dock to small",
    "Primary sweep / cockpit",
    "Engine glow", "Custom 1", "Custom 2", "Custom 3", "Custom 4", "Custom 5",
    "Custom 6", "Jamming point",
)


def _enum_items(names):
    return [(str(index), "{}: {}".format(index, name), name)
            for index, name in enumerate(names)]


def _blender_to_gltf(vector):
    return [float(vector[0]), float(vector[2]), -float(vector[1])]


def _blender_extent_to_gltf(vector):
    return [float(vector[0]), float(vector[2]), float(vector[1])]


def _blender_bounds_to_gltf(bounds_min, bounds_max):
    return (
        [float(bounds_min[0]), float(bounds_min[2]), -float(bounds_max[1])],
        [float(bounds_max[0]), float(bounds_max[2]), -float(bounds_min[1])],
    )


class AeronFlightProperties(PropertyGroup):
    role: EnumProperty(
        name="Flight role",
        items=[
            ('none', "None", "Not part of a flight-model hierarchy"),
            ('model', "Model", "Root of one flight model"),
            ('component', "Component", "Ordered polygon component"),
            ('hardpoint', "Hardpoint", "Ordered weapon or attachment point"),
            ('engineGlow', "Engine glow", "Ordered component-owned engine glow"),
        ],
        default='none',
    )
    order: IntProperty(name="Order", default=0, min=0)
    mesh_type: EnumProperty(name="Mesh type", items=_enum_items(MESH_TYPE_NAMES), default='0')
    explosion_flags: IntProperty(name="Explosion flags", default=0, min=0)
    target_id: IntProperty(name="Target ID", default=0)
    target: FloatVectorProperty(name="Target", size=3, subtype='XYZ')
    has_descriptor_geometry: BoolProperty(name="Authored descriptor geometry", default=False)
    descriptor_span: FloatVectorProperty(name="Span", size=3, subtype='XYZ', min=0.0)
    descriptor_center: FloatVectorProperty(name="Center", size=3, subtype='XYZ')
    descriptor_bounds_min: FloatVectorProperty(name="Bounds min", size=3, subtype='XYZ')
    descriptor_bounds_max: FloatVectorProperty(name="Bounds max", size=3, subtype='XYZ')
    has_rotation: BoolProperty(name="Articulated", default=False)
    pivot: FloatVectorProperty(name="Pivot", size=3, subtype='XYZ')
    rotation_axis: FloatVectorProperty(
        name="Rotation axis", size=3, subtype='XYZ', default=(0.0, 1.0, 0.0))
    direction_axis: FloatVectorProperty(
        name="Direction axis", size=3, subtype='XYZ', default=(0.0, 0.0, 1.0))
    up_axis: FloatVectorProperty(
        name="Up axis", size=3, subtype='XYZ', default=(1.0, 0.0, 0.0))
    hardpoint_type: EnumProperty(
        name="Hardpoint type", items=_enum_items(HARDPOINT_TYPE_NAMES), default='0')
    glow_enabled: BoolProperty(name="Enabled", default=True)
    core_color: FloatVectorProperty(
        name="Core color", size=4, subtype='COLOR', min=0.0, max=1.0,
        default=(1.0, 0.8, 0.3, 1.0))
    outer_color: FloatVectorProperty(
        name="Outer color", size=4, subtype='COLOR', min=0.0, max=1.0,
        default=(1.0, 0.2, 0.0, 0.0))


class OBJECT_PT_aeron_flight_model(Panel):
    bl_label = "Aeron Flight Model"
    bl_idname = "OBJECT_PT_aeron_flight_model"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'object'

    def draw(self, context):
        obj = context.object
        if not obj:
            return
        props = obj.aeron_flight
        layout = self.layout
        layout.prop(props, "role")
        if props.role in {'component', 'hardpoint', 'engineGlow'}:
            layout.prop(props, "order")
        if props.role == 'component':
            layout.prop(props, "mesh_type")
            layout.prop(props, "explosion_flags")
            layout.prop(props, "target_id")
            if props.target_id:
                layout.prop(props, "target")
            layout.prop(props, "has_descriptor_geometry")
            if props.has_descriptor_geometry:
                layout.prop(props, "descriptor_span")
                layout.prop(props, "descriptor_center")
                layout.prop(props, "descriptor_bounds_min")
                layout.prop(props, "descriptor_bounds_max")
            layout.prop(props, "has_rotation")
            if props.has_rotation:
                layout.prop(props, "pivot")
                layout.prop(props, "rotation_axis")
                layout.prop(props, "direction_axis")
                layout.prop(props, "up_axis")
        elif props.role == 'hardpoint':
            layout.prop(props, "hardpoint_type")
        elif props.role == 'engineGlow':
            layout.prop(props, "glow_enabled")
            layout.prop(props, "core_color")
            layout.prop(props, "outer_color")


def _default_cooker_path():
    return shutil.which("aeron_gltf_cook") or ""


def _prefs(context):
    return context.preferences.addons[ADDON_ID].preferences


def _tail(text, n=20):
    if not text:
        return ""
    lines = text.rstrip().splitlines()
    return "\n".join(lines[-n:])


def _export_objects(context, selection_only):
    objects = list(context.selected_objects) if selection_only else list(context.scene.objects)
    return [obj for obj in objects if obj.aeron_flight.role != 'none']


def _uniform_positive_scale(obj):
    scale = obj.scale
    return (scale.x > 0.0 and abs(scale.x - scale.y) <= 1.0e-6 and
            abs(scale.x - scale.z) <= 1.0e-6)


def _identity_rotation(obj):
    quat = obj.matrix_basis.decompose()[1]
    return abs(quat.x) <= 1.0e-6 and abs(quat.y) <= 1.0e-6 and abs(quat.z) <= 1.0e-6


def _validate_flight_hierarchy(objects):
    roots = [obj for obj in objects if obj.aeron_flight.role == 'model']
    if len(roots) != 1:
        return None, "export requires exactly one Aeron model root"
    root = roots[0]
    if root.type != 'EMPTY' or root.parent is not None or not _uniform_positive_scale(root):
        return None, "model root must be a top-level Empty with positive uniform scale"
    if root.animation_data:
        return None, "flight-model nodes cannot use animation"
    components = [obj for obj in objects if obj.aeron_flight.role == 'component']
    if not components:
        return None, "model root has no components"
    seen_orders = set()
    for component in components:
        if component.parent != root or component.type != 'MESH' or not _uniform_positive_scale(component):
            return None, "every component must be a direct mesh child with positive uniform scale"
        if component.aeron_flight.order in seen_orders:
            return None, "component order values must be unique"
        seen_orders.add(component.aeron_flight.order)
        if component.data.shape_keys or component.animation_data or component.find_armature():
            return None, "flight components cannot use shape keys, armatures or animation"
        props = component.aeron_flight
        if props.has_descriptor_geometry and any(
                props.descriptor_bounds_min[axis] > props.descriptor_bounds_max[axis]
                for axis in range(3)):
            return None, "component descriptor minimum bounds must not exceed maximum bounds"
        child_orders = set()
        for child in component.children:
            role = child.aeron_flight.role
            if role not in {'hardpoint', 'engineGlow'}:
                return None, "component children must be hardpoints or engine glows"
            if child.type != 'EMPTY' or child.children:
                return None, "hardpoints and engine glows must be childless Empties"
            if child.animation_data:
                return None, "flight-model nodes cannot use animation"
            if child.aeron_flight.order in child_orders:
                return None, "child order values must be unique within a component"
            child_orders.add(child.aeron_flight.order)
            if role == 'hardpoint' and (not _identity_rotation(child) or
                                        any(abs(value - 1.0) > 1.0e-6 for value in child.scale)):
                return None, "hardpoints may use translation only"
            if role == 'engineGlow' and (child.scale.x <= 0.0 or child.scale.y <= 0.0 or
                                         child.scale.z == 0.0):
                return None, "engine-glow X/Y scale must be positive and Z scale nonzero"
    roles = {obj for obj in objects}
    for obj in objects:
        role = obj.aeron_flight.role
        if role in {'hardpoint', 'engineGlow'} and (not obj.parent or obj.parent not in roles or
                                                     obj.parent.aeron_flight.role != 'component'):
            return None, "hardpoints and engine glows must belong to an exported component"
    return root, None


def _extension_for_object(obj):
    props = obj.aeron_flight
    extension = {"role": props.role}
    if props.role == 'component':
        extension["meshType"] = int(props.mesh_type)
        if props.explosion_flags:
            extension["explosionFlags"] = props.explosion_flags
        if props.target_id:
            extension["targetId"] = props.target_id
            extension["target"] = _blender_to_gltf(props.target)
        if props.has_descriptor_geometry:
            bounds_min, bounds_max = _blender_bounds_to_gltf(
                props.descriptor_bounds_min, props.descriptor_bounds_max)
            extension["span"] = _blender_extent_to_gltf(props.descriptor_span)
            extension["center"] = _blender_to_gltf(props.descriptor_center)
            extension["boundsMin"] = bounds_min
            extension["boundsMax"] = bounds_max
        if props.has_rotation:
            extension["rotation"] = {
                "pivot": _blender_to_gltf(props.pivot),
                "rotationAxis": _blender_to_gltf(props.rotation_axis),
                "directionAxis": _blender_to_gltf(props.direction_axis),
                "upAxis": _blender_to_gltf(props.up_axis),
            }
    elif props.role == 'hardpoint':
        extension["type"] = int(props.hardpoint_type)
    elif props.role == 'engineGlow':
        extension["enabled"] = props.glow_enabled
        extension["coreColor"] = list(props.core_color)
        extension["outerColor"] = list(props.outer_color)
    return extension


def _inject_flight_extension(gltf_path, objects, root):
    with open(gltf_path, "r", encoding="utf-8") as stream:
        document = json.load(stream)
    nodes = document.get("nodes", [])
    by_name = {node.get("name"): index for index, node in enumerate(nodes) if node.get("name")}
    for obj in objects:
        if obj.name not in by_name:
            raise ValueError("exported glTF has no node for {!r}".format(obj.name))
        node = nodes[by_name[obj.name]]
        node.setdefault("extensions", {})[EXTENSION_NAME] = _extension_for_object(obj)
    ordered_components = sorted(
        (obj for obj in objects if obj.aeron_flight.role == 'component'),
        key=lambda obj: (obj.aeron_flight.order, obj.name))
    nodes[by_name[root.name]]["children"] = [by_name[obj.name] for obj in ordered_components]
    for component in ordered_components:
        children = sorted(
            (child for child in component.children
             if child.aeron_flight.role in {'hardpoint', 'engineGlow'}),
            key=lambda obj: (obj.aeron_flight.order, obj.name))
        node = nodes[by_name[component.name]]
        if children:
            node["children"] = [by_name[child.name] for child in children]
        else:
            node.pop("children", None)
    used = document.setdefault("extensionsUsed", [])
    if EXTENSION_NAME not in used:
        used.append(EXTENSION_NAME)
    with open(gltf_path, "w", encoding="utf-8") as stream:
        json.dump(document, stream, separators=(",", ":"))


def _export_flight_objects(context, objects, filepath, export_apply):
    selected = list(context.selected_objects)
    active = context.view_layer.objects.active
    try:
        bpy.ops.object.select_all(action='DESELECT')
        for obj in objects:
            obj.select_set(True)
        context.view_layer.objects.active = objects[0]
        return bpy.ops.export_scene.gltf(
            filepath=filepath,
            export_format='GLTF_SEPARATE',
            export_image_format='AUTO',
            export_extras=True,
            use_selection=True,
            export_apply=export_apply,
        )
    finally:
        bpy.ops.object.select_all(action='DESELECT')
        for obj in selected:
            obj.select_set(True)
        context.view_layer.objects.active = active


class AeronExportPreferences(AddonPreferences):
    bl_idname = ADDON_ID

    cooker_path: StringProperty(
        name="aeron_gltf_cook binary",
        subtype='FILE_PATH',
        default=_default_cooker_path(),
        description="Path to the aeron_gltf_cook executable",
    )
    max_atlas_size: IntProperty(
        name="Max atlas size",
        default=2048, min=4, soft_max=8192, max=16384,
        description="Per-axis max atlas size; must be a power of 2 ≥ 4",
    )
    mip_min_subrect: IntProperty(
        name="Sub-rect gutter",
        default=4, min=1, max=64,
        description=(
            "Gutter texels around each sub-rect; also caps mip depth"
        ),
    )
    bc7_quality: EnumProperty(
        name="BC7 quality",
        items=[
            ('fast', "Fast", "Quickest BC7 encode"),
            ('med',  "Medium", "Default; seconds per atlas"),
            ('uber', "Uber", "Slowest; near-lossless"),
        ],
        default='med',
    )
    zstd_supercompress: BoolProperty(
        name="Zstd supercompress KTX2",
        default=True,
    )
    verbose: BoolProperty(
        name="Verbose cook log",
        default=False,
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "cooker_path")
        row = layout.row(align=True)
        row.prop(self, "max_atlas_size")
        row.prop(self, "mip_min_subrect")
        row = layout.row(align=True)
        row.prop(self, "bc7_quality")
        row.prop(self, "zstd_supercompress")
        layout.prop(self, "verbose")


class EXPORT_OT_aeron_glb(Operator, ExportHelper):
    """Export the scene to an Aeron runtime .glb via aeron_gltf_cook."""

    bl_idname = "export_scene.aeron_glb"
    bl_label = "Export Aeron GLB"
    bl_options = {'PRESET'}

    filename_ext = ".glb"
    filter_glob: StringProperty(default="*.glb", options={'HIDDEN'})

    use_selection: BoolProperty(
        name="Selection Only",
        description="Export only selected objects",
        default=False,
    )
    export_apply: BoolProperty(
        name="Apply Modifiers",
        description="Apply modifiers (preview viewport mesh) before export",
        default=False,
    )
    keep_intermediate: BoolProperty(
        name="Keep intermediate .gltf",
        description=(
            "Don't delete the scratch .gltf/.bin/.png after a successful "
            "cook (failures always keep them)"
        ),
        default=False,
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "use_selection")
        layout.prop(self, "export_apply")
        layout.prop(self, "keep_intermediate")

    def execute(self, context):
        prefs = _prefs(context)
        cooker = bpy.path.abspath(prefs.cooker_path) if prefs.cooker_path else ""
        if not cooker or not os.path.isfile(cooker) or not os.access(cooker, os.X_OK):
            self.report(
                {'ERROR'},
                "aeron_gltf_cook binary not configured or not executable. Set "
                "it in Edit > Preferences > Add-ons > Aeron GLB Exporter. "
                "Current: {!r}".format(cooker),
            )
            return {'CANCELLED'}

        flight_objects = _export_objects(context, self.use_selection)
        model_root, hierarchy_error = _validate_flight_hierarchy(flight_objects)
        if hierarchy_error:
            self.report({'ERROR'}, hierarchy_error)
            return {'CANCELLED'}

        out_glb = bpy.path.abspath(self.filepath)
        if not out_glb.lower().endswith(".glb"):
            out_glb += ".glb"
        out_dir = os.path.dirname(out_glb)
        if out_dir and not os.path.isdir(out_dir):
            self.report({'ERROR'}, "output directory does not exist: " + out_dir)
            return {'CANCELLED'}

        scratch = tempfile.mkdtemp(prefix="aeron_glb_")
        scratch_gltf = os.path.join(scratch, "scene.gltf")

        try:
            res = _export_flight_objects(
                context, flight_objects, scratch_gltf, self.export_apply)
        except Exception as e:
            self.report({'ERROR'}, "Blender glTF export raised: {}".format(e))
            return {'CANCELLED'}
        if 'FINISHED' not in res:
            self.report(
                {'ERROR'},
                "Blender glTF export did not finish: {!r}. Scratch: {}".format(
                    res, scratch
                ),
            )
            return {'CANCELLED'}

        try:
            _inject_flight_extension(scratch_gltf, flight_objects, model_root)
        except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
            self.report({'ERROR'}, "flight metadata export failed: {}".format(error))
            return {'CANCELLED'}

        argv = [
            cooker, scratch_gltf, "--out", out_glb,
            "--max-atlas", str(prefs.max_atlas_size),
            "--mip-min-subrect", str(prefs.mip_min_subrect),
            "--quality", prefs.bc7_quality,
        ]
        if not prefs.zstd_supercompress:
            argv.append("--no-zstd")
        if prefs.verbose:
            argv.append("--verbose")

        try:
            cp = subprocess.run(argv, capture_output=True, text=True)
        except OSError as e:
            self.report(
                {'ERROR'},
                "failed to launch {!r}: {}. Scratch: {}".format(
                    cooker, e, scratch
                ),
            )
            return {'CANCELLED'}

        if cp.returncode != 0:
            self.report(
                {'ERROR'},
                "aeron_gltf_cook exit {}. Scratch kept at {}.\n{}".format(
                    cp.returncode, scratch, _tail(cp.stderr or cp.stdout)
                ),
            )
            return {'CANCELLED'}

        if not self.keep_intermediate:
            shutil.rmtree(scratch, ignore_errors=True)

        if prefs.verbose and cp.stderr:
            self.report({'INFO'}, _tail(cp.stderr))
        self.report({'INFO'}, "Wrote " + out_glb)
        return {'FINISHED'}


def _menu_func(self, context):
    self.layout.operator(EXPORT_OT_aeron_glb.bl_idname, text="Aeron GLB (.glb)")


_classes = (
    AeronFlightProperties,
    AeronExportPreferences,
    OBJECT_PT_aeron_flight_model,
    EXPORT_OT_aeron_glb,
)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.Object.aeron_flight = PointerProperty(type=AeronFlightProperties)
    bpy.types.TOPBAR_MT_file_export.append(_menu_func)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(_menu_func)
    del bpy.types.Object.aeron_flight
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
