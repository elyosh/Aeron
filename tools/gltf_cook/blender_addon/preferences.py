# Add-on preferences: the cooker and its options.

import os
import shutil

import bpy
from bpy.props import BoolProperty, EnumProperty, IntProperty, StringProperty
from bpy.types import AddonPreferences


def _power_of_two(value):
    return value > 0 and value & (value - 1) == 0


class AeronFlightModelPreferences(AddonPreferences):
    bl_idname = __package__

    cooker: StringProperty(
        name="aeron_gltf_cook", subtype='FILE_PATH',
        description="The aeron_gltf_cook executable (empty: found on the PATH)")
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


def get(context):
    return context.preferences.addons[__package__].preferences


def cooker_path(context):
    """The cooker's path when it is an executable, else ""."""
    path = bpy.path.abspath(get(context).cooker) if get(context).cooker else shutil.which("aeron_gltf_cook")
    return path if path and os.path.isfile(path) and os.access(path, os.X_OK) else ""


classes = (AeronFlightModelPreferences,)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
