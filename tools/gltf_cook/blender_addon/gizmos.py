# Gizmos for the active component, shown with their overlays. There is a
# move gizmo for the target point (when the component has a target ID) and
# one for the hinge pivot, a dial for each hinge axis that turns all three
# axes together, and a cage that resizes the authored descriptor box. They
# write the component's properties in its local space. Blender does not
# record an undo step for a gizmo, so the add-on pushes one when a drag ends.

import bpy
from bpy.types import GizmoGroup
from mathutils import Matrix, Vector

from . import common, overlays

_DIAL_SIZE = 1.1
_BOX_COLOR = overlays._DESCRIPTOR_COLOR
_AXES = ("rotation_axis", "direction_axis", "up_axis")
_SYNC_TOLERANCE = 1e-5


def _component(context):
    obj = context.object
    return obj if common.role(obj) == 'component' and obj.mode == 'OBJECT' else None


def _shown(context, setting):
    settings = context.scene.aeron_flight
    space = context.space_data
    return (settings.overlays and getattr(settings, setting) and space is not None and
            space.overlay.show_overlays)


class _Drag:
    """The drag waiting for its undo step."""
    group = None
    message = None
    window = None


def _dragged(group, message):
    if _Drag.message is None:
        bpy.app.timers.register(_end_drag, first_interval=0.1)
    _Drag.group, _Drag.message, _Drag.window = group, message, bpy.context.window


def _end_drag():
    try:
        if any(gizmo.is_modal for gizmo in _Drag.group.gizmos):
            return 0.1
    except ReferenceError:  # the group is gone
        pass
    message, window = _Drag.message, _Drag.window
    _Drag.group = _Drag.message = _Drag.window = None
    try:
        with bpy.context.temp_override(window=window):
            bpy.ops.ed.undo_push(message=message)
    except (ReferenceError, RuntimeError, TypeError):
        pass  # the window was closed
    return None


def _sync_descriptor(props, low, high):
    """Sets the bounds. Also sets the center and span if they matched the old bounds."""
    old_low, old_high = Vector(props.descriptor_bounds_min), Vector(props.descriptor_bounds_max)
    followed = ((Vector(props.descriptor_center) - (old_low + old_high) * 0.5).length < _SYNC_TOLERANCE and
                (Vector(props.descriptor_span) - (old_high - old_low)).length < _SYNC_TOLERANCE)
    props.descriptor_bounds_min, props.descriptor_bounds_max = low, high
    if followed:
        props.descriptor_center = (low + high) * 0.5
        props.descriptor_span = high - low


class AERON_GGT_component(GizmoGroup):
    bl_idname = "AERON_GGT_component"
    bl_label = "Aeron Component"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'WINDOW'
    bl_options = {'3D', 'PERSISTENT'}

    @classmethod
    def poll(cls, context):
        return _component(context) is not None and _shown(context, "overlays")

    def setup(self, context):
        self.target = self._mover("target", "Move Target", overlays._TARGET_COLOR)
        self.pivot = self._mover("pivot", "Move Pivot", overlays._PIVOT_COLOR)
        self.dials = []
        self.angles = [0.0, 0.0, 0.0]
        for index, color in enumerate(overlays._AXIS_COLORS):
            dial = self.gizmos.new("GIZMO_GT_dial_3d")
            dial.draw_options = {'CLIP', 'ANGLE_VALUE'}
            dial.color, dial.alpha = color[:3], 0.7
            dial.color_highlight, dial.alpha_highlight = color[:3], 1.0
            dial.scale_basis = _DIAL_SIZE
            dial.line_width = 2.0
            dial.target_set_handler("offset", get=self._dial_get(index), set=self._dial_set(index))
            self.dials.append(dial)
        self.cage = self.gizmos.new("GIZMO_GT_cage_3d")
        self.cage.draw_style = 'BOX'
        self.cage.transform = {'TRANSLATE', 'SCALE'}
        self.cage.use_draw_scale = False
        self.cage.color, self.cage.alpha = _BOX_COLOR, 0.6
        self.cage.color_highlight, self.cage.alpha_highlight = _BOX_COLOR, 1.0
        self.cage.target_set_handler("matrix", get=self._cage_get, set=self._cage_set)

    def _mover(self, field, message, color):
        gizmo = self.gizmos.new("GIZMO_GT_move_3d")
        gizmo.draw_options = {'ALIGN_VIEW'}
        gizmo.use_snap = True
        gizmo.color, gizmo.alpha = color[:3], 0.8
        gizmo.color_highlight, gizmo.alpha_highlight = color[:3], 1.0
        gizmo.scale_basis = 0.15
        gizmo.target_set_handler("offset", get=lambda: self._point_get(field),
                                 set=lambda value: self._point_set(field, message, value))
        return gizmo

    # A point gizmo works in world space.
    def _point_get(self, field):
        component = _component(bpy.context)
        if component is None:
            return (0.0, 0.0, 0.0)
        return component.matrix_world @ Vector(getattr(component.aeron_flight, field))

    def _point_set(self, field, message, value):
        component = _component(bpy.context)
        if component is not None:
            setattr(component.aeron_flight, field, component.matrix_world.inverted_safe() @ Vector(value))
            _dragged(self, message)

    # A dial turns all three axes about its own axis. It reports the angle
    # it was last set to, and turns the axes by the change at each update.
    # The dial angle goes clockwise about its Z axis.
    def _dial_get(self, index):
        return lambda: self.angles[index]

    def _dial_set(self, index):
        def set_(value):
            component = _component(bpy.context)
            if component is None:
                return
            props = component.aeron_flight
            axis = Vector(getattr(props, _AXES[index]))
            if axis.length > 0.0:
                turn = Matrix.Rotation(self.angles[index] - value, 3, axis.normalized())
                for name in _AXES:
                    setattr(props, name, turn @ Vector(getattr(props, name)))
            self.angles[index] = value
            _dragged(self, "Turn Hinge Axes")
        return set_

    # The cage is a unit box under the component's world matrix. The
    # descriptor bounds set its position and size.
    def _cage_get(self):
        component = _component(bpy.context)
        if component is None:
            return [value for row in Matrix.Identity(4) for value in row]
        props = component.aeron_flight
        low, high = Vector(props.descriptor_bounds_min), Vector(props.descriptor_bounds_max)
        matrix = Matrix.Translation((low + high) * 0.5) @ Matrix.Diagonal((high - low).to_4d())
        matrix[3][3] = 1.0
        return [value for column in matrix.col for value in column]

    def _cage_set(self, value):
        component = _component(bpy.context)
        if component is None:
            return
        matrix = Matrix([value[column * 4:column * 4 + 4] for column in range(4)]).transposed()
        center = matrix.translation
        size = Vector([abs(matrix[axis][axis]) for axis in range(3)])
        _sync_descriptor(component.aeron_flight, center - size * 0.5, center + size * 0.5)
        _dragged(self, "Resize Bounds")

    def draw_prepare(self, context):
        component = _component(context)
        props = component.aeron_flight
        matrix = component.matrix_world
        self.target.hide = not (_shown(context, "overlay_targets") and props.target_id)
        hinge = _shown(context, "overlay_hinges") and props.has_rotation
        self.pivot.hide = not hinge
        pivot = matrix @ Vector(props.pivot)
        basis = matrix.to_3x3()
        for dial, name in zip(self.dials, _AXES):
            axis = basis @ Vector(getattr(props, name))
            dial.hide = not hinge or axis.length == 0.0
            if not dial.hide:
                orientation = axis.normalized().to_track_quat('Z', 'Y').to_matrix().to_4x4()
                dial.matrix_basis = Matrix.Translation(pivot) @ orientation
        self.cage.hide = not (_shown(context, "overlay_descriptors") and props.has_descriptor_geometry)
        self.cage.matrix_basis = matrix


def register():
    bpy.utils.register_class(AERON_GGT_component)


def unregister():
    bpy.utils.unregister_class(AERON_GGT_component)
