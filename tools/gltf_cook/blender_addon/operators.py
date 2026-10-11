# Authoring operators.

import bpy
from bpy.props import EnumProperty, StringProperty
from bpy.types import Operator
from mathutils import Vector

from . import common, validation


def _component(obj):
    """The component `obj` is or belongs to."""
    while obj is not None and common.role(obj) != 'component':
        obj = obj.parent
    return obj


def _select_only(context, obj):
    for other in context.selected_objects:
        other.select_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj


def _parent_at(obj, parent, world_location):
    """Parent `obj` with an identity local rotation and scale at `world_location`."""
    obj.parent = parent
    obj.matrix_parent_inverse.identity()
    obj.location = parent.matrix_world.inverted() @ world_location


def _renumber(objects):
    for order, obj in enumerate(objects):
        obj.aeron_flight.order = order


class AERON_OT_make_flight_model(Operator):
    """Make the selected meshes the components of a flight model (a new one, or the active model root)"""

    bl_idname = "aeron.make_flight_model"
    bl_label = "Make Flight Model"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return any(obj.type == 'MESH' for obj in context.selected_objects)

    def execute(self, context):
        root = context.active_object if common.role(context.active_object) == 'model' else None
        meshes = sorted((obj for obj in context.selected_objects
                         if obj.type == 'MESH' and common.role(obj) in {'none', 'component'}),
                        key=lambda obj: obj.name)
        if root is None:
            root = bpy.data.objects.new("flight_model", None)
            root.empty_display_type = 'PLAIN_AXES'
            context.collection.objects.link(root)
            root.aeron_flight.role = 'model'
        order = common.next_order(common.components(root))
        for obj in meshes:
            if obj.parent == root and common.role(obj) == 'component':
                continue
            world = obj.matrix_world.copy()
            obj.parent = root
            obj.matrix_world = world
            obj.aeron_flight.role = 'component'
            obj.aeron_flight.order = order
            order += 1
        _select_only(context, root)
        self.report({'INFO'}, "{}: {} component(s)".format(root.name, len(common.components(root))))
        return {'FINISHED'}


class AERON_OT_add(Operator):
    """Add a part to the active component, at the 3D cursor"""

    bl_idname = "aeron.add"
    bl_label = "Add to Component"
    bl_options = {'REGISTER', 'UNDO'}

    kind: EnumProperty(items=[
        ('hardpoint', "Hardpoint", "A weapon or attachment point"),
        ('engineGlow', "Engine Glow", "An engine glow, looking toward -Y; X and Z scale are its size"),
        ('collision', "Collision Mesh", "A copy of the component's mesh to simplify into its collision mesh"),
        ('point', "Point Light", "A point light"),
        ('panel', "Panel Light", "A one-sided rectangle light, such as a ceiling panel"),
        ('tube', "Tube Light", "A tube light along its X size"),
    ])

    @classmethod
    def poll(cls, context):
        return _component(context.active_object) is not None

    def execute(self, context):
        component = _component(context.active_object)
        cursor = context.scene.cursor.location.copy()
        if self.kind in {'hardpoint', 'engineGlow'}:
            obj = bpy.data.objects.new(self.kind == 'hardpoint' and "hardpoint" or "engine_glow", None)
            obj.empty_display_type = 'PLAIN_AXES' if self.kind == 'hardpoint' else 'ARROWS'
            obj.empty_display_size = 0.25 if self.kind == 'hardpoint' else 1.0
        elif self.kind == 'collision':
            if any(common.role(child) == 'collision' for child in component.children):
                self.report({'ERROR'}, "{} already has a collision mesh".format(component.name))
                return {'CANCELLED'}
            mesh = component.data.copy()
            mesh.materials.clear()
            obj = bpy.data.objects.new(component.name + "_collision", mesh)
            obj.display_type = 'WIRE'
            obj.hide_render = True
        else:
            light = bpy.data.lights.new(self.kind + "_light", 'POINT' if self.kind == 'point' else 'AREA')
            light.energy = 100.0
            if self.kind != 'point':
                light.shape = 'RECTANGLE'
                light.size, light.size_y = (1.0, 1.0) if self.kind == 'panel' else (2.0, 0.1)
            obj = bpy.data.objects.new(light.name, light)
            obj.aeron_flight.light_tube = self.kind == 'tube'
        context.collection.objects.link(obj)
        if self.kind == 'collision':
            obj.parent = component
        else:
            _parent_at(obj, component, cursor)
        if self.kind in {'hardpoint', 'engineGlow', 'collision'}:
            obj.aeron_flight.role = self.kind
        if context.scene.aeron_flight.game_preview and obj.type == 'LIGHT':
            obj.data.use_shadow = False
        _select_only(context, obj)
        return {'FINISHED'}


class AERON_OT_move(Operator):
    """Move the list's active item up or down in its order"""

    bl_idname = "aeron.move"
    bl_label = "Move"
    bl_options = {'REGISTER', 'UNDO'}

    target: EnumProperty(items=[('components', "Components", ""), ('children', "Children", "")])
    direction: EnumProperty(items=[('UP', "Up", ""), ('DOWN', "Down", "")])

    def execute(self, context):
        obj = context.object
        if self.target == 'components':
            root = common.model_root(obj)
            items = common.components(root) if root else []
            index = root.aeron_flight.active_component if root else -1
        else:
            items = common.ordered_children(obj) if common.role(obj) == 'component' else []
            index = obj.aeron_flight.active_child
        objects = context.scene.objects
        active = objects[index] if 0 <= index < len(objects) else None
        if active not in items:
            return {'CANCELLED'}
        position = items.index(active)
        other = position - 1 if self.direction == 'UP' else position + 1
        if not 0 <= other < len(items):
            return {'CANCELLED'}
        items[position], items[other] = items[other], items[position]
        _renumber(items)
        return {'FINISHED'}


class AERON_OT_select(Operator):
    """Select this object"""

    bl_idname = "aeron.select"
    bl_label = "Select"
    bl_options = {'REGISTER', 'UNDO'}

    name: StringProperty()

    def execute(self, context):
        obj = context.scene.objects.get(self.name)
        if obj is None:
            return {'CANCELLED'}
        if not obj.visible_get():
            self.report({'WARNING'}, "{} is hidden".format(obj.name))
        _select_only(context, obj)
        return {'FINISHED'}


class AERON_OT_from_cursor(Operator):
    """Set from the 3D cursor, in the component's local space"""

    bl_idname = "aeron.from_cursor"
    bl_label = "From 3D Cursor"
    bl_options = {'REGISTER', 'UNDO'}

    field: EnumProperty(items=[
        ('pivot', "Pivot", "The hinge's pivot at the cursor"),
        ('axes', "Axes", "The hinge's axes from the cursor's rotation: rotation axis its Y, "
                         "direction its Z, up its X"),
        ('target', "Target", "The target at the cursor"),
    ])

    @classmethod
    def poll(cls, context):
        return common.role(context.object) == 'component'

    def execute(self, context):
        obj = context.object
        props = obj.aeron_flight
        to_local = obj.matrix_world.inverted()
        cursor = context.scene.cursor
        if self.field == 'axes':
            basis = to_local.to_3x3() @ cursor.matrix.to_3x3()
            props.up_axis, props.rotation_axis, props.direction_axis = (
                basis.col[axis].normalized() for axis in range(3))
        else:
            setattr(props, self.field, to_local @ cursor.location)
        return {'FINISHED'}


class AERON_OT_descriptor_from_bounds(Operator):
    """Author the descriptor from the bounds of the collision mesh (or the component's mesh), as the game
    computes it when none is authored"""

    bl_idname = "aeron.descriptor_from_bounds"
    bl_label = "From Mesh Bounds"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return common.role(context.object) == 'component'

    def execute(self, context):
        obj = context.object
        source, matrix = obj, None
        for child in obj.children:
            if common.role(child) == 'collision':
                source, matrix = child, common.local_matrix(child)
        corners = [Vector(corner) for corner in source.bound_box]
        if matrix is not None:
            corners = [matrix @ corner for corner in corners]
        low = Vector([min(corner[axis] for corner in corners) for axis in range(3)])
        high = Vector([max(corner[axis] for corner in corners) for axis in range(3)])
        props = obj.aeron_flight
        props.has_descriptor_geometry = True
        props.descriptor_bounds_min, props.descriptor_bounds_max = low, high
        props.descriptor_center = (low + high) * 0.5
        props.descriptor_span = high - low
        return {'FINISHED'}


class AERON_OT_validate(Operator):
    """Check the active object's flight model and list its problems"""

    bl_idname = "aeron.validate"
    bl_label = "Check Model"

    def execute(self, context):
        root = common.model_root(context.active_object)
        if root is None:
            roots = [obj for obj in context.scene.objects if common.role(obj) == 'model']
            root = roots[0] if len(roots) == 1 else None
        problems = validation.validate(root)
        validation.store(context.scene, problems)
        errors = len(problems.errors)
        warnings = len(problems.items) - errors
        if errors:
            self.report({'ERROR'}, "{}: {} error(s), {} warning(s)".format(
                root.name if root else "No model", errors, warnings))
        else:
            self.report({'INFO'}, "{}: no errors, {} warning(s)".format(root.name, warnings))
        return {'FINISHED'}


class AERON_OT_list_edit(Operator):
    """Add or remove an item of the model root's list"""

    bl_idname = "aeron.list_edit"
    bl_label = "Edit List"
    bl_options = {'REGISTER', 'UNDO'}

    target: EnumProperty(items=[
        ('articulations', "Articulation", ""),
        ('articulation_settings', "Setting", ""),
        ('component_sets', "Component Set", ""),
        ('members', "Member", ""),
    ])
    action: EnumProperty(items=[('ADD', "Add", ""), ('REMOVE', "Remove", ""), ('ADD_SELECTED', "Add Selected", "")])

    @classmethod
    def poll(cls, context):
        return common.role(context.object) == 'model'

    def execute(self, context):
        props = context.object.aeron_flight
        if self.target == 'members':
            if not 0 <= props.active_set < len(props.component_sets):
                return {'CANCELLED'}
            owner = props.component_sets[props.active_set]
            items, active = owner.members, "active_member"
        else:
            owner, items = props, getattr(props, self.target)
            active = {'articulations': "active_articulation", 'articulation_settings': "active_setting",
                      'component_sets': "active_set"}[self.target]
        if self.action == 'ADD':
            item = items.add()
            if self.target in {'articulation_settings', 'component_sets'}:
                item.name = "name"
            setattr(owner, active, len(items) - 1)
        elif self.action == 'ADD_SELECTED':
            present = {member.component for member in items}
            for obj in common.ordered(o for o in context.selected_objects if o.parent == context.object and
                                      common.role(o) == 'component' and o not in present):
                items.add().component = obj
            setattr(owner, active, len(items) - 1)
        else:
            index = getattr(owner, active)
            if not 0 <= index < len(items):
                return {'CANCELLED'}
            items.remove(index)
            setattr(owner, active, min(index, len(items) - 1))
        return {'FINISHED'}


classes = (
    AERON_OT_make_flight_model,
    AERON_OT_add,
    AERON_OT_move,
    AERON_OT_select,
    AERON_OT_from_cursor,
    AERON_OT_descriptor_from_bounds,
    AERON_OT_validate,
    AERON_OT_list_edit,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
