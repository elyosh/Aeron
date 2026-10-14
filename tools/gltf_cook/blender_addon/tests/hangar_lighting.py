"""Tests the hangar lighting bake in a headless Blender.

    python3 hangar_lighting.py --blender BLENDER --baker ENVIRONMENT_BAKE [--keep]

Checks that:
- the panorama has the baker's layout: emissive spheres at the six cube
  directions around the probe are where the baker expects them (forward at
  the centre, +X at three quarters of the width, +Y at the top), with
  their linear values;
- Preview Lighting shows the panorama as the World, turned so that a
  camera with the probe's rotation sees the same panorama, also on a
  rotated model; a second click puts the World back;
- the probe is added above the InsideHangar hardpoint; its position goes to
  the glTF and back;
- the bake writes the package files, with the panorama, and restores the scene.
"""

import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile

ADDON_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
MODULE = "bl_ext.user_default.aeron_flight_model"

# Cube direction -> emission color of its sphere.
DIRECTIONS = {
    "+X": ((1, 0, 0), (2.0, 0.0, 0.0)), "-X": ((-1, 0, 0), (0.0, 2.0, 0.0)),
    "+Y": ((0, 1, 0), (0.0, 0.0, 2.0)), "-Y": ((0, -1, 0), (2.0, 2.0, 0.0)),
    "+Z": ((0, 0, 1), (0.0, 2.0, 2.0)), "-Z": ((0, 0, -1), (2.0, 0.0, 2.0)),
}


def expected_uv(c):
    """The baker's panorama convention (envmap.h)."""
    x, y, z = c
    return 0.5 + math.atan2(x, -z) / (2.0 * math.pi), 0.5 - math.asin(y) / math.pi


# ---- inside Blender ------------------------------------------------------------

def blender_main(baker, scratch):
    import addon_utils
    import bpy
    import mathutils

    addon_utils.enable(MODULE, default_set=True, handle_error=None)
    bpy.context.preferences.addons[MODULE].preferences.environment_bake = baker
    from bl_ext.user_default.aeron_flight_model import hangar_lighting

    failures = []

    def check(condition, message):
        print("HANGAR", "ok" if condition else "FAILED", message)
        if not condition:
            failures.append(message)

    scene = bpy.context.scene
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    root = bpy.data.objects.new("testhangar", None)
    scene.collection.objects.link(root)
    root.aeron_flight.role = 'model'
    project = os.path.join(scratch, "project", "models", "testhangar")
    os.makedirs(project)
    root.aeron_flight.source_path = os.path.join(project, "testhangar.gltf")

    def emissive(name, color):
        material = bpy.data.materials.new(name)
        tree = material.node_tree
        tree.nodes.clear()
        out = tree.nodes.new('ShaderNodeOutputMaterial')
        emission = tree.nodes.new('ShaderNodeEmission')
        emission.inputs["Color"].default_value = (*color, 1.0)
        emission.inputs["Strength"].default_value = 1.0
        tree.links.new(emission.outputs[0], out.inputs[0])
        return material

    for name, (c, color) in DIRECTIONS.items():
        # Blender position of cube direction c: cube = (-x, z, y).
        b = mathutils.Vector((-c[0], c[2], c[1])) * 10.0
        mesh = bpy.data.meshes.new(name)
        import bmesh
        bm = bmesh.new()
        bmesh.ops.create_uvsphere(bm, u_segments=16, v_segments=8, radius=1.0)
        bm.to_mesh(mesh)
        bm.free()
        mesh.materials.append(emissive(name, color))
        sphere = bpy.data.objects.new(name, mesh)
        sphere.location = b
        scene.collection.objects.link(sphere)
    with bpy.context.temp_override(active_object=root, object=root):
        bpy.ops.aeron.add_probe()
    probe = hangar_lighting.probe_of(root)
    probe.location = (0.0, 0.0, 0.0)
    probe.aeron_probe.samples = 16
    remaster = os.path.join(scratch, "remaster")
    os.makedirs(remaster)

    # The layout, on a small panorama.
    hangar_lighting.PANORAMA_SIZE = (256, 128)
    path = os.path.join(scratch, "panorama.hdr")
    hangar_lighting.render_panorama(scene, probe, path)

    def check_layout(path, what, reference=None):
        """Checks the spheres' colors, or the `reference` layout's, and returns them."""
        out = {}
        image = bpy.data.images.load(path)
        width, height = image.size
        pixels = list(image.pixels)
        bpy.data.images.remove(image)
        for name, (c, color) in DIRECTIONS.items():
            u, v = expected_uv(c)
            u = min(max(u, 0.01), 0.99)  # +Z is on the seam
            if name == "+Y":
                v = 0.02
            elif name == "-Y":
                v = 0.98
            x = min(int(u * width), width - 1)
            y = height - 1 - min(int(v * height), height - 1)  # Blender rows start at the bottom
            got = pixels[(y * width + x) * 4:(y * width + x) * 4 + 3]
            out[name] = got
            if reference:
                color = reference[name]
            # 16 samples and denoising leave a few percent of noise.
            check(all(abs(g - e) < 0.15 * max(e, 1.0) for g, e in zip(got, color)),
                  "{}: {} is at the place and level expected: {} at ({:.2f}, {:.2f})".format(
                      what, name, [round(g, 2) for g in got], u, v))
        return out

    layout = check_layout(path, "panorama")

    # The preview, seen by a camera with the probe's rotation, on a rotated model.
    root.rotation_euler = (0.3, 0.5, 1.1)
    bpy.context.view_layer.update()
    probe.aeron_probe.panorama = path
    previous = scene.world
    with bpy.context.temp_override(active_object=probe, object=probe):
        bpy.ops.aeron.preview_hangar_lighting()
    for name in DIRECTIONS:
        bpy.data.objects[name].hide_render = True
    camera_data = bpy.data.cameras.new("view")
    camera_data.type = 'PANO'
    camera_data.panorama_type = 'EQUIRECTANGULAR'
    camera = bpy.data.objects.new("view", camera_data)
    camera.matrix_world = hangar_lighting._camera_rotation(probe).to_4x4()
    scene.collection.objects.link(camera)
    scene.camera = camera
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 16
    scene.render.resolution_x, scene.render.resolution_y = hangar_lighting.PANORAMA_SIZE
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = 'Standard'
    scene.render.image_settings.file_format = 'HDR'
    bpy.ops.render.render()
    preview = os.path.join(scratch, "preview.hdr")
    bpy.data.images["Render Result"].save_render(preview, scene=scene)
    check_layout(preview, "preview", layout)
    with bpy.context.temp_override(active_object=probe, object=probe):
        bpy.ops.aeron.preview_hangar_lighting()
    check(scene.world == previous and not any(w.get(hangar_lighting.PREVIEW_TAG) for w in bpy.data.worlds),
          "a second click puts the World back")
    for name in DIRECTIONS:
        bpy.data.objects[name].hide_render = False
    bpy.data.objects.remove(camera)
    scene.camera = None
    root.rotation_euler = (0.0, 0.0, 0.0)

    # Added above the InsideHangar hardpoint.
    hangar = bpy.data.objects.new("hangar", bpy.data.meshes.new("hangar"))
    scene.collection.objects.link(hangar)
    hangar.parent = root
    hangar.aeron_flight.role = 'component'
    mark = bpy.data.objects.new("inside", None)
    scene.collection.objects.link(mark)
    mark.parent = hangar
    mark.location = (1.0, 2.0, -4.0)
    mark.aeron_flight.role = 'hardpoint'
    mark.aeron_flight.hardpoint_type = '25'
    bpy.context.view_layer.update()
    bpy.data.objects.remove(probe)
    with bpy.context.temp_override(active_object=root, object=root):
        bpy.ops.aeron.add_probe()
    probe = hangar_lighting.probe_of(root)
    probe.aeron_probe.samples = 16
    check((probe.matrix_world.translation - mathutils.Vector((1.0, 2.0, -1.0))).length < 1e-6,
          "the probe is 3 m above the InsideHangar hardpoint: {}".format(probe.matrix_world.translation))

    # The probe in the glTF.
    probe.location = (1.0, 2.0, 3.0)
    bpy.context.view_layer.update()
    item = hangar_lighting.probe_json(root)
    check([round(value, 6) for value in item["position"]] == [1.0, 3.0, -2.0],
          "probe position in glTF axes: {}".format(item))
    other = bpy.data.objects.new("other", None)
    scene.collection.objects.link(other)
    hangar_lighting.restore(other, {"environmentProbe": item})
    bpy.context.view_layer.update()
    back = hangar_lighting.probe_of(other)
    check(back is not None and (back.matrix_world.translation - mathutils.Vector((1.0, 2.0, 3.0))).length < 1e-6,
          "the import puts the probe back")
    bpy.data.objects.remove(back)
    bpy.data.objects.remove(other)

    # The bake.
    engine, objects = scene.render.engine, len(bpy.data.objects)
    with bpy.context.temp_override(active_object=probe, object=probe):
        result = bpy.ops.aeron.bake_hangar_lighting(directory=remaster)
    out = os.path.join(remaster, "flight", "environments", "hangars", "testhangar")
    files = sorted(os.listdir(out)) if os.path.isdir(out) else []
    check('FINISHED' in result and files == ["diffuse.ktx2", "mapping.yaml", "package.yaml", "panorama.hdr", "specular.ktx2"],
          "the bake writes the package files: {}".format(files))
    check(scene.render.engine == engine and len(bpy.data.objects) == objects and scene.camera is None,
          "the bake restores the scene")
    sys.exit(1 if failures else 0)


# ---- outside Blender -------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--blender", required=True)
    parser.add_argument("--baker", required=True)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--inside")
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    args = parser.parse_args(argv)
    args.baker = os.path.abspath(args.baker)
    if args.inside:
        blender_main(args.baker, args.inside)
        return

    scratch = tempfile.mkdtemp(prefix="aeron_hangar_lighting_")
    os.makedirs(os.path.join(scratch, "ext", "user_default"))
    os.symlink(ADDON_DIR, os.path.join(scratch, "ext", "user_default", "aeron_flight_model"))
    env = dict(os.environ, BLENDER_USER_EXTENSIONS=os.path.join(scratch, "ext"),
               BLENDER_USER_CONFIG=os.path.join(scratch, "config"),
               BLENDER_USER_SCRIPTS=os.path.join(scratch, "scripts"))
    completed = subprocess.run([args.blender, "-b", "--factory-startup", "--python-exit-code", "1",
                                "--python", os.path.abspath(__file__), "--", "--blender", args.blender,
                                "--baker", args.baker, "--inside", scratch],
                               env=env, capture_output=True, text=True)
    failed = completed.returncode != 0
    for line in completed.stdout.splitlines():
        if line.startswith(("HANGAR", "Error:")):
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
