"""Tests the material animation editor and the effect library operators in a headless Blender.

    python3 animation_editor.py --blender BLENDER --cooker AERON_GLTF_COOK [--keep]

Builds a scratch project from the animated example (../../example) with
two models, `panel` and `panel2`. Both use the shared library
`examplepanel`, which has an image `extra` that only `panel2` uses. Then,
in a headless Blender with this add-on, it:
- imports `panel` and checks that the material properties hold exactly the
  AERON_flight_model objects of its materials;
- loads the project's libraries and checks that only `panel2` uses `extra`;
- creates library `newlib` with Add Images (two emissive images, one with
  its base color), and plays them on material Screen with Add Recent
  Images. It edits frames and flipbooks, renames an image (the frames of
  the open model follow), edits an image, adds a Blender image and draws
  the panels;
- saves the model to the project, which writes the objects of its
  materials back, and exports it for the game, which builds `newlib` from
  the project;
- removes `extra` from `examplepanel` and exports the .glb again. The
  export must warn that `panel2` uses it.
Then it checks the files: Screen plays the frames of newlib in the saved
model and in the game model, and newlib is built.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ADDON_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The add-on is loaded from its source directory, so do not write bytecode caches there.
sys.dont_write_bytecode = True
EXAMPLE = os.path.join(ADDON_DIR, "..", "example")
MODULE = "bl_ext.user_default.aeron_flight_model"


# ---- inside Blender ------------------------------------------------------------

class _Layout:
    """A fake UI layout. A headless Blender draws no UI, so drawing a panel
    with this layout checks that every property and operator it shows exists."""

    def __getattr__(self, name):
        return lambda *args, **kwargs: _Layout()

    def prop(self, data, name, **kwargs):
        assert name in data.bl_rna.properties, "{} has no property {}".format(data, name)

    def template_list(self, list_type, list_id, data, name, active_data, active_name, **kwargs):
        import bpy

        assert hasattr(bpy.types, list_type), list_type
        assert name in data.bl_rna.properties and active_name in active_data.bl_rna.properties, name

    def operator(self, idname, **kwargs):
        import bpy

        module, name = idname.split(".")
        properties = getattr(getattr(bpy.ops, module), name).get_rna_type().properties
        return _OperatorProperties(idname, properties)


class _OperatorProperties:
    def __init__(self, idname, properties):
        object.__setattr__(self, "names", (idname, {p.identifier for p in properties}))

    def __setattr__(self, name, value):
        assert name in self.names[1], "{} has no property {}".format(self.names[0], name)


def _draw(context, root):
    """Draws the animation and library panels and lists, in each state the test reaches."""
    import bpy

    for name in ("MATERIAL_PT_aeron_animation", "MATERIAL_PT_aeron_flipbooks", "MATERIAL_PT_aeron_damage",
                 "MATERIAL_PT_aeron_libraries", "OBJECT_PT_aeron_model_file",
                 "OBJECT_PT_aeron_material_animations"):
        panel = getattr(bpy.types, name)
        panel.draw(type("Panel", (), {"layout": _Layout()})(), context)
    props = context.material.aeron_flight
    for book in props.flipbooks:
        bpy.types.AERON_UL_flipbooks.draw_item(None, context, _Layout(), props, book, 0, props, "")
        for frame in book.frames:
            bpy.types.AERON_UL_frames.draw_item(None, context, _Layout(), book, frame, 0, book, "")
    for alternative in props.damage.alternatives:
        bpy.types.AERON_UL_damage_alternatives.draw_item(None, context, _Layout(), props.damage, alternative,
                                                         0, props.damage, "")
    state = context.window_manager.aeron_libraries
    for item in state.libraries:
        bpy.types.AERON_UL_libraries.draw_item(None, context, _Layout(), state, item, 0, state, "")
    for item in state.images:
        bpy.types.AERON_UL_library_images.draw_item(None, context, _Layout(), state, item, 0, state, "")


def blender_main(cooker, project, effects, paint, scratch):
    import addon_utils
    import bpy

    addon_utils.enable(MODULE, default_set=True, handle_error=None)
    prefs = bpy.context.preferences.addons[MODULE].preferences
    # effects is <mod folder>/flight/effects.
    prefs.cooker, prefs.game_dir = cooker, os.path.dirname(os.path.dirname(effects))
    from bl_ext.user_default.aeron_flight_model import animation, libraries, sources, validation

    failures = []

    def check(condition, message):
        print("EDITOR", "ok" if condition else "FAILED", message)
        if not condition:
            failures.append(message)

    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    gltf = os.path.join(project, "models", "panel", "panel.gltf")
    check('FINISHED' in bpy.ops.import_scene.aeron_flight_model(filepath=gltf), "import")
    root = next(obj for obj in bpy.data.objects if obj.aeron_flight.role == 'model')
    bpy.context.view_layer.objects.active = root
    document = sources.read_document(gltf)
    materials = sources.material_extensions(document)
    held = {name: animation.extension(bpy.data.materials[name]) for name in materials}
    check(animation.float32(held) == animation.float32(materials) and
          animation.energy_default(root) == sources.model_extension(document).get("energy"),
          "the properties hold the materials' objects")

    with bpy.context.temp_override(object=root):
        check('FINISHED' in bpy.ops.aeron.libraries_refresh(), "load the libraries")
        loaded = libraries.project(bpy.context)
        check(loaded is not None and loaded.users_of("examplepanel", "extra", exclude="panel") == ["panel2"],
              "the libraries know panel2 names extra")
    lights = bpy.data.materials["Lights"]
    frame = lights.aeron_flight.flipbooks[0].frames[0]
    key, frame.key = frame.key, "screen_a"
    problems = [message for _, message, _ in validation.validate(root).items]
    check(any("screen_a is a baseColor image, not emissive" in message for message in problems),
          "Check Model reports a frame of the wrong kind")
    frame.key = key
    with bpy.context.temp_override(object=root):
        files = [{"name": name} for name in sorted(os.listdir(paint))]
        check('FINISHED' in bpy.ops.aeron.library_add_images(directory=paint, files=files, create=True,
                                                              new_library="newlib", kind='emissive'),
              "New Library with Add Images")
        check(sorted(loaded.libraries.get("newlib", {}).get("images", {})) == ["glow_0", "glow_1"] and
              "baseColor" in loaded.libraries["newlib"]["images"]["glow_0"], "newlib holds the images, paired")

    screen = bpy.data.materials["Screen"]
    with bpy.context.temp_override(object=root, material=screen):
        bpy.ops.aeron.flipbook_add()
        book = screen.aeron_flight.flipbooks[screen.aeron_flight.active_flipbook]
        book.layer = "emissive"
        book.library = ""
        check('FINISHED' in bpy.ops.aeron.frames_add_added(seconds=0.25), "Add Recent Images")
        check([frame.key for frame in book.frames] == ["glow_0", "glow_1"] and book.library == "newlib",
              "the flipbook plays the new images")
        bpy.ops.aeron.damage_add()
        screen.aeron_flight.energy_override = True
        try:
            _draw(bpy.context, root)
            check(True, "the panels draw")
        except AssertionError as error:
            check(False, "the panels draw: {}".format(error))
        bpy.ops.aeron.damage_remove()
        screen.aeron_flight.energy_override = False
        # Test the other frame and flipbook operators, then the library image operators.
        bpy.ops.aeron.flipbook_add()
        extra = screen.aeron_flight.flipbooks[-1]
        extra.layer, extra.library = "emissive", "newlib"
        check('FINISHED' in bpy.ops.aeron.frames_add_prefix(prefix="glow", seconds=0.5) and
              [frame.key for frame in extra.frames] == ["glow_0", "glow_1"], "Add Frames by Prefix")
        bpy.ops.aeron.frames_set_duration(seconds=0.75)
        check(all(abs(frame.seconds - 0.75) < 1e-6 for frame in extra.frames), "Set Duration")
        bpy.ops.aeron.frame_move(direction=-1)
        check([frame.key for frame in extra.frames] == ["glow_1", "glow_0"], "move a frame")
        bpy.ops.aeron.flipbook_move(direction=-1)
        moved = screen.aeron_flight.flipbooks[-2]
        check(moved.library == "newlib" and [frame.key for frame in moved.frames] == ["glow_1", "glow_0"],
              "move a flipbook")
        bpy.ops.aeron.flipbook_remove()
        check(len(screen.aeron_flight.flipbooks) == 2, "remove a flipbook")
    with bpy.context.temp_override(object=root, material=screen):
        libraries.select(bpy.context, "newlib", "glow_1")
        check('FINISHED' in bpy.ops.aeron.library_rename_image(new_key="glow_one") and
              [frame.key for frame in book.frames] == ["glow_0", "glow_one"],
              "Rename Image, with the open model's frames")
        libraries.select(bpy.context, "newlib", "glow_one")
        check('FINISHED' in bpy.ops.aeron.library_edit_image(kind='emissive', emissive="frames/glow_1.png",
                                                             max_size=64) and
              loaded.libraries["newlib"]["images"]["glow_one"].get("maxSize") == 64, "Edit Image")
        painted = bpy.data.images.new("painted", 8, 8, alpha=True)
        painted.pixels = [1.0, 0.0, 0.0, 1.0] * 64
        check('FINISHED' in bpy.ops.aeron.library_add_blender_image(image="painted", kind='baseColor') and
              "painted" in loaded.libraries["newlib"]["images"] and
              os.path.exists(bpy.path.abspath(painted.filepath)), "Add Blender Image")

    with bpy.context.temp_override(object=root):
        check('FINISHED' in bpy.ops.export_scene.aeron_flight_model_project(filepath=gltf), "save to project")
        glb = os.path.join(scratch, "panel.glb")
        check('FINISHED' in bpy.ops.export_scene.aeron_flight_model(filepath=glb), ".glb export")
        libraries.select(bpy.context, "examplepanel", "extra")
        check(libraries.selected_image(bpy.context) == "extra", "select extra")
        check('FINISHED' in bpy.ops.aeron.library_remove_image(), "remove extra")
        check('FINISHED' in bpy.ops.export_scene.aeron_flight_model(filepath=glb), ".glb export after removal")
    sys.exit(1 if failures else 0)


# ---- outside -------------------------------------------------------------------

def _copy_model(project, key, screen=None):
    """Copies the example as model `key`. When `screen` is given, it replaces
    the AERON_flight_model object of the Screen material."""
    model = os.path.join(project, "models", key)
    os.makedirs(model)
    with open(os.path.join(EXAMPLE, "panel.gltf"), encoding="utf-8") as stream:
        document = json.load(stream)
    if screen is not None:
        for material in document["materials"]:
            if material["name"] == "Screen":
                material["extensions"]["AERON_flight_model"] = screen
    with open(os.path.join(model, key + ".gltf"), "w", encoding="utf-8") as stream:
        json.dump(document, stream, indent=1)
    shutil.copyfile(os.path.join(EXAMPLE, "panel.bin"), os.path.join(model, "panel.bin"))
    shutil.copytree(os.path.join(EXAMPLE, "textures"), os.path.join(model, "textures"))
    return model


def _materials(path):
    """Returns the AERON_flight_model objects of the materials in a .gltf or .glb, by material name."""
    data = open(path, "rb").read()
    if data[:4] == b"glTF":
        data = data[20:20 + struct.unpack_from("<I", data, 12)[0]]
    return {m.get("name"): m.get("extensions", {}).get("AERON_flight_model", {})
            for m in json.loads(data)["materials"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--blender", required=True)
    parser.add_argument("--cooker", required=True)
    parser.add_argument("--keep", action="store_true", help="keep the scratch directory")
    parser.add_argument("--inside", nargs=4, help=argparse.SUPPRESS)
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    args = parser.parse_args(argv)
    if args.inside:
        blender_main(args.cooker, *args.inside)
        return

    scratch = tempfile.mkdtemp(prefix="aeron_animation_editor_")
    project, paint = (os.path.join(scratch, name) for name in ("project", "paint"))
    effects = os.path.join(scratch, "mod", "flight", "effects")
    os.makedirs(effects)
    os.makedirs(paint)
    # The project has the example's library with one more image, `extra`,
    # which only panel2 uses.
    library = os.path.join(project, "effects", "examplepanel")
    os.makedirs(library)
    shutil.copytree(os.path.join(EXAMPLE, "frames"), os.path.join(library, "frames"))
    shutil.copyfile(os.path.join(library, "frames", "screen_a.png"), os.path.join(library, "frames", "extra.png"))
    with open(os.path.join(EXAMPLE, "examplepanel.yaml"), encoding="utf-8") as stream:
        text = stream.read()
    with open(os.path.join(library, "examplepanel.yaml"), "w", encoding="utf-8") as stream:
        stream.write(text + "  extra: { kind: baseColor, baseColor: frames/extra.png }\n")
    _copy_model(project, "panel")
    _copy_model(project, "panel2", {"flipbooks": [{"state": "target", "layer": "baseColor", "mode": "loop",
                                                "library": "examplepanel", "frames": [["extra", 1]]}]})
    # Frames painted outside Blender: two emissive images, the first one with its base color.
    frames = os.path.join(EXAMPLE, "frames")
    shutil.copyfile(os.path.join(frames, "lights_0.png"), os.path.join(paint, "glow_0_emissive.png"))
    shutil.copyfile(os.path.join(frames, "screen_a.png"), os.path.join(paint, "glow_0_base_color.png"))
    shutil.copyfile(os.path.join(frames, "lights_1.png"), os.path.join(paint, "glow_1.png"))

    os.makedirs(os.path.join(scratch, "ext", "user_default"))
    os.symlink(ADDON_DIR, os.path.join(scratch, "ext", "user_default", "aeron_flight_model"))
    env = dict(os.environ, BLENDER_USER_EXTENSIONS=os.path.join(scratch, "ext"),
               BLENDER_USER_CONFIG=os.path.join(scratch, "config"),
               BLENDER_USER_SCRIPTS=os.path.join(scratch, "scripts"))
    completed = subprocess.run([args.blender, "-b", "--factory-startup", "--python-exit-code", "1",
                                "--python", os.path.abspath(__file__), "--", "--blender", args.blender,
                                "--cooker", args.cooker, "--inside", project, effects, paint, scratch],
                               env=env, capture_output=True, text=True)
    failures = []
    for line in completed.stdout.splitlines():
        if line.startswith("EDITOR"):
            print(line)
        elif line.startswith(("Info:", "Warning:", "Error:")):
            print("  " + line)
    if completed.returncode != 0:
        failures.append("Blender: " + " | ".join(completed.stderr.strip().splitlines()[-3:]))
    warned = [line for line in completed.stdout.splitlines()
              if line.startswith("Warning:") and "rebuilt libraries removed or changed" in line]
    if not any("panel2" in line and "extra" in line for line in warned):
        failures.append("the .glb export after the removal did not name panel2")

    books = _materials(os.path.join(project, "models", "panel", "panel.gltf")).get("Screen", {}).get("flipbooks", [])
    if not any(book.get("library") == "newlib" and [k for k, _ in book["frames"]] == ["glow_0", "glow_one"]
               for book in books):
        failures.append("the saved model does not play newlib's frames on Screen")
    if not os.path.exists(os.path.join(effects, "newlib.yaml")):
        failures.append("newlib was not built")
    glb = os.path.join(scratch, "panel.glb")
    played = os.path.exists(glb) and any(book.get("library") == "newlib" for book in
                                         _materials(glb).get("Screen", {}).get("flipbooks", []))
    if not played:
        failures.append("the game model does not play newlib's frames on Screen")

    for failure in failures:
        print("FAILED", failure)
    if args.keep or failures:
        print("scratch:", scratch)
    else:
        shutil.rmtree(scratch, ignore_errors=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
