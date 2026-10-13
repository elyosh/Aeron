# The animation of a material as Blender properties: the flipbooks, hull
# damage and energy effects of its AERON_flight_model object
# (../../extensions/AERON_flight_model, ../animated_materials.md), stored
# exactly as the format writes them. Also the energy default of the model
# root. Import and Reload Animations fill them from the .gltf, and the exports
# write them back (extension, energy_default). The add-on does not edit the
# screen of a material, so it keeps it as imported.

import json
import struct

import bpy
from bpy.props import (
    BoolProperty,
    CollectionProperty,
    EnumProperty,
    FloatProperty,
    FloatVectorProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import PropertyGroup

from . import common, libraries

# The game states that can play a flipbook (animated_materials.md, "States").
STATES = (
    ("always", "Always", "Active from the start"),
    ("target", "Target", "The player has a target"),
    ("laser_lock", "Laser Lock", ""),
    ("warhead.locking", "Warhead Locking", ""),
    ("warhead.locked", "Warhead Locked", ""),
    ("threat.laser", "Threat: Laser", ""),
    ("threat.turret", "Threat: Turret", ""),
    ("threat.beam", "Threat: Beam", ""),
    ("threat.missile_locking", "Threat: Missile Locking", ""),
    ("threat.missile_locked", "Threat: Missile Locked", ""),
    ("hull.75", "Hull 75%", "Hull at 75% or less"),
    ("hull.50", "Hull 50%", "Hull at 50% or less"),
    ("hull.25", "Hull 25%", "Hull at 25% or less"),
) + tuple(("cannon.{}".format(i), "Cannon {}".format(i), "") for i in range(1, 9)) + (
    ("broken.cmd", "Broken: CMD", ""),
    ("broken.laser_ion", "Broken: Laser and Ion", ""),
    ("broken.beam_weapon", "Broken: Beam Weapon", ""),
    ("broken.shields", "Broken: Shields", ""),
    ("broken.throttle", "Broken: Throttle", ""),
    ("broken.sensors", "Broken: Sensors", ""),
    ("broken.laser_recharge", "Broken: Laser Recharge", ""),
    ("broken.engine_power", "Broken: Engine Power", ""),
    ("broken.shield_recharge", "Broken: Shield Recharge", ""),
    ("broken.beam_recharge", "Broken: Beam Recharge", ""),
    ("hangar", "Hangar", "The player's craft is in a hangar"),
)
ENERGIES = ("shields", "tractor", "jamming")
# The flipbook layers. A layer is also the image kind of its frames.
LAYERS = (("baseColor", "Base Color", "Frames painted over the base color"),
          ("emissive", "Emissive", "Frames that replace the emissive"))


def _frame_search(self, context, text):
    """Keys of the images in the flipbook library whose kind matches the flipbook layer."""
    flipbook = _owner(self, "flipbooks")
    return libraries.search_keys(context, flipbook.library, flipbook.layer, text) if flipbook else []


def _stage_search(self, context, text):
    damage = self.id_data.aeron_flight.damage
    return libraries.search_keys(context, damage.library, 'damage', text)


def _library_search(self, context, text):
    return libraries.search_libraries(context, text)


def _preview_update(self, context):
    from . import flipbook_preview

    flipbook_preview.schedule()


def _owner(item, collection):
    """The element of `collection` in the material that holds `item`."""
    path = item.path_from_id()
    marker = "aeron_flight.{}[".format(collection)
    if marker not in path:
        return None
    index = int(path.split(marker, 1)[1].split("]", 1)[0])
    return getattr(item.id_data.aeron_flight, collection)[index]


class AeronEnergy(PropertyGroup):
    shields: BoolProperty(name="Shields", description="Show shield hits and shields going down")
    tractor: BoolProperty(name="Tractor", description="Show tractor beams")
    jamming: BoolProperty(name="Jamming", description="Show jamming beams")

    def get(self):
        return [name for name in ENERGIES if getattr(self, name)]

    def set(self, names):
        for name in ENERGIES:
            setattr(self, name, name in names)


class AeronFrame(PropertyGroup):
    key: StringProperty(name="Key", search=_frame_search, search_options={'SUGGESTION'}, update=_preview_update,
                        description="Key of the frame image in the flipbook library")
    seconds: FloatProperty(name="Seconds", default=0.1, min=0.0, precision=3, step=1, update=_preview_update,
                           description="How long the frame shows")


class AeronFlipbook(PropertyGroup):
    state: EnumProperty(name="State", items=STATES, default="always", update=_preview_update,
                        description="The flipbook plays from the moment this game state becomes active")
    layer: EnumProperty(name="Layer", items=LAYERS, default="baseColor", update=_preview_update,
                        description="What the frames change. On each layer, the first flipbook with an active "
                                    "state plays")
    mode: EnumProperty(name="Mode", items=(("loop", "Loop", "Repeat the frames"),
                                           ("once", "Once", "Play once and hold the last frame")),
                       default="loop", update=_preview_update)
    library: StringProperty(name="Library", search=_library_search, search_options={'SUGGESTION'},
                            update=_preview_update, description="Effect library that holds the frames")
    emissive_strength: FloatProperty(name="Strength", default=1.0, min=0.0, soft_max=10.0,
                                     update=_preview_update, description="Brightness of the emissive frames")
    unlit: FloatProperty(name="Unlit", default=0.0, min=0.0, max=1.0, subtype='FACTOR', update=_preview_update,
                         description="Part of the frames drawn unlit, as emission. For screens whose material "
                                     "is unlit")
    frames: CollectionProperty(type=AeronFrame)
    active_frame: IntProperty()


class AeronDamageAlternative(PropertyGroup):
    stage_75: StringProperty(name="75%", search=_stage_search, search_options={'SUGGESTION'},
                             description="Damage image at 75% hull. Empty shows none")
    stage_50: StringProperty(name="50%", search=_stage_search, search_options={'SUGGESTION'},
                             description="Damage image at 50% hull. Empty shows none")
    stage_25: StringProperty(name="25%", search=_stage_search, search_options={'SUGGESTION'},
                             description="Damage image at 25% hull. Empty shows none")
    has_scale: BoolProperty(name="Random Square",
                            description="Show the damage in a square of random size instead of the whole texture")
    scale: FloatVectorProperty(name="Square Size", size=2, default=(0.2, 0.5), min=0.0, max=1.0,
                               description="Smallest and largest side of the square, as a fraction of the texture")
    offset_mode: EnumProperty(
        name="Offset", default='NONE',
        items=(('NONE', "Centered", "Center the square"),
               ('INSIDE', "Inside", "Place the square anywhere within the texture"),
               ('RANGE', "Range", "Move the square by a random offset between Min and Max on each axis")))
    offset_min: FloatVectorProperty(name="Min", size=2, default=(-0.2, -0.2),
                                    description="Smallest x and y offset, as a fraction of the texture")
    offset_max: FloatVectorProperty(name="Max", size=2, default=(0.2, 0.2),
                                    description="Largest x and y offset, as a fraction of the texture")


class AeronDamage(PropertyGroup):
    library: StringProperty(name="Library", search=_library_search, search_options={'SUGGESTION'},
                            description="Effect library that holds the damage images")
    choice: IntProperty(name="Choice", min=0,
                        description="Each object picks one alternative. Materials with the same choice pick the "
                                    "same one")
    placement: IntProperty(name="Placement", min=0,
                           description="Materials with the same placement share the same random square")
    alternatives: CollectionProperty(type=AeronDamageAlternative)
    active_alternative: IntProperty()


def _energy_override_update(self, context):
    """When the override is turned on, start from the default of the model."""
    root = common.model_root(context.object) if self.energy_override and context.object else None
    if root is not None:
        self.energy.set(energy_default(root) or ())


class AeronAnimation:
    """The animation fields of AeronMaterialProperties (properties.py)."""
    # Whether the entry has `flipbooks`. An empty list still replaces the
    # flipbooks of the glTF material.
    flipbooks_given: BoolProperty()
    flipbooks: CollectionProperty(type=AeronFlipbook)
    active_flipbook: IntProperty()
    has_damage: BoolProperty()
    damage: PointerProperty(type=AeronDamage)
    energy_override: BoolProperty(
        name="Own Energy Effects", update=_energy_override_update,
        description="Use energy effects for this material instead of the model's default. The default is set "
                    "on the model root, in Object Properties > Material Animations")
    energy: PointerProperty(type=AeronEnergy)
    # The `screen` of the material's AERON_flight_model object, as JSON. The
    # add-on does not edit screens, so it keeps it as imported.
    screen: StringProperty(options={'HIDDEN'})


# ---- conversions ---------------------------------------------------------------

def has_animation(material):
    props = material.aeron_flight
    return props.flipbooks_given or props.has_damage or props.energy_override


def clear(material):
    from . import flipbook_preview

    flipbook_preview.schedule()
    props = material.aeron_flight
    props.flipbooks.clear()
    props.flipbooks_given = props.has_damage = props.energy_override = False
    props.damage.alternatives.clear()
    props.damage.library = ""
    props.damage.choice = props.damage.placement = 0
    props.energy.set(())


def set_entry(material, entry):
    """Fills the animation of `material` from its file entry. A None entry clears it."""
    clear(material)
    props = material.aeron_flight
    if not entry:
        return
    if "flipbooks" in entry:
        props.flipbooks_given = True
        for book in entry["flipbooks"]:
            item = props.flipbooks.add()
            item.state = book["state"]
            item.layer = book["layer"]
            item.mode = book["mode"]
            item.library = book.get("library", "")
            item.emissive_strength = book.get("emissiveStrength", 1.0)
            item.unlit = book.get("unlit", 0.0)
            for key, seconds in book.get("frames", []):
                frame = item.frames.add()
                frame.key = key
                frame.seconds = seconds
    if "damage" in entry:
        damage = entry["damage"]
        props.has_damage = True
        props.damage.library = damage["library"]
        props.damage.choice = int(damage["choice"])
        props.damage.placement = int(damage["placement"])
        for alternative in damage["alternatives"]:
            item = props.damage.alternatives.add()
            item.stage_75, item.stage_50, item.stage_25 = (stage or "" for stage in alternative["stages"])
            item.has_scale = "scale" in alternative
            if item.has_scale:
                item.scale = alternative["scale"]
            offset = alternative.get("offset")
            item.offset_mode = 'INSIDE' if offset == "inside" else 'RANGE' if offset else 'NONE'
            if isinstance(offset, dict):
                item.offset_min = offset["min"]
                item.offset_max = offset["max"]
    if "energy" in entry:
        props.energy_override = True
        props.energy.set(entry["energy"])


def entry(material):
    """The file entry of `material`, or None when it has no animation."""
    props = material.aeron_flight
    result = {}
    if props.flipbooks_given or len(props.flipbooks):
        books = []
        for book in props.flipbooks:
            out = {"state": book.state, "layer": book.layer, "mode": book.mode}
            if book.library or len(book.frames):
                out["library"] = book.library
            out["frames"] = [[frame.key, frame.seconds] for frame in book.frames]
            out["emissiveStrength"] = book.emissive_strength
            out["unlit"] = book.unlit
            books.append(out)
        result["flipbooks"] = books
    if props.has_damage:
        damage = props.damage
        alternatives = []
        for alternative in damage.alternatives:
            out = {"stages": [alternative.stage_75 or None, alternative.stage_50 or None,
                              alternative.stage_25 or None]}
            if alternative.has_scale:
                out["scale"] = list(alternative.scale)
            if alternative.offset_mode == 'INSIDE':
                out["offset"] = "inside"
            elif alternative.offset_mode == 'RANGE':
                out["offset"] = {"min": list(alternative.offset_min), "max": list(alternative.offset_max)}
            alternatives.append(out)
        result["damage"] = {"library": damage.library, "choice": damage.choice, "placement": damage.placement,
                            "alternatives": alternatives}
    if props.energy_override:
        result["energy"] = props.energy.get()
    return result or None


def entries(names):
    """The file entries of the Blender materials in `names`, for those that have one."""
    result = {}
    for name in names:
        material = bpy.data.materials.get(name)
        value = entry(material) if material is not None else None
        if value is not None:
            result[name] = value
    return result


def shortest(value):
    """Returns `value` with each number written as the shortest decimal that
    reads back as the same 32-bit float, the precision of the properties. Also
    removes the defaults that the format leaves out (emissiveStrength 1,
    unlit 0)."""
    if isinstance(value, dict):
        out = {key: shortest(item) for key, item in value.items()}
        if out.get("emissiveStrength") == 1:
            del out["emissiveStrength"]
        if out.get("unlit") == 0:
            del out["unlit"]
        return out
    if isinstance(value, list):
        return [shortest(item) for item in value]
    if isinstance(value, float):
        single = struct.unpack("f", struct.pack("f", value))[0]
        for digits in range(1, 10):
            text = "%.*g" % (digits, single)
            if struct.unpack("f", struct.pack("f", float(text)))[0] == single:
                number = float(text)
                return int(number) if number.is_integer() else number
    return value


def extension(material):
    """The AERON_flight_model object of `material`, with its animation and
    its kept screen. None when it has neither."""
    result = shortest(entry(material) or {})
    if material.aeron_flight.screen:
        result["screen"] = json.loads(material.aeron_flight.screen)
    return result or None


def energy_default(root):
    """The model's energy default (a list), or None."""
    props = root.aeron_flight
    return props.energy_default.get() if props.energy_default_given else None


def set_energy_default(root, energy):
    props = root.aeron_flight
    props.energy_default_given = energy is not None
    props.energy_default.set(energy or ())


def check(problems, root, materials):
    """Adds the animation problems of `materials` in the model `root` that
    the cooker would refuse: frames or stages without a library, and damage
    without alternatives. When the libraries of the model are loaded, also
    a library, key or image kind that they do not have."""
    loaded = libraries.project_of(root)
    users = {}
    for obj in [root] + list(root.children_recursive):
        for slot in obj.material_slots:
            users.setdefault(slot.material, obj)

    def image(owner, what, library, key, kind):
        if loaded is None or not key:
            return
        images = loaded.libraries.get(library, {}).get("images")
        if images is None:
            problems.error(owner, "{}: no library {} in the project".format(what, library))
        elif key not in images:
            problems.error(owner, "{}: {} is not in library {}".format(what, key, library))
        elif images[key].get("kind") != kind:
            problems.error(owner, "{}: {} is a {} image, not {}".format(what, key, images[key].get("kind"), kind))

    for material in sorted(materials, key=lambda m: m.name):
        props = material.aeron_flight
        owner = users.get(material, root)
        for index, book in enumerate(props.flipbooks):
            what = "material {} flipbook {}".format(material.name, index + 1)
            if len(book.frames) and not book.library:
                problems.error(owner, what + ": its frames need a library")
            for frame in book.frames:
                image(owner, what, book.library, frame.key, book.layer)
                if not frame.key:
                    problems.error(owner, what + ": a frame has no key")
        if props.has_damage:
            what = "material {} hull damage".format(material.name)
            if not props.damage.library:
                problems.error(owner, what + ": it needs a library")
            if not len(props.damage.alternatives):
                problems.error(owner, what + ": it needs at least one alternative")
            for alternative in props.damage.alternatives:
                for key in (alternative.stage_75, alternative.stage_50, alternative.stage_25):
                    image(owner, what, props.damage.library, key, "damage")


def float32(value):
    """Returns `value` with its numbers at 32-bit precision, as in the
    properties and the game, and without the defaults that the canonical form
    leaves out. Used to compare the values of a file with the properties."""
    if isinstance(value, dict):
        out = {key: float32(item) for key, item in value.items()}
        if out.get("emissiveStrength") == 1.0:
            del out["emissiveStrength"]
        if out.get("unlit") == 0.0:
            del out["unlit"]
        return out
    if isinstance(value, list):
        return [float32(item) for item in value]
    if isinstance(value, float) or (isinstance(value, int) and not isinstance(value, bool)):
        return struct.unpack("f", struct.pack("f", value))[0]
    return value


classes = (AeronEnergy, AeronFrame, AeronFlipbook, AeronDamageAlternative, AeronDamage)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
