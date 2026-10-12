# Shared definitions: the AERON_flight_model format as Blender sees it.
#
# Blender is Z-up and glTF Y-up: a Blender vector (x, y, z) is the glTF
# vector (x, z, -y) in the same object's local space (Blender's exporter
# converts object transforms and vertices that way).

import math
import re

EXTENSION_NAME = "AERON_flight_model"
LIGHTS_EXTENSION_NAME = "KHR_lights_punctual"
TAG_PATTERN = re.compile(r"^[a-z0-9._]{1,63}$")
MAX_COMPONENTS = 255
MAX_COMPONENT_ORDINAL = 254

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

# The hardpoint XWA reads as the pilot eye (index in HARDPOINT_TYPE_NAMES).
COCKPIT_HARDPOINT_TYPE = 31

# XWA's hardpoint types (as named by JeremyAnsel.Xwa.Opt).
HARDPOINT_TYPE_NAMES = (
    "None", "Rebel laser", "Turbo rebel laser", "Empire laser", "Turbo empire laser",
    "Ion cannon", "Turbo ion cannon", "Torpedo", "Missile", "Super rebel laser",
    "Super empire laser", "Super ion cannon", "Super torpedo", "Super missile",
    "Dumb bomb", "Fired bomb", "Mag pulse", "Turbo mag pulse", "Super mag pulse",
    "Gunner", "Cockpit sparks", "Docking point", "Towing", "Acceleration start",
    "Acceleration end", "Inside hangar", "Outside hangar", "Dock from big",
    "Dock from small", "Dock to big", "Dock to small", "Cockpit (pilot eye)",
    "Engine glow", "Custom 1", "Custom 2", "Custom 3", "Custom 4", "Custom 5",
    "Custom 6", "Jamming point",
)


def enum_items(names):
    return [(str(index), name, "{} ({})".format(name, index)) for index, name in enumerate(names)]


def parse_tags(text):
    tags = []
    for tag in (part.strip() for part in text.split(",")):
        if tag and tag not in tags:
            tags.append(tag)
    return tags


# ---- axes ------------------------------------------------------------------

def to_gltf(v):
    return [float(v[0]), float(v[2]), -float(v[1])]


def from_gltf(v):
    return (float(v[0]), -float(v[2]), float(v[1]))


def extent_to_gltf(v):
    return [float(v[0]), float(v[2]), float(v[1])]


def extent_from_gltf(v):
    return (float(v[0]), float(v[2]), float(v[1]))


def bounds_to_gltf(bounds_min, bounds_max):
    return ([float(bounds_min[0]), float(bounds_min[2]), -float(bounds_max[1])],
            [float(bounds_max[0]), float(bounds_max[2]), -float(bounds_min[1])])


def bounds_from_gltf(bounds_min, bounds_max):
    return ((float(bounds_min[0]), -float(bounds_max[2]), float(bounds_min[1])),
            (float(bounds_max[0]), -float(bounds_min[2]), float(bounds_max[1])))


# ---- hierarchy ---------------------------------------------------------------

def role(obj):
    return obj.aeron_flight.role if obj is not None else 'none'


def model_root(obj):
    """The model root `obj` belongs to, or None."""
    while obj is not None:
        if role(obj) == 'model':
            return obj
        obj = obj.parent
    return None


def ordered(objects):
    return sorted(objects, key=lambda obj: (obj.aeron_flight.order, obj.name))


def components(root):
    """The root's components in ordinal order."""
    return ordered(child for child in root.children if role(child) == 'component')


def ordered_children(component, roles=('hardpoint', 'engineGlow')):
    return ordered(child for child in component.children if role(child) in roles)


def is_model_light(obj):
    """A Blender light that is a model light: under a component, not excluded."""
    return (obj.type == 'LIGHT' and role(obj.parent) == 'component' and
            not obj.aeron_flight.light_exclude)


def next_order(siblings):
    return max((obj.aeron_flight.order for obj in siblings), default=-1) + 1


def local_matrix(obj):
    """`obj`'s matrix relative to its parent, as the glTF exporter writes it."""
    if obj.parent is None:
        return obj.matrix_world.copy()
    return obj.parent.matrix_world.inverted_safe() @ obj.matrix_world


# ---- lights ------------------------------------------------------------------
#
# KHR_lights_punctual intensity is candela; Blender light power is watts,
# converted as Blender's glTF exporter does (its default lighting mode,
# 683 lm/W): a point light radiates over 4 pi sr, a one-sided emitter's
# intensity straight ahead is its power over pi.

WATTS_TO_LUMENS = 683.0
DEFAULT_LIGHT_RADIUS = 0.1   # the format's default point radius and tube thickness
PANEL_TUBE_ASPECT = 3.0      # the game draws longer panels as one-sided tubes
DISK_TO_SQUARE = math.sqrt(math.pi) / 2.0  # a disk's diameter to the side of the same area


def candela(watts, one_sided):
    return watts * WATTS_TO_LUMENS / (math.pi if one_sided else 4.0 * math.pi)


def watts(cd, one_sided):
    return cd * (math.pi if one_sided else 4.0 * math.pi) / WATTS_TO_LUMENS


def light_one_sided(obj):
    light = obj.data
    return light.type == 'AREA' and (not obj.aeron_flight.light_tube or obj.aeron_flight.light_one_sided)


def light_shape(obj, scale):
    """The format's shape fields of a model light (`obj` a Blender light),
    `scale` its local scale baked into the sizes. None for an unsupported
    light type."""
    light = obj.data
    if light.type == 'POINT':
        shape = {"shape": "point"}
        if light.shadow_soft_size > 0.0:
            shape["radius"] = float(light.shadow_soft_size)
        return shape
    if light.type != 'AREA':
        return None
    size_x = light.size
    size_y = light.size_y if light.shape in {'RECTANGLE', 'ELLIPSE'} else light.size
    if obj.aeron_flight.light_tube:
        shape = {"shape": "tube", "length": float(size_x * scale[0])}
        if obj.aeron_flight.light_one_sided:
            shape["oneSided"] = True
        shape["radius"] = float(0.5 * size_y * scale[1])
        return shape
    if light.shape in {'DISK', 'ELLIPSE'}:
        size_x *= DISK_TO_SQUARE
        size_y *= DISK_TO_SQUARE
    return {"shape": "panel", "size": [float(size_x * scale[0]), float(size_y * scale[1])], "oneSided": True}


def light_punctual(obj, matrix_world):
    """The KHR_lights_punctual light of a model light."""
    light = obj.data
    power = light.energy
    if not light.normalize:
        power *= light.area(matrix_world=matrix_world)
    power *= 2.0 ** light.exposure
    color = list(light.color)
    if light.use_temperature:
        color = [c * t for c, t in zip(color, light.temperature_color)]
    punctual = {
        "type": "point",
        "name": obj.name,
        "color": [float(c) for c in color],
        "intensity": float(candela(power, light_one_sided(obj))),
    }
    if light.use_custom_distance:
        punctual["range"] = float(light.cutoff_distance)
    return punctual


def light_game_shape(obj):
    """What the game draws for a model light, for the UI."""
    shape = light_shape(obj, local_matrix(obj).to_scale())
    if shape is None:
        return "unsupported ({} light)".format(obj.data.type.lower())
    if shape["shape"] == "point":
        return "sphere, radius {:.3g} m".format(shape.get("radius", DEFAULT_LIGHT_RADIUS))
    if shape["shape"] == "tube":
        return "{}tube {:.3g} m, radius {:.3g} m".format(
            "one-sided " if shape.get("oneSided") else "", shape["length"], shape["radius"])
    size_x, size_y = shape["size"]
    longer, shorter = max(size_x, size_y), min(size_x, size_y)
    if longer >= PANEL_TUBE_ASPECT * shorter:
        return "panel {:.3g} x {:.3g} m -> one-sided tube {:.3g} m".format(size_x, size_y, longer)
    return "panel {:.3g} x {:.3g} m -> one-sided sphere, radius {:.3g} m".format(
        size_x, size_y, math.sqrt(size_x * size_y / math.pi))
