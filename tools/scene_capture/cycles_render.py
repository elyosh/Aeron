"""Renders the Cycles reference of a cycles_match.py scene.

    blender -b SCENE.blend --python cycles_render.py -- DIR [--samples 1024]

Uses DIR/scene.json (cycles_match.py) for the camera, the size and the models,
and writes DIR/cycles.exr: a multi-layer EXR with the combined image, the
diffuse and glossy passes (direct, indirect, colour), emission, position,
normal and an object index (1 on the models other than the hangar).

As the probes bake: no light from outside the model (the world is black),
only the .blend's lights and emission. The camera keeps its position and
direction but takes scene_capture's 60 degree vertical field of view. The
.blend is never saved."""

import json
import math
import os
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if not args:
    sys.exit(__doc__)
out = os.path.abspath(args[0])
samples = int(args[args.index("--samples") + 1]) if "--samples" in args else 1024
scene_info = json.load(open(os.path.join(out, "scene.json")))

scene = bpy.context.scene
world = bpy.data.worlds.new("Reference World")
world.use_nodes = True
for node in world.node_tree.nodes:
    if node.type == 'BACKGROUND':
        node.inputs["Strength"].default_value = 0.0
scene.world = world

camera = bpy.data.objects[scene_info["camera"]]
scene.camera = camera
camera.data.sensor_fit = 'VERTICAL'
camera.data.angle_y = math.radians(scene_info["vertical_fov_degrees"])
camera.data.shift_x = camera.data.shift_y = 0.0
camera.data.clip_start, camera.data.clip_end = 0.1, 10000.0  # metres

for name in scene_info["models"]:
    for obj in bpy.data.objects[name].children_recursive:
        obj.pass_index = 1

render = scene.render
render.engine = 'CYCLES'
render.resolution_x, render.resolution_y = scene_info["width"], scene_info["height"]
render.resolution_percentage = 100
render.film_transparent = False
# The compositor would write only the combined image.
render.use_compositing = False
render.use_sequencer = False
scene.cycles.samples = samples
scene.cycles.use_denoising = True
scene.view_settings.view_transform = 'Standard'
scene.view_settings.look = 'None'
scene.view_settings.exposure = 0.0
layer = scene.view_layers[0]
for name in ("use_pass_diffuse_direct", "use_pass_diffuse_indirect", "use_pass_diffuse_color",
             "use_pass_glossy_direct", "use_pass_glossy_indirect", "use_pass_glossy_color",
             "use_pass_emit", "use_pass_normal", "use_pass_position", "use_pass_object_index"):
    setattr(layer, name, True)
settings = render.image_settings
if hasattr(settings, "media_type"):  # Blender 5
    settings.media_type = 'MULTI_LAYER_IMAGE'
settings.file_format = 'OPEN_EXR_MULTILAYER'
settings.color_depth = '32'
render.filepath = os.path.join(out, "cycles.exr")

preferences = bpy.context.preferences.addons["cycles"].preferences
for backend in ("METAL", "OPTIX", "CUDA", "HIP", "ONEAPI"):
    try:
        preferences.compute_device_type = backend
    except TypeError:
        continue
    preferences.get_devices()
    gpus = [d for d in preferences.devices if d.type == backend]
    if gpus:
        for device in preferences.devices:
            device.use = device in gpus
        scene.cycles.device = 'GPU'
        print(f"cycles_render.py: rendering on {backend}")
        break
else:
    print("cycles_render.py: no GPU found, rendering on the CPU")
bpy.ops.render.render(write_still=True)
