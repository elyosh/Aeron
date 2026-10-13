# Imports an X-Wing Alliance OPT. opt2gltf converts the OPT into a project:
# it writes the model's folder and, for an XWA Upgrade model, the effect
# library images that its materials use. Then the model's .gltf is imported
# like any other.
#
# The OPT file browser asks only for the OPT and for one of the known
# projects. Blender opens only one file browser at a time, so a new project
# folder and the confirmation to replace a model are asked after the browser
# closes.

import os
import subprocess

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty
from bpy.types import FileHandler, Operator
from bpy_extras.io_utils import ImportHelper

from . import common, libraries, preferences, sources

RECENT_PROJECTS = 5


def _entry(directory, name):
    """The entry of `directory` named `name` in any case, or ""."""
    try:
        entries = os.listdir(directory)
    except OSError:
        return ""
    return next((os.path.join(directory, entry) for entry in entries if entry.lower() == name.lower()), "")


def installation(opt_path):
    """The game folder whose FlightModels folder holds the OPT, or ""."""
    folder = os.path.dirname(opt_path)
    return os.path.dirname(folder) if os.path.basename(folder).lower() == "flightmodels" else ""


def has_xwau(folder):
    """Whether `folder` is an XWA Upgrade installation, which means that it
    has a Materials folder."""
    return bool(folder) and os.path.isdir(_entry(folder, "Materials"))


def xwau_installation(context, opt_path):
    """The XWA Upgrade installation to read the OPT's data from, or "".
    When the OPT is in a game folder, only that folder is used, and only if
    it has XWA Upgrade. Otherwise the installation set in the preferences is
    used."""
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
    """The project that contains `folder` when `folder` is the models or
    effects folder of a project, or a model folder. Otherwise `folder`."""
    folder = os.path.normpath(folder)
    parent = os.path.dirname(folder)
    if os.path.basename(folder) in ("models", "effects"):
        return parent
    if os.path.basename(parent) == "models":
        return os.path.dirname(parent)
    return folder


def projects(context):
    """The existing projects that an import can use, best first. The
    projects of the scene's models come first, starting with the active
    model, then the recent projects."""
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
    """Moves `project` to the top of the recent projects."""
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
    """Invokes an operator in the same window after the current one returns.
    A file browser or popup cannot open while the OPT browser is still
    open."""
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
    """Imports `opt` into `project`. When run from the UI, it first asks for
    a project folder if there is none, and asks before it replaces a model
    that the project already has."""
    if not os.path.isfile(opt):
        operator.report({'ERROR'}, "File not found: " + opt)
        return {'CANCELLED'}
    interactive = operator.options.is_invoke
    if not project:
        if interactive:
            _invoke_later(context, "aeron.opt_import_project", opt=opt, data=data)
            return {'CANCELLED'}
        operator.report({'ERROR'}, "Choose a project folder")
        return {'CANCELLED'}
    project = project_root(project)
    key = _key(opt)
    if not replace and os.path.exists(os.path.join(project, "models", key)):
        if interactive:
            _invoke_later(context, "aeron.opt_import_replace", opt=opt, project=project, data=data)
            return {'CANCELLED'}
        operator.report({'ERROR'}, "{} already has models/{}. Pass replace=True to replace it".format(project, key))
        return {'CANCELLED'}
    return _convert(operator, context, opt, project, data, replace)


def _convert(operator, context, opt, project, data, replace):
    """Converts `opt` into `project` with opt2gltf, then imports the model."""
    opt2gltf = preferences.opt2gltf_path(context)
    if not opt2gltf:
        operator.report({'ERROR'}, "Set the opt2gltf path in the add-on preferences")
        return {'CANCELLED'}
    folder = xwau_installation(context, opt)
    xwau = data == 'XWAU' or (data == 'AUTO' and bool(folder))
    if xwau and not folder:
        own = installation(opt)
        operator.report({'ERROR'}, "{} has no XWA Upgrade data (no Materials folder)".format(own) if own else
                        "The OPT is not in an XWA Upgrade installation. Set one in the add-on "
                        "preferences")
        return {'CANCELLED'}
    argv = [opt2gltf] + (["--xwau-root", folder] if xwau else ["--stock"])
    argv += ["--project", project] + (["--replace"] if replace else []) + [opt]
    try:
        completed = subprocess.run(argv, capture_output=True, text=True)
    except OSError as error:
        operator.report({'ERROR'}, "Cannot run {}: {}".format(opt2gltf, error))
        return {'CANCELLED'}
    # Print the full conversion log to the system console.
    print(completed.stderr)
    if completed.returncode != 0:
        operator.report({'ERROR'}, "opt2gltf failed (exit code {}):\n{}".format(
            completed.returncode, _tail(completed.stderr or completed.stdout)))
        return {'CANCELLED'}
    key = _key(opt)
    gltf = os.path.join(project, "models", key, key + ".gltf")
    _remember(context, project)
    # After importing an OPT from an XWA Upgrade installation, use that
    # installation for OPTs that are outside any installation.
    if folder and folder == os.path.normpath(installation(opt)) and folder != preferences.get(context).xwau_folder:
        preferences.get(context).xwau_folder = folder
        context.preferences.is_dirty = True
    result = bpy.ops.import_scene.aeron_flight_model(filepath=gltf)
    if 'FINISHED' not in result:
        return {'CANCELLED'}
    # If the project's libraries are loaded, reload them so that they include
    # the libraries that the import added.
    effects = sources.project_effects(gltf)
    if effects in libraries._projects:
        cooker = preferences.cooker_path(context)
        try:
            if cooker:
                libraries.load(cooker, effects)
            else:
                libraries._projects.pop(effects, None)
        except (OSError, RuntimeError, ValueError) as error:
            operator.report({'WARNING'}, "Cannot read the effect libraries: {}".format(error))
    operator.report({'INFO'}, "Imported {} into {} ({})".format(
        key, project, "XWA Upgrade" if xwau else "original game"))
    return {'FINISHED'}


_DATA_ITEMS = [
    ('AUTO', "Detect", "Use XWA Upgrade data if found, otherwise the original game. The data comes from the "
                       "OPT's game folder, or from the preferences for an OPT outside a game folder"),
    ('STOCK', "Original Game", "Convert the OPT alone, as the original game draws it"),
    ('XWAU', "XWA Upgrade", "Convert the OPT with its XWA Upgrade data, such as materials, animations, skins, "
                            "moving parts and cockpit displays"),
]


class IMPORT_OT_xwa_opt(Operator, ImportHelper):
    """Convert an X-Wing Alliance OPT model into a project and import it"""

    bl_idname = "import_scene.xwa_opt"
    bl_label = "Import XWA OPT"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".opt"
    filter_glob: StringProperty(default="*.opt;*.OPT", options={'HIDDEN'})
    project_choice: EnumProperty(
        name="Project", items=_project_items, options={'SKIP_SAVE'},
        description="The project to import into. The model goes into its models folder and the animation "
                    "images into its effects folder")
    data: EnumProperty(name="Data", default='AUTO', items=_DATA_ITEMS)
    # For scripts: a project folder that overrides the choice above, and
    # whether to replace an existing model without asking.
    project: StringProperty(subtype='DIR_PATH', options={'HIDDEN', 'SKIP_SAVE'})
    replace: BoolProperty(options={'HIDDEN', 'SKIP_SAVE'})

    def invoke(self, context, event):
        # A file dropped on the window opens a dialog instead of the browser.
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
    """Choose the project folder for the OPT"""

    bl_idname = "aeron.opt_import_project"
    bl_label = "Choose Project Folder"
    bl_options = {'INTERNAL', 'UNDO'}

    directory: StringProperty(subtype='DIR_PATH', options={'SKIP_SAVE'})
    filter_folder: BoolProperty(default=True, options={'HIDDEN', 'SKIP_SAVE'})
    opt: StringProperty(options={'HIDDEN', 'SKIP_SAVE'})
    data: EnumProperty(items=_DATA_ITEMS, options={'HIDDEN', 'SKIP_SAVE'})

    def invoke(self, context, event):
        # Start in the folder of the last project, where a new one usually goes.
        recent = preferences.get(context).recent_projects
        if not self.directory and recent:
            parent = os.path.dirname(os.path.normpath(bpy.path.abspath(recent[0].path)))
            if os.path.isdir(parent):
                self.directory = parent + os.sep
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        if not self.directory:
            self.report({'ERROR'}, "Choose a project folder")
            return {'CANCELLED'}
        return _start(self, context, self.opt, os.path.normpath(bpy.path.abspath(self.directory)), self.data, False)


class AERON_OT_opt_import_replace(Operator):
    """Replace the project's model with the converted OPT"""

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
            message="{} already has models/{}/. The changes made to it will be lost.".format(
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