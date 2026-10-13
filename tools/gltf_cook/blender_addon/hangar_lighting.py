# Hangar lighting: an environment probe on a hangar model, and the bake of
# the hangar's environment package from it. The bake renders a panorama of
# the hangar from the probe with Cycles, and environment_bake turns it into
# the package's diffuse and specular cubes. In the game, the package lights
# the ships in the hangar and gives them reflections of it.
#
# The cube frame (+X right, +Y up, -Z forward) is fixed in the hangar
# model's frame: in Blender's coordinates, cube = (-x, z, y). This follows
# from opt2gltf's axes, the game's glTF loader and the hangar object's
# angles. A camera's local axes match the cube's when it looks toward -Y,
# with +Z up and its right toward -X.
#
# The panorama holds all of the hangar's light except the game's ceiling
# key light, which the game adds directly because it casts shadows. A Sun
# lamp stands in for it so that the walls receive it, and a Sun lamp is
# never seen by the camera. The hangar's light objects are seen by the probe:
# their light is baked, and the game no longer adds them.
#
# Shadows are the game's: the hangar model casts none (the key light stands
# for a ceiling source, which the game does not let the hangar block), and
# light objects cast none. Other models, such as placed props, do.
#
# The light levels come from the game's remaster/config.yaml at bake time,
# so that a bake follows the game's current calibration: the key light
# (hangar_lighting) has the illuminance intensity x
# lighting.sun_illuminance_scale, emission is scaled by
# lighting.emissive_scale, and light objects by
# point_lights.model_light_intensity x 683 (the game's candela to Blender's
# watts).

import math
import os
import re
import subprocess
import tempfile

import bpy
import mathutils
from bpy.props import IntProperty, StringProperty
from bpy.types import Operator, Panel, PropertyGroup

from . import common, libraries, preferences, ui

PANORAMA_SIZE = (2048, 1024)  # pi x 512 px specular faces, rounded up
CUBE_ROTATION = mathutils.Euler((math.pi / 2.0, 0.0, math.pi))
PACKAGE_NAME = re.compile(r"^[a-z0-9._]{1,63}$")


class AeronProbeProperties(PropertyGroup):
    samples: IntProperty(name="Samples", default=256, min=16, max=4096,
                         description="Cycles samples per pixel of the panorama. It is denoised")


# The game's config entries that the bake reads.
CONFIG_KEYS = (
    "hangar_lighting.enabled", "hangar_lighting.direction_x", "hangar_lighting.direction_y",
    "hangar_lighting.direction_z", "hangar_lighting.intensity", "hangar_lighting.color_r",
    "hangar_lighting.color_g", "hangar_lighting.color_b", "lighting.sun_illuminance_scale",
    "lighting.emissive_scale", "point_lights.model_light_intensity",
)


def read_config(path):
    """The CONFIG_KEYS entries of the game's config.yaml, as numbers (booleans
    as 0 or 1). It reads plain `section:` / `  key: value` lines, which is
    all these entries use. Raises ValueError when one is missing."""
    values, section, indent = {}, "", None
    with open(path) as stream:
        for line in stream:
            text = line.split("#", 1)[0].rstrip()
            if not text.strip() or ":" not in text:
                continue
            key, value = (part.strip() for part in text.split(":", 1))
            depth = len(text) - len(text.lstrip())
            if depth == 0:
                section, indent = key, None
                continue
            indent = depth if indent is None else indent
            name = section + "." + key
            if depth == indent and name in CONFIG_KEYS:
                values[name] = {"true": 1.0, "false": 0.0}.get(value.lower()) if value.lower() in {
                    "true", "false"} else float(value)
    missing = [key for key in CONFIG_KEYS if key not in values]
    if missing:
        raise ValueError("{} has no {}".format(path, ", ".join(missing)))
    return values


def probe_of(root):
    return next((child for child in root.children if common.role(child) == 'environmentProbe'), None)


def _new_probe(root, location):
    probe = bpy.data.objects.new(root.name + " Probe", None)
    probe.empty_display_type = 'SPHERE'
    probe.empty_display_size = 0.5
    probe.lock_rotation = (True, True, True)
    probe.aeron_flight.role = 'environmentProbe'
    for collection in root.users_collection:
        collection.objects.link(probe)
    probe.parent = root
    probe.matrix_world = mathutils.Matrix.Translation(location)
    return probe


# ---- import and export -------------------------------------------------------------

def restore(root, extension):
    """Creates the probe of the model root's `environmentProbe`, if any."""
    item = extension.get("environmentProbe")
    if not isinstance(item, dict):
        return
    x, y, z = item.get("position", (0.0, 0.0, 0.0))
    probe = _new_probe(root, root.matrix_world @ mathutils.Vector((x, -z, y)))
    probe.aeron_probe.samples = item.get("samples", probe.aeron_probe.samples)


def probe_json(root):
    """The model root's `environmentProbe`, or None."""
    probe = probe_of(root)
    if probe is None:
        return None
    t = (root.matrix_world.inverted() @ probe.matrix_world).translation
    return {"position": [t.x, t.z, -t.y], "samples": probe.aeron_probe.samples}


def check(problems, root):
    probes = [child for child in root.children if common.role(child) == 'environmentProbe']
    for probe in probes[1:]:
        problems.error(probe, "the model has another environment probe")


# ---- bake --------------------------------------------------------------------------------

def _srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


class _BakeScene:
    """Sets the scene up for the panorama, and puts everything back after."""

    def __init__(self, scene, probe, config):
        self.scene = scene
        self.probe = probe
        self.config = config
        self.saved = []
        self.added = []

    def _set(self, owner, name, value):
        self.saved.append((owner, name, getattr(owner, name)))
        setattr(owner, name, value)

    def __enter__(self):
        scene, settings, config = self.scene, self.probe.aeron_probe, self.config
        root = self.probe.parent
        render = scene.render
        self._set(render, "engine", 'CYCLES')
        self._set(render, "resolution_x", PANORAMA_SIZE[0])
        self._set(render, "resolution_y", PANORAMA_SIZE[1])
        self._set(render, "resolution_percentage", 100)
        self._set(render, "film_transparent", False)
        self._set(render, "use_border", False)
        self._set(scene.cycles, "samples", settings.samples)
        self._set(scene.cycles, "use_denoising", True)
        self._set(scene.view_settings, "view_transform", 'Standard')
        self._set(scene.view_settings, "look", 'None')
        self._set(scene.view_settings, "exposure", 0.0)
        self._set(scene.view_settings, "gamma", 1.0)
        image = render.image_settings
        self._set(image, "file_format", 'HDR')

        # Nothing comes from outside the hangar.
        world = bpy.data.worlds.new("Aeron Probe World")
        world.color = (0.0, 0.0, 0.0)
        for node in world.node_tree.nodes if world.node_tree else ():
            if node.type == 'BACKGROUND':
                node.inputs["Strength"].default_value = 0.0
        self.added.append(world)
        self._set(scene, "world", world)

        camera_data = bpy.data.cameras.new("Aeron Probe Camera")
        camera_data.type = 'PANO'
        camera_data.panorama_type = 'EQUIRECTANGULAR'
        camera_data.clip_start = 0.01
        camera_data.clip_end = 100000.0
        camera = bpy.data.objects.new("Aeron Probe Camera", camera_data)
        camera.matrix_world = (mathutils.Matrix.Translation(self.probe.matrix_world.translation) @
                               (root.matrix_world.to_quaternion() @ CUBE_ROTATION.to_quaternion()).to_matrix().to_4x4())
        scene.collection.objects.link(camera)
        self.added += [camera, camera_data]
        self._set(scene, "camera", camera)

        light_scale = config["point_lights.model_light_intensity"] * common.WATTS_TO_LUMENS
        for obj in scene.objects:
            if obj.type == 'LIGHT' and not obj.hide_render:
                self._set(obj, "visible_camera", True)
                self._set(obj.data, "energy", obj.data.energy * light_scale)
                self._set(obj.data, "use_shadow", False)
        for component in common.components(root):
            self._set(component, "visible_shadow", False)
        materials = {slot.material for obj in scene.objects if obj.type == 'MESH' and not obj.hide_render
                     for slot in obj.material_slots if slot.material and slot.material.node_tree}
        for material in materials:
            for node in material.node_tree.nodes:
                name = {'BSDF_PRINCIPLED': "Emission Strength", 'EMISSION': "Strength"}.get(node.type)
                if name and not node.inputs[name].is_linked:
                    socket = node.inputs[name]
                    self._set(socket, "default_value", socket.default_value * config["lighting.emissive_scale"])
        if config["hangar_lighting.enabled"]:
            sun_data = bpy.data.lights.new("Aeron Key Light", 'SUN')
            sun_data.energy = config["hangar_lighting.intensity"] * config["lighting.sun_illuminance_scale"]
            sun_data.color = [_srgb_to_linear(config["hangar_lighting.color_" + c]) for c in "rgb"]
            sun_data.angle = 0.0
            sun = bpy.data.objects.new("Aeron Key Light", sun_data)
            wx, wy, wz = (config["hangar_lighting.direction_" + c] for c in "xyz")
            toward = root.matrix_world.to_quaternion() @ mathutils.Vector((-wx, -wy, wz))
            if toward.length > 0.0:
                sun.rotation_euler = toward.to_track_quat('Z', 'Y').to_euler()
            scene.collection.objects.link(sun)
            self.added += [sun, sun_data]
        return self

    def __exit__(self, *exc):
        for owner, name, value in reversed(self.saved):
            setattr(owner, name, value)
        for block in self.added:
            if isinstance(block, bpy.types.Object):
                bpy.data.objects.remove(block)
            elif isinstance(block, bpy.types.Camera):
                bpy.data.cameras.remove(block)
            elif isinstance(block, bpy.types.Light):
                bpy.data.lights.remove(block)
            elif isinstance(block, bpy.types.World):
                bpy.data.worlds.remove(block)
        return False


def render_panorama(scene, probe, config, path):
    """Renders the panorama of the hangar from `probe` to a Radiance file at
    `path`, with the light levels of the game's config (read_config)."""
    with _BakeScene(scene, probe, config):
        bpy.ops.render.render(scene=scene.name)
        bpy.data.images["Render Result"].save_render(path, scene=scene)


def remaster_dir(context):
    """The game's remaster folder: the parent of the Game Effects Folder setting, or ""."""
    effects = preferences.get(context).effects_dir
    return os.path.dirname(os.path.normpath(bpy.path.abspath(effects))) if effects else ""


def write_mapping(path, key, package):
    with open(path, "w") as stream:
        stream.write("# Mapping for the hangar model: merge under the manifest's xwa section.\n"
                     "xwa:\n  hangars:\n    {}: {}\n".format(key, package))


class AERON_OT_add_probe(Operator):
    """Add the environment probe of the hangar lighting bake at the 3D cursor, where ships stand"""

    bl_idname = "aeron.add_probe"
    bl_label = "Add Environment Probe"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return common.model_root(context.active_object) is not None

    def execute(self, context):
        root = common.model_root(context.active_object)
        probe = probe_of(root)
        if probe is None:
            probe = _new_probe(root, context.scene.cursor.location)
        else:
            self.report({'INFO'}, "{} already has a probe".format(root.name))
        for obj in context.view_layer.objects:
            obj.select_set(False)
        probe.select_set(True)
        context.view_layer.objects.active = probe
        return {'FINISHED'}


class AERON_OT_bake_hangar_lighting(Operator):
    """Render the hangar from the probe and bake its environment package (lighting and reflections) into the \
game's remaster folder"""

    bl_idname = "aeron.bake_hangar_lighting"
    bl_label = "Bake Hangar Lighting"

    directory: StringProperty(name="Remaster Folder", subtype='DIR_PATH',
                              description="The game's remaster folder")

    @classmethod
    def poll(cls, context):
        return common.role(context.object) == 'environmentProbe' and common.model_root(context.object) is not None

    def invoke(self, context, event):
        self.directory = remaster_dir(context)
        if self.directory:
            return self.execute(context)
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        probe = context.object
        root = common.model_root(probe)
        if not root.aeron_flight.source_path:
            self.report({'ERROR'}, "Save the model to its project first: its folder name is its key")
            return {'CANCELLED'}
        key = libraries.model_key(root).lower()
        package = "hangar." + key
        if not PACKAGE_NAME.match(package):
            self.report({'ERROR'}, "The model key {!r} must be lower-case letters, digits, '.' and '_'".format(key))
            return {'CANCELLED'}
        if bpy.data.colorspace.working_space != 'Linear Rec.709':
            self.report({'ERROR'}, "The bake needs the Linear Rec.709 working color space")
            return {'CANCELLED'}
        baker = preferences.environment_bake_path(context)
        if not baker:
            self.report({'ERROR'}, "Set the environment_bake path in the add-on preferences")
            return {'CANCELLED'}
        remaster = bpy.path.abspath(self.directory or remaster_dir(context))
        if not remaster or not os.path.isdir(remaster):
            self.report({'ERROR'}, "Choose the game's remaster folder")
            return {'CANCELLED'}
        try:
            config = read_config(os.path.join(remaster, "config.yaml"))
        except (OSError, ValueError) as error:
            self.report({'ERROR'}, "Cannot read the game's config: {}".format(error))
            return {'CANCELLED'}
        out = os.path.join(remaster, "flight", "environments", "hangars", key)
        os.makedirs(out, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="aeron_probe_") as scratch:
            panorama = os.path.join(scratch, "panorama.hdr")
            render_panorama(context.scene, probe, config, panorama)
            completed = subprocess.run(
                [baker, "--input", panorama, "--kind", "lighting", "--name", package,
                 "--diffuse-out", os.path.join(out, "diffuse.ktx2"),
                 "--specular-out", os.path.join(out, "specular.ktx2"),
                 "--package-out", os.path.join(out, "package.yaml"), "--manifest-root", remaster],
                capture_output=True, text=True)
        if completed.returncode != 0:
            self.report({'ERROR'}, "environment_bake failed: " + (completed.stderr.strip().splitlines() or ["?"])[-1])
            return {'CANCELLED'}
        write_mapping(os.path.join(out, "mapping.yaml"), key, package)
        self.report({'INFO'}, "Baked {}. Merge its package.yaml and mapping.yaml into the manifest".format(out))
        return {'FINISHED'}


class OBJECT_PT_aeron_probe(ui._RolePanel, Panel):
    bl_label = "Hangar Lighting"
    role = 'environmentProbe'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(context.object.aeron_probe, "samples")
        layout.label(text="Light levels: the game's remaster/config.yaml")
        layout.operator("aeron.bake_hangar_lighting", icon='RENDER_STILL')


classes = (AeronProbeProperties, AERON_OT_add_probe, AERON_OT_bake_hangar_lighting, OBJECT_PT_aeron_probe)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Object.aeron_probe = bpy.props.PointerProperty(type=AeronProbeProperties)


def unregister():
    del bpy.types.Object.aeron_probe
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
