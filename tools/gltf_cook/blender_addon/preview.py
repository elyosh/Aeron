# Articulation preview. It turns components about their hinge the way the
# game does. Each articulation group has a slider, and each component also
# has its own preview angle for trying out its hinge. The preview starts
# when a slider moves away from 0 and saves the transforms of the
# components. It ends when all sliders are back at 0. Each component stores
# the rotation the preview gave it, so the end removes exactly that rotation
# and keeps any edit made during the preview. If nothing was edited, the
# component gets back its exact saved transform. The exports and Check Model
# refuse to run during a preview.

import math

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
    own preview angle when set. Otherwise it comes from the component's last
    articulation in list order whose group slider is not at 0, and it is the
    slider amount times the articulation angle."""
    if component.aeron_flight.preview_angle:
        return component.aeron_flight.preview_angle / 256.0
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
    return (any(group.amount > 0.0 for group in root.aeron_flight.preview_groups) or
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
    """Sets every slider back to 0, which ends the preview."""
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


@persistent
def _on_load(*_args):
    for obj in bpy.data.objects:
        if common.role(obj) == 'model':
            sync_groups(obj)


def register():
    bpy.app.handlers.load_post.append(_on_load)


def unregister():
    if _on_load in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load)
