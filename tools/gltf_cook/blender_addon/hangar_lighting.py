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
# The panorama is the scene as Blender renders it, with a black world. The
# package's intensity sets its level in the game.

import math
import os
import re
import subprocess

import bpy
import mathutils
from bpy.props import IntProperty, StringProperty
from bpy.types import Operator, Panel, PropertyGroup

from . import common, libraries, preferences, ui

PANORAMA_SIZE = (2048, 1024)  # pi x 512 px specular faces, rounded up
CUBE_ROTATION = mathutils.Euler((math.pi / 2.0, 0.0, math.pi))
# A panorama camera with this rotation (looking toward +X, +Z up) renders
# what a World's environment texture shows with no mapping.
WORLD_CAMERA = mathutils.Matrix(((0.0, 0.0, -1.0), (-1.0, 0.0, 0.0), (0.0, 1.0, 0.0)))
PREVIEW_TAG = "aeron_lighting_preview"
PACKAGE_NAME = re.compile(r"^[a-z0-9._]{1,63}$")
# The game parks the ship and puts the hangar floor at this hardpoint.
INSIDE_HANGAR_HARDPOINT = 25
PROBE_HEIGHT = 3.0  # metres above it


class AeronProbeProperties(PropertyGroup):
    samples: IntProperty(name="Samples", default=256, min=16, max=4096,
                         description="Cycles samples per pixel of the panorama. It is denoised")
    panorama: StringProperty(subtype='FILE_PATH', options={'HIDDEN'})  # written by the last bake


def probe_of(root):
    return next((child for child in root.children if common.role(child) == 'environmentProbe'), None)


def default_location(root):
    """PROBE_HEIGHT above the model's InsideHangar hardpoint, or None."""
    for component in common.components(root):
        for child in common.ordered_children(component, ('hardpoint',)):
            if int(child.aeron_flight.hardpoint_type) == INSIDE_HANGAR_HARDPOINT:
                up = root.matrix_world.to_quaternion() @ mathutils.Vector((0.0, 0.0, PROBE_HEIGHT))
                return child.matrix_world.translation + up
    return None


def _display_size(root):
    """1% of the model's largest dimension, at least 1 m, so that the probe shows at hangar scale."""
    corners = [component.matrix_world @ mathutils.Vector(corner)
               for component in common.components(root) for corner in component.bound_box]
    if not corners:
        return 1.0
    size = max(max(c[axis] for c in corners) - min(c[axis] for c in corners) for axis in range(3))
    return max(1.0, 0.01 * size)


def _new_probe(root, location):
    probe = bpy.data.objects.new(root.name + " Probe", None)
    probe.empty_display_type = 'SPHERE'
    probe.empty_display_size = _display_size(root)
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

def _camera_rotation(probe):
    """The rotation of the panorama camera: the cube frame in the hangar model's frame."""
    return (probe.parent.matrix_world.to_quaternion() @ CUBE_ROTATION.to_quaternion()).to_matrix()


class _BakeScene:
    """Sets the scene up for the panorama, and puts everything back after."""

    def __init__(self, scene, probe):
        self.scene = scene
        self.probe = probe
        self.saved = []
        self.added = []

    def _set(self, owner, name, value):
        self.saved.append((owner, name, getattr(owner, name)))
        setattr(owner, name, value)

    def __enter__(self):
        scene, settings = self.scene, self.probe.aeron_probe
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
                               _camera_rotation(self.probe).to_4x4())
        scene.collection.objects.link(camera)
        self.added += [camera, camera_data]
        self._set(scene, "camera", camera)
        return self

    def __exit__(self, *exc):
        for owner, name, value in reversed(self.saved):
            setattr(owner, name, value)
        for block in self.added:
            if isinstance(block, bpy.types.Object):
                bpy.data.objects.remove(block)
            elif isinstance(block, bpy.types.Camera):
                bpy.data.cameras.remove(block)
            elif isinstance(block, bpy.types.World):
                bpy.data.worlds.remove(block)
        return False


def render_panorama(scene, probe, path):
    """Renders the panorama of the hangar from `probe` to a Radiance file at `path`."""
    with _BakeScene(scene, probe):
        bpy.ops.render.render(scene=scene.name)
        bpy.data.images["Render Result"].save_render(path, scene=scene)


def write_mapping(path, key, package):
    with open(path, "w") as stream:
        stream.write("# Mapping for the hangar model: merge under the manifest's xwa section.\n"
                     "xwa:\n  hangars:\n    {}: {}\n".format(key, package))


class AERON_OT_add_probe(Operator):
    """Add the environment probe of the hangar lighting bake, where the game parks the ship (above the \
InsideHangar hardpoint), or else at the 3D cursor"""

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
            location = default_location(root)
            if location is None:
                self.report({'INFO'}, "No InsideHangar hardpoint: the probe is at the 3D cursor")
            probe = _new_probe(root, location if location is not None else context.scene.cursor.location)
        else:
            self.report({'INFO'}, "{} already has a probe".format(root.name))
        for obj in context.view_layer.objects:
            obj.select_set(False)
        probe.select_set(True)
        context.view_layer.objects.active = probe
        for area in (context.screen.areas if context.screen else ()):
            if area.type == 'VIEW_3D':
                region = next(region for region in area.regions if region.type == 'WINDOW')
                with context.temp_override(area=area, region=region):
                    bpy.ops.view3d.view_selected()
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
        self.directory = preferences.game_dir(context)
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
        remaster = bpy.path.abspath(self.directory or preferences.game_dir(context))
        if not remaster or not os.path.isdir(remaster):
            self.report({'ERROR'}, "Choose the game's remaster folder")
            return {'CANCELLED'}
        out = os.path.join(remaster, "flight", "environments", "hangars", key)
        os.makedirs(out, exist_ok=True)
        panorama = os.path.join(out, "panorama.hdr")
        render_panorama(context.scene, probe, panorama)
        probe.aeron_probe.panorama = panorama
        for image in bpy.data.images:  # a preview shows the new panorama
            if image.filepath and os.path.normpath(bpy.path.abspath(image.filepath)) == os.path.normpath(panorama):
                image.reload()
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


def _preview_world(scene):
    return scene.world if scene.world is not None and scene.world.get(PREVIEW_TAG) else None


class AERON_OT_preview_hangar_lighting(Operator):
    """Show the last baked panorama as the World, turned as in the game, to see its light and reflections on \
models in Material Preview or Rendered view. Click again to put the previous World back"""

    bl_idname = "aeron.preview_hangar_lighting"
    bl_label = "Preview Lighting"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return common.role(context.object) == 'environmentProbe' and common.model_root(context.object) is not None

    def execute(self, context):
        scene = context.scene
        world = _preview_world(scene)
        if world is not None:
            scene.world = world.get("previous")
            bpy.data.worlds.remove(world)
            return {'FINISHED'}
        probe = context.object
        path = bpy.path.abspath(probe.aeron_probe.panorama)
        if not probe.aeron_probe.panorama or not os.path.isfile(path):
            self.report({'ERROR'}, "Bake the hangar lighting first")
            return {'CANCELLED'}
        world = bpy.data.worlds.new("Aeron Lighting Preview")
        world[PREVIEW_TAG] = True
        if scene.world is not None:
            world["previous"] = scene.world
        tree = world.node_tree
        tree.nodes.clear()
        coordinates = tree.nodes.new('ShaderNodeTexCoord')
        mapping = tree.nodes.new('ShaderNodeMapping')
        mapping.vector_type = 'VECTOR'
        # World direction -> the panorama camera's frame -> the texture's.
        mapping.inputs["Rotation"].default_value = (WORLD_CAMERA @ _camera_rotation(probe).inverted()).to_euler()
        texture = tree.nodes.new('ShaderNodeTexEnvironment')
        texture.image = bpy.data.images.load(path, check_existing=True)
        background = tree.nodes.new('ShaderNodeBackground')
        output = tree.nodes.new('ShaderNodeOutputWorld')
        tree.links.new(coordinates.outputs["Generated"], mapping.inputs["Vector"])
        tree.links.new(mapping.outputs["Vector"], texture.inputs["Vector"])
        tree.links.new(texture.outputs["Color"], background.inputs["Color"])
        tree.links.new(background.outputs["Background"], output.inputs["Surface"])
        scene.world = world
        return {'FINISHED'}


class OBJECT_PT_aeron_probe(ui._RolePanel, Panel):
    bl_label = "Hangar Lighting"
    role = 'environmentProbe'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(context.object.aeron_probe, "samples")
        layout.operator("aeron.bake_hangar_lighting", icon='RENDER_STILL')
        layout.operator("aeron.preview_hangar_lighting", icon='WORLD', depress=_preview_world(context.scene) is not None)


classes = (AeronProbeProperties, AERON_OT_add_probe, AERON_OT_bake_hangar_lighting, AERON_OT_preview_hangar_lighting,
           OBJECT_PT_aeron_probe)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Object.aeron_probe = bpy.props.PointerProperty(type=AeronProbeProperties)


def unregister():
    del bpy.types.Object.aeron_probe
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
