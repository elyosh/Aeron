# The projectile models that the overlays draw at weapon hardpoints
# (overlays.py).
#
# The game turns hardpoint type t into projectile object type 279 + t, and each
# of these object types names a row of FlightModels/Equipment0.LST: row n is the
# n-th non-empty line of the file, counting from 0. opt2gltf converts each
# projectile once, from the OPT of the game folder set in the preferences, into
# the add-on's cache folder. It converts it as the original game draws it,
# self-lit with the texture's alpha, which an image can show. An XWA Upgrade
# conversion gets its look from glass materials instead.
#
# Blender's glTF importer is not used: it would add undo steps and change the
# selection. A small reader takes the geometry of opt2gltf's output, and the
# textures are Blender images that nothing in the file uses, so they are not
# saved in the .blend. Loading runs from a timer, never during a redraw.

import hashlib
import os
import subprocess
import tempfile

import bpy
import numpy

from . import preferences, sources

# The Equipment0.LST row of each weapon hardpoint type.
WEAPON_ROWS = {1: 17, 2: 18, 3: 19, 4: 20, 5: 22, 6: 23, 7: 24, 8: 25, 9: 18, 10: 20, 11: 23, 12: 24, 13: 25,
               14: 26, 15: 27, 16: 28, 17: 28, 18: 18}
# The models of these rows in the original game's list, for a game folder without Equipment0.LST.
_ORIGINAL_NAMES = {17: "LaserRebel", 18: "LaserRebelTurbo", 19: "LaserImp", 20: "LaserImpTurbo", 22: "LaserIon",
                   23: "LaserIonTurbo", 24: "ProtonTorpedo", 25: "ConcussionMissile", 26: "SpaceBomb",
                   27: "HeavyRocket", 28: "MagPulse"}

_COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
_TYPES = {5121: numpy.uint8, 5123: numpy.uint16, 5125: numpy.uint32, 5126: numpy.float32}


class Part:
    """One primitive of a projectile model, in Blender's axes, with its tail at
    the origin and its nose along -Y."""

    def __init__(self, positions, uvs, triangles, image_path):
        self.positions = positions  # (N, 3) float32
        self.uvs = uvs  # (N, 2) float32, Blender's image orientation
        self.triangles = triangles  # (M, 3) int32
        self.image_path = image_path  # the PNG drawn on it, or ""


_models = {}  # .gltf path: list of Part, or None when it cannot be read
_converting = {}  # .gltf path: running opt2gltf process
_to_load = set()
_names = {}  # game folder: {row: model name}


def _game_folder(context):
    folder = preferences.get(context).xwau_folder
    return os.path.normpath(bpy.path.abspath(folder)) if folder else ""


def _entry(directory, name):
    try:
        return next((os.path.join(directory, entry) for entry in os.listdir(directory)
                     if entry.lower() == name.lower()), "")
    except OSError:
        return ""


def _row_names(folder):
    """{row: model name} of the game folder's Equipment0.LST."""
    if folder not in _names:
        names = {}
        path = _entry(_entry(folder, "FlightModels"), "Equipment0.LST") if folder else ""
        try:
            with open(path, encoding="latin-1") as stream:
                rows = [line.strip() for line in stream if line.strip()]
            names = {row: os.path.splitext(os.path.basename(line.replace("\\", "/")))[0]
                     for row, line in enumerate(rows)}
        except OSError:
            pass
        _names[folder] = names
    return _names[folder] or _ORIGINAL_NAMES


def _cache_folder(folder):
    key = hashlib.sha1(folder.encode("utf-8")).hexdigest()[:12]
    try:
        base = bpy.utils.extension_path_user(__package__, path="projectiles", create=True)
    except (ValueError, KeyError):  # not installed as an extension
        base = os.path.join(tempfile.gettempdir(), "aeron_projectiles")
    return os.path.join(base, key)


def model(context, hardpoint_type):
    """The parts of the projectile that a weapon hardpoint fires, or None
    while the model is not ready or when there is none. A model that is
    missing is converted or loaded in the background."""
    row = WEAPON_ROWS.get(hardpoint_type)
    folder = _game_folder(context)
    name = _row_names(folder).get(row) if row is not None and folder else ""
    opt = _entry(_entry(folder, "FlightModels"), name + ".opt") if name else ""
    if not opt:
        return None
    out = os.path.join(_cache_folder(folder), name.lower())
    gltf = os.path.join(out, os.path.splitext(os.path.basename(opt))[0] + ".gltf")
    if not os.path.isfile(gltf) and gltf not in _converting and gltf not in _models:
        _convert(context, opt, out, gltf)
    if gltf in _models:
        return _models[gltf]
    if gltf not in _converting:
        _to_load.add(gltf)
        _start()
    return None


def _convert(context, opt, out, gltf):
    opt2gltf = preferences.opt2gltf_path(context)
    if not opt2gltf:
        _models[gltf] = None
        return
    os.makedirs(out, exist_ok=True)
    try:
        _converting[gltf] = subprocess.Popen([opt2gltf, "--stock", opt, out], stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL)
    except OSError:
        _models[gltf] = None
        return
    _start()


def _accessor(document, data, index):
    accessor = document["accessors"][index]
    view = document["bufferViews"][accessor["bufferView"]]
    dtype = numpy.dtype(_TYPES[accessor["componentType"]])
    width = _COMPONENTS[accessor["type"]]
    offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    stride = view.get("byteStride", 0)
    count = accessor["count"]
    if stride and stride != dtype.itemsize * width:
        rows = numpy.ndarray((count, width), dtype=dtype, buffer=data, offset=offset,
                             strides=(stride, dtype.itemsize))
        return numpy.array(rows)
    return numpy.frombuffer(data, dtype=dtype, count=count * width, offset=offset).reshape(count, width)


def _node_matrix(node):
    if "matrix" in node:
        return numpy.array(node["matrix"], dtype=numpy.float64).reshape(4, 4).T
    x, y, z, w = node.get("rotation", (0.0, 0.0, 0.0, 1.0))
    rotation = numpy.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    matrix = numpy.identity(4)
    matrix[:3, :3] = rotation * numpy.array(node.get("scale", (1.0, 1.0, 1.0)))
    matrix[:3, 3] = node.get("translation", (0.0, 0.0, 0.0))
    return matrix


def read(gltf):
    """Reads the parts of a projectile .gltf written by opt2gltf."""
    folder = os.path.dirname(gltf)
    document = sources.read_document(gltf)
    with open(os.path.join(folder, document["buffers"][0]["uri"]), "rb") as stream:
        data = stream.read()
    parts = []

    def visit(index, parent):
        node = document["nodes"][index]
        matrix = parent @ _node_matrix(node)
        if "mesh" in node:
            for primitive in document["meshes"][node["mesh"]]["primitives"]:
                attributes = primitive["attributes"]
                positions = _accessor(document, data, attributes["POSITION"]).astype(numpy.float64)
                positions = positions @ matrix[:3, :3].T + matrix[:3, 3]
                # glTF's X, Y and Z are Blender's X, Z and -Y.
                positions = numpy.stack((positions[:, 0], -positions[:, 2], positions[:, 1]), axis=1)
                if "TEXCOORD_0" in attributes:
                    uvs = _accessor(document, data, attributes["TEXCOORD_0"]).astype(numpy.float32)
                    uvs = numpy.stack((uvs[:, 0], 1.0 - uvs[:, 1]), axis=1)  # glTF's V goes down
                else:
                    uvs = numpy.zeros((len(positions), 2), dtype=numpy.float32)
                if "indices" in primitive:
                    triangles = _accessor(document, data, primitive["indices"]).astype(numpy.int32).reshape(-1, 3)
                else:
                    triangles = numpy.arange(len(positions), dtype=numpy.int32).reshape(-1, 3)
                image_path = ""
                material = document.get("materials", [])[primitive["material"]] if "material" in primitive else {}
                # Projectiles are self-lit: the emissive texture is what the game shows.
                texture = (material.get("emissiveTexture") or
                           material.get("pbrMetallicRoughness", {}).get("baseColorTexture"))
                if texture is not None:
                    image = document["images"][document["textures"][texture["index"]]["source"]]
                    image_path = os.path.join(folder, image.get("uri", ""))
                parts.append(Part(positions, uvs, triangles, image_path))
        for child in node.get("children", []):
            visit(child, matrix)

    for index in document["scenes"][document.get("scene", 0)]["nodes"]:
        visit(index, numpy.identity(4))
    # The nose is at the origin and the tail toward +Y. Move the tail to the
    # origin, so that the projectile leaves its hardpoint.
    tail = max((float(part.positions[:, 1].max()) for part in parts if len(part.positions)), default=0.0)
    for part in parts:
        part.positions = (part.positions - (0.0, tail, 0.0)).astype(numpy.float32)
    return parts


def image(path):
    """The Blender image of a projectile texture, or None while it is not loaded."""
    path = os.path.normpath(path)
    return next((image for image in bpy.data.images
                 if image.filepath and os.path.normpath(bpy.path.abspath(image.filepath)) == path), None)


def _load(gltf):
    try:
        parts = read(gltf)
    except (OSError, ValueError, KeyError, IndexError) as error:
        print("Aeron: cannot read the projectile model {}: {}".format(gltf, error))
        _models[gltf] = None
        return
    for part in parts:
        if part.image_path and image(part.image_path) is None and os.path.isfile(part.image_path):
            bpy.data.images.load(part.image_path, check_existing=True)
    _models[gltf] = parts


def _work():
    changed = False
    for gltf, process in list(_converting.items()):
        if process.poll() is None:
            continue
        del _converting[gltf]
        if process.returncode == 0 and os.path.isfile(gltf):
            _to_load.add(gltf)
        else:
            _models[gltf] = None
            changed = True
    while _to_load:
        _load(_to_load.pop())
        changed = True
    if changed:
        from . import overlays  # overlays imports this module
        overlays.tag_redraw()
    return 0.25 if _converting else None


def _start():
    if not bpy.app.timers.is_registered(_work):
        bpy.app.timers.register(_work, first_interval=0.0)


def missing_images():
    """Whether a loaded model lost an image, for example to Purge. Its model
    is then loaded again."""
    for gltf, parts in list(_models.items()):
        if parts and any(part.image_path and image(part.image_path) is None and os.path.isfile(part.image_path)
                         for part in parts):
            del _models[gltf]
            return True
    return False


def clear():
    _models.clear()
    _names.clear()
    _to_load.clear()
    for process in _converting.values():
        process.kill()
    _converting.clear()
    if bpy.app.timers.is_registered(_work):
        bpy.app.timers.unregister(_work)


def is_weapon(hardpoint_type):
    return hardpoint_type in WEAPON_ROWS

