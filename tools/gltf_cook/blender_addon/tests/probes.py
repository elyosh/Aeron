"""Tests the probes bake in a headless Blender.

    python3 probes.py --blender BLENDER --baker ENVIRONMENT_BAKE [--keep]

Checks that:
- a probe's panorama has the baker's layout: emissive spheres at the six
  cube directions around it are where the baker expects them (forward at the
  centre, +X at three quarters of the width, +Y at the top), with their
  linear values, also on a rotated model;
- Preview Lighting shows the panorama as the World, turned so that a camera
  with the cube frame sees the same panorama; a second click puts the World
  back;
- an object with hide_probe_sphere is left out of a probe's panorama, not out
  of a grid point's;
- Add Reflection Probe puts the first probe above the InsideHangar
  hardpoint; Add Light Grid covers the model with its outer points on the
  model's bounds;
- a probe and the grid get the package's coordinates (model frame: -x, y, z),
  and the grid points Blender's positions, model x first;
- validation refuses a grid over 8192 points and a rotated probe;
- probes, grid and an old environmentProbe go to the glTF and back, and a
  light inside the grid is baked;
- a grid point inside a closed box is invalid;
- the bake writes the package files and restores the scene.
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
    import bmesh
    import bpy
    import mathutils

    addon_utils.enable(MODULE, default_set=True, handle_error=None)
    bpy.context.preferences.addons[MODULE].preferences.environment_bake = baker
    from bl_ext.user_default.aeron_flight_model import probes, validation

    failures = []

    def check(condition, message):
        print("PROBES", "ok" if condition else "FAILED", message)
        if not condition:
            failures.append(message)

    def close(a, b, tolerance=1e-5):
        return all(abs(x - y) <= tolerance for x, y in zip(a, b))

    scene = bpy.context.scene
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    root = bpy.data.objects.new("testhangar", None)
    scene.collection.objects.link(root)
    root.aeron_flight.role = 'model'
    project = os.path.join(scratch, "project", "models", "testhangar")
    os.makedirs(project)
    root.aeron_flight.source_path = os.path.join(project, "testhangar.gltf")
    hangar = bpy.data.objects.new("hangar", bpy.data.meshes.new("hangar"))
    scene.collection.objects.link(hangar)
    hangar.parent = root
    hangar.aeron_flight.role = 'component'

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

    def mesh_object(name, make, location, material=None):
        mesh = bpy.data.meshes.new(name)
        bm = bmesh.new()
        make(bm)
        bm.to_mesh(mesh)
        bm.free()
        if material:
            mesh.materials.append(material)
        obj = bpy.data.objects.new(name, mesh)
        obj.location = location
        scene.collection.objects.link(obj)
        return obj

    for name, (c, color) in DIRECTIONS.items():
        # Blender position of cube direction c: cube = (-x, z, y).
        mesh_object(name, lambda bm: bmesh.ops.create_uvsphere(bm, u_segments=16, v_segments=8, radius=1.0),
                    mathutils.Vector((-c[0], c[2], c[1])) * 10.0, emissive(name, color))

    # Small renders for the test.
    probes.PROBE_PANORAMA_SIZE = (256, 128)
    probes.GRID_PANORAMA_SIZE = (64, 32)
    probes.GRID_SAMPLES = 16
    scene.aeron_probe_samples = 16

    def read_layout(path):
        image = bpy.data.images.load(path)
        width, height = image.size
        pixels = list(image.pixels)
        bpy.data.images.remove(image)
        out = {}
        for name, (c, color) in DIRECTIONS.items():
            u, v = expected_uv(c)
            u = min(max(u, 0.01), 0.99)  # +Z is on the seam
            v = 0.02 if name == "+Y" else 0.98 if name == "-Y" else v
            x = min(int(u * width), width - 1)
            y = height - 1 - min(int(v * height), height - 1)  # Blender rows start at the bottom
            out[name] = pixels[(y * width + x) * 4:(y * width + x) * 4 + 3]
        return out

    def check_layout(layout, what, reference=None):
        for name, (c, color) in DIRECTIONS.items():
            expected = (reference or {}).get(name, color)
            got = layout[name]
            # 16 samples and denoising leave a few percent of noise.
            check(all(abs(g - e) < 0.15 * max(e, 1.0) for g, e in zip(got, expected)),
                  "{}: {} is at the place and level expected: {}".format(what, name, [round(g, 2) for g in got]))

    # The layout from a probe, on a rotated model: the cube frame turns with it.
    root.rotation_euler = (0.3, 0.5, 1.1)
    bpy.context.view_layer.update()
    for name, (c, color) in DIRECTIONS.items():
        obj = bpy.data.objects[name]
        obj.location = root.matrix_world @ (mathutils.Vector((-c[0], c[2], c[1])) * 10.0)
    with probes._BakeScene(bpy.context, root) as bake:
        bake.hide("hide_probe_sphere")
        path = os.path.join(scratch, "probe.hdr")
        bake.render(path, root.matrix_world.translation, probes.PROBE_PANORAMA_SIZE, 16, True)
    layout = read_layout(path)
    check_layout(layout, "panorama")

    # The preview, seen by a camera with the cube frame.
    with bpy.context.temp_override(active_object=root, object=root):
        bpy.ops.aeron.add_reflection_probe()
    probe = probes.reflection_probes(root)[0]
    probe.aeron_probe.panorama = path
    previous = scene.world
    with bpy.context.temp_override(active_object=probe, object=probe):
        bpy.ops.aeron.preview_probe()
    for name in DIRECTIONS:
        bpy.data.objects[name].hide_render = True
    camera_data = bpy.data.cameras.new("view")
    camera_data.type = 'PANO'
    camera_data.panorama_type = 'EQUIRECTANGULAR'
    camera = bpy.data.objects.new("view", camera_data)
    camera.matrix_world = probes._camera_rotation(root).to_4x4()
    scene.collection.objects.link(camera)
    scene.camera = camera
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 16
    scene.render.resolution_x, scene.render.resolution_y = probes.PROBE_PANORAMA_SIZE
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = 'Standard'
    scene.render.image_settings.file_format = 'HDR'
    bpy.ops.render.render()
    preview = os.path.join(scratch, "preview.hdr")
    bpy.data.images["Render Result"].save_render(preview, scene=scene)
    check_layout(read_layout(preview), "preview", layout)
    with bpy.context.temp_override(active_object=probe, object=probe):
        bpy.ops.aeron.preview_probe()
    check(scene.world == previous and not any(w.get(probes.PREVIEW_TAG) for w in bpy.data.worlds),
          "a second click puts the World back")
    for name in DIRECTIONS:
        bpy.data.objects[name].hide_render = False
    bpy.data.objects.remove(camera)
    scene.camera = None
    bpy.data.objects.remove(probe)
    root.rotation_euler = (0.0, 0.0, 0.0)
    for name, (c, color) in DIRECTIONS.items():
        bpy.data.objects[name].location = mathutils.Vector((-c[0], c[2], c[1])) * 10.0
    bpy.context.view_layer.update()

    # hide_probe_sphere: out of the probes' panoramas, in the grid's.
    plus_x = bpy.data.objects["+X"]
    plus_x.hide_probe_sphere = True
    with probes._BakeScene(bpy.context, root) as bake:
        bake.hide("hide_probe_sphere")
        bake.render(os.path.join(scratch, "hidden.hdr"), mathutils.Vector(), (128, 64), 16, True)
        bake.hide("hide_probe_volume")
        bake.render(os.path.join(scratch, "shown.hdr"), mathutils.Vector(), (128, 64), 16, False)
    hidden, shown = read_layout(os.path.join(scratch, "hidden.hdr")), read_layout(os.path.join(scratch, "shown.hdr"))
    check(max(hidden["+X"]) < 0.2 and shown["+X"][0] > 1.0 and not plus_x.hide_render,
          "hide_probe_sphere leaves an object out of a probe's panorama only: {} / {}".format(
              [round(v, 2) for v in hidden["+X"]], [round(v, 2) for v in shown["+X"]]))
    plus_x.hide_probe_sphere = False

    # Adding: the first probe above the InsideHangar hardpoint, the grid's zone from the floor up.
    mark = bpy.data.objects.new("inside", None)
    scene.collection.objects.link(mark)
    mark.parent = hangar
    mark.location = (1.0, 2.0, -4.0)
    mark.aeron_flight.role = 'hardpoint'
    mark.aeron_flight.hardpoint_type = '25'
    # The hangar: a 40 x 20 x 8 m box.
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=(40.0, 20.0, 8.0), verts=bm.verts)
    bm.to_mesh(hangar.data)
    bm.free()
    bpy.context.view_layer.update()
    with bpy.context.temp_override(active_object=root, object=root):
        bpy.ops.aeron.add_reflection_probe()
        bpy.ops.aeron.add_light_grid()
    probe = probes.reflection_probes(root)[0]
    grid = probes.light_grid(root)
    check((probe.matrix_world.translation - mathutils.Vector((1.0, 2.0, -1.0))).length < 1e-5,
          "the first probe is 3 m above the InsideHangar hardpoint: {}".format(probe.matrix_world.translation))
    item = probes.grid_definition(root, grid)
    # 60 x 60 x 20 m from the floor (z -4) up; 4 m spacing: round(size / 4) - 1 points per axis.
    check(close(item["center"], (-1.0, 2.0, 6.0)) and close(item["size"], (60.0, 60.0, 20.0)) and
          item["resolution"] == [14, 14, 4] and
          [grid.data.resolution_x, grid.data.resolution_y, grid.data.resolution_z] == [14, 14, 4],
          "Add Light Grid: a zone from the floor up, its points from its spacing: {}".format(item))
    grid.aeron_probe.grid_spacing = 0.25
    check(probes.grid_point_count(grid) <= probes.MAX_GRID_POINTS and grid.data.resolution_x < 64,
          "a small spacing stays within 8192 points: {}".format(probes.grid_resolution(grid)))
    grid.aeron_probe.grid_spacing = probes.GRID_SPACING

    # Fill with Probes: 2 x 2 tiles of 30 m over the 60 m zone, reflecting onto the zone.
    with bpy.context.temp_override(active_object=grid, object=grid):
        bpy.ops.aeron.fill_probes()
        bpy.ops.aeron.fill_probes()  # again: replaces its probes
    filled = [p for p in probes.reflection_probes(root) if p.aeron_probe.filled]
    definitions = [probes.probe_definition(root, p) for p in filled]
    centres = sorted(tuple(round(c, 3) for c in d["center"]) for d in definitions)
    check(len(filled) == 4 and len(probes.reflection_probes(root)) == 5 and
          centres == [(-16.0, -13.0, 6.0), (-16.0, 17.0, 6.0), (14.0, -13.0, 6.0), (14.0, 17.0, 6.0)] and
          all(close(d["size"], (37.5, 37.5, 25.0)) and close(d["parallax_center"], (-1.0, 2.0, 6.0)) and
              close(d["parallax_size"], (60.0, 60.0, 20.0)) for d in definitions),
          "Fill with Probes tiles the zone, reflecting onto it, and replaces its probes: {}".format(centres))
    for obj in filled:
        bpy.data.objects.remove(obj)

    # Package coordinates: model frame (-x, y, z) relative to the root; Blender's grid positions.
    root.location = (5.0, 0.0, 0.0)
    probe.location = (1.0, 2.0, 3.0)
    probe.scale = (2.0, 3.0, 4.0)
    probe.data.influence_type = 'BOX'
    probe.data.influence_distance = 1.0
    probe.data.falloff = 0.25
    grid.location = (1.0, 0.0, 0.0)
    grid.scale = (3.0, 1.0, 1.0)
    grid.aeron_probe.grid_spacing = 100.0  # 2 points per axis
    bpy.context.view_layer.update()
    item = probes.probe_definition(root, probe)
    check(close(item["center"], (-1.0, 2.0, 3.0)) and close(item["size"], (4.0, 6.0, 8.0)) and
          item["shape"] == "box" and abs(item["falloff"] - 0.25) < 1e-6 and
          close(item["parallax_center"], item["center"]),
          "a probe's package coordinates: {}".format(item))
    item = probes.grid_definition(root, grid)
    points = probes.grid_points(grid)
    # Blender's x = 1 + 3 (2 (i + 1) / 3 - 1) = 0 or 2 (+5 for the root); model x = -x, first.
    check(close(item["center"], (-1.0, 0.0, 0.0)) and close(item["size"], (6.0, 2.0, 2.0)) and
          close(points[0], (7.0, -1.0 / 3.0, -1.0 / 3.0)) and close(points[1], (5.0, -1.0 / 3.0, -1.0 / 3.0)),
          "the grid's package box and Blender's point positions, model x first: {} {}".format(
              item, [tuple(round(v, 3) for v in p) for p in points[:2]]))
    root.location = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()

    # Validation.
    def problems_of(obj):
        return [message for o, message, warning in validation.validate(root).items if o == obj and not warning]

    grid.aeron_probe.grid_spacing = 1.5  # 3 x 2 x 2 points over 6 x 2 x 2 m
    probe.rotation_euler = (0.0, 0.0, 0.3)
    bpy.context.view_layer.update()
    check(any("rotated" in message for message in problems_of(probe)), "validation refuses a rotated probe")
    probe.rotation_euler = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()
    check(not problems_of(probe) and not problems_of(grid), "the probe and grid validate")

    # The glTF and back; a light inside the grid is baked, one outside is not.
    extension = probes.export_json(root)
    other = bpy.data.objects.new("other", None)
    scene.collection.objects.link(other)
    other.aeron_flight.role = 'model'
    probe.aeron_probe.room_parallax = True
    probe.aeron_probe.filled = True
    extension = probes.export_json(root)
    probes.restore(other, dict(extension, environmentProbe={"position": [1.0, 3.0, -2.0]}))
    bpy.context.view_layer.update()
    back = probes.export_json(other)
    restored = probes.reflection_probes(other)
    check(len(back["reflectionProbes"]) == 2 and
          close(back["reflectionProbes"][1]["position"], extension["reflectionProbes"][0]["position"]) and
          close(back["reflectionProbes"][1]["size"], extension["reflectionProbes"][0]["size"]) and
          back["lightGrid"] == extension["lightGrid"] and back["lightGrid"]["spacing"] == 1.5 and
          back["reflectionProbes"][1].get("roomParallax") and back["reflectionProbes"][1].get("filled") and
          any((p.matrix_world.translation - mathutils.Vector((1.0, 2.0, 3.0))).length < 1e-5 for p in restored),
          "probes, grid and an old environmentProbe go to the glTF and back: {} -> {}".format(extension, back))
    for obj in list(other.children):
        bpy.data.objects.remove(obj)
    bpy.data.objects.remove(other)
    probe.aeron_probe.room_parallax = False
    probe.aeron_probe.filled = False
    inside = bpy.data.objects.new("inside light", bpy.data.lights.new("inside light", 'POINT'))
    outside = bpy.data.objects.new("outside light", bpy.data.lights.new("outside light", 'POINT'))
    for light, location in ((inside, (1.5, 0.5, 0.5)), (outside, (30.0, 0.0, 0.0))):
        scene.collection.objects.link(light)
        light.parent = hangar
        light.matrix_world = mathutils.Matrix.Translation(location)
    bpy.context.view_layer.update()
    check(probes.light_baked(root, inside) and not probes.light_baked(root, outside),
          "a light inside the grid is baked, one outside is not")
    bpy.data.objects.remove(inside)
    bpy.data.objects.remove(outside)

    # Validity: a point inside a closed box is invalid, one outside is not.
    box = mesh_object("box", lambda bm: bmesh.ops.create_cube(bm, size=2.0), (100.0, 0.0, 0.0))
    bpy.context.view_layer.update()
    invalid = probes.invalid_points(bpy.context, [mathutils.Vector((100.0, 0.0, 0.0)),
                                                  mathutils.Vector((100.0, 0.0, 5.0))])
    check(invalid == [0], "a point inside a closed box is invalid, one outside is not: {}".format(invalid))
    bpy.data.objects.remove(box)

    # The bake.
    remaster = os.path.join(scratch, "remaster")
    os.makedirs(remaster)
    lamp = bpy.data.objects.new("lamp", bpy.data.lights.new("lamp", 'POINT'))
    scene.collection.objects.link(lamp)
    lamp.parent = hangar
    lamp.location = (-1.0, 0.0, 3.0)
    lamp.data.energy = 5000.0
    lamp.data.shadow_soft_size = 1.0
    engine, device, objects = scene.render.engine, scene.cycles.device, len(bpy.data.objects)
    persistent = scene.render.use_persistent_data
    with bpy.context.temp_override(active_object=probe, object=probe):
        result = bpy.ops.aeron.bake_probes(directory=remaster)
    out = os.path.join(remaster, "flight", "environments", "models", "testhangar")
    files = sorted(os.listdir(out)) if os.path.isdir(out) else []
    check('FINISHED' in result and files == ["grid.ktx2", "mapping.yaml", "package.yaml", "panorama_0.hdr",
                                            "probes.ktx2"],
          "the bake writes the package files: {}".format(files))
    package = open(os.path.join(out, "package.yaml")).read() if files else ""
    check("probes.testhangar:" in package and "grid_resolution: [3, 2, 2]" in package,
          "the package names the model's probes and grid")
    check(scene.render.engine == engine and scene.cycles.device == device and
          scene.render.use_persistent_data == persistent and len(bpy.data.objects) == objects and
          scene.camera is None and not lamp.visible_camera, "the bake restores the scene")
    pixels = bpy.data.images.load(os.path.join(out, "panorama_0.hdr")).pixels[:] if files else [0.0]
    check(max(pixels) > 100.0, "the panorama sees the lamp itself: {:.1f}".format(max(pixels)))
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

    scratch = tempfile.mkdtemp(prefix="aeron_probes_")
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
        if line.startswith(("PROBES", "Error:")):
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
