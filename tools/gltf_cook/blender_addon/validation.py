# Checks a flight model against the rules of the format and of the game's
# loader. Each problem is listed with the object it concerns. Transforms are
# checked as the glTF exporter writes them, relative to the world matrix of
# the parent.

from . import animation, common, hangar_lighting, skins, sources, variants

EPSILON = 1.0e-5


class Problems:
    def __init__(self):
        self.items = []  # (object or None, message, warning)

    def error(self, obj, message):
        self.items.append((obj, message, False))

    def warning(self, obj, message):
        self.items.append((obj, message, True))

    @property
    def errors(self):
        return [item for item in self.items if not item[2]]


def _scale(obj):
    return common.local_matrix(obj).to_scale()


def _uniform_positive(obj):
    s = _scale(obj)
    return (common.local_matrix(obj).determinant() > 0.0 and s.x > 0.0 and
            abs(s.x - s.y) <= EPSILON * s.x and abs(s.x - s.z) <= EPSILON * s.x)


def _animated(obj):
    return obj.animation_data is not None and obj.animation_data.action is not None


def _check_tag(problems, obj, what, text, optional=False):
    if optional and not text:
        return
    if not common.TAG_PATTERN.match(text):
        problems.error(obj, "{} {!r}: use 1-63 lower-case letters, digits, '.' or '_'".format(what, text))


def _check_reference(problems, root, what, ref):
    if ref.component is not None and (ref.component.parent != root or common.role(ref.component) != 'component'):
        problems.error(root, "{}: {} is not a component of this model".format(what, ref.component.name))


def _check_component(problems, component):
    props = component.aeron_flight
    if component.type != 'MESH':
        problems.error(component, "a component must be a mesh")
        return
    if not _uniform_positive(component):
        problems.error(component, "a component needs a positive uniform scale (apply the scale)")
    if component.data.shape_keys or component.find_armature() or _animated(component):
        problems.error(component, "a component cannot use shape keys, an armature or animation")
    for tag in common.parse_tags(props.tags):
        _check_tag(problems, component, "tag", tag)
    if props.has_descriptor_geometry and any(
            props.descriptor_bounds_min[axis] > props.descriptor_bounds_max[axis] for axis in range(3)):
        problems.error(component, "custom bounds: min is greater than max")
    collisions = 0
    for child in component.children:
        role = common.role(child)
        if child.type == 'LIGHT':
            _check_light(problems, child)
            continue
        if role == 'none':
            problems.warning(child, "no role under component {}, so it is not exported".format(component.name))
            continue
        if _animated(child):
            problems.error(child, "flight model objects cannot be animated")
        if role == 'collision':
            collisions += 1
            if collisions == 2:
                problems.error(component, "more than one collision mesh")
            if child.type != 'MESH' or child.children:
                problems.error(child, "a collision mesh must be a mesh without children")
            elif child.data.shape_keys or child.find_armature():
                problems.error(child, "a collision mesh cannot use shape keys or an armature")
            elif child.data.users > 1:
                problems.error(child, "a collision mesh cannot share its mesh data")
            if not _uniform_positive(child):
                problems.error(child, "a collision mesh needs a positive uniform scale")
        elif role in {'hardpoint', 'engineGlow'}:
            if child.type != 'EMPTY' or child.children:
                problems.error(child, "hardpoints and engine glows must be Empties without children")
            matrix = common.local_matrix(child)
            if role == 'hardpoint':
                rotation, scale = matrix.to_quaternion(), matrix.to_scale()
                if (abs(abs(rotation.w) - 1.0) > EPSILON or
                        any(abs(value - 1.0) > EPSILON for value in scale)):
                    problems.error(child, "a hardpoint has only a position. Clear its rotation and scale")
            elif min(abs(value) for value in matrix.to_scale()) <= EPSILON:
                problems.error(child, "an engine glow needs a nonzero scale. X and Z set its size, "
                                      "Y its length")
        else:
            problems.error(child, "a {} cannot be under a component".format(role))


def _check_light(problems, obj):
    if obj.aeron_flight.light_exclude:
        return
    light = obj.data
    if light.type not in {'POINT', 'AREA'}:
        problems.error(obj, "{} lights are not supported. Use a point or area light, or exclude "
                            "the light".format(light.type.capitalize()))
        return
    if obj.children:
        problems.error(obj, "a model light cannot have children")
    if _animated(obj):
        problems.error(obj, "flight model objects cannot be animated")
    scale = _scale(obj)
    if min(scale) <= EPSILON or common.local_matrix(obj).determinant() <= 0.0:
        problems.error(obj, "a light needs a positive scale (not mirrored)")
        return
    if not light.energy > 0.0:
        problems.error(obj, "the light needs a positive power")
    shape = common.light_shape(obj, scale)
    sizes = [shape.get("length", 1.0), shape.get("radius", 1.0)] + shape.get("size", [])
    if light.type == 'AREA' and min(sizes) <= 0.0:
        problems.error(obj, "the light's size must be positive")
    if light.use_custom_distance and not light.cutoff_distance > 0.0:
        problems.error(obj, "the custom distance (range) must be positive")


def validate(root):
    problems = Problems()
    if root is None:
        problems.error(None, "No model root. Select an object of the model, or create a model")
        return problems
    if root.type != 'EMPTY' or root.parent is not None:
        problems.error(root, "the model root must be an Empty without a parent")
    if not _uniform_positive(root):
        problems.error(root, "the model root needs a positive uniform scale")
    if _animated(root):
        problems.error(root, "flight model objects cannot be animated")
    if root.aeron_flight.preview_active:
        problems.error(root, "the articulation preview has turned components. Reset it in Articulation Preview")
    parts = common.components(root)
    if not parts:
        problems.error(root, "the model has no components")
    elif len(parts) > common.MAX_COMPONENTS:
        problems.error(root, "the model has {} components, more than the limit of {}".format(
            len(parts), common.MAX_COMPONENTS))
    for child in root.children:
        role = common.role(child)
        if role == 'none':
            problems.warning(child, "no role under the model root, so it is not exported")
        elif role not in {'component', 'environmentProbe'}:
            problems.error(child, "a {} must be under a component, not the model root".format(role))
    for component in parts:
        _check_component(problems, component)
    cockpits = [obj for component in parts for obj in common.ordered_children(component, ('hardpoint',))
                if int(obj.aeron_flight.hardpoint_type) == common.COCKPIT_HARDPOINT_TYPE]
    for obj in cockpits[1:]:
        problems.error(obj, "a model can have only one pilot eye hardpoint (type {})".format(
            common.COCKPIT_HARDPOINT_TYPE))

    props = root.aeron_flight
    for index, articulation in enumerate(props.articulations):
        what = "articulation {}".format(index + 1)
        _check_tag(problems, root, what + " group", articulation.group)
        _check_tag(problems, root, what + " behavior", articulation.behavior, optional=True)
        _check_reference(problems, root, what, articulation)
    names = set()
    for setting in props.articulation_settings:
        _check_tag(problems, root, "articulation setting", setting.name)
        if setting.name in names:
            problems.error(root, "articulation setting {!r} is repeated".format(setting.name))
        names.add(setting.name)
    names = set()
    for component_set in props.component_sets:
        _check_tag(problems, root, "component set", component_set.name)
        if component_set.name in names:
            problems.error(root, "component set {!r} is repeated".format(component_set.name))
        names.add(component_set.name)
        for member in component_set.members:
            _check_reference(problems, root, "component set {!r}".format(component_set.name), member)

    sources.check_markings(problems, root)
    skins.check(problems, root)
    variants.check(problems, root)
    hangar_lighting.check(problems, root)
    animation.check(problems, root, sources.model_materials(root))

    for obj in root.children_recursive:
        if common.role(obj) in {'model'}:
            problems.error(obj, "a model root cannot be inside another model")
    return problems


def store(scene, problems):
    """Stores the problems on the scene for the Problems list."""
    items = scene.aeron_flight.problems
    items.clear()
    for obj, message, warning in problems.items:
        item = items.add()
        item.message = message
        item.object = obj
        item.warning = warning
