# Viewport overlays that show the flight model of the active object as the game
# uses it: engine glows as the game draws them, hardpoints by family with the
# projectile each weapon fires, bounds, targets, hinges, the shapes the game
# draws for model lights, and labels.
# The overlays only draw and never change the scene.
#
# The geometry is rebuilt when the depsgraph reports a change. The engine glows
# depend on the view, so they are rebuilt at each redraw.

import math

import blf
import bpy
import gpu
import numpy
from bpy.app.handlers import persistent
from bpy_extras.view3d_utils import location_3d_to_region_2d
from gpu_extras.batch import batch_for_shader
from mathutils import Matrix, Vector

from . import common, projectiles

# Families of hardpoint types (indices into common.HARDPOINT_TYPE_NAMES): label,
# theme collection colour (None: grey) and types. The last family gets every type
# that no other family lists.
HARDPOINT_FAMILIES = (
    ("Lasers", 0, {1, 2, 3, 4, 9, 10}),
    ("Ion Cannons and Mag Pulse", 4, {5, 6, 11, 16, 17, 18}),
    ("Warheads", 1, {7, 8, 12, 13, 14, 15}),
    ("Docking and Towing", 3, {21, 22, 27, 28, 29, 30}),
    ("Pilot Eye", 2, {common.COCKPIT_HARDPOINT_TYPE}),
    ("Hangar and Acceleration", 5, {23, 24, 25, 26}),
    ("Other", None, set()),
)
_GREY = (0.7, 0.7, 0.7)
_TARGET_COLOR = (0.9, 0.35, 0.9, 1.0)
_CENTER_TARGET_COLOR = (0.9, 0.35, 0.9, 0.45)
_DESCRIPTOR_COLOR = (0.35, 0.8, 0.9)
_PIVOT_COLOR = (1.0, 0.85, 0.2, 1.0)
_AXIS_COLORS = ((0.9, 0.3, 0.3, 1.0), (0.4, 0.85, 0.4, 1.0), (0.4, 0.6, 1.0, 1.0))  # rotation, direction, up
_LABEL_COLOR = (1.0, 1.0, 1.0, 0.9)
_DISABLED_GLOW_COLOR = (0.6, 0.6, 0.6, 0.6)

# Constants of the game's engine glows (XwaRemasterShip_SubmitEngineGlows). The
# quad size follows the engine scale up to GLOW_SIZE_SCALE_MAX. The engine scale
# of a craft comes from its throttle and is at least GLOW_SCALE_MIN.
GLOW_SIZE_SCALE_MAX = 0.80000001
GLOW_SCALE_MIN = 0.34999999
GLOW_ROUND_RATIOS = (0.85000002, 1.2)
GLOW_DIAMOND_RADIUS = 1.415 * 0.5
GLOW_NEAR_GAIN = 10000.0 / 32768.0
# The quad is drawn as a grid of this many vertices per side.
_GLOW_GRID = 7


def family_color(index):
    color_index = HARDPOINT_FAMILIES[index][1]
    if color_index is None:
        return _GREY
    return tuple(bpy.context.preferences.themes[0].collection_color[color_index].color)


def family_icon(index):
    color_index = HARDPOINT_FAMILIES[index][1]
    return 'LAYER_USED' if color_index is None else 'COLLECTION_COLOR_{:02d}'.format(color_index + 1)


def hardpoint_family(hardpoint_type):
    for index, (_label, _color, types) in enumerate(HARDPOINT_FAMILIES):
        if hardpoint_type in types:
            return index
    return len(HARDPOINT_FAMILIES) - 1


# ---- engine glows ----------------------------------------------------------------


def glow_frame(matrix):
    """Returns the position, the right, up and look directions, and the
    dimensions (width, height, signed depth) of an engine glow from its world
    matrix, the same way the exporter writes them and the loader reads them.
    Blender's X, Z and -Y are the glTF X, Y and Z. A negative determinant
    makes the depth negative. Returns None for a zero scale."""
    basis = matrix.to_3x3()
    columns = [basis.col[axis].copy() for axis in range(3)]
    sign = -1.0 if basis.determinant() < 0.0 else 1.0
    width, depth, height = (column.length for column in columns)
    if not width or not depth or not height:
        return None
    right = columns[0] / width
    up = columns[2] / height
    look = -columns[1] / (depth * sign)
    return matrix.translation.copy(), right, up, look, (width, height, depth * sign)


def glow_corners(position, right, up, look, dimensions, eye, forward, scale):
    """Returns the corners (G, 4, 3) of the quads the game draws for engine
    glows, from (G, 3) arrays. A quad is a rectangle in the right/up plane or,
    for a round glow, a square turned toward the eye. Its corners are then
    pushed along the look axis by their depth from the glow's center. `eye` is
    the camera position, or None for an orthographic view. `forward` is the
    view direction and `scale` is the engine scale."""
    to_glow = position - eye if eye is not None else numpy.broadcast_to(forward, position.shape)
    clamped = min(scale, GLOW_SIZE_SCALE_MAX)
    width, height, depth = dimensions[:, 0:1], dimensions[:, 1:2], dimensions[:, 2:3]
    ratio = numpy.divide(width, height, out=numpy.ones_like(width), where=height != 0.0)
    rectangle = (ratio <= GLOW_ROUND_RATIOS[0]) | (ratio >= GLOW_ROUND_RATIOS[1])

    a = right * (width * clamped * 0.5)
    b = up * (height * clamped * 0.5)
    rectangles = numpy.stack([position - a + b, position + a + b, position + a - b, position - a - b], axis=1)

    radius = clamped * width * GLOW_DIAMOND_RADIUS
    dot_f = numpy.sum(right * to_glow, axis=1, keepdims=True)
    dot_s = numpy.sum(up * to_glow, axis=1, keepdims=True)
    length = numpy.hypot(dot_f, dot_s)
    level = length == 0.0
    safe = numpy.where(level, 1.0, length)
    fw, sw = dot_f / safe, dot_s / safe
    major = numpy.where(level, right, up * sw + right * fw) * radius
    minor = numpy.where(level, up, right * sw - up * fw) * radius
    diamonds = numpy.stack([position + minor, position + major, position - minor, position - major], axis=1)

    corners = numpy.where(rectangle[:, :, None], rectangles, diamonds)
    center_dot = numpy.sum(look * to_glow, axis=1, keepdims=True)
    offset = (corners - position[:, None, :]) @ forward
    z_delta = numpy.where(center_dot < 0.0, offset, -offset)
    gain = numpy.where(z_delta < 0.0, GLOW_NEAR_GAIN, scale * depth)
    return corners + look[:, None, :] * (z_delta * gain)[:, :, None]


def _glow_grid():
    """Returns the bilinear weights of the grid vertices over the corners of a
    quad, the (u, v) of each vertex in [-1, 1], and the triangles of the grid."""
    steps = numpy.linspace(0.0, 1.0, _GLOW_GRID)
    t, s = numpy.meshgrid(steps, steps, indexing='ij')
    s, t = s.ravel(), t.ravel()
    weights = numpy.stack([(1 - s) * (1 - t), s * (1 - t), s * t, (1 - s) * t], axis=1)
    triangles = []
    for row in range(_GLOW_GRID - 1):
        for column in range(_GLOW_GRID - 1):
            i = row * _GLOW_GRID + column
            triangles += [(i, i + 1, i + _GLOW_GRID + 1), (i, i + _GLOW_GRID + 1, i + _GLOW_GRID)]
    return weights, numpy.stack([2 * s - 1, 2 * t - 1], axis=1), numpy.array(triangles, dtype=numpy.int32)


_GRID_WEIGHTS, _GRID_UV, _GRID_TRIANGLES = _glow_grid()


def glow_colors(core, outer):
    """Premultiplied vertex colours (G * N, 4) for the grids of the glows. As
    in the game, the colour goes from the core colour at the center to the
    outer colour at the rim. A round falloff replaces the game's sprite."""
    rim = numpy.max(numpy.abs(_GRID_UV), axis=1)[None, :, None]
    distance = numpy.minimum(numpy.hypot(_GRID_UV[:, 0], _GRID_UV[:, 1]), 1.0)
    falloff = (1.0 - distance * distance * (3.0 - 2.0 * distance))[None, :, None]
    tint = core[:, None, :] * (1.0 - rim) + outer[:, None, :] * rim
    alpha = tint[:, :, 3:4] * falloff
    return numpy.concatenate([tint[:, :, :3] * alpha, alpha], axis=2).reshape(-1, 4).astype(numpy.float32)


# ---- geometry ------------------------------------------------------------------


class Geometry:
    """What the overlays draw, in world space: lines (start, end, colour,
    thick), points (position, colour, selected), engine glows and labels."""

    def __init__(self):
        self.lines = []
        self.points = []
        self.labels = []
        self.glows = []  # (position, right, up, look, dimensions, core, outer, enabled, selected)
        self.projectiles = []  # (parts, world matrix)

    def line(self, start, end, color, thick=False):
        self.lines.append((Vector(start), Vector(end), color, thick))

    def polyline(self, points, color, thick=False, closed=False):
        for index in range(len(points) - (0 if closed else 1)):
            self.line(points[index], points[(index + 1) % len(points)], color, thick)

    def cross(self, center, size, color):
        for axis in range(3):
            offset = Vector((0.0, 0.0, 0.0))
            offset[axis] = size
            self.line(center - offset, center + offset, color)

    def arrow(self, start, end, color):
        self.line(start, end, color)
        direction = end - start
        if direction.length == 0.0:
            return
        head = direction.length * 0.12
        side = direction.orthogonal().normalized()
        other = direction.cross(side).normalized()
        back = end - direction.normalized() * head
        for offset in (side, -side, other, -other):
            self.line(end, back + offset * head * 0.4, color)

    def circle(self, center, normal, radius, color, start=None, angle=2.0 * math.pi):
        """A circle around `normal`, or an arc of `angle` (right-handed) that
        starts at `start`, a vector perpendicular to `normal`."""
        normal = normal.normalized()
        start = (start if start is not None else normal.orthogonal()).normalized() * radius
        steps = max(2, int(math.ceil(abs(angle) / math.radians(7.5))))
        points = [center + Matrix.Rotation(angle * step / steps, 3, normal) @ start for step in range(steps + 1)]
        self.polyline(points, color)
        return points[-1]

    def sphere(self, center, radius, color):
        for axis in ((1, 0, 0), (0, 1, 0), (0, 0, 1)):
            self.circle(center, Vector(axis), radius, color)

    def box(self, matrix, low, high, color, thick=False):
        corners = [matrix @ Vector((x, y, z)) for x in (low[0], high[0]) for y in (low[1], high[1])
                   for z in (low[2], high[2])]
        for a, b in ((0, 1), (2, 3), (4, 5), (6, 7), (0, 2), (1, 3), (4, 6), (5, 7), (0, 4), (1, 5), (2, 6), (3, 7)):
            self.line(corners[a], corners[b], color, thick)

    def capsule(self, center, half_axis, radius, color):
        axis = half_axis.normalized() if half_axis.length > 0.0 else Vector((1.0, 0.0, 0.0))
        side = axis.orthogonal().normalized()
        other = axis.cross(side)
        for end in (center + half_axis, center - half_axis):
            self.circle(end, axis, radius, color)
        for offset in (side, -side, other, -other):
            self.line(center - half_axis + offset * radius, center + half_axis + offset * radius, color)
        for sign in (1.0, -1.0):
            for normal in (other, side):
                start = axis.cross(normal)
                self.circle(center + half_axis * sign, normal * sign, radius, color, start=start, angle=math.pi)


def _component_of(obj):
    while obj is not None and common.role(obj) != 'component':
        obj = obj.parent
    return obj


def _computed_bounds(root, component):
    """The bounds, in model space, from which the game computes the
    descriptor when none is authored. They are the bounds of the collision
    mesh vertices, or of the render mesh vertices when there is no collision
    mesh. None when the mesh has no vertices."""
    source = next((child for child in component.children if common.role(child) == 'collision'), component)
    count = len(source.data.vertices)
    if not count:
        return None
    coords = numpy.empty(count * 3, dtype=numpy.float32)
    source.data.vertices.foreach_get("co", coords)
    matrix = numpy.array(root.matrix_world.inverted_safe() @ source.matrix_world)
    points = coords.reshape(-1, 3) @ matrix[:3, :3].T + matrix[:3, 3]
    return points.min(axis=0), points.max(axis=0)


def _hinge(geometry, root, component, size, labelled):
    props = component.aeron_flight
    matrix = component.matrix_world
    basis = matrix.to_3x3()
    pivot = matrix @ Vector(props.pivot)
    axes = [basis @ Vector(vector) for vector in (props.rotation_axis, props.direction_axis, props.up_axis)]
    geometry.cross(pivot, size * 0.08, _PIVOT_COLOR)
    rotation_axis = axes[0]
    if rotation_axis.length == 0.0:
        return
    rotation_axis = rotation_axis.normalized()
    geometry.arrow(pivot - rotation_axis * size, pivot + rotation_axis * size, _AXIS_COLORS[0])
    for vector, color in zip(axes[1:], _AXIS_COLORS[1:]):
        if vector.length > 0.0:
            geometry.line(pivot, pivot + vector.normalized() * size * 0.5, color)
    # An arc from the direction axis to the angle of each articulation, with
    # the articulation's name on the active component (`labelled`). The
    # renderer turns a component by -angle around its axis in the game's
    # frame. That frame mirrors Blender's X axis, so here the turn is +angle,
    # right-handed.
    start = axes[1] - rotation_axis * axes[1].dot(rotation_axis)
    if start.length < 1e-6:
        start = rotation_axis.orthogonal()
    radius = size * 0.6
    for articulation in root.aeron_flight.articulations:
        if articulation.component != component:
            continue
        end = geometry.circle(pivot, rotation_axis, radius, _PIVOT_COLOR, start=start,
                              angle=2.0 * math.pi * articulation.angle / 256.0)
        geometry.line(pivot, end, _PIVOT_COLOR)
        if labelled:
            geometry.labels.append((end, "{} {}".format(articulation.group or "(no group)", articulation.angle),
                                    _PIVOT_COLOR))
    geometry.line(pivot, pivot + start.normalized() * radius, _PIVOT_COLOR)


def _pilot_eye(geometry, root, position, color):
    """A small view frustum, looking along the model's forward (Blender -Y)."""
    basis = root.matrix_world.to_3x3().normalized()
    forward, right, up = basis @ Vector((0, -1, 0)), basis @ Vector((1, 0, 0)), basis @ Vector((0, 0, 1))
    far = position + forward * 0.5
    corners = [far + right * x + up * y for x, y in ((-0.3, 0.2), (0.3, 0.2), (0.3, -0.2), (-0.3, -0.2))]
    for corner in corners:
        geometry.line(position, corner, color)
    geometry.polyline(corners, color, closed=True)
    geometry.line(far + up * 0.25 - right * 0.08, far + up * 0.33, color)
    geometry.line(far + up * 0.33, far + up * 0.25 + right * 0.08, color)


def _light(geometry, light, settings):
    """The shape the game draws for a model light (see light_game_shape)."""
    shape = common.light_shape(light, common.local_matrix(light).to_scale())
    if shape is None:
        return
    color = tuple(light.data.color) + (0.9,)
    location, rotation, _scale = light.matrix_world.decompose()
    parent_scale = light.parent.matrix_world.to_scale()[0]
    basis = rotation.to_matrix()
    one_sided = shape.get("oneSided", False)
    if shape["shape"] == "point":
        geometry.sphere(location, shape.get("radius", common.DEFAULT_LIGHT_RADIUS) * parent_scale, color)
    elif shape["shape"] == "tube":
        geometry.capsule(location, basis @ Vector((0.5 * shape["length"] * parent_scale, 0, 0)),
                         shape["radius"] * parent_scale, color)
    else:
        half_x = basis @ Vector((0.5 * shape["size"][0] * parent_scale, 0, 0))
        half_y = basis @ Vector((0, 0.5 * shape["size"][1] * parent_scale, 0))
        longer, shorter = (half_x, half_y) if half_x.length > half_y.length else (half_y, half_x)
        if longer.length >= common.PANEL_TUBE_ASPECT * shorter.length:
            geometry.capsule(location, longer, shorter.length, color)
        else:
            geometry.sphere(location, 2.0 * math.sqrt(half_x.length * half_y.length / math.pi), color)
    if one_sided:
        size = max(0.25, shape.get("radius", 0.0) * parent_scale * 4.0)
        geometry.arrow(location, location + basis @ Vector((0, 0, -size)), color)
    if settings.overlay_light_ranges and light.data.use_custom_distance:
        geometry.sphere(location, light.data.cutoff_distance, color[:3] + (0.3,))


def build(root, active, settings):
    """Builds the overlay geometry for the model `root`."""
    geometry = Geometry()
    theme = bpy.context.preferences.themes[0].view_3d
    selected_color = tuple(theme.object_selected) + (1.0,)
    active_component = _component_of(active)
    family_colors = [family_color(index) + (1.0,) for index in range(len(HARDPOINT_FAMILIES))]
    for ordinal, component in enumerate(common.components(root)):
        props = component.aeron_flight
        matrix = component.matrix_world
        size = max(0.25, 0.5 * max(component.dimensions))
        if component.visible_get():
            authored = props.has_descriptor_geometry
            computed = None
            if not authored and (settings.overlay_descriptors or
                                 (settings.overlay_targets and not props.target_id)):
                computed = _computed_bounds(root, component)
            if settings.overlay_descriptors and (authored or computed is not None):
                highlighted = component == active_component
                color = (selected_color if highlighted else _DESCRIPTOR_COLOR + ((0.9,) if authored else (0.35,)))
                if authored:
                    geometry.box(matrix, props.descriptor_bounds_min, props.descriptor_bounds_max, color,
                                 thick=highlighted)
                else:
                    geometry.box(root.matrix_world, computed[0], computed[1], color, thick=highlighted)
            if settings.overlay_targets:
                # The point the game aims the targeting box at, and that the
                # component turns around when it breaks off. This is the
                # target when there is a target ID, otherwise the center of
                # the descriptor.
                if props.target_id:
                    target = matrix @ Vector(props.target)
                    geometry.cross(target, size * 0.08, _TARGET_COLOR)
                    geometry.line(matrix.translation, target, _TARGET_COLOR)
                elif authored or computed is not None:
                    center = (matrix @ Vector(props.descriptor_center) if authored else
                              root.matrix_world @ Vector((computed[0] + computed[1]) * 0.5))
                    geometry.cross(center, size * 0.08, _CENTER_TARGET_COLOR)
            if settings.overlay_hinges and props.has_rotation:
                _hinge(geometry, root, component, size, component == active_component)
            if settings.overlay_component_labels:
                center = matrix @ (sum((Vector(corner) for corner in component.bound_box), Vector()) / 8.0)
                geometry.labels.append((center, "{} {}".format(ordinal, component.name), _LABEL_COLOR))
        hardpoint = 0
        for child in common.ordered_children(component):
            if common.role(child) == 'hardpoint':
                ordinal_in_component, hardpoint = hardpoint, hardpoint + 1
                if not settings.overlay_hardpoints or not child.visible_get():
                    continue
                hardpoint_type = int(child.aeron_flight.hardpoint_type)
                color = family_colors[hardpoint_family(hardpoint_type)]
                position = child.matrix_world.translation.copy()
                geometry.points.append((position, color, child.select_get()))
                if projectiles.is_weapon(hardpoint_type):
                    # The projectile it fires, leaving it along the model's -Y,
                    # or else an arrow in that direction.
                    parts = (projectiles.model(bpy.context, root, hardpoint_type)
                             if settings.overlay_projectiles else None)
                    turn = root.matrix_world.to_3x3()
                    if parts:
                        geometry.projectiles.append((parts, Matrix.Translation(position) @ turn.to_4x4()))
                    else:
                        forward = (turn @ Vector((0.0, -1.0, 0.0))).normalized()
                        geometry.arrow(position, position + forward * size * 0.4, color)
                if hardpoint_type == common.COCKPIT_HARDPOINT_TYPE:
                    _pilot_eye(geometry, root, position, color)
                if settings.overlay_hardpoint_labels:
                    geometry.labels.append((position, "{}: {}".format(
                        ordinal_in_component, common.HARDPOINT_TYPE_NAMES[hardpoint_type]), color))
            elif settings.overlay_glows and child.visible_get():
                frame = glow_frame(child.matrix_world)
                if frame is None:  # a zero scale, which Check Model reports
                    continue
                glow = child.aeron_flight
                geometry.glows.append(frame + (tuple(glow.core_color), tuple(glow.outer_color),
                                               glow.glow_enabled, child.select_get()))
        if settings.overlay_lights:
            for child in component.children:
                if common.is_model_light(child) and child.visible_get():
                    _light(geometry, child, settings)
    return geometry


# ---- drawing -------------------------------------------------------------------


class _Cache:
    key = None
    dirty = True
    geometry = None
    batches = None
    glows = None
    projectiles = None


@persistent
def _on_change(*_args):
    _Cache.dirty = True


def tag_redraw(_self=None, _context=None):
    """Redraws the 3D views. This is the update callback of the overlay settings."""
    _Cache.dirty = True
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()


_SETTINGS = ("overlay_glows", "overlay_hardpoints", "overlay_hardpoint_labels", "overlay_projectiles",
             "overlay_descriptors",
             "overlay_targets", "overlay_hinges", "overlay_lights", "overlay_light_ranges",
             "overlay_component_labels")


def _current(context):
    """The geometry for this redraw, rebuilt when something changed."""
    settings = context.scene.aeron_flight
    space = context.space_data
    if not settings.overlays or space is None or not space.overlay.show_overlays:
        return None, settings
    active = context.active_object
    root = common.model_root(active)
    if root is None:
        return None, settings
    key = (root.name, active.name, tuple(getattr(settings, name) for name in _SETTINGS))
    if _Cache.dirty or key != _Cache.key:
        _Cache.key, _Cache.dirty = key, False
        _Cache.geometry = build(root, active, settings)
        _Cache.batches = None
        _Cache.projectiles = None
        glows = _Cache.geometry.glows
        if glows:
            columns = list(zip(*glows))
            arrays = [numpy.array([tuple(v) for v in column], dtype=numpy.float64) for column in columns[:7]]
            enabled = numpy.array(columns[7], dtype=bool)
            _Cache.glows = {
                "frame": arrays[:5], "enabled": enabled, "selected": numpy.array(columns[8], dtype=bool),
                "colors": glow_colors(arrays[5][enabled], arrays[6][enabled]),
            }
        else:
            _Cache.glows = None
    return _Cache.geometry, settings


def _line_batches(geometry):
    shader = gpu.shader.from_builtin('POLYLINE_SMOOTH_COLOR')
    batches = []
    for thick in (False, True):
        lines = [line for line in geometry.lines if line[3] == thick]
        if lines:
            positions = [point for line in lines for point in line[:2]]
            colors = [line[2] for line in lines for _ in range(2)]
            batches.append((batch_for_shader(shader, 'LINES', {"pos": positions, "color": colors}),
                            3.0 if thick else 1.5))
    point_shader = gpu.shader.from_builtin('POINT_FLAT_COLOR')
    points = []
    for selected_only, size in ((True, 13.0), (False, 9.0)):
        chosen = [point for point in geometry.points if point[2] or not selected_only]
        if chosen:
            theme = bpy.context.preferences.themes[0].view_3d
            colors = [tuple(theme.object_selected) + (1.0,) if selected_only else point[1] for point in chosen]
            points.append((batch_for_shader(point_shader, 'POINTS', {
                "pos": [point[0] for point in chosen], "color": colors}), size))
    return shader, batches, point_shader, points


def _view(context):
    region_data = context.region_data
    inverse = region_data.view_matrix.inverted()
    forward = -inverse.col[2].xyz.normalized()
    eye = inverse.translation if region_data.is_perspective else None
    return (numpy.array(eye) if eye is not None else None), numpy.array(forward)


def _draw_glows(context, settings, scale_ui):
    glows = _Cache.glows
    if glows is None:
        return
    eye, forward = _view(context)
    scale = max(settings.overlay_throttle, GLOW_SCALE_MIN)
    corners = glow_corners(*glows["frame"], eye, forward, scale)
    enabled = glows["enabled"]
    region = context.region
    if enabled.any():
        positions = numpy.einsum('nk,gkd->gnd', _GRID_WEIGHTS, corners[enabled]).reshape(-1, 3)
        count = int(enabled.sum())
        offsets = (numpy.arange(count, dtype=numpy.int32) * len(_GRID_WEIGHTS))[:, None, None]
        triangles = (_GRID_TRIANGLES[None, :, :] + offsets).reshape(-1, 3)
        shader = gpu.shader.from_builtin('SMOOTH_COLOR')
        batch = batch_for_shader(shader, 'TRIS', {"pos": positions.astype(numpy.float32),
                                                  "color": glows["colors"]}, indices=triangles)
        gpu.state.depth_test_set('LESS_EQUAL')
        gpu.state.depth_mask_set(False)
        gpu.state.blend_set('ALPHA_PREMULT')
        batch.draw(shader)
    outlined = glows["selected"] | ~enabled
    if outlined.any():
        theme = bpy.context.preferences.themes[0].view_3d
        selected_color = tuple(theme.object_selected) + (1.0,)
        positions, colors = [], []
        for quad, selected in zip(corners[outlined], glows["selected"][outlined]):
            for index in range(4):
                positions += [quad[index], quad[(index + 1) % 4]]
            colors += [selected_color if selected else _DISABLED_GLOW_COLOR] * 8
        shader = gpu.shader.from_builtin('POLYLINE_SMOOTH_COLOR')
        shader.uniform_float("viewportSize", (region.width, region.height))
        shader.uniform_float("lineWidth", 1.5 * scale_ui)
        gpu.state.blend_set('ALPHA')
        batch_for_shader(shader, 'LINES', {"pos": [tuple(p) for p in positions], "color": colors}).draw(shader)


def _projectile_batches(geometry):
    """One batch per projectile image, with every copy placed in world space."""
    shader = gpu.shader.from_builtin('IMAGE')
    groups = {}
    for parts, matrix in geometry.projectiles:
        turn = numpy.array(matrix.to_3x3(), dtype=numpy.float32)
        offset = numpy.array(matrix.translation, dtype=numpy.float32)
        for part in parts:
            groups.setdefault(id(part.rgba), (part.rgba, []))[1].append((part.positions @ turn.T + offset, part))
    batches = []
    for rgba, copies in groups.values():
        positions, uvs, triangles, start = [], [], [], 0
        for placed, part in copies:
            positions.append(placed)
            uvs.append(part.uvs)
            triangles.append(part.triangles + start)
            start += len(placed)
        batches.append((_texture(rgba), batch_for_shader(shader, 'TRIS', {
            "pos": numpy.concatenate(positions), "texCoord": numpy.concatenate(uvs)},
            indices=numpy.concatenate(triangles))))
    return shader, batches


_textures = {}  # id of a projectile image: (image, GPU texture)


def _texture(rgba):
    entry = _textures.get(id(rgba))
    if entry is None or entry[0] is not rgba:
        height, width = rgba.shape[:2]
        data = gpu.types.Buffer('FLOAT', width * height * 4, rgba.ravel().tolist())
        entry = _textures[id(rgba)] = (rgba, gpu.types.GPUTexture((width, height), format='RGBA16F', data=data))
    return entry[1]


def _draw_projectiles(geometry):
    if not geometry.projectiles:
        return
    if _Cache.projectiles is None:
        _Cache.projectiles = _projectile_batches(geometry)
    shader, batches = _Cache.projectiles
    gpu.state.depth_test_set('LESS_EQUAL')
    gpu.state.depth_mask_set(False)
    gpu.state.blend_set('ALPHA')
    for texture, batch in batches:
        shader.uniform_sampler("image", texture)
        batch.draw(shader)


def _draw_view():
    context = bpy.context
    geometry, settings = _current(context)
    if geometry is None:
        return
    scale_ui = context.preferences.system.ui_scale
    _draw_projectiles(geometry)
    _draw_glows(context, settings, scale_ui)
    if _Cache.batches is None:
        _Cache.batches = _line_batches(geometry)
    shader, lines, point_shader, points = _Cache.batches
    region = context.region
    gpu.state.depth_test_set('ALWAYS' if settings.overlay_in_front else 'LESS_EQUAL')
    gpu.state.depth_mask_set(False)
    gpu.state.blend_set('ALPHA')
    for batch, width in lines:
        shader.uniform_float("viewportSize", (region.width, region.height))
        shader.uniform_float("lineWidth", width * scale_ui)
        batch.draw(shader)
    for batch, size in points:
        gpu.state.point_size_set(size * scale_ui)
        batch.draw(point_shader)
    gpu.state.point_size_set(1.0)
    gpu.state.blend_set('NONE')
    gpu.state.depth_test_set('NONE')


def _draw_labels():
    context = bpy.context
    geometry, _settings = _current(context)
    if geometry is None or not geometry.labels:
        return
    scale_ui = context.preferences.system.ui_scale
    font = 0
    blf.size(font, 11.0 * scale_ui)
    blf.enable(font, blf.SHADOW)
    blf.shadow(font, 3, 0.0, 0.0, 0.0, 1.0)
    # Labels at the same position, such as hardpoints at the same place, are stacked.
    line = 14.0 * scale_ui
    stacked = {}
    for position, text, color in geometry.labels:
        location = location_3d_to_region_2d(context.region, context.region_data, position)
        if location is None:
            continue
        spot = tuple(round(value, 4) for value in position)
        row = stacked.get(spot, 0)
        stacked[spot] = row + 1
        blf.color(font, *color[:3], 1.0)
        blf.position(font, location.x + 6.0 * scale_ui, location.y + 4.0 * scale_ui - row * line, 0.0)
        blf.draw(font, text)
    blf.disable(font, blf.SHADOW)


_handlers = []
_app_handlers = (bpy.app.handlers.depsgraph_update_post, bpy.app.handlers.undo_post,
                 bpy.app.handlers.redo_post, bpy.app.handlers.load_post)


def register():
    _handlers.append(bpy.types.SpaceView3D.draw_handler_add(_draw_view, (), 'WINDOW', 'POST_VIEW'))
    _handlers.append(bpy.types.SpaceView3D.draw_handler_add(_draw_labels, (), 'WINDOW', 'POST_PIXEL'))
    for handlers in _app_handlers:
        handlers.append(_on_change)


def unregister():
    for handlers in _app_handlers:
        if _on_change in handlers:
            handlers.remove(_on_change)
    for handler in _handlers:
        bpy.types.SpaceView3D.draw_handler_remove(handler, 'WINDOW')
    _handlers.clear()
    _Cache.key, _Cache.dirty, _Cache.geometry, _Cache.batches, _Cache.glows = None, True, None, None, None
    _Cache.projectiles = None
    _textures.clear()
    projectiles.clear()
