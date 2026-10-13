"""Tests the variants and skin layers in a headless Blender.

    python3 skin_layers.py --blender BLENDER --cooker AERON_GLTF_COOK [--keep]

Builds the skinned example of the cooker's test (../../tests/skin_layers.py)
as model `panel` of a scratch project, and a second copy whose glTF variant
has another name. In a headless Blender with this add-on, it checks that:
- the import restores the materials' skins, the variants (the glTF one
  first) and the markings; importing the second model keeps the variants
  of the first;
- showing a variant puts preview copies, which mix the layers, in the
  slots of the materials it paints, and the replaced or base variant's
  material elsewhere; a variant that no markings row names shows from its
  eye, including the glTF one; showing the materials as modeled puts the
  own ones back and removes the copies;
- renaming the glTF variant renames its replacements and the bases that
  name it; a new variant can replace a material and paint a skin;
- Save to Project during a preview writes no preview material, and writes
  the same skins (by image file), the variants as glTF variants and skin
  variants, with the images under textures/skins/;
- Check Model reports no problem, and the saved model cooks;
- removing a variant removes its replacements;
- the eye of a skin shows it alone; Paint shows the skin alone, selects the
  parts with the material and paints its image in Texture Paint mode, and
  stops on a second click;
- the preview follows a skin image that is swapped; the game export writes
  unsaved paint of a skin image; a new skin image is transparent (base
  color) or a copy of the material's (metallic-roughness), with its size,
  and is saved in the model folder;
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
    from bl_ext.user_default.aeron_flight_model import display, skins, skins_ui, validation, variants, variants_ui

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
    check(glow.base_color.name == "glow_base" and glow.emissive.name == "glow_emissive",
          "skin images have their glTF names: {}".format(glow.base_color.name))
    check(variants.names(root) == ["State 1", "Red", "Glow", "Red glow"], "the variants: " + str(variants.names(root)))
    check(variants.skin_variants_json(root) == _root_extension(source)["skinVariants"], "the skin variants are restored")
    check(variants.replacements(root, "State 1") == [(lights, bpy.data.materials["LightsAlt"])],
          "State 1 replaces Lights with LightsAlt")
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

    # Variants that no markings row names.
    bpy.context.view_layer.objects.active = root
    alt = bpy.data.materials["LightsAlt"]

    def op(name, **properties):
        with bpy.context.temp_override(object=root, active_object=root):
            return getattr(bpy.ops.aeron, name)(**properties)
    op("show_skin", markings=-1, variant="Red glow")
    check(slot_lights.material.aeron_flight.preview_of == bpy.data.materials["Lights"] and
          len([n for n in slot_lights.material.node_tree.nodes if n.type == 'MIX']) == 3 and
          display.models_showing_skins() == [root],
          "the eye of Red glow mixes its three layers over Lights")
    op("show_skin", markings=-1, variant="State 1")
    check(slot_lights.material == alt and slot_screen.material == screen, "the eye of State 1 shows LightsAlt")

    # Rename the glTF variant.
    root.aeron_flight.variants[0].name = "Paint"
    check(variants.replacements(root, "Paint") == [(lights, alt)] and not variants.replacements(root, "State 1"),
          "the renamed variant keeps its replacement")
    check([v.base for v in root.aeron_flight.variants] == ["", "", "Paint", "Paint"] and
          root.aeron_flight.preview_variant == "Paint", "the bases and the preview follow the rename")

    # A new variant that replaces Screen and paints the glow.
    check('FINISHED' in op("variant_edit", target='VARIANTS', action='ADD'), "add a variant")
    blue = root.aeron_flight.variants[-1]
    blue.name = "Blue"
    check('FINISHED' in op("variant_replace", material="Screen", replacement="LightsAlt"), "replace Screen")
    op("variant_edit", target='LAYERS', action='ADD')
    blue.layers[0].skin = "glow"
    op("show_skin", markings=-1, variant="Blue")
    check(slot_lights.material.aeron_flight.preview_of == lights and slot_screen.material == alt,
          "Blue paints the glow over Lights and replaces Screen")
    op("show_skin", markings=2)
    check(root.aeron_flight.preview_variant == "", "showing a markings value ends the variant's preview")
    previous = glow.emissive
    other = bpy.data.images.load(os.path.join(project, "models", "panel", "textures", "skins", "red_screen.png"))
    glow.emissive = other
    shown = {node.image for node in slot_lights.material.node_tree.nodes if node.type == 'TEX_IMAGE'}
    check(other in shown and previous not in shown, "the preview follows a swapped skin image")
    glow.emissive = previous

    # Save to Project during the preview.
    bpy.context.view_layer.objects.active = root
    with bpy.context.temp_override(object=root, active_object=root):
        result = bpy.ops.export_scene.aeron_flight_model_project(filepath=gltf)
    check('FINISHED' in result, "Save to Project")
    saved = json.load(open(gltf))
    check(not any("(skin preview)" in m.get("name", "") for m in saved["materials"]), "no preview material is saved")
    folder = os.path.dirname(gltf)
    check(_skins_by_file(saved, folder) == _skins_by_file(source, folder), "the skins are saved with the same images")
    expected = [dict(v, base="Paint") if v.get("base") else v for v in _root_extension(source)["skinVariants"]]
    expected.append({"name": "Blue", "layers": [{"skin": "glow"}]})
    check(_root_extension(saved).get("skinVariants") == expected and
          _root_extension(saved).get("markings") == [None, "Red", "Glow"],
          "the skin variants and markings are saved: {}".format(_root_extension(saved).get("skinVariants")))
    khr = [v["name"] for v in saved["extensions"]["KHR_materials_variants"]["variants"]]
    check({"Paint", "Blue"} <= set(khr) and "State 1" not in khr, "the glTF variants are saved: {}".format(khr))
    uris = [image["uri"] for image in saved["images"]]
    check(all(not uri.startswith("..") for uri in uris) and any(uri.startswith("textures/skins/") for uri in uris),
          "the skin images stay in the model folder")
    check(slot_lights.material.aeron_flight.preview_of is not None, "the preview shows again after the save")

    problems = validation.validate(root)
    check(not problems.errors, "Check Model: {}".format(problems.errors[:3]))
    out = os.path.join(project, "panel.glb")
    run = subprocess.run([cooker, gltf, "--out", out, "--quality", "fast"], capture_output=True, text=True)
    check(run.returncode == 0, "the saved model cooks: " + run.stderr.strip()[-200:])

    # The game export writes a skin image's unsaved paint.
    import numpy
    red_image = lights.aeron_flight.skins["red"].base_color
    pixels = numpy.empty(len(red_image.pixels), dtype=numpy.float32)
    red_image.pixels.foreach_get(pixels)
    pixels[:4] = (0.0, 1.0, 0.0, 1.0)
    red_image.pixels.foreach_set(pixels)
    document = {"materials": [{"name": "Lights"}]}
    skins.add_material_skins(document, os.path.join(project, "game", "panel.gltf"), False)
    written = bpy.data.images.load(os.path.join(project, "game", document["images"][0]["uri"]), check_existing=False)
    first = [round(value, 3) for value in written.pixels[:4]]
    check(first == [0.0, 1.0, 0.0, 1.0], "the game export writes unsaved paint: {}".format(first))
    red_image.reload()

    root.aeron_flight.active_variant = len(root.aeron_flight.variants) - 1
    op("variant_edit", target='VARIANTS', action='REMOVE')
    check("Blue" not in variants.names(root) and not variants.replacements(root, "Blue"),
          "removing Blue removes its replacement")

    display.apply_skin(root, -1)
    check(slot_lights.material == bpy.data.materials["Lights"] and slot_screen.material == bpy.data.materials["Screen"]
          and not any(m.get(skins.PREVIEW_TAG) for m in bpy.data.materials),
          "As modeled restores the materials and removes the copies")

    # The eye of a skin, and Paint.
    components["LightsPanel"].select_set(True)
    bpy.context.view_layer.objects.active = components["LightsPanel"]
    lights.aeron_flight.active_skin = 0  # red

    def material_op(name, **properties):
        obj = bpy.context.view_layer.objects.active
        with bpy.context.temp_override(object=obj, active_object=obj, material=lights):
            return getattr(bpy.ops.aeron, name)(**properties)
    material_op("show_skin", markings=-1, skin="red")
    check(slot_lights.material.aeron_flight.preview_of == lights and
          slot_screen.material.aeron_flight.preview_of == screen and root.aeron_flight.preview_skin == "red",
          "the eye of skin red shows it on both materials")
    material_op("show_skin", markings=-1, skin="glow")
    check(slot_lights.material.aeron_flight.preview_of == lights and slot_screen.material == screen,
          "the eye of skin glow shows it on Lights only")
    lights.aeron_flight.active_skin = 1  # glow
    check('FINISHED' in material_op("skin_paint", channel='emissive'), "Paint")
    paint = bpy.context.scene.tool_settings.image_paint
    target = bpy.context.view_layer.objects.active
    check(target == components["LightsPanel"] and target.mode == 'TEXTURE_PAINT' and paint.mode == 'IMAGE' and
          paint.canvas == glow.emissive and root.aeron_flight.preview_skin == "glow" and
          slot_lights.material.aeron_flight.preview_of == lights,
          "Paint shows the skin alone and paints its emissive image")
    check('FINISHED' in material_op("skin_paint", channel='emissive') and target.mode == 'OBJECT' and
          root.aeron_flight.preview_skin == "" and slot_lights.material == lights, "a second click stops painting")

    # Buttons set only some properties of these operators: Blender must not
    # reuse the others from the last click.
    show = bpy.ops.aeron.show_skin.get_rna_type().properties
    check(all(show[name].is_skip_save for name in ("markings", "variant", "skin")),
          "the show operator does not reuse the last click's properties")

    # New skin images.
    with bpy.context.temp_override(object=root, active_object=root, material=lights):
        bpy.ops.aeron.skin_edit(action='ADD')
        bpy.ops.aeron.skin_new_image(channel='base_color')
        bpy.ops.aeron.skin_new_image(channel='metallic_roughness')
    fresh = lights.aeron_flight.skins[-1]
    own_base = skins._own_image(lights, "base_color")
    alpha = numpy.empty(len(fresh.base_color.pixels), dtype=numpy.float32)
    fresh.base_color.pixels.foreach_get(alpha)
    check(tuple(fresh.base_color.size) == tuple(own_base.size) and not alpha[3::4].any(),
          "a new base color image is transparent, with the material's size")
    own_mr = skins._own_image(lights, "metallic_roughness")
    new_mr = numpy.empty(len(fresh.metallic_roughness.pixels), dtype=numpy.float32)
    fresh.metallic_roughness.pixels.foreach_get(new_mr)
    if own_mr is not None:
        expected = numpy.empty(len(own_mr.pixels), dtype=numpy.float32)
        own_mr.pixels.foreach_get(expected)
        same = numpy.array_equal(expected, new_mr)
    else:
        bsdf = next(n for n in lights.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
        same = numpy.allclose(new_mr[1::4], bsdf.inputs["Roughness"].default_value, atol=1 / 255)
    check(same and fresh.metallic_roughness.colorspace_settings.name == 'Non-Color',
          "a new metallic-roughness image is the material's")
    folder = os.path.join(project, "models", "panel", "textures", "skins", fresh.name)
    check(sorted(os.listdir(folder)) == ["Lights_base_color.png", "Lights_metallic_roughness.png"] and
          not fresh.base_color.is_dirty, "the new images are saved in the model folder: {}".format(os.listdir(folder)))

    # The panels draw.
    class Context:
        object = root
        material = lights
        scene = bpy.context.scene
    root.aeron_flight.active_variant = 0
    for panel in (skins_ui.MATERIAL_PT_aeron_skins, variants_ui.OBJECT_PT_aeron_variants):
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
