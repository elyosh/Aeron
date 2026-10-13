# Articulation preview. It turns components about their hinge the way the
# game does. Each articulation group has a slider, and each component also
# has its own preview angle for trying out its hinge. The preview starts
# when a slider moves away from 0 and saves the transforms of the
# components. It ends when all sliders are back at 0. Each component stores
# the rotation the preview gave it, so the end removes exactly that rotation
# and keeps any edit made during the preview. If nothing was edited, the
# component gets back its exact saved transform. The exports and Check Model
# refuse to run during a preview.
#
# Play moves the model the way the game does: each articulation group opens at
# its speed, stays open, closes at its closing speed, stays closed, and starts
# again, and rotary gun turrets turn on their hinge. Nothing is keyframed.

import math
import time

import bpy
from bpy.app.handlers import persistent
from mathutils import Matrix, Vector

from . import common

IDENTITY = tuple(value for row in Matrix.Identity(4) for value in row)
_REST_TOLERANCE = 1e-5


def _rest(obj):
    """Returns the exact transform channels of `obj`, for all rotation modes."""
    return (tuple(obj.location) + tuple(obj.rotation_quaternion) + tuple(obj.rotation_euler) +
            tuple(obj.rotation_axis_angle) + tuple(obj.scale))


def _restore(obj, rest):
    obj.location, obj.rotation_quaternion, obj.rotation_euler = rest[0:3], rest[3:7], rest[7:10]
    obj.rotation_axis_angle, obj.scale = rest[10:14], rest[14:17]


def hinge_matrix(component, turns):
    """Returns the rotation by `turns` about the component's hinge, in its
    local space. The renderer turns a component by -angle about its axis in
    the game's frame. That frame mirrors Blender's X axis, so here the
    rotation is +angle, right-handed. A zero axis does not turn, as in the
    renderer."""
    props = component.aeron_flight
    axis = Vector(props.rotation_axis)
    if not props.has_rotation or turns == 0.0 or axis.length < 1e-4:
        return Matrix.Identity(4)
    pivot = Vector(props.pivot)
    return (Matrix.Translation(pivot) @ Matrix.Rotation(2.0 * math.pi * turns, 4, axis.normalized()) @
            Matrix.Translation(-pivot))


def component_turns(root, component):
    """Returns the component's angle in the preview, in turns. This is its
    own preview angle when set, or else its angle in the playback when the
    model plays or is paused. Otherwise it comes from the component's last
    articulation in list order whose group slider is not at 0, and it is the
    slider amount times the articulation angle."""
    if component.aeron_flight.preview_angle:
        return component.aeron_flight.preview_angle / 256.0
    played = _playback.get(root.name)
    if played is not None:
        return played["turns"].get(component.name, 0.0)
    props = root.aeron_flight
    amounts = {group.name: group.amount for group in props.preview_groups}
    turns = 0.0
    for articulation in props.articulations:
        if articulation.component == component and amounts.get(articulation.group, 0.0) > 0.0:
            turns = amounts[articulation.group] * articulation.angle / 256.0
    return turns


def _set(component, matrix):
    """Replaces the rotation that the preview gave the component with `matrix`."""
    given = Matrix([component.aeron_flight.preview_matrix[row * 4:row * 4 + 4] for row in range(4)])
    component.matrix_basis = component.matrix_basis @ given.inverted_safe() @ matrix
    component.aeron_flight.preview_matrix = [value for row in matrix for value in row]


def _wanted(root):
    return (root.name in _playback or any(group.amount > 0.0 for group in root.aeron_flight.preview_groups) or
            any(component.aeron_flight.preview_angle for component in common.components(root)))


def apply(root):
    """Turns the model's components to their preview angles. Starts the
    preview when a slider leaves 0, and ends it when all are back at 0."""
    props = root.aeron_flight
    wanted = _wanted(root)
    if not props.preview_active:
        if not wanted:
            return
        for component in common.components(root):
            component.aeron_flight.preview_matrix = IDENTITY
            component.aeron_flight.preview_rest = _rest(component)
        props.preview_active = True
    if not wanted:
        _end(root)
        return
    for component in common.components(root):
        _set(component, hinge_matrix(component, component_turns(root, component)))


def _end(root):
    root.aeron_flight.preview_active = False
    for component in common.components(root):
        _set(component, Matrix.Identity(4))
        rest = tuple(component.aeron_flight.preview_rest)
        restored = component.matrix_basis.copy()
        _restore(component, rest)
        if any(abs(a - b) > _REST_TOLERANCE for row_a, row_b in zip(component.matrix_basis, restored)
               for a, b in zip(row_a, row_b)):
            component.matrix_basis = restored  # edited during the preview


def reset(root):
    """Sets every slider back to 0 and stops the playback, which ends the preview."""
    _playback.pop(root.name, None)
    for group in root.aeron_flight.preview_groups:
        if group.amount:
            group.amount = 0.0
    for component in common.components(root):
        if component.aeron_flight.preview_angle:
            component.aeron_flight.preview_angle = 0
    if root.aeron_flight.preview_active:
        _end(root)


def sync_groups(root):
    """Makes one slider for each group named by the articulations, in their
    order. Groups that still exist keep their amounts."""
    props = root.aeron_flight
    names = []
    for articulation in props.articulations:
        if articulation.group and articulation.group not in names:
            names.append(articulation.group)
    if [group.name for group in props.preview_groups] == names:
        return
    amounts = {group.name: group.amount for group in props.preview_groups}
    props.preview_groups.clear()
    for name in names:
        group = props.preview_groups.add()
        group.name = name
        group.amount = amounts.get(name, 0.0)
    apply(root)


# ---- playback --------------------------------------------------------------------

# The game clock runs at 236 ticks per second. Articulations move by their speed
# (1/256 turn) every step_ticks ticks, an articulation setting of the model. Idle
# rotary gun turrets turn 4/256 turn every 15 ticks, and the game reverses them
# at random, on average every 43 updates. The playback reverses them every 43
# updates, so it is the same every time.
TICKS_PER_SECOND = 236.0
DEFAULT_STEP_TICKS = 15
HOLD_SECONDS = 1.0
TURRET_SPEED = 4.0 * TICKS_PER_SECOND / 15.0  # 1/256 turn per second
TURRET_REVERSE_SECONDS = 43 * 15 / TICKS_PER_SECOND
ROTARY_GUN_TURRET = 21  # index into common.MESH_TYPE_NAMES
_FRAME_SECONDS = 1.0 / 30.0

# Per playing or paused model root name: "time" (seconds of playback), "last"
# (wall clock of the last step, None when paused) and "turns" (per component name).
_playback = {}


def playing(root):
    state = _playback.get(root.name)
    return state is not None and state["last"] is not None


def _speed(root, units):
    """Converts a speed in 1/256 turn per step into 1/256 turn per second."""
    settings = {item.name: item.value for item in root.aeron_flight.articulation_settings}
    step = max(1, settings.get("step_ticks", DEFAULT_STEP_TICKS))
    return units * TICKS_PER_SECOND / step


def _travel(angle, speed, seconds):
    """Angle reached after `seconds` from 0 toward `angle` at `speed` (both in
    1/256 turn). A speed of 0 jumps to the angle."""
    if speed <= 0.0:
        return float(angle)
    return math.copysign(min(abs(angle), speed * seconds), angle)


def _group_turns(root, articulations, seconds):
    """Angles in turns of one group's articulations at `seconds` in its cycle:
    opening, open, closing, closed."""
    opening = max((abs(a.angle) / _speed(root, a.speed) if a.speed else 0.0) for a in articulations)
    closing = max((abs(a.angle) / _speed(root, a.speed_back if a.has_speed_back else a.speed)
                   if (a.speed_back if a.has_speed_back else a.speed) else 0.0) for a in articulations)
    cycle = opening + closing + 2.0 * HOLD_SECONDS
    t = seconds % cycle if cycle > 0.0 else 0.0
    turns = {}
    for a in articulations:
        back = a.speed_back if a.has_speed_back else a.speed
        if t < opening:
            angle = _travel(a.angle, _speed(root, a.speed), t)
        elif t < opening + HOLD_SECONDS:
            angle = a.angle
        elif t < opening + HOLD_SECONDS + closing:
            angle = a.angle - _travel(a.angle, _speed(root, back), t - opening - HOLD_SECONDS)
        else:
            angle = 0.0
        turns[a] = angle / 256.0
    return turns


def _turret_turns(seconds):
    """Angle in turns of an idle rotary gun turret after `seconds`: it turns at
    the game's speed and reverses every TURRET_REVERSE_SECONDS."""
    legs, rest = divmod(seconds, TURRET_REVERSE_SECONDS)
    full = TURRET_SPEED * TURRET_REVERSE_SECONDS
    # Even legs turn forward, odd legs back, so the angle swings between 0 and one leg.
    angle = rest * TURRET_SPEED if int(legs) % 2 == 0 else full - rest * TURRET_SPEED
    return angle / 256.0


def played_turns(root, seconds):
    """Angles in turns of the model's components at `seconds` of playback."""
    turns = {}
    groups = {}
    for articulation in root.aeron_flight.articulations:
        if articulation.component is not None and articulation.group:
            groups.setdefault(articulation.group, []).append(articulation)
    for articulations in groups.values():
        for articulation, value in _group_turns(root, articulations, seconds).items():
            turns[articulation.component.name] = value
    for component in common.components(root):
        props = component.aeron_flight
        if props.has_rotation and int(props.mesh_type) == ROTARY_GUN_TURRET:
            turns[component.name] = _turret_turns(seconds)
    return turns


def _step():
    now = time.monotonic()
    for name in list(_playback):
        state = _playback[name]
        root = bpy.data.objects.get(name)
        if root is None or common.role(root) != 'model':
            del _playback[name]
            continue
        if state["last"] is None:
            continue
        state["time"] += now - state["last"]
        state["last"] = now
        state["turns"] = played_turns(root, state["time"])
        apply(root)
    return _FRAME_SECONDS if any(state["last"] is not None for state in _playback.values()) else None


def play(root):
    """Starts or resumes the playback of the model."""
    state = _playback.setdefault(root.name, {"time": 0.0, "last": None, "turns": {}})
    state["last"] = time.monotonic()
    if not bpy.app.timers.is_registered(_step):
        bpy.app.timers.register(_step, first_interval=0.0)


def pause(root):
    """Stops the playback and leaves the components where they are."""
    state = _playback.get(root.name)
    if state is not None:
        state["last"] = None


@persistent
def _on_load(*_args):
    _playback.clear()
    for obj in bpy.data.objects:
        if common.role(obj) == 'model':
            sync_groups(obj)


def register():
    bpy.app.handlers.load_post.append(_on_load)


def unregister():
    _playback.clear()
    if bpy.app.timers.is_registered(_step):
        bpy.app.timers.unregister(_step)
    if _on_load in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load)
