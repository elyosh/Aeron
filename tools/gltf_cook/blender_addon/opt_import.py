# Import of an X-Wing Alliance OPT: opt2gltf converts it into a project (the
# model's folder and, for an XWA Upgrade model, the effect library images
# its materials name), then the model's .gltf is imported as any other.

import os
import subprocess

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty
from bpy.types import Operator
from bpy_extras.io_utils import ImportHelper

from . import libraries, preferences, sources


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


def xwau_roots(context, opt_path):
    """The folders XWA Upgrade data is read from, the first holding a file
    winning: the preferences' folders, then the OPT's installation."""
    roots = [bpy.path.abspath(folder.path) for folder in preferences.get(context).xwau_folders if folder.path]
    roots = [os.path.normpath(root) for root in roots if os.path.isdir(root)]
    own = installation(opt_path)
    if own and os.path.normpath(own) not in roots:
        roots.append(os.path.normpath(own))
    return roots


def has_xwau(roots):
    return any(os.path.isdir(_entry(root, "Materials")) for root in roots)


def _tail(text, count=12):
    return "\n".join((text or "").rstrip().splitlines()[-count:])


class IMPORT_OT_xwa_opt(Operator, ImportHelper):
    """Import an X-Wing Alliance OPT model into a project, with its XWA Upgrade data"""

    bl_idname = "import_scene.xwa_opt"
    bl_label = "Import XWA OPT"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".opt"
    filter_glob: StringProperty(default="*.opt;*.OPT", options={'HIDDEN'})
    project: StringProperty(
        name="Project", subtype='DIR_PATH',
        description="The folder the model goes into: its folder in models/, and the effect libraries it "
                    "plays frames from in effects/")
    data: EnumProperty(
        name="Data", default='AUTO',
        items=[('AUTO', "Detect", "XWA Upgrade when a Materials folder is beside the OPT's FlightModels "
                                  "folder or in the preferences' XWA Upgrade folders, else the original game"),
               ('STOCK', "Original Game", "The OPT alone, converted as the original game draws it"),
               ('XWAU', "XWA Upgrade", "The OPT with what XWA Upgrade gives it: materials, animations, "
                                       "skins, profiles, moving parts, pilot eye and cockpit displays")])
    replace: BoolProperty(name="Replace", description="Replace the model's folder when the project has it")

    def invoke(self, context, event):
        if not self.project:
            self.project = preferences.get(context).project
        return ImportHelper.invoke(self, context, event)

    def execute(self, context):
        opt2gltf = preferences.opt2gltf_path(context)
        if not opt2gltf:
            self.report({'ERROR'}, "opt2gltf is not set: see the add-on's preferences")
            return {'CANCELLED'}
        project = os.path.normpath(bpy.path.abspath(self.project)) if self.project else ""
        if not project:
            self.report({'ERROR'}, "choose a project folder")
            return {'CANCELLED'}
        opt = os.path.abspath(bpy.path.abspath(self.filepath))
        if not os.path.isfile(opt):
            self.report({'ERROR'}, "no such file: " + opt)
            return {'CANCELLED'}
        roots = xwau_roots(context, opt)
        xwau = self.data == 'XWAU' or (self.data == 'AUTO' and has_xwau(roots))
        if xwau and not roots:
            self.report({'ERROR'}, "no XWA Upgrade folder: the OPT is not in a FlightModels folder, and the "
                                   "preferences list none")
            return {'CANCELLED'}
        argv = [opt2gltf] + ([arg for root in roots for arg in ("--xwau-root", root)] if xwau else ["--stock"])
        argv += ["--project", project] + (["--replace"] if self.replace else []) + [opt]
        try:
            completed = subprocess.run(argv, capture_output=True, text=True)
        except OSError as error:
            self.report({'ERROR'}, "cannot run {}: {}".format(opt2gltf, error))
            return {'CANCELLED'}
        # The whole conversion log, in the system console.
        print(completed.stderr)
        if completed.returncode != 0:
            self.report({'ERROR'}, "opt2gltf failed (exit {}):\n{}".format(
                completed.returncode, _tail(completed.stderr or completed.stdout)))
            return {'CANCELLED'}
        key = os.path.splitext(os.path.basename(opt))[0].lower()
        gltf = os.path.join(project, "models", key, key + ".gltf")
        preferences.get(context).project = self.project
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
                self.report({'WARNING'}, "reading the libraries: {}".format(error))
        self.report({'INFO'}, "Imported {} into {} ({})".format(
            key, project, "XWA Upgrade" if xwau else "original game"))
        return {'FINISHED'}


def menu_func(self, context):
    self.layout.operator(IMPORT_OT_xwa_opt.bl_idname, text="XWA OPT (.opt)")


classes = (IMPORT_OT_xwa_opt,)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(menu_func)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
