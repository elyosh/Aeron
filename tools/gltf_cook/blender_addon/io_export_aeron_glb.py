bl_info = {
    "name": "Aeron GLB Exporter",
    "author": "Aeron",
    "version": (1, 0, 0),
    "blender": (3, 6, 0),
    "location": "File > Export > Aeron GLB (.glb)",
    "description": (
        "Export the scene to an Aeron runtime .glb: runs the built-in glTF "
        "exporter into a scratch dir, then post-processes via the "
        "aeron_gltf_cook CLI (per-channel BC7/KTX2 atlases + "
        "KHR_texture_transform)."
    ),
    "category": "Import-Export",
}

import os
import shutil
import subprocess
import tempfile

import bpy
from bpy.props import (
    BoolProperty,
    EnumProperty,
    IntProperty,
    StringProperty,
)
from bpy.types import AddonPreferences, Operator
from bpy_extras.io_utils import ExportHelper

ADDON_ID = __name__


def _default_cooker_path():
    return shutil.which("aeron_gltf_cook") or ""


def _prefs(context):
    return context.preferences.addons[ADDON_ID].preferences


def _tail(text, n=20):
    if not text:
        return ""
    lines = text.rstrip().splitlines()
    return "\n".join(lines[-n:])


class AeronExportPreferences(AddonPreferences):
    bl_idname = ADDON_ID

    cooker_path: StringProperty(
        name="aeron_gltf_cook binary",
        subtype='FILE_PATH',
        default=_default_cooker_path(),
        description="Path to the aeron_gltf_cook executable",
    )
    max_atlas_size: IntProperty(
        name="Max atlas size",
        default=2048, min=4, soft_max=8192, max=16384,
        description="Per-axis max atlas size; must be a power of 2 ≥ 4",
    )
    mip_min_subrect: IntProperty(
        name="Sub-rect gutter",
        default=4, min=1, max=64,
        description=(
            "Gutter texels around each sub-rect; also caps mip depth"
        ),
    )
    bc7_quality: EnumProperty(
        name="BC7 quality",
        items=[
            ('fast', "Fast", "Quickest BC7 encode"),
            ('med',  "Medium", "Default; seconds per atlas"),
            ('uber', "Uber", "Slowest; near-lossless"),
        ],
        default='med',
    )
    zstd_supercompress: BoolProperty(
        name="Zstd supercompress KTX2",
        default=True,
    )
    verbose: BoolProperty(
        name="Verbose cook log",
        default=False,
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "cooker_path")
        row = layout.row(align=True)
        row.prop(self, "max_atlas_size")
        row.prop(self, "mip_min_subrect")
        row = layout.row(align=True)
        row.prop(self, "bc7_quality")
        row.prop(self, "zstd_supercompress")
        layout.prop(self, "verbose")


class EXPORT_OT_aeron_glb(Operator, ExportHelper):
    """Export the scene to an Aeron runtime .glb via aeron_gltf_cook."""

    bl_idname = "export_scene.aeron_glb"
    bl_label = "Export Aeron GLB"
    bl_options = {'PRESET'}

    filename_ext = ".glb"
    filter_glob: StringProperty(default="*.glb", options={'HIDDEN'})

    use_selection: BoolProperty(
        name="Selection Only",
        description="Export only selected objects",
        default=False,
    )
    export_apply: BoolProperty(
        name="Apply Modifiers",
        description="Apply modifiers (preview viewport mesh) before export",
        default=False,
    )
    keep_intermediate: BoolProperty(
        name="Keep intermediate .gltf",
        description=(
            "Don't delete the scratch .gltf/.bin/.png after a successful "
            "cook (failures always keep them)"
        ),
        default=False,
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "use_selection")
        layout.prop(self, "export_apply")
        layout.prop(self, "keep_intermediate")

    def execute(self, context):
        prefs = _prefs(context)
        cooker = bpy.path.abspath(prefs.cooker_path) if prefs.cooker_path else ""
        if not cooker or not os.path.isfile(cooker) or not os.access(cooker, os.X_OK):
            self.report(
                {'ERROR'},
                "aeron_gltf_cook binary not configured or not executable. Set "
                "it in Edit > Preferences > Add-ons > Aeron GLB Exporter. "
                "Current: {!r}".format(cooker),
            )
            return {'CANCELLED'}

        out_glb = bpy.path.abspath(self.filepath)
        if not out_glb.lower().endswith(".glb"):
            out_glb += ".glb"
        out_dir = os.path.dirname(out_glb)
        if out_dir and not os.path.isdir(out_dir):
            self.report({'ERROR'}, "output directory does not exist: " + out_dir)
            return {'CANCELLED'}

        scratch = tempfile.mkdtemp(prefix="aeron_glb_")
        scratch_gltf = os.path.join(scratch, "scene.gltf")

        try:
            res = bpy.ops.export_scene.gltf(
                filepath=scratch_gltf,
                export_format='GLTF_SEPARATE',
                export_image_format='AUTO',
                export_extras=True,
                use_selection=self.use_selection,
                export_apply=self.export_apply,
            )
        except Exception as e:
            self.report({'ERROR'}, "Blender glTF export raised: {}".format(e))
            return {'CANCELLED'}
        if 'FINISHED' not in res:
            self.report(
                {'ERROR'},
                "Blender glTF export did not finish: {!r}. Scratch: {}".format(
                    res, scratch
                ),
            )
            return {'CANCELLED'}

        argv = [
            cooker, scratch_gltf, "--out", out_glb,
            "--max-atlas", str(prefs.max_atlas_size),
            "--mip-min-subrect", str(prefs.mip_min_subrect),
            "--quality", prefs.bc7_quality,
        ]
        if not prefs.zstd_supercompress:
            argv.append("--no-zstd")
        if prefs.verbose:
            argv.append("--verbose")

        try:
            cp = subprocess.run(argv, capture_output=True, text=True)
        except OSError as e:
            self.report(
                {'ERROR'},
                "failed to launch {!r}: {}. Scratch: {}".format(
                    cooker, e, scratch
                ),
            )
            return {'CANCELLED'}

        if cp.returncode != 0:
            self.report(
                {'ERROR'},
                "aeron_gltf_cook exit {}. Scratch kept at {}.\n{}".format(
                    cp.returncode, scratch, _tail(cp.stderr or cp.stdout)
                ),
            )
            return {'CANCELLED'}

        if not self.keep_intermediate:
            shutil.rmtree(scratch, ignore_errors=True)

        if prefs.verbose and cp.stderr:
            self.report({'INFO'}, _tail(cp.stderr))
        self.report({'INFO'}, "Wrote " + out_glb)
        return {'FINISHED'}


def _menu_func(self, context):
    self.layout.operator(EXPORT_OT_aeron_glb.bl_idname, text="Aeron GLB (.glb)")


_classes = (AeronExportPreferences, EXPORT_OT_aeron_glb)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_export.append(_menu_func)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(_menu_func)
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
