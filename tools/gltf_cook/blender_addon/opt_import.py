# Import of an X-Wing Alliance OPT: opt2gltf converts it into a project (the
# model's folder and, for an XWA Upgrade model, the effect library images
# its materials use), then the model's .gltf is imported like any other.
#
# The OPT browser asks only for the OPT and the project, chosen from the
# known ones. A new project folder, and the confirmation to replace a model,
# are asked once the browser has closed: Blender opens one file browser at
# a time.

import os
import subprocess

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty
from bpy.types import FileHandler, Operator
from bpy_extras.io_utils import ImportHelper

from . import common, libraries, preferences, sources

RECENT_PROJECTS = 5


def _entry(directory, name):
    """`directory`'s entry named `name` (any case), or ""."""
    try:
        entries = os.listdir(directory)
    except OSError:
        return ""
    return next((os.path.join(directory, entry) for entry in entries if entry.lower() == name.lower()), "")


def installation(opt_path):
    """The game folder holding the OPT's FlightModels folder, or ""."""
    folder = os.path.dirname(opt_path)
    return os.path.dirname(folder) if os.path.basename(folder).lower() == "flightmodels" else ""


def has_xwau(folder):
    """Whether `folder` is an XWA Upgrade installation: it has a Materials
    folder."""
    return bool(folder) and os.path.isdir(_entry(folder, "Materials"))


def xwau_installation(context, opt_path):
    """The XWA Upgrade installation to read the OPT's data from, or "": the
    OPT's own installation when it is in one (with or without XWA Upgrade),
    else the preferences' one."""
    own = installation(opt_path)
    if own:
        return os.path.normpath(own) if has_xwau(own) else ""
    folder = preferences.get(context).xwau_folder
    folder = os.path.normpath(bpy.path.abspath(folder)) if folder else ""
    return folder if has_xwau(folder) else ""


def _tail(text, count=12):
    return "\n".join((text or "").rstrip().splitlines()[-count:])


def _key(opt):
    return os.path.splitext(os.path.basename(opt))[0].lower()


def project_root(folder):
    """The project `folder` is in when it is a project's models or effects
    folder, or a model's folder; else `folder`."""
    folder = os.path.normpath(folder)
    parent = os.path.dirname(folder)
    if os.path.basename(folder) in ("models", "effects"):
        return parent
    if os.path.basename(parent) == "models":
        return os.path.dirname(parent)
    return folder


def projects(context):
    """The existing projects an import can go into, best first: those of the
    scene's models (the active one first), then the recent ones."""
    active = common.model_root(getattr(context, "active_object", None))
    roots = [active] if active else []
    roots += [obj for obj in context.scene.objects if common.role(obj) == 'model' and obj != active]
    paths = [sources.model_project(bpy.path.abspath(root.aeron_flight.source_path)) for root in roots]
    paths += [bpy.path.abspath(entry.path) for entry in preferences.get(context).recent_projects]
    found = []
    for path in paths:
        path = os.path.normpath(path) if path else ""
        if path and path not in found and os.path.isdir(path):
            found.append(path)
    return found


def _remember(context, project):
    """Puts `project` first in the recent projects."""
    recent = preferences.get(context).recent_projects
    paths = [project] + [entry.path for entry in recent if os.path.normpath(bpy.path.abspath(entry.path)) != project]
    recent.clear()
    for path in paths[:RECENT_PROJECTS]:
        recent.add().path = path
    context.preferences.is_dirty = True


# Blender needs the strings of a dynamic enum's items kept alive.
_project_items_cache = []


def _project_items(self, context):
    global _project_items_cache
    known = projects(context) if context else []
    _project_items_cache = [(path, os.path.basename(path) or path, path) for path in known]
    _project_items_cache.append(('CHOOSE', "Choose Folder...", "Choose the project folder in the next step"))
    return _project_items_cache


def _invoke_later(context, idname, **props):
    """Invokes an operator once the current one has returned, in the same
    window: a file browser or popup cannot open while the OPT browser is
    still open."""
    window = context.window
    category, name = idname.split(".")

    def run():
        windows = bpy.context.window_manager.windows[:]
        target = window if window in windows else (windows[0] if windows else None)
        if target is None:
            return None
        area = next((area for area in target.screen.areas if area.type == 'VIEW_3D'), target.screen.areas[0])
        region = next((region for region in area.regions if region.type == 'WINDOW'), None)
        with bpy.context.temp_override(window=target, area=area, region=region):
            getattr(getattr(bpy.ops, category), name)('INVOKE_DEFAULT', **props)
        return None

    bpy.app.timers.register(run, first_interval=0.0)


def _start(operator, context, opt, project, data, replace):
    """Imports `opt` into `project`. Run from the UI, it first asks for a
    project folder when there is none, and whether to replace a model the
    project already has."""
    if not os.path.isfile(opt):
        operator.report({'ERROR'}, "no such file: " + opt)
        return {'CANCELLED'}
    interactive = operator.options.is_invoke
    if not project:
        if interactive:
            _invoke_later(context, "aeron.opt_import_project", opt=opt, data=data)
            return {'CANCELLED'}
        operator.report({'ERROR'}, "choose a project folder")
        return {'CANCELLED'}
    project = project_root(project)
    key = _key(opt)
    if not replace and os.path.exists(os.path.join(project, "models", key)):
        if interactive:
            _invoke_later(context, "aeron.opt_import_replace", opt=opt, project=project, data=data)
            return {'CANCELLED'}
        operator.report({'ERROR'}, "{} already has models/{}: use Replace".format(project, key))
        return {'CANCELLED'}
    return _convert(operator, context, opt, project, data, replace)


def _convert(operator, context, opt, project, data, replace):
    """Converts `opt` into `project` with opt2gltf, then imports the model."""
    opt2gltf = preferences.opt2gltf_path(context)
    if not opt2gltf:
        operator.report({'ERROR'}, "opt2gltf is not set: see the add-on's preferences")
        return {'CANCELLED'}
    folder = xwau_installation(context, opt)
    xwau = data == 'XWAU' or (data == 'AUTO' and bool(folder))
    if xwau and not folder:
        own = installation(opt)
        operator.report({'ERROR'}, "{} has no XWA Upgrade data (no Materials folder)".format(own) if own else
                        "no XWA Upgrade installation: the OPT is not in one's FlightModels folder, and the "
                        "preferences set none")
        return {'CANCELLED'}
    argv = [opt2gltf] + (["--xwau-root", folder] if xwau else ["--stock"])
    argv += ["--project", project] + (["--replace"] if replace else []) + [opt]
    try:
        completed = subprocess.run(argv, capture_output=True, text=True)
    except OSError as error:
        operator.report({'ERROR'}, "cannot run {}: {}".format(opt2gltf, error))
        return {'CANCELLED'}
    # The whole conversion log, in the system console.
    print(completed.stderr)
    if completed.returncode != 0:
        operator.report({'ERROR'}, "opt2gltf failed (exit {}):\n{}".format(
            completed.returncode, _tail(completed.stderr or completed.stdout)))
        return {'CANCELLED'}
    key = _key(opt)
    gltf = os.path.join(project, "models", key, key + ".gltf")
    _remember(context, project)
    # An OPT of an XWA Upgrade installation makes it the one for OPTs outside.
    if folder and folder == os.path.normpath(installation(opt)) and folder != preferences.get(context).xwau_folder:
        preferences.get(context).xwau_folder = folder
        context.preferences.is_dirty = True
    result = bpy.ops.import_scene.aeron_flight_model(filepath=gltf)
    if 'FINISHED' not in result:
        return {'CANCELLED'}
    # A loaded project gets the libraries the import added.
    effects = sources.project_effects(gltf)
    if effects in libraries._projects:
        cooker = preferences.cooker_path(context)
        try:
            if cooker:
                libraries.load(cooker, effects)
            else:
                libraries._projects.pop(effects, None)
        except (OSError, RuntimeError, ValueError) as error:
            operator.report({'WARNING'}, "reading the libraries: {}".format(error))
    operator.report({'INFO'}, "Imported {} into {} ({})".format(
        key, project, "XWA Upgrade" if xwau else "original game"))
    return {'FINISHED'}


_DATA_ITEMS = [
    ('AUTO', "Detect", "XWA Upgrade when its data is found: in the OPT's game folder, or for an OPT outside "
                       "one, in the preferences' installation. Else the original game"),
    ('STOCK', "Original Game", "The OPT alone, converted as the original game draws it"),
    ('XWAU', "XWA Upgrade", "The OPT with what XWA Upgrade gives it: materials, animations, skins, profiles, "
                            "moving parts, pilot eye and cockpit displays"),
]


class IMPORT_OT_xwa_opt(Operator, ImportHelper):
    """Import an X-Wing Alliance OPT model into a project, with its XWA Upgrade data"""

    bl_idname = "import_scene.xwa_opt"
    bl_label = "Import XWA OPT"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".opt"
    filter_glob: StringProperty(default="*.opt;*.OPT", options={'HIDDEN'})
    project_choice: EnumProperty(
        name="Project", items=_project_items, options={'SKIP_SAVE'},
        description="The folder the model goes into: models/<key>/ for the model, effects/ for the images "
                    "its animations play")
    data: EnumProperty(name="Data", default='AUTO', items=_DATA_ITEMS)
    # For scripts: the project folder (over the choice above), and replacing
    # a model the project has without asking.
    project: StringProperty(subtype='DIR_PATH', options={'HIDDEN', 'SKIP_SAVE'})
    replace: BoolProperty(options={'HIDDEN', 'SKIP_SAVE'})

    def invoke(self, context, event):
        # A file dropped on the window: a dialog instead of the browser.
        if self.properties.is_property_set("filepath"):
            return context.window_manager.invoke_props_dialog(
                self, title=os.path.basename(self.filepath), confirm_text="Import", translate=False)
        return ImportHelper.invoke(self, context, event)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "project_choice")
        layout.prop(self, "data")

    def execute(self, context):
        project = self.project or ("" if self.project_choice == 'CHOOSE' else self.project_choice)
        return _start(self, context, os.path.abspath(bpy.path.abspath(self.filepath)),
                      os.path.normpath(bpy.path.abspath(project)) if project else "", self.data, self.replace)


class AERON_OT_opt_import_project(Operator):
    """Choose the project folder the OPT goes into"""

    bl_idname = "aeron.opt_import_project"
    bl_label = "Choose Project Folder"
    bl_options = {'INTERNAL', 'UNDO'}

    directory: StringProperty(subtype='DIR_PATH', options={'SKIP_SAVE'})
    filter_folder: BoolProperty(default=True, options={'HIDDEN', 'SKIP_SAVE'})
    opt: StringProperty(options={'HIDDEN', 'SKIP_SAVE'})
    data: EnumProperty(items=_DATA_ITEMS, options={'HIDDEN', 'SKIP_SAVE'})

    def invoke(self, context, event):
        # Start beside the last project, where a new one usually goes.
        recent = preferences.get(context).recent_projects
        if not self.directory and recent:
            parent = os.path.dirname(os.path.normpath(bpy.path.abspath(recent[0].path)))
            if os.path.isdir(parent):
                self.directory = parent + os.sep
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        if not self.directory:
            self.report({'ERROR'}, "choose a project folder")
            return {'CANCELLED'}
        return _start(self, context, self.opt, os.path.normpath(bpy.path.abspath(self.directory)), self.data, False)


class AERON_OT_opt_import_replace(Operator):
    """Replace a model of the project with the OPT's conversion"""

    bl_idname = "aeron.opt_import_replace"
    bl_label = "Replace Model"
    bl_options = {'INTERNAL', 'UNDO'}

    opt: StringProperty(options={'HIDDEN', 'SKIP_SAVE'})
    project: StringProperty(options={'HIDDEN', 'SKIP_SAVE'})
    data: EnumProperty(items=_DATA_ITEMS, options={'HIDDEN', 'SKIP_SAVE'})

    def invoke(self, context, event):
        key = _key(self.opt)
        return context.window_manager.invoke_confirm(
            self, event, title="Replace {}?".format(key), confirm_text="Replace", icon='WARNING',
            message="{} already has models/{}/. Replacing it loses the changes made there.".format(
                os.path.basename(self.project), key), translate=False)

    def execute(self, context):
        return _convert(self, context, self.opt, self.project, self.data, True)


class AERON_FH_xwa_opt(FileHandler):
    bl_idname = "AERON_FH_xwa_opt"
    bl_label = "XWA OPT"
    bl_import_operator = "import_scene.xwa_opt"
    bl_file_extensions = ".opt"

    @classmethod
    def poll_drop(cls, context):
        return context.area is not None and context.area.type in {'VIEW_3D', 'OUTLINER'}


def menu_func(self, context):
    self.layout.operator(IMPORT_OT_xwa_opt.bl_idname, text="XWA OPT (.opt)")


classes = (IMPORT_OT_xwa_opt, AERON_OT_opt_import_project, AERON_OT_opt_import_replace, AERON_FH_xwa_opt)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(menu_func)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)