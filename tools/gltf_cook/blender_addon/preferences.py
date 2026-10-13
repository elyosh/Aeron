# Add-on preferences: the tools (the cooker and opt2gltf), the cooker's
# options, and the folders the OPT import reads.

import os
import shutil
import stat
import sys

import bpy
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, IntProperty, StringProperty
from bpy.types import AddonPreferences, Operator, PropertyGroup, UIList


def _power_of_two(value):
    return value > 0 and value & (value - 1) == 0


class AeronDataFolder(PropertyGroup):
    path: StringProperty(name="Folder", subtype='DIR_PATH',
                         description="A folder holding XWA Upgrade data (an installation, or an unpacked "
                                     "package group): FlightModels, Materials, Effects, Missions...")


class AERON_UL_data_folders(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_property, index=0, flt_flag=0):
        layout.prop(item, "path", text="", emboss=False)


class AERON_OT_data_folder(Operator):
    """Add or remove an XWA Upgrade folder, or move it up or down (earlier folders win)"""

    bl_idname = "aeron.data_folder"
    bl_label = "Edit XWA Upgrade Folders"
    bl_options = {'INTERNAL'}

    action: EnumProperty(items=[('ADD', "Add", ""), ('REMOVE', "Remove", ""), ('UP', "Up", ""), ('DOWN', "Down", "")])

    def execute(self, context):
        prefs = get(context)
        folders, index = prefs.xwau_folders, prefs.active_xwau_folder
        if self.action == 'ADD':
            folders.add()
            prefs.active_xwau_folder = len(folders) - 1
        elif 0 <= index < len(folders):
            if self.action == 'REMOVE':
                folders.remove(index)
                prefs.active_xwau_folder = min(index, len(folders) - 1)
            else:
                target = index - 1 if self.action == 'UP' else index + 1
                if 0 <= target < len(folders):
                    folders.move(index, target)
                    prefs.active_xwau_folder = target
        context.preferences.is_dirty = True
        return {'FINISHED'}


class AeronFlightModelPreferences(AddonPreferences):
    bl_idname = __package__

    cooker: StringProperty(
        name="aeron_gltf_cook", subtype='FILE_PATH',
        description="The aeron_gltf_cook executable (empty: the one packaged with the add-on, else "
                    "found on the PATH)")
    atlas_page: IntProperty(name="Atlas page", default=2048, min=128, max=16384,
                            description="Atlas page size per axis, a power of 2")
    max_texture: IntProperty(name="Max texture", default=4096, min=4, max=16384,
                             description="Larger textures are downscaled to this size")
    mip_min_subrect: EnumProperty(
        name="Sub-rect gutter", default='16',
        items=[(value, value, "Gutter texels around each sub-rect; also caps mip depth")
               for value in ('4', '8', '16', '32')])
    bc7_quality: EnumProperty(
        name="BC7 quality", default='med',
        items=[('fast', "Fast", "Quickest encode"), ('med', "Medium", "Default"),
               ('uber', "Uber", "Slowest, near-lossless")])
    zstd: BoolProperty(name="Zstd supercompression", default=True)
    opt2gltf: StringProperty(
        name="opt2gltf", subtype='FILE_PATH',
        description="The opt2gltf executable, which converts OPT models (empty: the one packaged with the "
                    "add-on, else found on the PATH)")
    xwau_folders: CollectionProperty(
        type=AeronDataFolder, name="XWA Upgrade Folders",
        description="Where Import XWA OPT reads XWA Upgrade data, before the OPT's own installation: unpacked "
                    "packages, the update's groups before the release's")
    active_xwau_folder: IntProperty()
    project: StringProperty(name="Project", subtype='DIR_PATH', options={'HIDDEN'},
                            description="The project of the last OPT import")
    effects_dir: StringProperty(
        name="Effects directory", subtype='DIR_PATH',
        description="The game's remaster/effects directory: with it, Export for Game rebuilds there "
                    "the effect libraries of the model's project that changed, then checks every "
                    "animation frame against its library (empty: not checked)")

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "cooker")
        if not cooker_path(context):
            layout.label(text="aeron_gltf_cook not found: set its path", icon='ERROR')
        layout.prop(self, "opt2gltf")
        if not opt2gltf_path(context):
            layout.label(text="opt2gltf not found: set its path", icon='ERROR')
        row = layout.row()
        row.prop(self, "atlas_page")
        row.prop(self, "max_texture")
        if not _power_of_two(self.atlas_page):
            layout.label(text="The atlas page must be a power of 2", icon='ERROR')
        row = layout.row()
        row.prop(self, "mip_min_subrect")
        row.prop(self, "bc7_quality")
        layout.prop(self, "zstd")
        layout.prop(self, "effects_dir")
        layout.label(text="XWA Upgrade folders:")
        row = layout.row()
        row.template_list("AERON_UL_data_folders", "", self, "xwau_folders", self, "active_xwau_folder", rows=3)
        column = row.column(align=True)
        for action, icon in (('ADD', 'ADD'), ('REMOVE', 'REMOVE'), ('UP', 'TRIA_UP'), ('DOWN', 'TRIA_DOWN')):
            column.operator("aeron.data_folder", text="", icon=icon).action = action


def get(context):
    return context.preferences.addons[__package__].preferences


def _packaged(tool):
    """The tool packaged in the add-on's bin/, or "". Installing a package
    does not keep file permissions: it is made executable first."""
    name = tool + ".exe" if sys.platform == "win32" else tool
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin", name)
    if os.path.isfile(path) and not os.access(path, os.X_OK):
        try:
            os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        except OSError:
            return ""
    return path if os.path.isfile(path) else ""


def _tool_path(setting, tool):
    """A tool's path when it is an executable, else "": the preference's
    when set, else the packaged one, else the PATH's (an add-on linked from
    a checkout has no bin/)."""
    path = bpy.path.abspath(setting) if setting else _packaged(tool) or shutil.which(tool)
    return path if path and os.path.isfile(path) and os.access(path, os.X_OK) else ""


def cooker_path(context):
    return _tool_path(get(context).cooker, "aeron_gltf_cook")


def opt2gltf_path(context):
    return _tool_path(get(context).opt2gltf, "opt2gltf")


classes = (AeronDataFolder, AERON_UL_data_folders, AERON_OT_data_folder, AeronFlightModelPreferences)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
