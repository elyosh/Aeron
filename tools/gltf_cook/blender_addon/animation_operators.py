# Operators of the material animation editor (animation.py) and of the
# effect libraries of the project (libraries.py). The animation operators edit
# the material properties, and the exports write them to the material's
# AERON_flight_model object. The library operators write the library files at
# once: <library>.yaml through the cooker, and the PNGs in its frames/ folder.
# A library belongs to the project and models share it, so Blender's undo
# does not revert these files.

import filecmp
import json
import os
import re
import shutil

import bpy
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty, \
    IntProperty, StringProperty
from bpy.types import Operator, OperatorFileListElement
from bpy_extras.io_utils import ImportHelper

from . import common, flipbook_preview, libraries, sources

LIBRARY_NAME = re.compile(r"^[a-z0-9_-]+$")


def _material(context):
    return getattr(context, "material", None)


def _flipbook(context):
    props = _material(context).aeron_flight
    return props.flipbooks[props.active_flipbook] if 0 <= props.active_flipbook < len(props.flipbooks) else None


def _move(collection, index, direction):
    """Moves item `index` up (-1) or down (1) and returns its new index."""
    target = index + direction
    if 0 <= index < len(collection) and 0 <= target < len(collection):
        collection.move(index, target)
        return target
    return index


class _MaterialOperator:
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return _material(context) is not None


class _FlipbookOperator(_MaterialOperator):
    @classmethod
    def poll(cls, context):
        return _material(context) is not None and _flipbook(context) is not None


# ---- flipbooks and frames ------------------------------------------------------

class AERON_OT_flipbook_add(_MaterialOperator, Operator):
    """Add a flipbook: frames from an effect library that play on a game state"""

    bl_idname = "aeron.flipbook_add"
    bl_label = "Add Flipbook"

    def execute(self, context):
        props = _material(context).aeron_flight
        book = props.flipbooks.add()
        book.library = libraries.selected_library(context)
        props.flipbooks_given = True
        props.active_flipbook = len(props.flipbooks) - 1
        flipbook_preview.schedule()
        return {'FINISHED'}


class AERON_OT_flipbook_remove(_FlipbookOperator, Operator):
    """Remove the selected flipbook"""

    bl_idname = "aeron.flipbook_remove"
    bl_label = "Remove Flipbook"

    def execute(self, context):
        props = _material(context).aeron_flight
        props.flipbooks.remove(props.active_flipbook)
        props.active_flipbook = max(min(props.active_flipbook, len(props.flipbooks) - 1), 0)
        props.flipbooks_given = len(props.flipbooks) > 0
        flipbook_preview.schedule()
        return {'FINISHED'}


class AERON_OT_flipbook_move(_FlipbookOperator, Operator):
    """Move the selected flipbook. On each layer, the first flipbook with an active state plays"""

    bl_idname = "aeron.flipbook_move"
    bl_label = "Move Flipbook"
    direction: IntProperty(default=-1)

    def execute(self, context):
        props = _material(context).aeron_flight
        props.active_flipbook = _move(props.flipbooks, props.active_flipbook, self.direction)
        flipbook_preview.schedule()
        return {'FINISHED'}


class AERON_OT_frame_add(_FlipbookOperator, Operator):
    """Add a frame after the selected one"""

    bl_idname = "aeron.frame_add"
    bl_label = "Add Frame"

    def execute(self, context):
        book = _flipbook(context)
        previous = book.frames[book.active_frame] if 0 <= book.active_frame < len(book.frames) else None
        frame = book.frames.add()
        if previous is not None:
            frame.seconds = previous.seconds
            book.frames.move(len(book.frames) - 1, book.active_frame + 1)
            book.active_frame += 1
        else:
            book.active_frame = len(book.frames) - 1
        flipbook_preview.schedule()
        return {'FINISHED'}


class AERON_OT_frame_remove(_FlipbookOperator, Operator):
    """Remove the selected frame"""

    bl_idname = "aeron.frame_remove"
    bl_label = "Remove Frame"

    @classmethod
    def poll(cls, context):
        return super().poll(context) and len(_flipbook(context).frames) > 0

    def execute(self, context):
        book = _flipbook(context)
        book.frames.remove(book.active_frame)
        book.active_frame = max(min(book.active_frame, len(book.frames) - 1), 0)
        flipbook_preview.schedule()
        return {'FINISHED'}


class AERON_OT_frame_move(_FlipbookOperator, Operator):
    """Move the selected frame"""

    bl_idname = "aeron.frame_move"
    bl_label = "Move Frame"
    direction: IntProperty(default=-1)

    def execute(self, context):
        book = _flipbook(context)
        book.active_frame = _move(book.frames, book.active_frame, self.direction)
        flipbook_preview.schedule()
        return {'FINISHED'}


def _append_frames(book, keys, seconds):
    for key in keys:
        frame = book.frames.add()
        frame.key = key
        frame.seconds = seconds
    book.active_frame = len(book.frames) - 1


class AERON_OT_frames_add_prefix(_FlipbookOperator, Operator):
    """Add the images of the flipbook library whose key starts with a prefix, sorted by key"""

    bl_idname = "aeron.frames_add_prefix"
    bl_label = "Add Frames by Prefix"
    prefix: StringProperty(name="Prefix", description="Start of the keys to add. Empty adds all images")
    seconds: FloatProperty(name="Seconds", default=0.1, min=0.0, precision=3, description="Duration of each frame")

    @classmethod
    def poll(cls, context):
        return super().poll(context) and libraries.project(context) is not None

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        book = _flipbook(context)
        keys = [key for key in libraries.search_keys(context, book.library, book.layer, "")
                if key.startswith(self.prefix)]
        if not keys:
            self.report({'ERROR'}, "Library {} has no {} image whose key starts with '{}'".format(
                book.library or "(none)", book.layer, self.prefix))
            return {'CANCELLED'}
        _append_frames(book, keys, self.seconds)
        self.report({'INFO'}, "Added {} frame(s)".format(len(keys)))
        flipbook_preview.schedule()
        return {'FINISHED'}


class AERON_OT_frames_set_duration(_FlipbookOperator, Operator):
    """Give every frame of the selected flipbook the same duration"""

    bl_idname = "aeron.frames_set_duration"
    bl_label = "Set Duration"
    seconds: FloatProperty(name="Seconds", default=0.1, min=0.0, precision=3)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        for frame in _flipbook(context).frames:
            frame.seconds = self.seconds
        flipbook_preview.schedule()
        return {'FINISHED'}


class AERON_OT_frames_add_added(_FlipbookOperator, Operator):
    """Add the images that Add Images last added to a library as frames of the selected flipbook"""

    bl_idname = "aeron.frames_add_added"
    bl_label = "Add Recent Images"
    seconds: FloatProperty(name="Seconds", default=0.1, min=0.0, precision=3, description="Duration of each frame")

    @classmethod
    def poll(cls, context):
        return super().poll(context) and bool(libraries.state(context).added)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        book = _flipbook(context)
        library, keys = json.loads(libraries.state(context).added)
        if book.library and book.library != library:
            self.report({'ERROR'}, "The images are in library {}, but the flipbook uses library {}".format(
                library, book.library))
            return {'CANCELLED'}
        book.library = library
        _append_frames(book, keys, self.seconds)
        flipbook_preview.schedule()
        return {'FINISHED'}


# ---- hull damage and energy ------------------------------------------------------

class AERON_OT_damage_add(_MaterialOperator, Operator):
    """Add hull damage images that show on the material as the hull weakens"""

    bl_idname = "aeron.damage_add"
    bl_label = "Add Hull Damage"

    def execute(self, context):
        props = _material(context).aeron_flight
        props.has_damage = True
        if not len(props.damage.alternatives):
            props.damage.alternatives.add()
        return {'FINISHED'}


class AERON_OT_damage_remove(_MaterialOperator, Operator):
    """Remove the material's hull damage"""

    bl_idname = "aeron.damage_remove"
    bl_label = "Remove Hull Damage"

    def execute(self, context):
        props = _material(context).aeron_flight
        props.has_damage = False
        props.damage.alternatives.clear()
        return {'FINISHED'}


class AERON_OT_damage_alternative_add(_MaterialOperator, Operator):
    """Add a damage alternative. Each object picks one alternative"""

    bl_idname = "aeron.damage_alternative_add"
    bl_label = "Add Alternative"

    def execute(self, context):
        damage = _material(context).aeron_flight.damage
        damage.alternatives.add()
        damage.active_alternative = len(damage.alternatives) - 1
        return {'FINISHED'}


class AERON_OT_damage_alternative_remove(_MaterialOperator, Operator):
    """Remove the selected damage alternative"""

    bl_idname = "aeron.damage_alternative_remove"
    bl_label = "Remove Alternative"

    @classmethod
    def poll(cls, context):
        return super().poll(context) and len(_material(context).aeron_flight.damage.alternatives) > 1

    def execute(self, context):
        damage = _material(context).aeron_flight.damage
        damage.alternatives.remove(damage.active_alternative)
        damage.active_alternative = max(min(damage.active_alternative, len(damage.alternatives) - 1), 0)
        return {'FINISHED'}


# ---- libraries -------------------------------------------------------------------

def _cooker(operator, context):
    from . import preferences

    cooker = preferences.cooker_path(context)
    if not cooker:
        operator.report({'ERROR'}, "Set the aeron_gltf_cook path in the add-on preferences")
    return cooker


def _loaded(operator, context):
    """Returns (project, cooker path) for the active model, and loads the project if needed. Returns
    (None, None) after reporting an error."""
    effects = libraries.effects_dir(context)
    if not effects:
        operator.report({'ERROR'}, "The model is not in a project. Save it to <project>/models/<key>/ "
                                   "first")
        return None, None
    cooker = _cooker(operator, context)
    if not cooker:
        return None, None
    loaded = libraries.project(context)
    try:
        return (loaded or libraries.load(cooker, effects)), cooker
    except (OSError, RuntimeError, ValueError) as error:
        operator.report({'ERROR'}, "Cannot read the libraries: {}".format(error))
        return None, None


def _document(loaded, name):
    return {"version": 1, "library": name, "images": dict(loaded.libraries.get(name, {}).get("images", {}))}


def _write(operator, context, loaded, cooker, name, document, message):
    try:
        path = libraries.write_library(cooker, loaded, name, document)
    except (OSError, RuntimeError, ValueError) as error:
        operator.report({'ERROR'}, "Cannot write library {}: {}".format(name, error))
        return False
    libraries.sync(context)
    operator.report({'INFO'}, "{} (wrote {})".format(message, path))
    return True


def _model_users(context, library, key):
    """Names of the materials of the open model whose animation uses image `key` of `library`."""
    root = common.model_root(context.object) if context.object else None
    names = set()
    for material in sources.model_materials(root) if root else ():
        props = material.aeron_flight
        for book in props.flipbooks:
            if book.library == library and any(frame.key == key for frame in book.frames):
                names.add(material.name)
        if props.has_damage and props.damage.library == library and any(
                key in (alternative.stage_75, alternative.stage_50, alternative.stage_25)
                for alternative in props.damage.alternatives):
            names.add(material.name)
    return sorted(names)


class AERON_OT_libraries_refresh(Operator):
    """Read the effect libraries of the project again, and which models use them"""

    bl_idname = "aeron.libraries_refresh"
    bl_label = "Refresh Libraries"

    @classmethod
    def poll(cls, context):
        return bool(libraries.effects_dir(context))

    def execute(self, context):
        cooker = _cooker(self, context)
        if not cooker:
            return {'CANCELLED'}
        try:
            loaded = libraries.load(cooker, libraries.effects_dir(context))
        except (OSError, RuntimeError, ValueError) as error:
            self.report({'ERROR'}, "Cannot read the libraries: {}".format(error))
            return {'CANCELLED'}
        libraries.sync(context)
        if loaded.errors:
            self.report({'WARNING'}, "Could not read {} file(s): {}".format(len(loaded.errors), loaded.errors[0]))
        else:
            self.report({'INFO'}, "Loaded {} libraries".format(len(loaded.libraries)))
        flipbook_preview.schedule()
        return {'FINISHED'}


def _place(source, library_dir):
    """Returns where the image file `source` goes in the library, as (target,
    relative path). A file inside the library stays where it is, and other
    files go to frames/<file name>. Raises ValueError when the target exists
    with other content."""
    source = os.path.abspath(source)
    if os.path.commonpath([source, os.path.abspath(library_dir)]) == os.path.abspath(library_dir):
        target = source
    else:
        target = os.path.join(library_dir, "frames", os.path.basename(source))
        if os.path.exists(target) and not filecmp.cmp(source, target, shallow=False):
            raise ValueError("{} exists with other content".format(target))
    return target, os.path.relpath(target, library_dir).replace(os.sep, "/")


class AERON_OT_library_add_images(Operator, ImportHelper):
    """Add PNG images to the selected library, keyed by file name, and write the library file at once.
An emissive image takes its base color from <key>_base_color.png"""

    bl_idname = "aeron.library_add_images"
    bl_label = "Add Images"

    filename_ext = ".png"
    filter_glob: StringProperty(default="*.png", options={'HIDDEN'})
    files: CollectionProperty(type=OperatorFileListElement, options={'HIDDEN', 'SKIP_SAVE'})
    directory: StringProperty(subtype='DIR_PATH', options={'HIDDEN', 'SKIP_SAVE'})
    create: BoolProperty(options={'HIDDEN', 'SKIP_SAVE'})
    new_library: StringProperty(name="Library Name", options={'SKIP_SAVE'},
                                description="Name of the new library. Use a-z, 0-9, '_' and '-'")
    kind: EnumProperty(name="Kind", items=libraries.KINDS, default="baseColor")
    max_size: IntProperty(name="Max Size", default=0, min=0, max=1024,
                          description="Largest size of the longer side, in texels. 0 uses the default, 1024")

    @classmethod
    def poll(cls, context):
        return bool(libraries.effects_dir(context))

    @classmethod
    def description(cls, context, properties):
        if properties.create:
            return "Create an effect library from PNG images and write its files at once"
        return cls.__doc__

    def draw(self, context):
        layout = self.layout
        if self.create:
            layout.prop(self, "new_library")
        else:
            layout.label(text="Library: " + (libraries.selected_library(context) or "none selected"))
        layout.prop(self, "kind")
        layout.prop(self, "max_size")

    def execute(self, context):
        loaded, cooker = _loaded(self, context)
        if loaded is None:
            return {'CANCELLED'}
        name = self.new_library if self.create else libraries.selected_library(context)
        if self.create and (not LIBRARY_NAME.match(name) or name in loaded.libraries or
                            os.path.exists(loaded.library_path(name))):
            self.report({'ERROR'}, "Give the new library an unused name of a-z, 0-9, '_' and '-'")
            return {'CANCELLED'}
        if not name:
            self.report({'ERROR'}, "Select a library or name a new one")
            return {'CANCELLED'}
        paths = [os.path.join(self.directory, item.name) for item in self.files if item.name]
        images = {}
        # The base colour of an emissive image is <key>_base_color.png in the same folder.
        inputs = libraries.INPUTS[self.kind]
        for path in sorted(paths):
            stem = os.path.splitext(os.path.basename(path))[0]
            if self.kind == "emissive" and stem.endswith("_base_color"):
                images.setdefault(stem[:-len("_base_color")], {})["baseColor"] = path
            else:
                key = stem[:-len("_emissive")] if self.kind == "emissive" and stem.endswith("_emissive") else stem
                images.setdefault(key, {})[inputs[0]] = path
        lonely = sorted(key for key, files in images.items() if inputs[0] not in files)
        taken = sorted(key for key in images if key in loaded.libraries.get(name, {}).get("images", {}))
        if not images or lonely or taken:
            self.report({'ERROR'}, "No images selected" if not images else
                        "Base color images without an emissive image: " + ", ".join(lonely) if lonely else
                        "Library {} already has these keys: {}".format(name, ", ".join(taken)))
            return {'CANCELLED'}
        library_dir = os.path.dirname(loaded.library_path(name))
        document = _document(loaded, name)
        copies = []
        try:
            for key, files in images.items():
                entry = {"kind": self.kind}
                for field, path in files.items():
                    target, relative = _place(path, library_dir)
                    entry[field] = relative
                    copies.append((path, target))
                if self.max_size:
                    entry["maxSize"] = self.max_size
                document["images"][key] = entry
        except ValueError as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        for source, target in copies:
            if os.path.abspath(source) != os.path.abspath(target):
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copyfile(source, target)
        if not _write(self, context, loaded, cooker, name, document,
                      "Added {} image(s) to {}".format(len(images), name)):
            return {'CANCELLED'}
        libraries.state(context).added = json.dumps([name, sorted(images)])
        libraries.select(context, name, sorted(images)[0])
        flipbook_preview.schedule()
        return {'FINISHED'}


_image_list = []


def _image_items(self, context):
    """The images in Blender, painted ones included. The list stays
    referenced because Blender reads the strings after the call returns."""
    _image_list[:] = [(image.name, image.name, "") for image in bpy.data.images
                      if image.type not in {'RENDER_RESULT', 'COMPOSITING'} and not image.get(flipbook_preview.TAG)
                      ] or [("", "(no image)", "")]
    return _image_list


class AERON_OT_library_add_blender_image(Operator):
    """Save an image painted in Blender as a PNG in the selected library, and add it.
Writes the library file at once"""

    bl_idname = "aeron.library_add_blender_image"
    bl_label = "Add Blender Image"
    image: EnumProperty(name="Image", items=_image_items)
    key: StringProperty(name="Key", description="Key of the image in the library. Empty uses the image name")
    kind: EnumProperty(name="Kind", items=libraries.KINDS, default="baseColor")
    max_size: IntProperty(name="Max Size", default=0, min=0, max=1024,
                          description="Largest size of the longer side, in texels. 0 uses the default, 1024")

    @classmethod
    def poll(cls, context):
        return bool(libraries.effects_dir(context))

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        loaded, cooker = _loaded(self, context)
        image = bpy.data.images.get(self.image)
        name = libraries.selected_library(context)
        if loaded is None or image is None or not name:
            if loaded is not None:
                self.report({'ERROR'}, "Select a library and an image")
            return {'CANCELLED'}
        key = self.key or os.path.splitext(image.name)[0]
        document = _document(loaded, name)
        if key in document["images"]:
            self.report({'ERROR'}, "Library {} already has key {}".format(name, key))
            return {'CANCELLED'}
        library_dir = os.path.dirname(loaded.library_path(name))
        target = os.path.join(library_dir, "frames", key + ".png")
        if os.path.exists(target):
            self.report({'ERROR'}, "{} already exists".format(target))
            return {'CANCELLED'}
        os.makedirs(os.path.dirname(target), exist_ok=True)
        # The image now uses that file, so later repaints are saved with Image > Save.
        image.filepath_raw = target
        image.file_format = 'PNG'
        image.save()
        entry = {"kind": self.kind, libraries.INPUTS[self.kind][0]: "frames/" + key + ".png"}
        if self.max_size:
            entry["maxSize"] = self.max_size
        document["images"][key] = entry
        if not _write(self, context, loaded, cooker, name, document, "Added image {} to {}".format(key, name)):
            return {'CANCELLED'}
        libraries.state(context).added = json.dumps([name, [key]])
        libraries.select(context, name, key)
        flipbook_preview.schedule()
        return {'FINISHED'}


class _ImageOperator:
    @classmethod
    def poll(cls, context):
        return libraries.project(context) is not None and bool(libraries.selected_image(context))


class AERON_OT_library_edit_image(_ImageOperator, Operator):
    """Change the kind, inputs, size cap and crop of the selected library image.
Writes the library file at once"""

    bl_idname = "aeron.library_edit_image"
    bl_label = "Edit Image"
    kind: EnumProperty(name="Kind", items=libraries.KINDS)
    base_color: StringProperty(name="Base Color",
                               description="Path of the base color input, relative to the library. Empty for none")
    emissive: StringProperty(name="Emissive",
                             description="Path of the emissive input, relative to the library. Empty for none")
    factor: StringProperty(name="Factor",
                           description="Path of the damage factor input, relative to the library. Empty for none")
    max_size: IntProperty(name="Max Size", min=0, max=1024,
                          description="Largest size of the longer side, in texels. 0 uses the default, 1024")
    has_crop: BoolProperty(name="Crop",
                           description="Part of the image that the [0, 1] texture range of a blast mark maps to")
    crop: FloatVectorProperty(name="Crop", size=4, default=(0.0, 0.0, 1.0, 1.0))

    def invoke(self, context, event):
        loaded = libraries.project(context)
        entry = loaded.libraries[libraries.selected_library(context)]["images"][libraries.selected_image(context)]
        self.kind = entry["kind"]
        self.base_color = entry.get("baseColor", "")
        self.emissive = entry.get("emissive", "")
        self.factor = entry.get("factor", "")
        self.max_size = entry.get("maxSize", 0)
        self.has_crop = "crop" in entry
        if self.has_crop:
            self.crop = entry["crop"]
        return context.window_manager.invoke_props_dialog(self, width=450)

    def draw(self, context):
        layout = self.layout
        layout.label(text="{} in {}".format(libraries.selected_image(context), libraries.selected_library(context)))
        layout.prop(self, "kind")
        for field, prop in (("baseColor", "base_color"), ("emissive", "emissive"), ("factor", "factor")):
            if field in libraries.INPUTS[self.kind]:
                layout.prop(self, prop)
        layout.prop(self, "max_size")
        layout.prop(self, "has_crop")
        if self.has_crop:
            layout.prop(self, "crop", text="")

    def execute(self, context):
        loaded, cooker = _loaded(self, context)
        if loaded is None:
            return {'CANCELLED'}
        name, key = libraries.selected_library(context), libraries.selected_image(context)
        entry = {"kind": self.kind}
        for field, value in (("baseColor", self.base_color), ("emissive", self.emissive),
                             ("factor", self.factor)):
            if field in libraries.INPUTS[self.kind] and value:
                entry[field] = value
        if self.max_size:
            entry["maxSize"] = self.max_size
        if self.has_crop:
            entry["crop"] = list(self.crop)
        document = _document(loaded, name)
        document["images"][key] = entry
        if not _write(self, context, loaded, cooker, name, document, "Changed image {} in {}".format(key, name)):
            return {'CANCELLED'}
        libraries.select(context, name, key)
        flipbook_preview.schedule()
        return {'FINISHED'}


def _names(names):
    return ", ".join(names[:5]) + (" (+{})".format(len(names) - 5) if len(names) > 5 else "")


def _draw_users(layout, context, library, key):
    loaded = libraries.project(context)
    root = common.model_root(context.object) if context.object else None
    others = loaded.users_of(library, key, exclude=libraries.model_key(root) if root else None)
    here = _model_users(context, library, key)
    if others:
        layout.label(text="Used by other models: " + _names(others), icon='ERROR')
    if here:
        layout.label(text="Used by this model: " + _names(here))
    if not others and not here:
        layout.label(text="Not used")


class AERON_OT_library_remove_image(_ImageOperator, Operator):
    """Remove the selected image from its library. Its PNG files stay in frames/.
Writes the library file at once. Models that use the image lose it"""

    bl_idname = "aeron.library_remove_image"
    bl_label = "Remove Image"

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=450)

    def draw(self, context):
        library, key = libraries.selected_library(context), libraries.selected_image(context)
        self.layout.label(text="Remove {} from {}?".format(key, library))
        _draw_users(self.layout, context, library, key)

    def execute(self, context):
        loaded, cooker = _loaded(self, context)
        if loaded is None:
            return {'CANCELLED'}
        name, key = libraries.selected_library(context), libraries.selected_image(context)
        document = _document(loaded, name)
        if len(document["images"]) == 1:
            self.report({'ERROR'}, "A library needs at least one image. Delete its folder to remove the library")
            return {'CANCELLED'}
        del document["images"][key]
        if not _write(self, context, loaded, cooker, name, document, "Removed image {} from {}".format(key, name)):
            return {'CANCELLED'}
        flipbook_preview.schedule()
        return {'FINISHED'}


class AERON_OT_library_rename_image(_ImageOperator, Operator):
    """Rename the key of the selected image, and its uses in this model. Other models keep the old key.
Writes the library file at once"""

    bl_idname = "aeron.library_rename_image"
    bl_label = "Rename Image"
    new_key: StringProperty(name="New Key")

    def invoke(self, context, event):
        self.new_key = libraries.selected_image(context)
        return context.window_manager.invoke_props_dialog(self, width=450)

    def draw(self, context):
        library, key = libraries.selected_library(context), libraries.selected_image(context)
        self.layout.prop(self, "new_key")
        _draw_users(self.layout, context, library, key)

    def execute(self, context):
        loaded, cooker = _loaded(self, context)
        if loaded is None:
            return {'CANCELLED'}
        name, key = libraries.selected_library(context), libraries.selected_image(context)
        document = _document(loaded, name)
        if not self.new_key or self.new_key in document["images"]:
            self.report({'ERROR'}, "Enter a key that library {} does not have yet".format(name))
            return {'CANCELLED'}
        document["images"] = {self.new_key if k == key else k: v for k, v in document["images"].items()}
        if not _write(self, context, loaded, cooker, name, document,
                      "Renamed image {} in {} to {}".format(key, name, self.new_key)):
            return {'CANCELLED'}
        renamed = 0
        root = common.model_root(context.object) if context.object else None
        for material in sources.model_materials(root) if root else ():
            props = material.aeron_flight
            for book in props.flipbooks:
                for frame in book.frames:
                    if book.library == name and frame.key == key:
                        frame.key = self.new_key
                        renamed += 1
            for alternative in props.damage.alternatives if props.damage.library == name else ():
                for stage in ("stage_75", "stage_50", "stage_25"):
                    if getattr(alternative, stage) == key:
                        setattr(alternative, stage, self.new_key)
                        renamed += 1
        libraries.select(context, name, self.new_key)
        if renamed:
            self.report({'INFO'}, "Renamed {} use(s) in this model. Save the model to its project to write them"
                        .format(renamed))
        flipbook_preview.schedule()
        return {'FINISHED'}


classes = (
    AERON_OT_flipbook_add,
    AERON_OT_flipbook_remove,
    AERON_OT_flipbook_move,
    AERON_OT_frame_add,
    AERON_OT_frame_remove,
    AERON_OT_frame_move,
    AERON_OT_frames_add_prefix,
    AERON_OT_frames_set_duration,
    AERON_OT_frames_add_added,
    AERON_OT_damage_add,
    AERON_OT_damage_remove,
    AERON_OT_damage_alternative_add,
    AERON_OT_damage_alternative_remove,
    AERON_OT_libraries_refresh,
    AERON_OT_library_add_images,
    AERON_OT_library_add_blender_image,
    AERON_OT_library_edit_image,
    AERON_OT_library_remove_image,
    AERON_OT_library_rename_image,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
