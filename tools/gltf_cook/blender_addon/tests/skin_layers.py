"""Tests the skin layers in a headless Blender.

    python3 skin_layers.py --blender BLENDER --cooker AERON_GLTF_COOK [--keep]

Builds the skinned example of the cooker's test (../../tests/skin_layers.py)
as model `panel` of a scratch project, and a second copy whose glTF variant
has another name. In a headless Blender with this add-on, it checks that:
- the import restores the materials' skins, the skin variants and the
  markings; importing the second model keeps the variants of the first;
- showing a skin variant puts preview copies, which mix the layers, in the
  slots of the materials it changes, and the base variant's material
  elsewhere; a skin variant that no markings row names shows from its
  eye; showing the materials as modeled puts the own ones back and
  removes the copies;
- Save to Project during a preview writes no preview material, and writes
  the same skins (by image file) and skin variants, with the images under
  textures/skins/;
- Check Model reports no problem, and the saved model cooks;
- the panels draw.
"""

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.parse

ADDON_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
MODULE = "bl_ext.user_default.aeron_flight_model"
COOKER_TEST = os.path.join(ADDON_DIR, "..", "tests", "skin_layers.py")


def _cooker_test():
    spec = importlib.util.spec_from_file_location("cooker_skin_layers", COOKER_TEST)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _skins_by_file(document, folder):
    """Each material's skins, with each image as its file's bytes."""
    out = {}
    for material in document.get("materials", []):
        skins = material.get("extensions", {}).get("AERON_flight_model", {}).get("skins")
        if not skins:
            continue
        entry = {}
        for name, layer in skins.items():
            files = {}
            for key, value in layer.items():
                if isinstance(value, dict):
                    image = document["images"][document["textures"][value["index"]]["source"]]
                    with open(os.path.join(folder, urllib.parse.unquote(image["uri"])), "rb") as stream:
                        files[key] = stream.read()
                else:
                    files[key] = value
            entry[name] = files
        out[material["name"]] = entry
    return out


def _root_extension(document):
    return next(node["extensions"]["AERON_flight_model"] for node in document["nodes"]
                if node.get("extensions", {}).get("AERON_flight_model", {}).get("role") == "model")


class _Layout:
    """A fake UI layout: drawing a panel with it checks that every property it shows exists."""

    def __getattr__(self, name):
        return lambda *args, **kwargs: _Layout()

    def prop(self, data, name, **kwargs):
        assert name in data.bl_rna.properties, "{} has no property {}".format(data, name)
        return _Layout()

    def template_ID(self, data, name, **kwargs):
        return self.prop(data, name)


# ---- inside Blender ------------------------------------------------------------

def blender_main(cooker, project, second):
    import addon_utils
    import bpy

    addon_utils.enable(MODULE, default_set=True, handle_error=None)
    bpy.context.preferences.addons[MODULE].preferences.cooker = cooker
    from bl_ext.user_default.aeron_flight_model import display, skins, skins_ui, validation

    failures = []

    def check(condition, message):
        print("SKINS", "ok" if condition else "FAILED", message)
        if not condition:
            failures.append(message)

    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    gltf = os.path.join(project, "models", "panel", "panel.gltf")
    source = json.load(open(gltf))
    check('FINISHED' in bpy.ops.import_scene.aeron_flight_model(filepath=gltf), "import")
    root = next(obj for obj in bpy.data.objects if obj.aeron_flight.role == 'model')
    lights, screen = bpy.data.materials["Lights"], bpy.data.materials["Screen"]
    check([skin.name for skin in lights.aeron_flight.skins] == ["red", "glow"] and
          [skin.name for skin in screen.aeron_flight.skins] == ["red"], "the materials' skins are restored")
    glow = lights.aeron_flight.skins["glow"]
    check(glow.base_color is not None and glow.emissive is not None and glow.metallic_roughness is None and
          glow.emissive_strength_given and glow.emissive_strength == 2, "a skin keeps its images and strength")
    check(skins.variants_json(root) == _root_extension(source)["skinVariants"], "the skin variants are restored")
    check([entry.variant for entry in root.aeron_flight.markings] == ["", "Red", "Glow"], "the markings are restored")

    # A second model with another glTF variant: the first keeps its variants.
    check('FINISHED' in bpy.ops.import_scene.aeron_flight_model(filepath=second), "import of a second model")
    names = [variant.name for variant in bpy.data.scenes[0].gltf2_KHR_materials_variants_variants]
    first_mesh = next(c for c in root.children if c.type == 'MESH' and c.data.gltf2_variant_mesh_data)
    mapped = [names[p.variant.variant_idx] for item in first_mesh.data.gltf2_variant_mesh_data for p in item.variants]
    check(names == ["State 1", "Other"] and mapped == ["State 1"],
          "the variants of both models are kept: {} {}".format(names, mapped))
    second_root = next(obj for obj in bpy.data.objects if obj.aeron_flight.role == 'model' and obj != root)
    for obj in [second_root] + list(second_root.children_recursive):
        bpy.data.objects.remove(obj)

    components = {c.name: c for c in root.children if c.aeron_flight.role == 'component'}
    slot_lights = components["LightsPanel"].material_slots[0]
    slot_screen = components["ScreenPanel"].material_slots[0]
    display.apply_skin(root, 1)  # Red
    check(slot_lights.material.aeron_flight.preview_of == bpy.data.materials["Lights"] and
          slot_screen.material.aeron_flight.preview_of == bpy.data.materials["Screen"],
          "Red shows preview copies of both materials")
    mixes = [node for node in slot_lights.material.node_tree.nodes if node.type == 'MIX']
    check(len(mixes) == 1, "the Red copy of Lights mixes one layer")
    display.apply_skin(root, 2)  # Glow, from State 1
    check(slot_lights.material.aeron_flight.preview_of == bpy.data.materials["Lights"] and
          len([n for n in slot_lights.material.node_tree.nodes if n.type == 'MIX']) == 2,
          "Glow mixes its layer over the base color and the emissive")
    check(slot_screen.material == bpy.data.materials["Screen"], "Glow leaves Screen, which has no glow, as it is")
    root.aeron_flight.preview_markings = 2

    # A skin variant that no markings row names.
    bpy.context.view_layer.objects.active = root
    with bpy.context.temp_override(object=root, active_object=root):
        bpy.ops.aeron.show_skin(markings=-1, variant="Red glow")
    check(slot_lights.material.aeron_flight.preview_of == bpy.data.materials["Lights"] and
          len([n for n in slot_lights.material.node_tree.nodes if n.type == 'MIX']) == 3 and
          display.models_showing_skins() == [root],
          "the eye of Red glow mixes its three layers over Lights")
    with bpy.context.temp_override(object=root, active_object=root):
        bpy.ops.aeron.show_skin(markings=2)
    check(root.aeron_flight.preview_variant == "", "showing a markings value ends the skin variant's preview")

    # Save to Project during the preview.
    bpy.context.view_layer.objects.active = root
    with bpy.context.temp_override(object=root, active_object=root):
        result = bpy.ops.export_scene.aeron_flight_model_project(filepath=gltf)
    check('FINISHED' in result, "Save to Project")
    saved = json.load(open(gltf))
    check(not any("(skin preview)" in m.get("name", "") for m in saved["materials"]), "no preview material is saved")
    folder = os.path.dirname(gltf)
    check(_skins_by_file(saved, folder) == _skins_by_file(source, folder), "the skins are saved with the same images")
    check(_root_extension(saved).get("skinVariants") == _root_extension(source)["skinVariants"] and
          _root_extension(saved).get("markings") == [None, "Red", "Glow"], "the skin variants and markings are saved")
    uris = [image["uri"] for image in saved["images"]]
    check(all(not uri.startswith("..") for uri in uris) and any(uri.startswith("textures/skins/") for uri in uris),
          "the skin images stay in the model folder")
    check(slot_lights.material.aeron_flight.preview_of is not None, "the preview shows again after the save")

    problems = validation.validate(root)
    check(not problems.errors, "Check Model: {}".format(problems.errors[:3]))
    out = os.path.join(project, "panel.glb")
    run = subprocess.run([cooker, gltf, "--out", out, "--quality", "fast"], capture_output=True, text=True)
    check(run.returncode == 0, "the saved model cooks: " + run.stderr.strip()[-200:])

    display.apply_skin(root, -1)
    check(slot_lights.material == bpy.data.materials["Lights"] and slot_screen.material == bpy.data.materials["Screen"]
          and not any(m.get(skins.PREVIEW_TAG) for m in bpy.data.materials),
          "As modeled restores the materials and removes the copies")

    # The panels draw.
    class Context:
        object = root
        material = lights
    for panel in (skins_ui.MATERIAL_PT_aeron_skins, skins_ui.OBJECT_PT_aeron_skin_variants):
        drawn = type("Drawn", (), {"layout": _Layout()})()
        try:
            panel.draw(drawn, Context)
            ok = True
        except Exception as error:  # noqa: BLE001
            ok = False
            print(error)
        check(ok, "panel {} draws".format(panel.__name__))
    sys.exit(1 if failures else 0)


# ---- outside Blender -------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--blender", required=True)
    parser.add_argument("--cooker", required=True)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--inside", nargs=2)
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    args = parser.parse_args(argv)
    args.cooker = os.path.abspath(args.cooker)
    if args.inside:
        blender_main(args.cooker, *args.inside)
        return

    cooker_test = _cooker_test()
    scratch = tempfile.mkdtemp(prefix="aeron_skin_layers_addon_")
    project = os.path.join(scratch, "project")
    os.makedirs(os.path.join(project, "models"))
    cooker_test.build(os.path.join(project, "models", "panel"))
    # The second model: the same, with a glTF variant of another name.
    second_folder = os.path.join(scratch, "second")
    second, doc = cooker_test.build(second_folder)
    doc["extensions"]["KHR_materials_variants"]["variants"][0]["name"] = "Other"
    for variant in doc["nodes"][0]["extensions"]["AERON_flight_model"]["skinVariants"]:
        if variant.get("base") == "State 1":
            variant["base"] = "Other"
    json.dump(doc, open(second, "w"))

    os.makedirs(os.path.join(scratch, "ext", "user_default"))
    os.symlink(ADDON_DIR, os.path.join(scratch, "ext", "user_default", "aeron_flight_model"))
    env = dict(os.environ, BLENDER_USER_EXTENSIONS=os.path.join(scratch, "ext"),
               BLENDER_USER_CONFIG=os.path.join(scratch, "config"),
               BLENDER_USER_SCRIPTS=os.path.join(scratch, "scripts"))
    completed = subprocess.run([args.blender, "-b", "--factory-startup", "--python-exit-code", "1",
                                "--python", os.path.abspath(__file__), "--", "--blender", args.blender,
                                "--cooker", args.cooker, "--inside", project, second],
                               env=env, capture_output=True, text=True)
    failed = completed.returncode != 0
    for line in completed.stdout.splitlines():
        if line.startswith(("SKINS", "Error:")):
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
