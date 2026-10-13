# Definitions shared by the add-on: the AERON_flight_model format as Blender sees it.
#
# Blender is Z-up and glTF is Y-up. A Blender vector (x, y, z) is the glTF
# vector (x, z, -y) in the same local space. Blender's glTF exporter converts
# object transforms and vertices the same way.

import math
import re

EXTENSION_NAME = "AERON_flight_model"
LIGHTS_EXTENSION_NAME = "KHR_lights_punctual"
TAG_PATTERN = re.compile(r"^[a-z0-9._]{1,63}$")
MAX_COMPONENTS = 255
MAX_COMPONENT_ORDINAL = 254

MESH_TYPE_NAMES = (
    "Default", "Main Hull", "Wing", "Fuselage", "Gun Turret", "Small Gun",
    "Engine", "Bridge", "Shield Generator", "Energy Generator", "Launcher",
    "Communication System", "Beam System", "Command Beam", "Docking Platform",
    "Landing Platform", "Hangar", "Cargo Pod", "Miscellaneous Hull", "Antenna",
    "Rotary Wing", "Rotary Gun Turret", "Rotary Launcher",
    "Rotary Communication System", "Rotary Beam System", "Rotary Command Beam",
    "Hatch", "Custom", "Weapon System 1", "Weapon System 2", "Power Regenerator",
    "Reactor",
)

# The hardpoint type that XWA uses as the pilot eye (an index into HARDPOINT_TYPE_NAMES).
COCKPIT_HARDPOINT_TYPE = 31

# XWA's hardpoint types, named after JeremyAnsel.Xwa.Opt.
HARDPOINT_TYPE_NAMES = (
    "None", "Rebel Laser", "Turbo Rebel Laser", "Empire Laser", "Turbo Empire Laser",
    "Ion Cannon", "Turbo Ion Cannon", "Torpedo", "Missile", "Super Rebel Laser",
    "Super Empire Laser", "Super Ion Cannon", "Super Torpedo", "Super Missile",
    "Dumb Bomb", "Fired Bomb", "Mag Pulse", "Turbo Mag Pulse", "Super Mag Pulse",
    "Gunner", "Cockpit Sparks", "Docking Point", "Towing", "Acceleration Start",
    "Acceleration End", "Inside Hangar", "Outside Hangar", "Dock from Big",
    "Dock from Small", "Dock to Big", "Dock to Small", "Cockpit (Pilot Eye)",
    "Engine Glow", "Custom 1", "Custom 2", "Custom 3", "Custom 4", "Custom 5",
    "Custom 6", "Jamming Point",
)


def enum_items(names):
    return [(str(index), name, "Type {} in the game".format(index)) for index, name in enumerate(names)]


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
    """True for a light whose parent is a component, unless the light is excluded."""
    return (obj.type == 'LIGHT' and role(obj.parent) == 'component' and
            not obj.aeron_flight.light_exclude)


def next_order(siblings):
    return max((obj.aeron_flight.order for obj in siblings), default=-1) + 1


def local_matrix(obj):
    """The matrix of `obj` relative to its parent, as the glTF exporter writes it."""
    if obj.parent is None:
        return obj.matrix_world.copy()
    return obj.parent.matrix_world.inverted_safe() @ obj.matrix_world


# ---- lights ------------------------------------------------------------------
#
# KHR_lights_punctual gives the intensity in candela, and Blender gives the light
# power in watts. They are converted the way Blender's glTF exporter does in its
# default lighting mode (683 lm/W). A point light spreads its power over 4 pi sr.
# The intensity of a one-sided emitter straight ahead is its power divided by pi.

WATTS_TO_LUMENS = 683.0
DEFAULT_LIGHT_RADIUS = 0.1   # default radius of points and tubes in the format
PANEL_TUBE_ASPECT = 3.0      # the game draws a panel this many times longer than wide as a one-sided tube
DISK_TO_SQUARE = math.sqrt(math.pi) / 2.0  # from a disk's diameter to the side of a square of the same area


def candela(watts, one_sided):
    return watts * WATTS_TO_LUMENS / (math.pi if one_sided else 4.0 * math.pi)


def watts(cd, one_sided):
    return cd * (math.pi if one_sided else 4.0 * math.pi) / WATTS_TO_LUMENS


def light_one_sided(obj):
    light = obj.data
    return light.type == 'AREA' and (not obj.aeron_flight.light_tube or obj.aeron_flight.light_one_sided)


def light_shape(obj, scale):
    """The shape fields of the format for the model light `obj`, with its local
    `scale` baked into the sizes. Returns None for an unsupported light type."""
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
    """Describes for the UI what the game draws for a model light."""
    shape = light_shape(obj, local_matrix(obj).to_scale())
    if shape is None:
        return "unsupported {} light".format(obj.data.type.lower())
    if shape["shape"] == "point":
        return "sphere, radius {:.3g} m".format(shape.get("radius", DEFAULT_LIGHT_RADIUS))
    if shape["shape"] == "tube":
        return "{}tube {:.3g} m, radius {:.3g} m".format(
            "one-sided " if shape.get("oneSided") else "", shape["length"], shape["radius"])
    size_x, size_y = shape["size"]
    longer, shorter = max(size_x, size_y), min(size_x, size_y)
    if longer >= PANEL_TUBE_ASPECT * shorter:
        return "one-sided tube {:.3g} m".format(longer)
    return "one-sided sphere, radius {:.3g} m".format(math.sqrt(size_x * size_y / math.pi))
