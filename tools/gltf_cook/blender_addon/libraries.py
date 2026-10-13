# The effect libraries of a project (../animated_materials.md). The cooker reads
# them so that frames can be picked, and writes them when the add-on edits them.
# They are cached per project for the session. Libraries are shared by models, so
# the material animations of all the models in the project are read too, to know
# which models use which keys. A model belongs to the project that holds its .gltf
# (sources.project_effects).

import concurrent.futures
import glob
import json
import os
import subprocess

import bpy
import bpy.utils.previews
from bpy.props import CollectionProperty, IntProperty, PointerProperty, StringProperty
from bpy.types import PropertyGroup

from . import common, sources

# For each kind, the input whose image is shown as the thumbnail.
SHOWN_INPUT = {"baseColor": "baseColor", "emissive": "emissive", "damage": "factor", "mark": "baseColor",
               "energy": "emissive"}
# The inputs of each kind: required, then optional.
INPUTS = {"baseColor": ("baseColor",), "emissive": ("emissive", "baseColor"), "damage": ("factor",),
          "mark": ("baseColor",), "energy": ("emissive",)}
KINDS = (("baseColor", "Base Color", "Painted over the base color, using its alpha"),
         ("emissive", "Emissive",
          "Replaces the emission. An optional base color image is painted over the base color"),
         ("damage", "Damage", "Hull damage. Multiplies the base color, and its alpha dulls the surface"),
         ("mark", "Mark", "Blast mark, for the game's own effects"),
         ("energy", "Energy", "Energy effect, for the game's own effects"))

_projects = {}
_previews = None


class Project:
    """The libraries of a project, as {name: {"path": file, "images": {key: entry}}},
    and the models that use their keys, as {library: {key: {model}}}."""

    def __init__(self, effects):
        self.effects = effects
        self.libraries = {}
        self.users = {}
        self.errors = []

    def library_path(self, name):
        return os.path.join(self.effects, name, name + ".yaml")

    def models_using(self, library):
        return set().union(*self.users.get(library, {}).values())

    def users_of(self, library, key, exclude=None):
        return sorted(self.users.get(library, {}).get(key, set()) - {exclude})


def effects_dir(context):
    """The effects folder of the project that holds the model of the active object, or "" if there is none."""
    root = common.model_root(context.object) if context.object else None
    return sources.project_effects(bpy.path.abspath(root.aeron_flight.source_path)) if root else ""


def model_key(root):
    """The key of the model in its project, which is the name of its folder."""
    return os.path.basename(os.path.dirname(bpy.path.abspath(root.aeron_flight.source_path)))


def project(context):
    """The loaded project of the model of the active object, or None."""
    return _projects.get(effects_dir(context))


def project_of(root):
    """The loaded project of model `root`, or None."""
    effects = sources.project_effects(bpy.path.abspath(root.aeron_flight.source_path))
    return _projects.get(effects) if effects else None


def _run(cooker, argv, stdin=None):
    completed = subprocess.run([cooker] + argv, input=stdin, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError((completed.stderr or "").strip() or "aeron_gltf_cook failed (exit code {})".format(
            completed.returncode))
    return completed.stdout


def _read_library(cooker, path):
    return json.loads(_run(cooker, ["--library-json", path]))


def _model_users(path):
    """The keys used by the materials of the model .gltf `path`, as {library: {key}}."""
    names = {}
    for entry in sources.material_extensions(sources.read_document(path)).values():
        for book in entry.get("flipbooks", []):
            names.setdefault(book.get("library", ""), set()).update(key for key, _ in book.get("frames", []))
        damage = entry.get("damage")
        if damage:
            names.setdefault(damage["library"], set()).update(
                stage for alternative in damage["alternatives"] for stage in alternative["stages"] if stage)
    return names


def load(cooker, effects):
    """Reads the libraries and models of the project, or reads them again."""
    result = Project(effects)
    files = sorted(path for path in glob.glob(os.path.join(effects, "*", "*.yaml"))
                   if os.path.basename(path) == os.path.basename(os.path.dirname(path)) + ".yaml")
    models = sorted(path for path in glob.glob(os.path.join(os.path.dirname(effects), "models", "*", "*.gltf"))
                    if os.path.basename(path) == os.path.basename(os.path.dirname(path)) + ".gltf")
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        library_jobs = {pool.submit(_read_library, cooker, path): path for path in files}
        model_jobs = {pool.submit(_model_users, path): path for path in models}
        for job, path in library_jobs.items():
            try:
                document = job.result()
                result.libraries[document["library"]] = {"path": path, "images": document["images"]}
            except (OSError, RuntimeError, ValueError) as error:
                result.errors.append("{}: {}".format(path, error))
        for job, path in model_jobs.items():
            model = os.path.basename(os.path.dirname(path))
            try:
                for library, keys in job.result().items():
                    for key in keys:
                        result.users.setdefault(library, {}).setdefault(key, set()).add(model)
            except (OSError, RuntimeError, ValueError) as error:
                result.errors.append("{}: {}".format(path, error))
    _projects[effects] = result
    clear_icons()
    return result


def update_model(effects, model, path):
    """Reads the .gltf `path` of `model` again after it was written."""
    loaded = _projects.get(effects)
    if loaded is None:
        return
    for keys in loaded.users.values():
        for models in keys.values():
            models.discard(model)
    if os.path.exists(path):
        for library, keys in _model_users(path).items():
            for key in keys:
                loaded.users.setdefault(library, {}).setdefault(key, set()).add(model)


def write_library(cooker, loaded, name, document):
    """Writes the file of library `name` in its canonical form through the cooker,
    which checks it, and reads it back. Raises RuntimeError."""
    path = loaded.library_path(name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    _run(cooker, ["--library-from-json", "-", path], json.dumps(document))
    written = _read_library(cooker, path)
    loaded.libraries[name] = {"path": path, "images": written["images"]}
    clear_icons()
    return path


# ---- pickers and thumbnails ------------------------------------------------------

def search_libraries(context, text):
    loaded = project(context)
    return sorted(name for name in loaded.libraries if text.lower() in name) if loaded else []


def search_keys(context, library, kind, text):
    """The keys of `library` whose image is of `kind`, containing `text`."""
    loaded = project(context)
    images = loaded.libraries.get(library, {}).get("images", {}) if loaded else {}
    return sorted(key for key, image in images.items() if image.get("kind") == kind and text in key)


def image_file(loaded, library, key):
    """The PNG that shows image `key` of `library`, or ""."""
    entry = loaded.libraries.get(library, {}).get("images", {}).get(key)
    if entry is None:
        return ""
    name = entry.get(SHOWN_INPUT.get(entry.get("kind"), ""))
    return os.path.join(os.path.dirname(loaded.libraries[library]["path"]), name) if name else ""


def icon(context, library, key):
    """The icon id of the thumbnail of image `key` in `library`, or 0 if there is
    none. The thumbnail is loaded the first time it is drawn."""
    global _previews
    loaded = project(context)
    path = image_file(loaded, library, key) if loaded else ""
    if not path or not os.path.isfile(path):
        return 0
    if _previews is None:
        _previews = bpy.utils.previews.new()
    name = library + "/" + key
    preview = _previews.get(name) or _previews.load(name, path, 'IMAGE')
    return preview.icon_id


def clear_icons():
    if _previews is not None:
        _previews.clear()


# ---- lists shown in the panels (session data, never saved) -----------------------

class AeronLibraryItem(PropertyGroup):
    images: IntProperty()
    models: IntProperty()


class AeronLibraryImageItem(PropertyGroup):
    kind: StringProperty()


def _active_library_update(self, context):
    sync(context)


class AeronLibrariesState(PropertyGroup):
    libraries: CollectionProperty(type=AeronLibraryItem)
    active_library: IntProperty(update=_active_library_update)
    images: CollectionProperty(type=AeronLibraryImageItem)
    active_image: IntProperty()
    # The keys that Add Images added last, as JSON [library, [keys]], for Add
    # Frames from Images.
    added: StringProperty()


def state(context):
    return context.window_manager.aeron_libraries


def selected_library(context):
    current = state(context)
    return current.libraries[current.active_library].name if 0 <= current.active_library < len(
        current.libraries) else ""


def selected_image(context):
    current = state(context)
    return current.images[current.active_image].name if 0 <= current.active_image < len(current.images) else ""


def select(context, library, key=None):
    """Selects `library` (and image `key`) in the lists."""
    current = state(context)
    for index, item in enumerate(current.libraries):
        if item.name == library:
            current.active_library = index
    sync(context)
    for index, item in enumerate(current.images):
        if item.name == key:
            current.active_image = index


def sync(context):
    """Fills the lists from the active model's loaded project."""
    current = state(context)
    loaded = project(context)
    names = sorted(loaded.libraries) if loaded else []
    if [item.name for item in current.libraries] != names:
        selected = selected_library(context)
        current.libraries.clear()
        for name in names:
            current.libraries.add().name = name
        current["active_library"] = names.index(selected) if selected in names else 0
    for item in current.libraries:
        item.images = len(loaded.libraries[item.name]["images"])
        item.models = len(loaded.models_using(item.name))
    library = selected_library(context)
    images = loaded.libraries.get(library, {}).get("images", {}) if loaded else {}
    if [item.name for item in current.images] != sorted(images):
        current.images.clear()
        for key in sorted(images):
            item = current.images.add()
            item.name = key
    for item in current.images:
        item.kind = images[item.name].get("kind", "")
    current["active_image"] = min(current.active_image, max(len(current.images) - 1, 0))


classes = (AeronLibraryItem, AeronLibraryImageItem, AeronLibrariesState)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.WindowManager.aeron_libraries = PointerProperty(type=AeronLibrariesState)


def unregister():
    global _previews
    del bpy.types.WindowManager.aeron_libraries
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
    if _previews is not None:
        bpy.utils.previews.remove(_previews)
        _previews = None
    _projects.clear()
