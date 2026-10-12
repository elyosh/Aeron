# Add-on preferences: the cooker and its options.

import os
import shutil
import stat
import sys

import bpy
from bpy.props import BoolProperty, EnumProperty, IntProperty, StringProperty
from bpy.types import AddonPreferences


def _power_of_two(value):
    return value > 0 and value & (value - 1) == 0


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
    effects_dir: StringProperty(
        name="Effects directory", subtype='DIR_PATH',
        description="The game's remaster/effects directory: with it, the .glb export rebuilds the "
                    "effect libraries of the model's source tree that changed, then checks every "
                    "animation frame against its library (empty: not checked)")

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "cooker")
        if not cooker_path(context):
            layout.label(text="aeron_gltf_cook not found: set its path", icon='ERROR')
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


def get(context):
    return context.preferences.addons[__package__].preferences


def _packaged_cooker():
    """The cooker packaged in the add-on's bin/, or "". Installing a package
    does not keep file permissions: it is made executable first."""
    name = "aeron_gltf_cook.exe" if sys.platform == "win32" else "aeron_gltf_cook"
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin", name)
    if os.path.isfile(path) and not os.access(path, os.X_OK):
        try:
            os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        except OSError:
            return ""
    return path if os.path.isfile(path) else ""


def cooker_path(context):
    """The cooker's path when it is an executable, else "": the preference's
    when set, else the packaged one, else the PATH's (an add-on linked from
    a checkout has no bin/)."""
    setting = get(context).cooker
    path = bpy.path.abspath(setting) if setting else _packaged_cooker() or shutil.which("aeron_gltf_cook")
    return path if path and os.path.isfile(path) and os.access(path, os.X_OK) else ""


classes = (AeronFlightModelPreferences,)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
