"""Tests the hangar lighting bake in a headless Blender.

    python3 hangar_lighting.py --blender BLENDER --baker ENVIRONMENT_BAKE [--keep]

Checks that:
- the panorama has the baker's layout: emissive spheres at the six cube
  directions around the probe are where the baker expects them (forward at
  the centre, +X at three quarters of the width, +Y at the top), with
  their linear values times the emissive scale, and a light object is seen
  by the probe;
- the probe's position goes to the glTF and back;
- the bake reads the light levels from the game's config.yaml, writes the
  package files and restores the scene.
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
    # A point light between +X and +Y, seen by the probe.
    light = bpy.data.objects.new("lamp", bpy.data.lights.new("lamp", 'POINT'))
    light.data.shadow_soft_size = 0.5
    light.data.energy = 5000.0
    light.location = mathutils.Vector((-1.0, 0.0, 1.0)).normalized() * 10.0
    scene.collection.objects.link(light)

    with bpy.context.temp_override(active_object=root, object=root):
        bpy.ops.aeron.add_probe()
    probe = hangar_lighting.probe_of(root)
    probe.location = (0.0, 0.0, 0.0)
    probe.aeron_probe.samples = 16
    remaster = os.path.join(scratch, "remaster")
    os.makedirs(remaster)
    with open(os.path.join(remaster, "config.yaml"), "w") as stream:
        stream.write("hangar_lighting:\n  enabled: false\n  direction_x: 0.0\n  direction_y: 0.0\n"
                     "  direction_z: 1.0\n  intensity: 0.8\n  color_r: 1.0\n  color_g: 1.0\n  color_b: 1.0\n"
                     "point_lights:\n  model_light_intensity: 0.0014641288 # 1/683\n"
                     "lighting:\n  sun_illuminance_scale: 2.5\n  emissive_scale: 3.0\n")
    config = hangar_lighting.read_config(os.path.join(remaster, "config.yaml"))

    # The layout, on a small panorama.
    hangar_lighting.PANORAMA_SIZE = (256, 128)
    path = os.path.join(scratch, "panorama.hdr")
    hangar_lighting.render_panorama(scene, probe, config, path)
    image = bpy.data.images.load(path)
    width, height = image.size
    pixels = list(image.pixels)
    bpy.data.images.remove(image)

    def pixel(u, v):  # v from the top
        x = min(int(u * width), width - 1) % width
        y = height - 1 - min(int(v * height), height - 1)  # Blender rows start at the bottom
        i = (y * width + x) * 4
        return pixels[i:i + 3]

    for name, (c, color) in DIRECTIONS.items():
        u, v = expected_uv(c)
        u = min(max(u, 0.01), 0.99)  # +Z is on the seam
        if name == "+Y":
            v = 0.02
        elif name == "-Y":
            v = 0.98
        got = pixel(u, v)
        # Emissive scale 3; 16 samples and denoising leave a few percent of noise.
        check(all(abs(g - 3.0 * e) < 0.15 * max(3.0 * e, 1.0) for g, e in zip(got, color)),
              "{} is at the baker's place, linear, x 3: {} at ({:.2f}, {:.2f})".format(
                  name, [round(g, 2) for g in got], u, v))
    u, v = expected_uv((math.sqrt(0.5), math.sqrt(0.5), 0.0))
    check(sum(pixel(u, v)) > 1.0, "the probe sees the light object: {}".format(pixel(u, v)))

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
    check('FINISHED' in result and files == ["diffuse.ktx2", "mapping.yaml", "package.yaml", "specular.ktx2"],
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
