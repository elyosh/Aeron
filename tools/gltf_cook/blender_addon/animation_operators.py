# Operators of the material animation editor (animation.py) and of the
# source tree's effect libraries (libraries.py). Animation operators edit the
# material's properties, written to the model's animation file by the source
# export. Library operators write the library's files at once (its
# <library>.yaml through the cooker, and the PNGs of its frames/): a library
# is the tree's, shared by models, so Blender's undo does not revert them.

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

from . import common, libraries, sources

LIBRARY_NAME = re.compile(r"^[a-z0-9_-]+$")


def _material(context):
    return getattr(context, "material", None)


def _flipbook(context):
    props = _material(context).aeron_flight
    return props.flipbooks[props.active_flipbook] if 0 <= props.active_flipbook < len(props.flipbooks) else None


def _move(collection, index, direction):
    """Moves item `index` up (-1) or down (1); returns its new index."""
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
    """Add a flipbook to the material (frames of an effect library played on a game state)"""

    bl_idname = "aeron.flipbook_add"
    bl_label = "Add Flipbook"

    def execute(self, context):
        props = _material(context).aeron_flight
        book = props.flipbooks.add()
        book.library = libraries.selected_library(context)
        props.flipbooks_given = True
        props.active_flipbook = len(props.flipbooks) - 1
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
        return {'FINISHED'}


class AERON_OT_flipbook_move(_FlipbookOperator, Operator):
    """Move the selected flipbook: per layer, the first flipbook whose state is active plays"""

    bl_idname = "aeron.flipbook_move"
    bl_label = "Move Flipbook"
    direction: IntProperty(default=-1)

    def execute(self, context):
        props = _material(context).aeron_flight
        props.active_flipbook = _move(props.flipbooks, props.active_flipbook, self.direction)
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
        return {'FINISHED'}


class AERON_OT_frame_move(_FlipbookOperator, Operator):
    """Move the selected frame"""

    bl_idname = "aeron.frame_move"
    bl_label = "Move Frame"
    direction: IntProperty(default=-1)

    def execute(self, context):
        book = _flipbook(context)
        book.active_frame = _move(book.frames, book.active_frame, self.direction)
        return {'FINISHED'}


def _append_frames(book, keys, seconds):
    for key in keys:
        frame = book.frames.add()
        frame.key = key
        frame.seconds = seconds
    book.active_frame = len(book.frames) - 1


class AERON_OT_frames_add_prefix(_FlipbookOperator, Operator):
    """Add every image of the flipbook's library whose key starts with a prefix, in key order"""

    bl_idname = "aeron.frames_add_prefix"
    bl_label = "Add Frames"
    prefix: StringProperty(name="Prefix", description="The keys to add start with it (empty: all)")
    seconds: FloatProperty(name="Seconds", default=0.1, min=0.0, precision=3, description="Each frame's duration")

    @classmethod
    def poll(cls, context):
        return super().poll(context) and libraries.tree(context) is not None

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        book = _flipbook(context)
        keys = [key for key in libraries.search_keys(context, book.library, book.layer, "")
                if key.startswith(self.prefix)]
        if not keys:
            self.report({'ERROR'}, "library {} has no {} image starting with '{}'".format(
                book.library or "(none)", book.layer, self.prefix))
            return {'CANCELLED'}
        _append_frames(book, keys, self.seconds)
        self.report({'INFO'}, "{} frame(s) added".format(len(keys)))
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
        return {'FINISHED'}


class AERON_OT_frames_add_added(_FlipbookOperator, Operator):
    """Add the images Add Images last added to a library as frames of the selected flipbook"""

    bl_idname = "aeron.frames_add_added"
    bl_label = "Add Frames from Images"
    seconds: FloatProperty(name="Seconds", default=0.1, min=0.0, precision=3, description="Each frame's duration")

    @classmethod
    def poll(cls, context):
        return super().poll(context) and bool(libraries.state(context).added)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        book = _flipbook(context)
        library, keys = json.loads(libraries.state(context).added)
        if book.library and book.library != library:
            self.report({'ERROR'}, "the images are in library {}, the flipbook's frames in {}".format(
                library, book.library))
            return {'CANCELLED'}
        book.library = library
        _append_frames(book, keys, self.seconds)
        return {'FINISHED'}


# ---- hull damage and energy ------------------------------------------------------

class AERON_OT_damage_add(_MaterialOperator, Operator):
    """Give the material hull damage: images that show as the hull weakens"""

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
    """Add a damage alternative (each object picks one)"""

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
        operator.report({'ERROR'}, "the effect libraries need aeron_gltf_cook: see the add-on's preferences")
    return cooker


def _loaded(operator, context):
    """The active model's tree, loaded if it was not; None after a message."""
    effects = libraries.effects_dir(context)
    if not effects:
        operator.report({'ERROR'}, "the model is not in a source tree (export its source into "
                                   "<tree>/models/<key>/ first)")
        return None, None
    cooker = _cooker(operator, context)
    if not cooker:
        return None, None
    loaded = libraries.tree(context)
    try:
        return (loaded or libraries.load(cooker, effects)), cooker
    except (OSError, RuntimeError, ValueError) as error:
        operator.report({'ERROR'}, "reading the libraries: {}".format(error))
        return None, None


def _document(loaded, name):
    return {"version": 1, "library": name, "images": dict(loaded.libraries.get(name, {}).get("images", {}))}


def _write(operator, context, loaded, cooker, name, document, message):
    try:
        path = libraries.write_library(cooker, loaded, name, document)
    except (OSError, RuntimeError, ValueError) as error:
        operator.report({'ERROR'}, "writing library {}: {}".format(name, error))
        return False
    libraries.sync(context)
    operator.report({'INFO'}, "{} (wrote {})".format(message, path))
    return True


def _model_users(context, library, key):
    """The open model's materials whose animation names image `key` of `library`."""
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
    """Read the source tree's effect libraries, and which models use them, again"""

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
            self.report({'ERROR'}, "reading the libraries: {}".format(error))
            return {'CANCELLED'}
        libraries.sync(context)
        if loaded.errors:
            self.report({'WARNING'}, "{} file(s) not read: {}".format(len(loaded.errors), loaded.errors[0]))
        else:
            self.report({'INFO'}, "{} libraries".format(len(loaded.libraries)))
        return {'FINISHED'}


def _place(source, library_dir):
    """Where image file `source` goes in the library: where it is when inside
    it, else frames/<its name>. Returns (target, relative path); raises
    ValueError when the target holds another file."""
    source = os.path.abspath(source)
    if os.path.commonpath([source, os.path.abspath(library_dir)]) == os.path.abspath(library_dir):
        target = source
    else:
        target = os.path.join(library_dir, "frames", os.path.basename(source))
        if os.path.exists(target) and not filecmp.cmp(source, target, shallow=False):
            raise ValueError("{} exists with other content".format(target))
    return target, os.path.relpath(target, library_dir).replace(os.sep, "/")


class AERON_OT_library_add_images(Operator, ImportHelper):
    """Add PNG images to the selected library: copied into its frames/, one image per file, keyed by file name
(an emissive image's base colour: <key>_base_color.png). Writes the library file at once"""

    bl_idname = "aeron.library_add_images"
    bl_label = "Add Images"

    filename_ext = ".png"
    filter_glob: StringProperty(default="*.png", options={'HIDDEN'})
    files: CollectionProperty(type=OperatorFileListElement, options={'HIDDEN', 'SKIP_SAVE'})
    directory: StringProperty(subtype='DIR_PATH', options={'HIDDEN', 'SKIP_SAVE'})
    create: BoolProperty(options={'HIDDEN', 'SKIP_SAVE'})
    new_library: StringProperty(name="Library Name", options={'SKIP_SAVE'},
                                description="The new library's name: a-z, 0-9, '_' and '-'")
    kind: EnumProperty(name="Kind", items=libraries.KINDS, default="baseColor")
    max_size: IntProperty(name="Max Size", default=0, min=0, max=1024,
                          description="Cap of the images' longer side (0: the default, 1024)")

    @classmethod
    def poll(cls, context):
        return bool(libraries.effects_dir(context))

    @classmethod
    def description(cls, context, properties):
        if properties.create:
            return "Create an effect library from PNG images (writes its files at once)"
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
            self.report({'ERROR'}, "a new library needs a new name of a-z, 0-9, '_' and '-'")
            return {'CANCELLED'}
        if not name:
            self.report({'ERROR'}, "select a library, or name a new one")
            return {'CANCELLED'}
        paths = [os.path.join(self.directory, item.name) for item in self.files if item.name]
        images = {}
        # An emissive image's base colour is <key>_base_color.png beside it.
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
            self.report({'ERROR'}, "no image" if not images else
                        "base colour without its emissive image: " + ", ".join(lonely) if lonely else
                        "library {} has these keys already: {}".format(name, ", ".join(taken)))
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
                      "{} image(s) added to {}".format(len(images), name)):
            return {'CANCELLED'}
        libraries.state(context).added = json.dumps([name, sorted(images)])
        libraries.select(context, name, sorted(images)[0])
        return {'FINISHED'}


_image_list = []


def _image_items(self, context):
    """Blender's images, painted ones included (kept referenced: Blender
    reads the strings after the call)."""
    _image_list[:] = [(image.name, image.name, "") for image in bpy.data.images
                      if image.type not in {'RENDER_RESULT', 'COMPOSITING'}] or [("", "(no image)", "")]
    return _image_list


class AERON_OT_library_add_blender_image(Operator):
    """Save an image painted in Blender as a PNG in the selected library's frames/, and add it.
Writes the library file at once"""

    bl_idname = "aeron.library_add_blender_image"
    bl_label = "Add Blender Image"
    image: EnumProperty(name="Image", items=_image_items)
    key: StringProperty(name="Key", description="The image's key (empty: the image's name)")
    kind: EnumProperty(name="Kind", items=libraries.KINDS, default="baseColor")
    max_size: IntProperty(name="Max Size", default=0, min=0, max=1024,
                          description="Cap of the image's longer side (0: the default, 1024)")

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
                self.report({'ERROR'}, "select a library and an image")
            return {'CANCELLED'}
        key = self.key or os.path.splitext(image.name)[0]
        document = _document(loaded, name)
        if key in document["images"]:
            self.report({'ERROR'}, "library {} has key {} already".format(name, key))
            return {'CANCELLED'}
        library_dir = os.path.dirname(loaded.library_path(name))
        target = os.path.join(library_dir, "frames", key + ".png")
        if os.path.exists(target):
            self.report({'ERROR'}, "{} exists".format(target))
            return {'CANCELLED'}
        os.makedirs(os.path.dirname(target), exist_ok=True)
        # The image becomes that file: repaints are saved with Image > Save.
        image.filepath_raw = target
        image.file_format = 'PNG'
        image.save()
        entry = {"kind": self.kind, libraries.INPUTS[self.kind][0]: "frames/" + key + ".png"}
        if self.max_size:
            entry["maxSize"] = self.max_size
        document["images"][key] = entry
        if not _write(self, context, loaded, cooker, name, document, "image {} added to {}".format(key, name)):
            return {'CANCELLED'}
        libraries.state(context).added = json.dumps([name, [key]])
        libraries.select(context, name, key)
        return {'FINISHED'}


class _ImageOperator:
    @classmethod
    def poll(cls, context):
        return libraries.tree(context) is not None and bool(libraries.selected_image(context))


class AERON_OT_library_edit_image(_ImageOperator, Operator):
    """Change the selected library image: its kind, inputs (paths relative to the library), size cap and crop.
Writes the library file at once"""

    bl_idname = "aeron.library_edit_image"
    bl_label = "Edit Image"
    kind: EnumProperty(name="Kind", items=libraries.KINDS)
    base_color: StringProperty(name="Base Colour", description="baseColor input (empty: none)")
    emissive: StringProperty(name="Emissive", description="emissive input (empty: none)")
    factor: StringProperty(name="Factor", description="factor input (empty: none)")
    max_size: IntProperty(name="Max Size", min=0, max=1024, description="0: the default, 1024")
    has_crop: BoolProperty(name="Crop", description="The part of the image a blast mark's [0, 1] maps to")
    crop: FloatVectorProperty(name="Crop", size=4, default=(0.0, 0.0, 1.0, 1.0))

    def invoke(self, context, event):
        loaded = libraries.tree(context)
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
        layout.label(text="{} of {}".format(libraries.selected_image(context), libraries.selected_library(context)))
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
        if not _write(self, context, loaded, cooker, name, document, "image {} of {} changed".format(key, name)):
            return {'CANCELLED'}
        libraries.select(context, name, key)
        return {'FINISHED'}


def _names(names):
    return ", ".join(names[:5]) + (" (+{})".format(len(names) - 5) if len(names) > 5 else "")


def _draw_users(layout, context, library, key):
    loaded = libraries.tree(context)
    root = common.model_root(context.object) if context.object else None
    others = loaded.users_of(library, key, exclude=libraries.model_key(root) if root else None)
    here = _model_users(context, library, key)
    if others:
        layout.label(text="Used by: " + _names(others), icon='ERROR')
    if here:
        layout.label(text="This model: " + _names(here))
    if not others and not here:
        layout.label(text="Not used")


class AERON_OT_library_remove_image(_ImageOperator, Operator):
    """Remove the selected image from its library (its PNG files stay in frames/).
Writes the library file at once; models that use it lose it"""

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
            self.report({'ERROR'}, "a library keeps at least one image: delete its directory to remove it")
            return {'CANCELLED'}
        del document["images"][key]
        if not _write(self, context, loaded, cooker, name, document, "image {} removed from {}".format(key, name)):
            return {'CANCELLED'}
        return {'FINISHED'}


class AERON_OT_library_rename_image(_ImageOperator, Operator):
    """Rename the selected image's key, and this model's frames that use it (not other models').
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
            self.report({'ERROR'}, "the new key must be new to library {}".format(name))
            return {'CANCELLED'}
        document["images"] = {self.new_key if k == key else k: v for k, v in document["images"].items()}
        if not _write(self, context, loaded, cooker, name, document,
                      "image {} of {} renamed {}".format(key, name, self.new_key)):
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
            self.report({'INFO'}, "{} use(s) renamed in this model (written by its source export)".format(renamed))
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
