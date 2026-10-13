"""The flipbook preview, headless.

    python3 flipbook_preview.py --blender BLENDER --cooker AERON_GLTF_COOK [--keep]

Builds a scratch source tree holding the animated example (../../example)
as model `panel`, imports it in a headless Blender with this add-on, and
turns the preview on:
- materials Lights and Screen get the preview's nodes;
- at given frames and states, the frame images and values the game would
  show: Lights' emissive frames (always, half a second each), Screen's
  base colour frames only while `target` is checked, its unlit part, and a
  `once` flipbook holding its last frame;
- saving during the preview writes no preview data, and the preview is
  back after;
- turning it off gives the materials back their nodes exactly, and the
  source export writes the same file as before the preview.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ADDON_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The add-on is loaded from its source directory: no bytecode caches there.
sys.dont_write_bytecode = True
EXAMPLE = os.path.join(ADDON_DIR, "..", "example")
MODULE = "bl_ext.user_default.aeron_flight_model"


# ---- inside Blender ------------------------------------------------------------

def _snapshot(material):
    """A material's nodes, links and BSDF input values."""
    tree = material.node_tree
    links = sorted((link.from_node.name, link.from_socket.identifier, link.to_node.name, link.to_socket.identifier)
                   for link in tree.links)
    values = sorted((node.name, socket.identifier, tuple(socket.default_value)
                     if hasattr(socket.default_value, "__len__") else socket.default_value)
                    for node in tree.nodes if node.type == 'BSDF_PRINCIPLED' for socket in node.inputs
                    if hasattr(socket, "default_value") and socket.type in {'RGBA', 'VALUE'})
    return sorted(node.name for node in tree.nodes), links, values


def _source_export(path):
    """The source export, over the model's own file (the export makes the
    model's source the file it writes)."""
    import bpy

    assert 'FINISHED' in bpy.ops.export_scene.aeron_flight_model_source(filepath=path)
    with open(path, encoding="utf-8") as stream:
        return json.load(stream)


def blender_main(cooker, tree, scratch):
    import addon_utils
    import bpy

    addon_utils.enable(MODULE, default_set=True, handle_error=None)
    bpy.context.preferences.addons[MODULE].preferences.cooker = cooker
    from bl_ext.user_default.aeron_flight_model import flipbook_preview

    failures = []

    def check(condition, message):
        print("PREVIEW", "ok" if condition else "FAILED", message)
        if not condition:
            failures.append(message)

    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    gltf = os.path.join(tree, "models", "panel", "panel.gltf")
    check('FINISHED' in bpy.ops.import_scene.aeron_flight_model(filepath=gltf), "import")
    root = next(obj for obj in bpy.data.objects if obj.aeron_flight.role == 'model')
    bpy.context.view_layer.objects.active = root
    scene = bpy.context.scene
    scene.frame_start, scene.render.fps, scene.render.fps_base = 1, 24, 1.0
    lights, screen = bpy.data.materials["Lights"], bpy.data.materials["Screen"]
    before = {material.name: _snapshot(material) for material in (lights, screen)}
    with bpy.context.temp_override(object=root):
        exported = _source_export(gltf)

    wm = bpy.context.window_manager
    wm.aeron_flipbook_preview = True
    check(not flipbook_preview.message and not flipbook_preview.skipped,
          "the preview starts ({} {})".format(flipbook_preview.message, flipbook_preview.skipped))
    for material in (lights, screen):
        nodes = material.node_tree.nodes
        group = nodes.get(flipbook_preview.NODE)
        bsdf = next(node for node in nodes if node.type == 'BSDF_PRINCIPLED')
        check(group is not None and all(name in nodes for name in flipbook_preview.IMAGE_NODES.values()) and
              bsdf.inputs["Base Color"].links[0].from_node == group and
              bsdf.inputs["Emission Color"].links[0].from_node == group,
              "{}: the preview's nodes are inserted".format(material.name))

    def showing(material, role):
        node = material.node_tree.nodes[flipbook_preview.IMAGE_NODES[role]]
        return os.path.basename(node.image.filepath) if node.image else ""

    def value(material, name):
        return material.node_tree.nodes[flipbook_preview.NODE].inputs[name].default_value

    for frame, expected in ((1, "lights_0.png"), (12, "lights_0.png"), (13, "lights_1.png"), (25, "lights_0.png"),
                            (37, "lights_1.png")):
        scene.frame_set(frame)
        check(showing(lights, "emissive") == expected and value(lights, "Replace") == 1.0 and
              value(lights, "Lit Strength") == 3.0 and value(lights, "Lit Base On") == 0.0,
              "Lights at frame {} shows {} at strength 3".format(frame, expected))
    scene.frame_set(13)
    check(value(screen, "Frame On") == 0.0 and value(screen, "Replace") == 0.0, "Screen: nothing without target")
    states = scene.aeron_flight.flipbook_states
    states.target = True
    check(value(screen, "Frame On") == 1.0 and showing(screen, "colour") == "screen_b.png",
          "Screen with target, at frame 13: screen_b")
    scene.frame_set(1)
    check(showing(screen, "colour") == "screen_a.png", "Screen with target, at frame 1: screen_a")
    screen.aeron_flight.flipbooks[0].unlit = 0.5
    check(bpy.app.timers.is_registered(flipbook_preview._deferred), "an edit schedules a refresh")
    flipbook_preview._deferred()  # a headless script runs no timers
    check(abs(value(screen, "Unlit") - 128.0 / 255.0) < 1e-6 and value(screen, "Replace") == 1.0,
          "Screen's unlit part, as the game quantizes it")
    lights.aeron_flight.flipbooks[0].mode = "once"
    flipbook_preview._deferred()
    scene.frame_set(100)
    check(showing(lights, "emissive") == "lights_1.png", "a once flipbook holds its last frame")
    check(flipbook_preview.playing(lights, {"always"}, -0.25)["emissive"][1] == "lights_0" and
          flipbook_preview.playing(screen, {"always"}, 0.0) == {}, "playing: before the start and states")
    lights.aeron_flight.flipbooks[0].mode = "loop"
    screen.aeron_flight.flipbooks[0].unlit = 0.0
    states.target = False

    try:
        _draw(bpy.context, screen)
        check(True, "the panel draws")
    except AssertionError as error:
        check(False, "the panel draws: {}".format(error))

    blend = os.path.join(scratch, "saved.blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend, copy=True)
    check(flipbook_preview._complete(lights) and flipbook_preview._complete(screen), "the preview is back after saving")
    with bpy.context.temp_override(object=root):
        during = _source_export(gltf)
    check(flipbook_preview._complete(lights), "the preview is back after the source export")

    wm.aeron_flipbook_preview = False
    after = {material.name: _snapshot(material) for material in (lights, screen)}
    check(after == before, "turning the preview off gives the materials back their nodes")
    check(not any(group.get(flipbook_preview.TAG) for group in bpy.data.node_groups) and
          not any(image.get(flipbook_preview.TAG) for image in bpy.data.images), "no preview data stays")
    with bpy.data.libraries.load(blend) as (source, target):
        names, groups, images = list(source.materials), list(source.node_groups), list(source.images)
        target.materials = list(names)
    saved = dict(zip(names, target.materials))  # appended as Lights.001…
    check(flipbook_preview.GROUP not in groups and not any(name.startswith(".Aeron Frame") for name in images) and
          all(_snapshot(saved[name]) == before[name] for name in ("Lights", "Screen")),
          "the saved file holds no preview data")
    with bpy.context.temp_override(object=root):
        exported_after = _source_export(gltf)
    check(exported_after == exported and during == exported, "the source export writes the same file")
    sys.exit(1 if failures else 0)


class _Layout:
    """A stand-in UI layout: drawing a panel with it checks that every
    property it shows exists (a headless Blender draws no UI)."""

    def __getattr__(self, name):
        return lambda *args, **kwargs: _Layout()

    def prop(self, data, name, **kwargs):
        assert name in data.bl_rna.properties, "{} has no property {}".format(data, name)


def _draw(context, material):
    import bpy

    panel = bpy.types.MATERIAL_PT_aeron_preview
    with context.temp_override(material=material):
        instance = type("Panel", (), {"layout": _Layout()})()
        panel.draw_header(instance, bpy.context)
        panel.draw(instance, bpy.context)


# ---- outside -------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--blender", required=True)
    parser.add_argument("--cooker", required=True)
    parser.add_argument("--keep", action="store_true", help="keep the scratch directory")
    parser.add_argument("--inside", nargs=2, help=argparse.SUPPRESS)
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    args = parser.parse_args(argv)
    if args.inside:
        blender_main(args.cooker, *args.inside)
        return

    scratch = tempfile.mkdtemp(prefix="aeron_flipbook_preview_")
    tree = os.path.join(scratch, "tree")
    library = os.path.join(tree, "effects", "examplepanel")
    os.makedirs(library)
    shutil.copytree(os.path.join(EXAMPLE, "frames"), os.path.join(library, "frames"))
    shutil.copyfile(os.path.join(EXAMPLE, "examplepanel.yaml"), os.path.join(library, "examplepanel.yaml"))
    model = os.path.join(tree, "models", "panel")
    os.makedirs(model)
    for name in ("panel.gltf", "panel.bin"):
        shutil.copyfile(os.path.join(EXAMPLE, name), os.path.join(model, name))
    shutil.copytree(os.path.join(EXAMPLE, "textures"), os.path.join(model, "textures"))

    os.makedirs(os.path.join(scratch, "ext", "user_default"))
    os.symlink(ADDON_DIR, os.path.join(scratch, "ext", "user_default", "aeron_flight_model"))
    env = dict(os.environ, BLENDER_USER_EXTENSIONS=os.path.join(scratch, "ext"),
               BLENDER_USER_CONFIG=os.path.join(scratch, "config"),
               BLENDER_USER_SCRIPTS=os.path.join(scratch, "scripts"))
    completed = subprocess.run([args.blender, "-b", "--factory-startup", "--python-exit-code", "1",
                                "--python", os.path.abspath(__file__), "--", "--blender", args.blender,
                                "--cooker", args.cooker, "--inside", tree, scratch],
                               env=env, capture_output=True, text=True)
    failed = completed.returncode != 0
    for line in completed.stdout.splitlines():
        if line.startswith(("PREVIEW", "Error:")):
            print(line)
    if failed:
        print("FAILED Blender: " + " | ".join(completed.stderr.strip().splitlines()[-5:]))
    if args.keep or failed:
        print("scratch:", scratch)
    else:
        shutil.rmtree(scratch, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
