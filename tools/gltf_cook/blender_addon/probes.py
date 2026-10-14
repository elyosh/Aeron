# Environment probes: a model's reflection probes (Blender Sphere light
# probes) and its light grid (a Volume light probe) under its root, and their
# bake into the model's probes package. The bake renders a panorama from
# each probe and from each grid point with Cycles, and environment_bake
# turns them into a cube array and an irradiance grid. In the game, the
# package lights the model and the objects that reach it (a hangar's ships,
# a capital ship's fighters in its bays), blended with the scene's
# environment where it does not apply.
#
# The model frame is the game's: from Blender, (-x, y, z) relative to the
# root, in metres. The cube frame (+X right, +Y up, -Z forward) is fixed in
# it: in Blender's coordinates, cube = (-x, z, y). A camera's local axes
# match the cube's when it looks toward -Y, with +Z up and its right toward
# -X.
#
# Panoramas are the scene as Blender renders it, with a black World. The
# package's intensity sets its level in the game.

import math
import os
import re
import shutil
import subprocess
import tempfile
import time

import bpy
import mathutils
from bpy.props import BoolProperty, FloatProperty, IntProperty, StringProperty
from bpy.types import Operator, Panel, PropertyGroup

from . import common, libraries, preferences

PROBE_PANORAMA_SIZE = (1024, 512)  # pi x 256 px faces, rounded up
# A grid point keeps only four SH coefficients, so its noise depends on the
# light paths it traces (pixels x samples), not on its resolution. A GPU
# renders many pixels at a few samples much faster than few pixels at many
# samples: 256 x 128 at 16 samples is as accurate as 64 x 32 at 256, 2.6x
# faster.
GRID_PANORAMA_SIZE = (256, 128)
GRID_SAMPLES = 16
CUBE_ROTATION = mathutils.Euler((math.pi / 2.0, 0.0, math.pi))
# A panorama camera with this rotation (looking toward +X, +Z up) renders
# what a World's environment texture shows with no mapping.
WORLD_CAMERA = mathutils.Matrix(((0.0, 0.0, -1.0), (-1.0, 0.0, 0.0), (0.0, 1.0, 0.0)))
PREVIEW_TAG = "aeron_lighting_preview"
# The game reads package.yaml from the model's folder; only a first bake needs the manifest.
BAKED_MESSAGE = "Baked {}. After a model's first bake, add the line in its mapping.yaml to the manifest's xwa.probes"
PACKAGE_NAME = re.compile(r"^[a-z0-9._]{1,63}$")
MAX_PROBES = 42  # one cube array within 256 array layers
MAX_GRID_POINTS = 8192  # about 7 min of bake on a GPU
GRID_RESOLUTION = (2, 64)
GRID_SPACING = 4.0  # default metres between grid points
KEY_LIGHT_SHADOW = 0.7  # the runtime's default
FILL_TILE = 25.0  # Fill with Probes' target probe size, metres
FILL_MAX_PROBES = 16
FILL_FALLOFF = 0.2
OVERLAP_LIMIT = 4  # probes a pixel uses
VALIDITY_RAYS = 64
INVALID_BACK_FACES = 0.3
# The game parks the ship and puts the hangar floor at this hardpoint.
INSIDE_HANGAR_HARDPOINT = 25
PROBE_HEIGHT = 3.0  # metres above it
PROBE_SIZE = 10.0  # Add Reflection Probe's box
HANGAR_ZONE = (60.0, 60.0, 20.0)  # Add Light Grid's box above an InsideHangar hardpoint
BAY_ZONE = (50.0, 50.0, 50.0)  # else at the 3D cursor
# Bake estimate, seconds (measured on an Apple M4 Pro, Blender 5.1).
SECONDS_PER_PROBE = 3.5
SECONDS_PER_POINT = {'GPU': 0.05, 'CPU': 0.13}


def _spacing_update(self, context):
    grid = self.id_data
    if grid.type == 'LIGHT_PROBE' and grid.data.type == 'VOLUME':
        sync_grid(grid)


class AeronProbeProperties(PropertyGroup):
    panorama: StringProperty(subtype='FILE_PATH', options={'HIDDEN'})  # written by the last bake
    room_parallax: BoolProperty(
        name="Room Parallax", default=False,
        description="Project the probe's reflections on the light grid's box (the room) instead of its own "
                    "parallax volume, so that the probes of a room show its walls in place")
    filled: BoolProperty(
        name="Filled", default=False,
        description="Made by Fill with Probes, which replaces it when run again. Uncheck to keep it")
    grid_spacing: FloatProperty(
        name="Spacing", default=GRID_SPACING, min=0.25, max=1000.0, subtype='DISTANCE', update=_spacing_update,
        description="About how far apart the light grid's points are. The points per axis follow from the "
                    "box's size, within {} points".format(8192))
    key_light_shadow: FloatProperty(
        name="Key Light Shadow", default=KEY_LIGHT_SHADOW, min=0.0, max=1.0, subtype='FACTOR',
        description="When the scene's key light is in the bake (a hangar's ceiling light): the part of the "
                    "probe light its shadow removes. The bake writes it to the package")


# ---- the model's probes ------------------------------------------------------------

def reflection_probes(root):
    return sorted((c for c in root.children if c.type == 'LIGHT_PROBE' and c.data.type == 'SPHERE'),
                  key=lambda obj: obj.name)


def light_grids(root):
    return sorted((c for c in root.children if c.type == 'LIGHT_PROBE' and c.data.type == 'VOLUME'),
                  key=lambda obj: obj.name)


def light_grid(root):
    grids = light_grids(root)
    return grids[0] if grids else None


def _relative(root, obj):
    return root.matrix_world.inverted() @ obj.matrix_world


def _model_point(root, world):
    """A world point in the model frame: (-x, y, z) relative to the root."""
    p = root.matrix_world.inverted() @ world
    return (-p.x, p.y, p.z)


def _shape(influence_type):
    return 'box' if influence_type == 'BOX' else 'ellipsoid'


def probe_definition(root, probe):
    """A reflection probe in the package's terms: model frame, metres, full sizes."""
    data = probe.data
    scale = _relative(root, probe).to_scale()
    size = [2.0 * data.influence_distance * abs(s) for s in scale]
    center = list(_model_point(root, probe.matrix_world.translation))
    grid = light_grid(root)
    if probe.aeron_probe.room_parallax and grid is not None:
        room = grid_definition(root, grid)
        parallax_center, parallax, parallax_shape = room["center"], room["size"], 'box'
    elif data.use_custom_parallax:
        parallax_center = center
        parallax = [2.0 * data.parallax_distance * abs(s) for s in scale]
        parallax_shape = _shape(data.parallax_type)
    else:
        parallax_center, parallax, parallax_shape = center, list(size), _shape(data.influence_type)
    return {"center": center, "size": size, "shape": _shape(data.influence_type), "falloff": data.falloff,
            "parallax_center": parallax_center, "parallax_size": parallax, "parallax_shape": parallax_shape}


def _model_diagonal(root):
    """The length of the diagonal of the model's meshes' bounding box, in Blender units."""
    corners = [obj.matrix_world @ mathutils.Vector(c) for obj in root.children_recursive if obj.type == 'MESH'
               for c in obj.bound_box]
    if not corners:
        return 0.0
    return (mathutils.Vector([max(c[a] for c in corners) for a in range(3)]) -
            mathutils.Vector([min(c[a] for c in corners) for a in range(3)])).length


def parallax_reach(root, probe):
    """How far the probe's parallax volume reaches from the probe (to the far corner of its box), in
    Blender units. The probe's render must reach that far: what lies beyond its Clip End is black in its
    reflections."""
    item = probe_definition(root, probe)
    far = mathutils.Vector([abs(p - c) + s / 2.0 for c, p, s in
                            zip(item["center"], item["parallax_center"], item["parallax_size"])])
    return far.length * max(abs(s) for s in root.matrix_world.to_scale())


def _set_clip_end(root, probe):
    """Makes the probe's render reach the whole model and its parallax volume."""
    bpy.context.view_layer.update()  # the probe's matrix
    probe.data.clip_end = max(probe.data.clip_end, _model_diagonal(root), 1.1 * parallax_reach(root, probe))


def _grid_size(grid):
    """The grid box's full size (Blender's -1..1 cube), in the model root's frame."""
    root = common.model_root(grid)
    matrix = _relative(root, grid) if root is not None else grid.matrix_world
    return [2.0 * abs(s) for s in matrix.to_scale()]


def grid_resolution(grid):
    """Points per axis from the grid's spacing: about that far apart (size / (points + 1)), 2 to 64
    per axis, the spacing widened to stay within MAX_GRID_POINTS."""
    size = _grid_size(grid)
    spacing = max(grid.aeron_probe.grid_spacing, 1e-3)
    while True:
        r = [min(GRID_RESOLUTION[1], max(GRID_RESOLUTION[0], int(round(s / spacing)) - 1)) for s in size]
        if r[0] * r[1] * r[2] <= MAX_GRID_POINTS:
            return r
        spacing *= 1.05


def sync_grid(grid):
    """Gives Blender's volume probe the grid's points, so that EEVEE shows them too."""
    grid.data.resolution_x, grid.data.resolution_y, grid.data.resolution_z = grid_resolution(grid)


def grid_definition(root, grid):
    """The light grid in the package's terms: its box (Blender's -1..1 cube) and resolution."""
    return {"center": list(_model_point(root, grid.matrix_world.translation)), "size": _grid_size(grid),
            "resolution": grid_resolution(grid)}


def grid_points(grid):
    """World positions of the grid's points, as Blender's volume probe places them, in the
    package's order: model x fastest, then y, then z. Model x runs along Blender's -x."""
    r = grid_resolution(grid)
    points = []
    for iz in range(r[2]):
        for iy in range(r[1]):
            for ix in range(r[0]):
                local = mathutils.Vector((2.0 * (r[0] - ix) / (r[0] + 1) - 1.0,
                                          2.0 * (iy + 1) / (r[1] + 1) - 1.0,
                                          2.0 * (iz + 1) / (r[2] + 1) - 1.0))
                points.append(grid.matrix_world @ local)
    return points


def grid_point_count(grid):
    r = grid_resolution(grid)
    return r[0] * r[1] * r[2]


def light_baked(root, light):
    """True for a model light inside the model's light grid box: its light is in the probes."""
    grid = light_grid(root)
    if grid is None:
        return False
    p = grid.matrix_world.inverted() @ light.matrix_world.translation
    return all(abs(c) <= 1.0 for c in p)


def bake_seconds(root, device):
    grid = light_grid(root)
    points = grid_point_count(grid) if grid is not None else 0
    return len(reflection_probes(root)) * SECONDS_PER_PROBE + points * SECONDS_PER_POINT[device]


def _gpu_available(context):
    addon = context.preferences.addons.get("cycles")
    return addon is not None and addon.preferences.has_active_device()


# ---- validation ---------------------------------------------------------------------

def _boxes_overlap_count(boxes):
    """The most boxes (min, max) holding one point, tried at each box's centre and at the centre of
    each pair's overlap."""
    candidates = [[(a + b) / 2.0 for a, b in zip(lo, hi)] for lo, hi in boxes]
    for i, (lo_a, hi_a) in enumerate(boxes):
        for lo_b, hi_b in boxes[i + 1:]:
            lo = [max(a, b) for a, b in zip(lo_a, lo_b)]
            hi = [min(a, b) for a, b in zip(hi_a, hi_b)]
            if all(a < b for a, b in zip(lo, hi)):
                candidates.append([(a + b) / 2.0 for a, b in zip(lo, hi)])
    return max((sum(1 for lo, hi in boxes if all(l <= c <= h for l, c, h in zip(lo, p, hi)))
                for p in candidates), default=0)


def check(problems, root):
    probes, grids = reflection_probes(root), light_grids(root)
    for obj in root.children:
        if obj.type == 'LIGHT_PROBE' and obj.data.type == 'PLANE':
            problems.warning(obj, "plane probes are not exported; use sphere probes")
    if len(probes) > MAX_PROBES:
        problems.error(root, "the model has {} reflection probes, more than the limit of {}".format(
            len(probes), MAX_PROBES))
    for grid in grids[1:]:
        problems.error(grid, "the model has another light grid")
    if probes and not grids:
        problems.error(probes[0], "the reflection probes need a light grid (Add Light Grid)")
    if grids and not probes:
        problems.error(grids[0], "the light grid needs at least one reflection probe")
    for obj in probes + grids:
        relative = _relative(root, obj)
        if relative.to_quaternion().angle > 1e-4:
            problems.error(obj, "a probe cannot be rotated relative to the model root")
        if min(abs(s) for s in relative.to_scale()) <= 0.0:
            problems.error(obj, "a probe needs a positive scale")
    for probe in probes:
        if probe.data.influence_distance <= 0.0 or (probe.data.use_custom_parallax and
                                                    probe.data.parallax_distance <= 0.0):
            problems.error(probe, "a reflection probe needs a positive size")
    for probe in probes:
        if probe.aeron_probe.room_parallax and not grids:
            problems.error(probe, "Room Parallax uses the light grid's box: add a light grid")
        elif probe.data.clip_end < parallax_reach(root, probe):
            problems.warning(probe, "its Clip End ({:.0f} m) does not reach the far side of its parallax volume "
                             "({:.0f} m): what lies beyond is black in its reflections".format(
                                 probe.data.clip_end, parallax_reach(root, probe)))
    boxes = []
    for probe in probes:
        item = probe_definition(root, probe)
        boxes.append(([c - s / 2.0 for c, s in zip(item["center"], item["size"])],
                      [c + s / 2.0 for c, s in zip(item["center"], item["size"])]))
    overlap = _boxes_overlap_count(boxes)
    if overlap > OVERLAP_LIMIT:
        problems.warning(root, "{} reflection probes overlap somewhere; the game uses the {} smallest there".format(
            overlap, OVERLAP_LIMIT))


# ---- import and export ----------------------------------------------------------------

def _link(root, obj):
    for collection in root.users_collection:
        collection.objects.link(obj)
    obj.parent = root
    obj.lock_rotation = (True, True, True)


def _new_reflection_probe(root, location, size):
    """A box probe at a world `location`, of full `size` (Blender axes, metres in the root's frame)."""
    data = bpy.data.lightprobes.new(root.name + " Probe", 'SPHERE')
    data.influence_type = 'BOX'
    data.influence_distance = 1.0
    probe = bpy.data.objects.new(data.name, data)
    _link(root, probe)
    probe.matrix_world = root.matrix_world @ mathutils.Matrix.Translation(
        root.matrix_world.inverted() @ location)
    probe.scale = [s / 2.0 for s in size]
    return probe


def _new_light_grid(root, location, size, spacing):
    data = bpy.data.lightprobes.new(root.name + " Light Grid", 'VOLUME')
    grid = bpy.data.objects.new(data.name, data)
    _link(root, grid)
    grid.matrix_world = root.matrix_world @ mathutils.Matrix.Translation(
        root.matrix_world.inverted() @ location)
    grid.scale = [s / 2.0 for s in size]
    bpy.context.view_layer.update()  # its matrix, for its points
    grid.aeron_probe.grid_spacing = spacing
    sync_grid(grid)
    return grid


def restore(root, extension):
    """Creates the probes and light grid of the model root's extension. An old `environmentProbe`
    becomes one reflection probe at its position."""
    old = extension.get("environmentProbe")
    if isinstance(old, dict):
        position = root.matrix_world @ mathutils.Vector(common.from_gltf(old.get("position", (0, 0, 0))))
        _new_reflection_probe(root, position, (PROBE_SIZE,) * 3)
    for item in extension.get("reflectionProbes", []):
        size = common.extent_from_gltf(item["size"])
        probe = _new_reflection_probe(root, root.matrix_world @ mathutils.Vector(common.from_gltf(item["position"])),
                                      size)
        data = probe.data
        data.influence_type = 'BOX' if item.get("shape", "box") == "box" else 'ELIPSOID'
        data.falloff = item.get("falloff", data.falloff)
        if "parallaxSize" in item:
            data.use_custom_parallax = True
            data.parallax_type = 'BOX' if item.get("parallaxShape", "box") == "box" else 'ELIPSOID'
            data.parallax_distance = item["parallaxSize"][0] / item["size"][0]
        data.clip_start = item.get("clipStart", data.clip_start)
        data.clip_end = item.get("clipEnd", data.clip_end)
        probe.aeron_probe.room_parallax = bool(item.get("roomParallax", False))
        probe.aeron_probe.filled = bool(item.get("filled", False))
    item = extension.get("lightGrid")
    if isinstance(item, dict):
        grid = _new_light_grid(root, root.matrix_world @ mathutils.Vector(common.from_gltf(item["position"])),
                               common.extent_from_gltf(item["size"]), item.get("spacing", GRID_SPACING))
        grid.aeron_probe.key_light_shadow = item.get("keyLightShadow", KEY_LIGHT_SHADOW)


def export_json(root):
    """The root extension's `reflectionProbes` and `lightGrid`, in glTF axes and metres."""
    out = {}
    probes = []
    for probe in reflection_probes(root):
        relative = _relative(root, probe)
        scale = relative.to_scale()
        data = probe.data
        size = [2.0 * data.influence_distance * abs(s) for s in scale]
        item = {"position": common.to_gltf(relative.translation), "size": common.extent_to_gltf(size),
                "shape": _shape(data.influence_type), "falloff": data.falloff,
                "clipStart": data.clip_start, "clipEnd": data.clip_end}
        if data.use_custom_parallax:
            item["parallaxSize"] = common.extent_to_gltf([2.0 * data.parallax_distance * abs(s) for s in scale])
            item["parallaxShape"] = _shape(data.parallax_type)
        if probe.aeron_probe.room_parallax:
            item["roomParallax"] = True
        if probe.aeron_probe.filled:
            item["filled"] = True
        probes.append(item)
    if probes:
        out["reflectionProbes"] = probes
    grid = light_grid(root)
    if grid is not None:
        relative = _relative(root, grid)
        r = grid_resolution(grid)
        out["lightGrid"] = {"position": common.to_gltf(relative.translation),
                            "size": common.extent_to_gltf([2.0 * abs(s) for s in relative.to_scale()]),
                            "spacing": grid.aeron_probe.grid_spacing, "resolution": [r[0], r[2], r[1]],
                            "keyLightShadow": grid.aeron_probe.key_light_shadow}
    return out


# ---- bake ------------------------------------------------------------------------------

class BakeError(Exception):
    pass


def _camera_rotation(root):
    """The rotation of the panorama cameras: the cube frame in the model's frame."""
    return (root.matrix_world.to_quaternion() @ CUBE_ROTATION.to_quaternion()).to_matrix()


class _BakeScene:
    """Sets the scene up for the panoramas, and puts everything back after."""

    def __init__(self, context, root):
        self.context = context
        self.scene = context.scene
        self.root = root
        self.saved = []
        self.added = []
        self.hidden = []

    def _set(self, owner, name, value):
        self.saved.append((owner, name, getattr(owner, name)))
        setattr(owner, name, value)

    def __enter__(self):
        scene = self.scene
        render = scene.render
        self._set(render, "engine", 'CYCLES')
        self._set(render, "resolution_percentage", 100)
        self._set(render, "film_transparent", False)
        self._set(render, "use_border", False)
        # Keeping the scene between renders: about 8x faster per grid point on a GPU.
        self._set(render, "use_persistent_data", True)
        self._set(scene.cycles, "device", 'GPU' if _gpu_available(self.context) else 'CPU')
        self._set(scene.view_settings, "view_transform", 'Standard')
        self._set(scene.view_settings, "look", 'None')
        self._set(scene.view_settings, "exposure", 0.0)
        self._set(scene.view_settings, "gamma", 1.0)
        self._set(render.image_settings, "file_format", 'HDR')

        # Nothing comes from outside the model.
        world = bpy.data.worlds.new("Aeron Probe World")
        world.color = (0.0, 0.0, 0.0)
        for node in world.node_tree.nodes if world.node_tree else ():
            if node.type == 'BACKGROUND':
                node.inputs["Strength"].default_value = 0.0
        self.added.append(world)
        self._set(scene, "world", world)
        # Cycles' camera rays do not see lights by default: the panoramas
        # would miss the light they give directly, which the grid holds.
        for obj in scene.objects:
            if obj.type == 'LIGHT':
                self._set(obj, "visible_camera", True)

        camera_data = bpy.data.cameras.new("Aeron Probe Camera")
        camera_data.type = 'PANO'
        camera_data.panorama_type = 'EQUIRECTANGULAR'
        camera = bpy.data.objects.new("Aeron Probe Camera", camera_data)
        scene.collection.objects.link(camera)
        self.added += [camera, camera_data]
        self.camera = camera
        self._set(scene, "camera", camera)
        return self

    def hide(self, flag):
        """Leaves out of the renders the objects whose `flag` (hide_probe_sphere or hide_probe_volume) is set."""
        for obj in self.hidden:
            obj.hide_render = False
        self.hidden = [obj for obj in self.scene.objects if getattr(obj, flag, False) and not obj.hide_render]
        for obj in self.hidden:
            obj.hide_render = True

    def render(self, path, location, size, samples, denoise, clip=(0.01, 100000.0)):
        render, scene = self.scene.render, self.scene
        render.resolution_x, render.resolution_y = size
        scene.cycles.samples = samples
        scene.cycles.use_denoising = denoise
        self.camera.data.clip_start, self.camera.data.clip_end = clip
        self.camera.matrix_world = (mathutils.Matrix.Translation(location) @
                                    _camera_rotation(self.root).to_4x4())
        bpy.ops.render.render(scene=scene.name)
        bpy.data.images["Render Result"].save_render(path, scene=scene)

    def __exit__(self, *exc):
        for obj in self.hidden:
            obj.hide_render = False
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


def _validity_directions():
    """VALIDITY_RAYS directions spread over the sphere (a Fibonacci lattice)."""
    golden = math.pi * (3.0 - math.sqrt(5.0))
    out = []
    for i in range(VALIDITY_RAYS):
        z = 1.0 - 2.0 * (i + 0.5) / VALIDITY_RAYS
        r = math.sqrt(max(0.0, 1.0 - z * z))
        out.append(mathutils.Vector((r * math.cos(golden * i), r * math.sin(golden * i), z)))
    return out


def invalid_points(context, points):
    """Indices of the points inside geometry: more than INVALID_BACK_FACES of their rays first hit a back
    face of the meshes the grid's renders show."""
    from mathutils.bvhtree import BVHTree
    depsgraph = context.evaluated_depsgraph_get()
    vertices, polygons = [], []
    for obj in context.scene.objects:
        if obj.type != 'MESH' or obj.hide_render or obj.hide_probe_volume:
            continue
        evaluated = obj.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh()
        base = len(vertices)
        vertices += [obj.matrix_world @ v.co for v in mesh.vertices]
        polygons += [[base + i for i in polygon.vertices] for polygon in mesh.polygons]
        evaluated.to_mesh_clear()
    if not polygons:
        return []
    tree = BVHTree.FromPolygons(vertices, polygons)
    directions = _validity_directions()
    out = []
    for index, point in enumerate(points):
        back = 0
        for direction in directions:
            location, normal, _, _ = tree.ray_cast(point, direction)
            if location is not None and normal.dot(direction) > 0.0:
                back += 1
        if back > INVALID_BACK_FACES * len(directions):
            out.append(index)
    return out


def _vec(v):
    return "[{}]".format(", ".join("{:.9g}".format(c) for c in v))


def write_job(path, root, probes, grid, probe_names, grid_names, invalid):
    """environment_bake's probes job: the package's probe and grid fields, and the panoramas."""
    with open(path, "w") as stream:
        stream.write("probes:\n")
        for probe in probes:
            item = probe_definition(root, probe)
            stream.write("  - {{center: {}, size: {}, shape: {}, falloff: {:.9g}, parallax_center: {}, "
                         "parallax_size: {}, parallax_shape: {}}}\n".format(
                             _vec(item["center"]), _vec(item["size"]), item["shape"], item["falloff"],
                             _vec(item["parallax_center"]), _vec(item["parallax_size"]), item["parallax_shape"]))
        stream.write("probe_panoramas: [{}]\n".format(", ".join(probe_names)))
        item = grid_definition(root, grid)
        stream.write("grid_box: {{center: {}, size: {}}}\ngrid_resolution: [{}]\n".format(
            _vec(item["center"]), _vec(item["size"]), ", ".join(str(r) for r in item["resolution"])))
        stream.write("grid_panoramas: [{}]\n".format(", ".join(grid_names)))
        stream.write("grid_invalid: [{}]\n".format(", ".join(str(i) for i in invalid)))
        stream.write("key_light_shadow: {:.9g}\n".format(grid.aeron_probe.key_light_shadow))


def write_mapping(path, key, package):
    with open(path, "w") as stream:
        stream.write("# For the model's first bake: add this under the manifest's xwa section.\n"
                     "xwa:\n  probes:\n    {}: {}\n".format(key, package))


def bake_steps(context, root, remaster, result):
    """Bakes the model's probes package, one render per step, and sets result["out"] to its folder.
    Yields (done, total); raises BakeError."""
    if not root.aeron_flight.source_path:
        raise BakeError("Save the model to its project first: its folder name is its key")
    key = libraries.model_key(root).lower()
    package = "probes." + key
    if not PACKAGE_NAME.match(package):
        raise BakeError("The model key {!r} must be lower-case letters, digits, '.' and '_'".format(key))
    if bpy.data.colorspace.working_space != 'Linear Rec.709':
        raise BakeError("The bake needs the Linear Rec.709 working color space")
    baker = preferences.environment_bake_path(context)
    if not baker:
        raise BakeError("Set the environment_bake path in the add-on preferences")
    if not remaster or not os.path.isdir(remaster):
        raise BakeError("Choose the game's remaster folder")
    from . import validation
    problems = [message for obj, message, warning in validation.validate(root).items
                if not warning and (obj is None or obj == root or obj.type == 'LIGHT_PROBE')]
    probes, grid = reflection_probes(root), light_grid(root)
    if problems or not probes or grid is None:
        raise BakeError(problems[0] if problems else "The model needs reflection probes and a light grid")

    sync_grid(grid)
    work = tempfile.mkdtemp(prefix="aeron_probes_")
    try:
        points = grid_points(grid)
        with _BakeScene(context, root) as bake:
            total = len(probes) + len(points)
            done = 0
            bake.hide("hide_probe_sphere")
            samples = context.scene.aeron_probe_samples
            for k, probe in enumerate(probes):
                bake.render(os.path.join(work, "probe_{}.hdr".format(k)), probe.matrix_world.translation,
                            PROBE_PANORAMA_SIZE, samples, True, (probe.data.clip_start, probe.data.clip_end))
                done += 1
                yield done, total
            bake.hide("hide_probe_volume")
            invalid = set(invalid_points(context, points))
            for index, point in enumerate(points):
                if index not in invalid:
                    bake.render(os.path.join(work, "grid_{}.hdr".format(index)), point, GRID_PANORAMA_SIZE,
                                GRID_SAMPLES, False)
                done += 1
                yield done, total
        # Invalid points take their neighbours' light: the baker reads no panorama for them.
        grid_names = ["grid_{}.hdr".format(i) if i not in invalid else "none.hdr" for i in range(len(points))]
        job = os.path.join(work, "job.yaml")
        write_job(job, root, probes, grid, ["probe_{}.hdr".format(k) for k in range(len(probes))], grid_names,
                  sorted(invalid))
        out = os.path.join(remaster, "flight", "environments", "models", key)
        os.makedirs(out, exist_ok=True)
        completed = subprocess.run(
            [baker, "--probes", job, "--name", package, "--cubes-out", os.path.join(out, "probes.ktx2"),
             "--grid-out", os.path.join(out, "grid.ktx2"), "--package-out", os.path.join(out, "package.yaml"),
             "--manifest-root", remaster], capture_output=True, text=True)
        if completed.returncode != 0:
            raise BakeError("environment_bake failed: " + (completed.stderr.strip().splitlines() or ["?"])[-1])
        for k, probe in enumerate(probes):
            panorama = os.path.join(out, "panorama_{}.hdr".format(k))
            shutil.copyfile(os.path.join(work, "probe_{}.hdr".format(k)), panorama)
            probe.aeron_probe.panorama = panorama
            for image in bpy.data.images:  # a preview shows the new panorama
                if image.filepath and os.path.normpath(bpy.path.abspath(image.filepath)) == os.path.normpath(panorama):
                    image.reload()
        write_mapping(os.path.join(out, "mapping.yaml"), key, package)
        result["out"] = out
    finally:
        shutil.rmtree(work, ignore_errors=True)


class AERON_OT_add_reflection_probe(Operator):
    """Add a reflection probe to the model: a 10 m box, above the InsideHangar hardpoint for the model's \
first probe, else at the 3D cursor. Its Clip End reaches the whole model"""

    bl_idname = "aeron.add_reflection_probe"
    bl_label = "Add Reflection Probe"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return common.model_root(context.active_object) is not None

    def execute(self, context):
        root = common.model_root(context.active_object)
        location = default_location(root) if not reflection_probes(root) else None
        probe = _new_reflection_probe(root, location if location is not None else context.scene.cursor.location,
                                      (PROBE_SIZE,) * 3)
        _set_clip_end(root, probe)
        _select(context, probe)
        return {'FINISHED'}


def default_location(root):
    """PROBE_HEIGHT above the model's InsideHangar hardpoint, or None."""
    for component in common.components(root):
        for child in common.ordered_children(component, ('hardpoint',)):
            if int(child.aeron_flight.hardpoint_type) == INSIDE_HANGAR_HARDPOINT:
                up = root.matrix_world.to_quaternion() @ mathutils.Vector((0.0, 0.0, PROBE_HEIGHT))
                return child.matrix_world.translation + up
    return None


def zone_tiles(size):
    """Fill with Probes' tiling of a zone of full `size` (model axes): about FILL_TILE wide across its
    floor plan, the zone's height, at most FILL_MAX_PROBES. Returns the tile counts along x and y."""
    tile = FILL_TILE
    while True:
        n = [max(1, int(round(size[axis] / tile))) for axis in (0, 1)]
        if n[0] * n[1] <= FILL_MAX_PROBES:
            return n
        tile *= 1.1


class AERON_OT_add_light_grid(Operator):
    """Add the model's light grid: a box around the zone its points light (and its probes fill), above the InsideHangar hardpoint, else at the 3D cursor. Size it to the zone: its points follow from its Spacing"""

    bl_idname = "aeron.add_light_grid"
    bl_label = "Add Light Grid"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        root = common.model_root(context.active_object)
        return root is not None and light_grid(root) is None

    def execute(self, context):
        root = common.model_root(context.active_object)
        floor = default_location(root)
        if floor is not None:
            # From the floor (PROBE_HEIGHT below the probes' default) up.
            up = root.matrix_world.to_quaternion() @ mathutils.Vector((0.0, 0.0, 1.0))
            centre, size = floor + up * (HANGAR_ZONE[2] / 2.0 - PROBE_HEIGHT), HANGAR_ZONE
        else:
            centre, size = context.scene.cursor.location.copy(), BAY_ZONE
        grid = _new_light_grid(root, centre, size, GRID_SPACING)
        _select(context, grid)
        return {'FINISHED'}


class AERON_OT_fill_probes(Operator):
    """Fill the light grid's box with reflection probes: about 25 m wide across its floor plan, the box's height, at most 16, each reflecting onto the box (Room Parallax), their Clip End reaching the whole model and the box. Replaces the probes it made before"""

    bl_idname = "aeron.fill_probes"
    bl_label = "Fill with Probes"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        root = common.model_root(context.object)
        return root is not None and light_grid(root) is not None

    def execute(self, context):
        root = common.model_root(context.object)
        grid = light_grid(root)
        for probe in reflection_probes(root):
            if probe.aeron_probe.filled:
                data = probe.data
                bpy.data.objects.remove(probe)
                if data.users == 0:
                    bpy.data.lightprobes.remove(data)
        relative = _relative(root, grid)
        size = [2.0 * abs(s) for s in relative.to_scale()]
        n = zone_tiles(size)
        tile = (size[0] / n[0], size[1] / n[1], size[2])
        made = []
        for j in range(n[1]):
            for i in range(n[0]):
                local = relative.translation + mathutils.Vector((-size[0] / 2.0 + tile[0] * (i + 0.5),
                                                                 -size[1] / 2.0 + tile[1] * (j + 0.5), 0.0))
                # Full weight over its tile; the falloff blends into its neighbours.
                influence = [t / (1.0 - FILL_FALLOFF) for t in tile]
                probe = _new_reflection_probe(root, root.matrix_world @ local, influence)
                probe.data.falloff = FILL_FALLOFF
                probe.aeron_probe.room_parallax = True
                probe.aeron_probe.filled = True
                made.append(probe)
        for probe in made:
            _set_clip_end(root, probe)
        self.report({'INFO'}, "{} reflection probes fill the light grid's box".format(len(made)))
        return {'FINISHED'}


def _select(context, obj):
    for other in context.view_layer.objects:
        other.select_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj


class AERON_OT_bake_probes(Operator):
    """Render the model's reflection probes and light grid with Cycles and bake its probes package into the \
game's remaster folder. Esc cancels"""

    bl_idname = "aeron.bake_probes"
    bl_label = "Bake Probes"

    directory: StringProperty(name="Remaster Folder", subtype='DIR_PATH',
                              description="The game's remaster folder")

    @classmethod
    def poll(cls, context):
        root = common.model_root(context.object)
        return root is not None and bool(reflection_probes(root)) and light_grid(root) is not None

    def invoke(self, context, event):
        self.directory = preferences.game_dir(context)
        if not self.directory:
            context.window_manager.fileselect_add(self)
            return {'RUNNING_MODAL'}
        return self._start(context)

    def _start(self, context):
        self._result = {}
        self._steps = bake_steps(context, common.model_root(context.object), bpy.path.abspath(self.directory),
                                 self._result)
        self._timer = context.window_manager.event_timer_add(0.01, window=context.window)
        context.window_manager.modal_handler_add(self)
        context.window_manager.progress_begin(0, 1)
        return {'RUNNING_MODAL'}

    def _end(self, context):
        context.window_manager.event_timer_remove(self._timer)
        context.window_manager.progress_end()
        context.workspace.status_text_set(None)

    def modal(self, context, event):
        if event.type == 'ESC':
            self._steps.close()
            self._end(context)
            self.report({'WARNING'}, "Probe bake cancelled")
            return {'CANCELLED'}
        if event.type != 'TIMER':
            return {'PASS_THROUGH'}
        try:
            done, total = next(self._steps)
        except StopIteration:
            self._end(context)
            self.report({'INFO'}, BAKED_MESSAGE.format(
                self._result.get("out", "")))
            return {'FINISHED'}
        except BakeError as error:
            self._end(context)
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        context.window_manager.progress_update(done / total)
        context.workspace.status_text_set("Baking probes: {} of {} renders (Esc to cancel)".format(done, total))
        return {'RUNNING_MODAL'}

    def execute(self, context):
        # Without a window (a script, a headless Blender): all the renders at once.
        if context.window is not None and not bpy.app.background:
            return self._start(context)
        result = {}
        try:
            for _ in bake_steps(context, common.model_root(context.object),
                                bpy.path.abspath(self.directory or preferences.game_dir(context)), result):
                pass
        except BakeError as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        self.report({'INFO'}, BAKED_MESSAGE.format(
            result.get("out", "")))
        return {'FINISHED'}


def _preview_world(scene):
    return scene.world if scene.world is not None and scene.world.get(PREVIEW_TAG) else None


class AERON_OT_preview_probe(Operator):
    """Show the probe's last baked panorama as the World, turned as in the game, to see its light and \
reflections on models in Material Preview or Rendered view. Click again to put the previous World back"""

    bl_idname = "aeron.preview_probe"
    bl_label = "Preview Lighting"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        obj = context.object
        return (obj is not None and obj.type == 'LIGHT_PROBE' and obj.data.type == 'SPHERE' and
                common.model_root(obj) is not None)

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
            self.report({'ERROR'}, "Bake the probes first")
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
        mapping.inputs["Rotation"].default_value = (
            WORLD_CAMERA @ _camera_rotation(common.model_root(probe)).inverted()).to_euler()
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


class OBJECT_PT_aeron_probe(Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'object'
    bl_label = "Aeron Probe"

    @classmethod
    def poll(cls, context):
        obj = context.object
        return (obj is not None and obj.type == 'LIGHT_PROBE' and obj.data.type in {'SPHERE', 'VOLUME'} and
                common.model_root(obj) is not None)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        obj = context.object
        root = common.model_root(obj)
        data = obj.data
        if obj.parent != root:
            layout.label(text="Not directly under the model root: not exported", icon='ERROR')
            return
        if data.type == 'SPHERE':
            layout.label(text="Reflection probe {} of {}".format(reflection_probes(root).index(obj) + 1, root.name),
                         icon='LIGHTPROBE_SPHERE')
            layout.prop(data, "influence_type")
            layout.prop(data, "influence_distance", text="Radius" if data.influence_type == 'ELIPSOID' else "Size")
            layout.prop(data, "falloff")
            layout.prop(obj.aeron_probe, "room_parallax")
            column = layout.column()
            column.active = not obj.aeron_probe.room_parallax
            column.prop(data, "use_custom_parallax")
            column = column.column()
            column.active = data.use_custom_parallax and not obj.aeron_probe.room_parallax
            column.prop(data, "parallax_type")
            column.prop(data, "parallax_distance")
            layout.prop(data, "clip_start")
            layout.prop(data, "clip_end")
            layout.prop(context.scene, "aeron_probe_samples")
            layout.prop(obj.aeron_probe, "filled")
            layout.operator("aeron.preview_probe", icon='WORLD', depress=_preview_world(context.scene) is not None)
        else:
            layout.label(text="Light grid of {}".format(root.name), icon='LIGHTPROBE_VOLUME')
            layout.prop(obj.aeron_probe, "grid_spacing")
            r = grid_resolution(obj)
            layout.label(text="{} x {} x {} = {} points (at most {})".format(r[0], r[1], r[2], r[0] * r[1] * r[2],
                                                                          MAX_GRID_POINTS))
            layout.prop(obj.aeron_probe, "key_light_shadow")
            layout.operator("aeron.fill_probes", icon='LIGHTPROBE_SPHERE')
        device = 'GPU' if _gpu_available(context) else 'CPU'
        layout.label(text="Bake: about {:.0f} min on the {}".format(math.ceil(bake_seconds(root, device) / 60.0),
                                                                   device))
        layout.operator("aeron.bake_probes", icon='RENDER_STILL')


classes = (AeronProbeProperties, AERON_OT_add_reflection_probe, AERON_OT_add_light_grid, AERON_OT_fill_probes,
           AERON_OT_bake_probes, AERON_OT_preview_probe, OBJECT_PT_aeron_probe)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Object.aeron_probe = bpy.props.PointerProperty(type=AeronProbeProperties)
    bpy.types.Scene.aeron_probe_samples = IntProperty(
        name="Samples", default=128, min=16, max=4096,
        description="Cycles samples per pixel of the reflection probes' panoramas, denoised. The light grid's "
                    "points use {} x {} pixels at {} samples".format(GRID_PANORAMA_SIZE[0], GRID_PANORAMA_SIZE[1],
                                                                    GRID_SAMPLES))


def unregister():
    del bpy.types.Scene.aeron_probe_samples
    del bpy.types.Object.aeron_probe
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
