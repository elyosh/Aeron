# Add-on preferences: the tools (the cooker, opt2gltf and environment_bake),
# the cooker's options, and the XWA Upgrade installation and recent projects
# used by the OPT import.

import os
import shutil
import stat
import sys

import bpy
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, IntProperty, StringProperty
from bpy.types import AddonPreferences, PropertyGroup


def _power_of_two(value):
    return value > 0 and value & (value - 1) == 0


class AeronRecentProject(PropertyGroup):
    path: StringProperty(subtype='DIR_PATH')


class AeronFlightModelPreferences(AddonPreferences):
    bl_idname = __package__

    cooker: StringProperty(
        name="aeron_gltf_cook", subtype='FILE_PATH',
        description="The aeron_gltf_cook executable. If empty, the one packaged with the add-on is used, "
                    "or else the one on the PATH")
    atlas_page: IntProperty(name="Atlas Page", default=2048, min=128, max=16384,
                            description="Width and height of an atlas page, in pixels. Must be a power of 2")
    max_texture: IntProperty(name="Max Texture", default=4096, min=4, max=16384,
                             description="Larger textures are downscaled to this size")
    mip_min_subrect: EnumProperty(
        name="Atlas Padding", default='16',
        items=[(value, value, "Texels of padding around each sub-rect. It also limits the number of mip levels")
               for value in ('4', '8', '16', '32')])
    bc7_quality: EnumProperty(
        name="BC7 Quality", default='med',
        items=[('fast', "Fast", "Fastest encoding"), ('med', "Medium", "Default speed and quality"),
               ('uber', "Uber", "Slowest encoding, nearly lossless")])
    zstd: BoolProperty(name="Zstd Supercompression", default=True)
    opt2gltf: StringProperty(
        name="opt2gltf", subtype='FILE_PATH',
        description="The opt2gltf executable, which converts OPT models. If empty, the one packaged with "
                    "the add-on is used, or else the one on the PATH")
    environment_bake: StringProperty(
        name="environment_bake", subtype='FILE_PATH',
        description="The environment_bake executable, which bakes probes. If empty, the one packaged "
                    "with the add-on is used, or else the one on the PATH")
    xwau_folder: StringProperty(
        name="XWA Upgrade Folder", subtype='DIR_PATH',
        description="The XWA Upgrade game folder, with FlightModels and Materials. Import XWA OPT uses it "
                    "for an OPT outside an installation, and sets it when the OPT is inside one")
    recent_projects: CollectionProperty(type=AeronRecentProject, options={'HIDDEN'},
                                        description="The projects of the last OPT imports, most recent first")
    game_dir: StringProperty(
        name="Mod Folder", subtype='DIR_PATH',
        description="The game's mod folder. Export for Game rebuilds the project's changed effect "
                    "libraries in its flight/effects folder and checks each animation frame against them")
    # Before Mod Folder: the mod folder's effects folder. Its parent is the
    # Mod Folder when that is not set.
    effects_dir: StringProperty(subtype='DIR_PATH', options={'HIDDEN'})

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "cooker")
        if not cooker_path(context):
            layout.label(text="aeron_gltf_cook not found, set its path", icon='ERROR')
        layout.prop(self, "opt2gltf")
        layout.prop(self, "environment_bake")
        if not opt2gltf_path(context):
            layout.label(text="opt2gltf not found, set its path", icon='ERROR')
        row = layout.row()
        row.prop(self, "atlas_page")
        row.prop(self, "max_texture")
        if not _power_of_two(self.atlas_page):
            layout.label(text="Atlas page must be a power of 2", icon='ERROR')
        row = layout.row()
        row.prop(self, "mip_min_subrect")
        row.prop(self, "bc7_quality")
        layout.prop(self, "zstd")
        layout.prop(self, "game_dir")
        layout.prop(self, "xwau_folder")
        from . import opt_import  # opt_import imports this module
        if self.xwau_folder and not opt_import.has_xwau(bpy.path.abspath(self.xwau_folder)):
            layout.label(text="Not an XWA Upgrade folder (no Materials folder)", icon='ERROR')


def get(context):
    return context.preferences.addons[__package__].preferences


def _packaged(tool):
    """The tool packaged in the add-on's bin/, or "". Installing a package
    does not keep file permissions, so the tool is made executable first."""
    name = tool + ".exe" if sys.platform == "win32" else tool
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin", name)
    if os.path.isfile(path) and not os.access(path, os.X_OK):
        try:
            os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        except OSError:
            return ""
    return path if os.path.isfile(path) else ""


def _tool_path(setting, tool):
    """The path of an executable tool, or "". It is the preference if set,
    otherwise the packaged tool, or else the one on the PATH (an add-on
    linked from a checkout has no bin/)."""
    path = bpy.path.abspath(setting) if setting else _packaged(tool) or shutil.which(tool)
    return path if path and os.path.isfile(path) and os.access(path, os.X_OK) else ""


def cooker_path(context):
    return _tool_path(get(context).cooker, "aeron_gltf_cook")


def opt2gltf_path(context):
    return _tool_path(get(context).opt2gltf, "opt2gltf")


def environment_bake_path(context):
    return _tool_path(get(context).environment_bake, "environment_bake")


def game_dir(context):
    """The game's mod folder (Mod Folder), or ""."""
    prefs = get(context)
    if prefs.game_dir:
        return os.path.normpath(bpy.path.abspath(prefs.game_dir))
    if prefs.effects_dir:
        return os.path.dirname(os.path.normpath(bpy.path.abspath(prefs.effects_dir)))
    return ""


def game_effects_dir(context):
    """The mod folder's flight/effects folder, or ""."""
    root = game_dir(context)
    return os.path.join(root, "flight", "effects") if root else ""


classes = (AeronRecentProject, AeronFlightModelPreferences)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
