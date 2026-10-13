# Imports an X-Wing Alliance OPT. opt2gltf converts the OPT into a project:
# it writes the model's folder and, for an XWA Upgrade model, the effect
# library images that its materials use. Then the model's .gltf is imported
# like any other.
#
# The OPT file browser imports into the project chosen in its sidebar. When
# no project is chosen, or the project already has the model, a dialog asks
# after the browser closes: Blender opens only one file browser at a time, so
# the sidebar cannot browse for a folder. A dropped OPT opens the same dialog.

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
    """The existing projects that an import can use, best first: the scene's
    OPT project, the projects of the scene's models starting with the active
    model, then the recent projects."""
    active = common.model_root(getattr(context, "active_object", None))
    roots = [active] if active else []
    roots += [obj for obj in context.scene.objects if common.role(obj) == 'model' and obj != active]
    paths = [bpy.path.abspath(context.scene.aeron_flight.opt_project)]
    paths += [sources.model_project(bpy.path.abspath(root.aeron_flight.source_path)) for root in roots]
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


def _project_search(self, context, edit_text):
    return [(path, os.path.basename(path)) for path in projects(context)]


def _invoke_later(context, idname, **props):
    """Invokes an operator in the same window after the current one returns.
    A dialog cannot open while the OPT browser is still open."""
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


def _exists(project, opt):
    return bool(project) and os.path.exists(os.path.join(project_root(project), "models", _key(opt)))


def _data_status(context, opt, data):
    """The data the import will use, for the dialog: (text, icon), or None."""
    if data == 'STOCK' or not opt:
        return None
    folder = xwau_installation(context, opt)
    if folder:
        return "XWA Upgrade: " + os.path.basename(folder), 'CHECKMARK'
    if data == 'XWAU':
        return "No XWA Upgrade installation found", 'ERROR'
    return "Original game (no XWA Upgrade found)", 'INFO'


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
    context.scene.aeron_flight.opt_project = project
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
    ('AUTO', "Auto", "XWA Upgrade if it is installed in the OPT's game folder or set in the preferences, "
                     "or else the original game"),
    ('STOCK', "Original Game", "The model as the original game draws it"),
    ('XWAU', "XWA Upgrade", "The model with XWA Upgrade's materials, animations, skins, moving parts and "
                            "cockpit displays"),
]


class IMPORT_OT_xwa_opt(Operator, ImportHelper):
    """Convert an X-Wing Alliance OPT model into a project and import it"""

    bl_idname = "import_scene.xwa_opt"
    bl_label = "Import XWA OPT"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".opt"
    filter_glob: StringProperty(default="*.opt;*.OPT", options={'HIDDEN'})
    project: StringProperty(
        name="Project Folder", subtype='DIR_PATH', search=_project_search, search_options={'SUGGESTION'},
        options={'SKIP_SAVE'},
        description="Folder that receives the model (in models) and its animation images (in effects). "
                    "A new folder starts a new project")
    # The same project in the browser's sidebar, without the folder button: the
    # browser cannot open another browser.
    browser_project: StringProperty(
        name="Project Folder", search=_project_search, search_options={'SUGGESTION'}, options={'SKIP_SAVE'},
        description="Folder that receives the model (in models) and its animation images (in effects). "
                    "Leave it empty to choose a folder in the next step")
    data: EnumProperty(name="Source", default='AUTO', items=_DATA_ITEMS,
                       description="Game the model comes from. XWA Upgrade adds its materials, animations, skins, "
                                   "moving parts and cockpit displays")
    # For scripts: whether to replace a model that the project already has.
    replace: BoolProperty(options={'HIDDEN', 'SKIP_SAVE'})
    dialog: BoolProperty(options={'HIDDEN', 'SKIP_SAVE'})

    def invoke(self, context, event):
        known = projects(context)
        if not self.properties.is_property_set("project"):
            self.project = known[0] if known else ""
        # A dropped file, or an OPT that the browser could not import alone.
        if self.properties.is_property_set("filepath"):
            self.dialog = True
            title = "Import " + os.path.basename(self.filepath)
            return context.window_manager.invoke_props_dialog(
                self, title=title, confirm_text="Import", width=360, translate=False)
        self.browser_project = self.project
        return ImportHelper.invoke(self, context, event)

    def _opt(self, context):
        """The selected OPT: the browser's selection while it is open."""
        space = context.space_data
        if not self.dialog and space is not None and space.type == 'FILE_BROWSER' and space.params.filename:
            return os.path.join(space.params.directory.decode("utf-8", "replace"), space.params.filename)
        return os.path.abspath(bpy.path.abspath(self.filepath)) if self.filepath else ""

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "project" if self.dialog else "browser_project")
        layout.prop(self, "data")
        opt = self._opt(context)
        status = _data_status(context, opt, self.data)
        if status:
            layout.label(text=status[0], icon=status[1])
        project = self.project if self.dialog else self.browser_project
        if opt and _exists(bpy.path.abspath(project), opt):
            layout.label(text="Replaces models/" + _key(opt), icon='ERROR')

    def execute(self, context):
        opt = os.path.abspath(bpy.path.abspath(self.filepath))
        if not os.path.isfile(opt):
            self.report({'ERROR'}, "File not found: " + opt)
            return {'CANCELLED'}
        interactive = self.options.is_invoke
        project = self.browser_project if interactive and not self.dialog else self.project
        project = os.path.normpath(bpy.path.abspath(project)) if project else ""
        if interactive and not self.dialog and (not project or _exists(project, opt)):
            # Ask in the dialog, which can browse for a folder and shows the replace warning.
            _invoke_later(context, "import_scene.xwa_opt", filepath=opt, project=project, data=self.data)
            return {'CANCELLED'}
        if not project and not interactive:
            known = projects(context)
            project = known[0] if known else ""
        if not project:
            if self.dialog:
                _invoke_later(context, "import_scene.xwa_opt", filepath=opt, project="", data=self.data)
            self.report({'ERROR'}, "Choose a project folder")
            return {'CANCELLED'}
        project = project_root(project)
        replace = self.replace or self.dialog
        if not replace and _exists(project, opt):
            self.report({'ERROR'}, "{} already has models/{}. Pass replace=True to replace it".format(
                project, _key(opt)))
            return {'CANCELLED'}
        return _convert(self, context, opt, project, self.data, replace)


class AERON_OT_opt_project(Operator):
    """Choose the project that Import XWA OPT uses by default in this file"""

    bl_idname = "aeron.opt_project"
    bl_label = "Set OPT Project"
    bl_options = {'REGISTER', 'UNDO'}

    directory: StringProperty(subtype='DIR_PATH', options={'SKIP_SAVE'})
    filter_folder: BoolProperty(default=True, options={'HIDDEN', 'SKIP_SAVE'})

    def invoke(self, context, event):
        known = projects(context)
        if known:
            self.directory = known[0] + os.sep
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        if not self.directory:
            return {'CANCELLED'}
        context.scene.aeron_flight.opt_project = project_root(bpy.path.abspath(self.directory))
        return {'FINISHED'}


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


classes = (IMPORT_OT_xwa_opt, AERON_OT_opt_project, AERON_FH_xwa_opt)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(menu_func)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)