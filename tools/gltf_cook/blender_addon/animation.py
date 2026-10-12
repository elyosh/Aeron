# A material's animation, as Blender properties: its entry of the model
# animation file (../animated_materials.md), held exactly as the format
# expresses it, and the model root's energy default. Import and Reload
# Animations fill them from the file; the exports build the file's entries
# from them (entry, energy_default) and write it through the cooker.

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

# The game states a flipbook plays on (animated_materials.md, "States").
STATES = (
    ("always", "Always", "From the start"),
    ("target", "Target", "The player has a target"),
    ("laser_lock", "Laser lock", ""),
    ("warhead.locking", "Warhead locking", ""),
    ("warhead.locked", "Warhead locked", ""),
    ("threat.laser", "Threat: laser", ""),
    ("threat.turret", "Threat: turret", ""),
    ("threat.beam", "Threat: beam", ""),
    ("threat.missile_locking", "Threat: missile locking", ""),
    ("threat.missile_locked", "Threat: missile locked", ""),
    ("hull.75", "Hull 75%", "Hull at 75% or less"),
    ("hull.50", "Hull 50%", "Hull at 50% or less"),
    ("hull.25", "Hull 25%", "Hull at 25% or less"),
) + tuple(("cannon.{}".format(i), "Cannon {}".format(i), "") for i in range(1, 9)) + (
    ("broken.cmd", "Broken: CMD", ""),
    ("broken.laser_ion", "Broken: laser and ion", ""),
    ("broken.beam_weapon", "Broken: beam weapon", ""),
    ("broken.shields", "Broken: shields", ""),
    ("broken.throttle", "Broken: throttle", ""),
    ("broken.sensors", "Broken: sensors", ""),
    ("broken.laser_recharge", "Broken: laser recharge", ""),
    ("broken.engine_power", "Broken: engine power", ""),
    ("broken.shield_recharge", "Broken: shield recharge", ""),
    ("broken.beam_recharge", "Broken: beam recharge", ""),
    ("hangar", "Hangar", "The craft is in a hangar"),
)
ENERGIES = ("shields", "tractor", "jamming")
# A flipbook layer, and the image kind its frames have.
LAYERS = (("baseColor", "Base colour", "Frames painted over the base colour"),
          ("emissive", "Emissive", "Frames that replace the emissive"))


def _frame_search(self, context, text):
    """The keys of the frame's flipbook's library, of its layer's kind."""
    flipbook = _owner(self, "flipbooks")
    return libraries.search_keys(context, flipbook.library, flipbook.layer, text) if flipbook else []


def _stage_search(self, context, text):
    damage = self.id_data.aeron_flight.damage
    return libraries.search_keys(context, damage.library, 'damage', text)


def _library_search(self, context, text):
    return libraries.search_libraries(context, text)


def _owner(item, collection):
    """The element of the material's `collection` that holds `item`."""
    path = item.path_from_id()
    marker = "aeron_flight.{}[".format(collection)
    if marker not in path:
        return None
    index = int(path.split(marker, 1)[1].split("]", 1)[0])
    return getattr(item.id_data.aeron_flight, collection)[index]


class AeronEnergy(PropertyGroup):
    shields: BoolProperty(name="Shields", description="Shield hits and shields going down")
    tractor: BoolProperty(name="Tractor", description="Tractor beams")
    jamming: BoolProperty(name="Jamming", description="Jamming beams")

    def get(self):
        return [name for name in ENERGIES if getattr(self, name)]

    def set(self, names):
        for name in ENERGIES:
            setattr(self, name, name in names)


class AeronFrame(PropertyGroup):
    key: StringProperty(name="Key", search=_frame_search, search_options={'SUGGESTION'},
                        description="The frame's image, a key of the flipbook's library")
    seconds: FloatProperty(name="Seconds", default=0.1, min=0.0, precision=3, step=1,
                           description="How long the frame shows")


class AeronFlipbook(PropertyGroup):
    state: EnumProperty(name="State", items=STATES, default="always",
                        description="The game state the flipbook plays on, from the moment it becomes active")
    layer: EnumProperty(name="Layer", items=LAYERS, default="baseColor",
                        description="What the frames change; per layer, the first active flipbook plays")
    mode: EnumProperty(name="Mode", items=(("loop", "Loop", "Repeat"), ("once", "Once", "Hold the last frame")),
                       default="loop")
    library: StringProperty(name="Library", search=_library_search, search_options={'SUGGESTION'},
                            description="The effect library of the frames")
    emissive_strength: FloatProperty(name="Emissive Strength", default=1.0, min=0.0, soft_max=10.0,
                                     description="Brightness of emissive frames")
    unlit: FloatProperty(name="Unlit", default=0.0, min=0.0, max=1.0, subtype='FACTOR',
                         description="Base colour flipbooks: the part of the frames drawn unlit, as emission")
    frames: CollectionProperty(type=AeronFrame)
    active_frame: IntProperty()


class AeronDamageAlternative(PropertyGroup):
    stage_75: StringProperty(name="75%", search=_stage_search, search_options={'SUGGESTION'},
                             description="The damage image at 75% hull (empty: none)")
    stage_50: StringProperty(name="50%", search=_stage_search, search_options={'SUGGESTION'},
                             description="The damage image at 50% hull (empty: none)")
    stage_25: StringProperty(name="25%", search=_stage_search, search_options={'SUGGESTION'},
                             description="The damage image at 25% hull (empty: none)")
    has_scale: BoolProperty(name="Scale", description="Place a random square instead of the whole texture")
    scale: FloatVectorProperty(name="Scale", size=2, default=(0.2, 0.5), min=0.0, max=1.0,
                               description="The square's [min, max] side, a fraction of the texture")
    offset_mode: EnumProperty(
        name="Offset", default='NONE',
        items=(('NONE', "Centred", "The square is centred"),
               ('INSIDE', "Inside", "Anywhere within the texture"),
               ('RANGE', "Range", "Moved by (x1 - x0)·r - x0 per axis, r random in [0, 1)")))
    offset: FloatVectorProperty(name="Range", size=4, default=(-0.2, -0.2, 0.2, 0.2),
                                description="[x0, y0, x1, y1]")


class AeronDamage(PropertyGroup):
    library: StringProperty(name="Library", search=_library_search, search_options={'SUGGESTION'},
                            description="The effect library of the damage images")
    choice: IntProperty(name="Choice", min=0,
                        description="Materials with the same choice pick the same alternative")
    placement: IntProperty(name="Placement", min=0,
                           description="Materials with the same placement share the random placement")
    alternatives: CollectionProperty(type=AeronDamageAlternative)
    active_alternative: IntProperty()


def _energy_override_update(self, context):
    """Overriding starts from the model's default."""
    root = common.model_root(context.object) if self.energy_override and context.object else None
    if root is not None:
        self.energy.set(energy_default(root) or ())


class AeronAnimation:
    """The animation fields of AeronMaterialProperties (properties.py)."""
    # Whether the entry gives `flipbooks` (an empty list still replaces the
    # glTF material's own).
    flipbooks_given: BoolProperty()
    flipbooks: CollectionProperty(type=AeronFlipbook)
    active_flipbook: IntProperty()
    has_damage: BoolProperty()
    damage: PointerProperty(type=AeronDamage)
    energy_override: BoolProperty(
        name="Own Energy Effects", update=_energy_override_update,
        description="Give the material its own energy effects instead of the model's default (set on the "
                    "model root: Object properties > Material Animations)")
    energy: PointerProperty(type=AeronEnergy)


# ---- conversions ---------------------------------------------------------------

def has_animation(material):
    props = material.aeron_flight
    return props.flipbooks_given or props.has_damage or props.energy_override


def clear(material):
    props = material.aeron_flight
    props.flipbooks.clear()
    props.flipbooks_given = props.has_damage = props.energy_override = False
    props.damage.alternatives.clear()
    props.damage.library = ""
    props.damage.choice = props.damage.placement = 0
    props.energy.set(())


def set_entry(material, entry):
    """Fill `material`'s animation from its file entry (None: none)."""
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
            if isinstance(offset, list):
                item.offset = offset
    if "energy" in entry:
        props.energy_override = True
        props.energy.set(entry["energy"])


def entry(material):
    """`material`'s file entry, or None when it has no animation."""
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
                out["offset"] = list(alternative.offset)
            alternatives.append(out)
        result["damage"] = {"library": damage.library, "choice": damage.choice, "placement": damage.placement,
                            "alternatives": alternatives}
    if props.energy_override:
        result["energy"] = props.energy.get()
    return result or None


def entries(names):
    """The file entries of the Blender materials `names` that have one."""
    result = {}
    for name in names:
        material = bpy.data.materials.get(name)
        value = entry(material) if material is not None else None
        if value is not None:
            result[name] = value
    return result


def energy_default(root):
    """The model's energy default (a list), or None."""
    props = root.aeron_flight
    return props.energy_default.get() if props.energy_default_given else None


def set_energy_default(root, energy):
    props = root.aeron_flight
    props.energy_default_given = energy is not None
    props.energy_default.set(energy or ())


def check(problems, root, materials):
    """Adds the problems of `materials`' animations (the model `root`'s) that
    the cooker would refuse: frames or stages without a library, damage
    without alternatives, and, when the model's libraries are loaded, a
    library, key or image kind they do not have."""
    loaded = libraries.tree_of(root)
    users = {}
    for obj in [root] + list(root.children_recursive):
        for slot in obj.material_slots:
            users.setdefault(slot.material, obj)

    def image(owner, what, library, key, kind):
        if loaded is None or not key:
            return
        images = loaded.libraries.get(library, {}).get("images")
        if images is None:
            problems.error(owner, "{}: no library {} in the source tree".format(what, library))
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
                problems.error(owner, what + ": it needs an alternative")
            for alternative in props.damage.alternatives:
                for key in (alternative.stage_75, alternative.stage_50, alternative.stage_25):
                    image(owner, what, props.damage.library, key, "damage")


def float32(value):
    """`value` with its numbers at 32-bit precision (the properties' and the
    game's), and the defaults the canonical form leaves out removed, to
    compare a file's values with the properties'."""
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
