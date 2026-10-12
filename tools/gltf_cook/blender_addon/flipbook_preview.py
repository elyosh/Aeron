# Flipbook preview: the materials' flipbooks played in the viewport (Material
# Preview or Rendered shading) as the game's shader plays them
# (scene_pbr_mesh_impl.hlsli): a base colour frame is painted over the base
# colour by its alpha; an emissive frame replaces the emissive over the whole
# material, its optional base colour painted over; without an emissive frame,
# a base colour flipbook's unlit part u moves c·u of the colour c to emission
# and leaves c·(1 − u) lit. Hull damage and energy effects are not shown.
#
# The scene's checked states (and `always`) are active from its start frame,
# so the look is a function of the frame alone. A previewed material (a
# Principled BSDF whose base colour is a texture or a value) gets a node
# group before its base colour and emission inputs, and image nodes for the
# playing frames (the library sources' PNGs), sampled with the base colour
# texture's own vector. All of it is tagged, and removed when the preview
# stops, around the exports, while the file is saved, when a file is opened
# and when the add-on is disabled.

import contextlib
import math
import os

import bpy
from bpy.app.handlers import persistent
from bpy.props import BoolProperty
from bpy.types import PropertyGroup

from . import animation, common, libraries, sources

TAG = "aeron_flipbook_preview"
GROUP = ".Aeron Flipbook Preview"
NODE = "Aeron Flipbook Preview"
# The image nodes: the base colour frame, and an emissive frame's two inputs.
IMAGE_NODES = {"colour": "Aeron Frame", "emissive": "Aeron Frame Emissive", "lit_base": "Aeron Frame Base Colour"}

# Materials not previewed, and why {name: reason}, and why the libraries
# could not be loaded, for the panel.
skipped = {}
message = ""
_images = {}  # {PNG path: image name}


# ---- what plays ----------------------------------------------------------------

def state_property(state):
    return state.replace(".", "_")


def active_states(scene):
    states = scene.aeron_flight.flipbook_states
    return {"always"} | {state for state, _, _ in animation.STATES[1:] if getattr(states, state_property(state))}


def seconds(scene):
    """The preview's time at the scene's frame: 0 at its start frame."""
    return (scene.frame_current - scene.frame_start) * scene.render.fps_base / scene.render.fps


def local_time(mode, total, t):
    """Time `t` into a flipbook of `total` seconds: wrapped for a loop, held at the end for `once`."""
    if not total > 0.0:
        return 0.0
    if mode == "once":
        return min(max(t, 0.0), total)
    return t - total * math.floor(t / total)


def frame_index(durations, t):
    """The frame showing at local time `t`: the first whose end exceeds it, the last one past the end."""
    end = 0.0
    for index, duration in enumerate(durations):
        end += duration
        if t < end:
            return index
    return len(durations) - 1


def playing(material, states, t):
    """{layer: (flipbook, key)} of `material` at `t` seconds with `states`
    active: per layer the first flipbook whose state is active, at its frame
    (none for a flipbook without frames)."""
    result = {}
    for book in material.aeron_flight.flipbooks:
        if book.layer in result or book.state not in states:
            continue
        frames = book.frames
        if not len(frames):
            result[book.layer] = None
            continue
        durations = [frame.seconds for frame in frames]
        result[book.layer] = (book, frames[frame_index(durations, local_time(book.mode, sum(durations), t))].key)
    return {layer: value for layer, value in result.items() if value is not None}


# ---- the node group ------------------------------------------------------------

def _socket(interface, name, in_out, socket_type, default=None):
    socket = interface.new_socket(name, in_out=in_out, socket_type=socket_type)
    if default is not None:
        socket.default_value = default
    return socket


def _group():
    """The shared node group: base colour and emission from the material's
    own and the playing frames' (the game's composition)."""
    group = bpy.data.node_groups.get(GROUP)
    if group is not None and group.get(TAG):
        return group
    group = bpy.data.node_groups.new(GROUP, 'ShaderNodeTree')
    group[TAG] = True
    face = group.interface
    _socket(face, "Base Color", 'OUTPUT', 'NodeSocketColor')
    _socket(face, "Emission", 'OUTPUT', 'NodeSocketColor')
    for name, kind, default in (
            ("Base Color", 'NodeSocketColor', (0.8, 0.8, 0.8, 1.0)), ("Emission", 'NodeSocketColor', None),
            ("Strength", 'NodeSocketFloat', 1.0), ("Frame", 'NodeSocketColor', None),
            ("Frame Alpha", 'NodeSocketFloat', 0.0), ("Frame On", 'NodeSocketFloat', 0.0),
            ("Unlit", 'NodeSocketFloat', 0.0), ("Lit Base", 'NodeSocketColor', None),
            ("Lit Base Alpha", 'NodeSocketFloat', 0.0), ("Lit Base On", 'NodeSocketFloat', 0.0),
            ("Lit Emission", 'NodeSocketColor', None), ("Lit Strength", 'NodeSocketFloat', 0.0),
            ("Replace", 'NodeSocketFloat', 0.0)):
        _socket(face, name, 'INPUT', kind, default)
    nodes, links = group.nodes, group.links
    inputs = nodes.new('NodeGroupInput')
    outputs = nodes.new('NodeGroupOutput')

    def math_node(operation, a, b):
        node = nodes.new('ShaderNodeMath')
        node.operation = operation
        links.new(a, node.inputs[0])
        links.new(b, node.inputs[1])
        return node.outputs[0]

    def mix(factor, a, b):
        """a + (b − a)·factor (black for None), colours unclamped."""
        node = nodes.new('ShaderNodeMix')
        node.data_type = 'RGBA'
        sockets = {socket.identifier: socket for socket in node.inputs}
        links.new(factor, sockets["Factor_Float"])
        for value, identifier in ((a, "A_Color"), (b, "B_Color")):
            if value is None:
                sockets[identifier].default_value = (0.0, 0.0, 0.0, 1.0)
            else:
                links.new(value, sockets[identifier])
        return next(socket for socket in node.outputs if socket.identifier == "Result_Color")

    def scale(colour, factor):
        node = nodes.new('ShaderNodeVectorMath')
        node.operation = 'SCALE'
        links.new(colour, node.inputs[0])
        links.new(factor, node.inputs["Scale"])
        return node.outputs[0]

    def add(a, b):
        node = nodes.new('ShaderNodeVectorMath')
        node.operation = 'ADD'
        links.new(a, node.inputs[0])
        links.new(b, node.inputs[1])
        return node.outputs[0]

    get = inputs.outputs.get
    # The base colour frame over the colour c, then its unlit part u:
    # c·(1 − u) lit, c·u emitted; an emissive frame's base colour over that.
    painted = mix(math_node('MULTIPLY', get("Frame Alpha"), get("Frame On")), get("Base Color"), get("Frame"))
    lit = mix(get("Unlit"), painted, None)
    base = mix(math_node('MULTIPLY', get("Lit Base Alpha"), get("Lit Base On")), lit, get("Lit Base"))
    # The frames' emission replaces the material's own.
    frames = add(scale(painted, get("Unlit")), scale(get("Lit Emission"), get("Lit Strength")))
    emission = mix(get("Replace"), scale(get("Emission"), get("Strength")), frames)
    links.new(base, outputs.inputs["Base Color"])
    links.new(emission, outputs.inputs["Emission"])
    return group


# ---- inserting and removing ------------------------------------------------------

def _bsdf(tree):
    """The Principled BSDF the material's active output shows, or None."""
    for node in tree.nodes:
        if node.type == 'OUTPUT_MATERIAL' and node.is_active_output:
            links = node.inputs["Surface"].links
            if links and links[0].from_node.type == 'BSDF_PRINCIPLED':
                return links[0].from_node
    return None


def _texture(socket):
    """The first image texture upstream of `socket`, or None."""
    seen, pending = set(), [socket]
    while pending:
        for link in pending.pop(0).links:
            node = link.from_node
            if node.type == 'TEX_IMAGE':
                return node
            if node.name not in seen:
                seen.add(node.name)
                pending.extend(input for input in node.inputs if input.is_linked)
    return None


def _insert(material):
    """Inserts the preview's nodes into `material`; returns why it cannot, or None."""
    tree = material.node_tree
    bsdf = _bsdf(tree) if tree is not None else None
    if bsdf is None:
        return "Not a Principled BSDF"
    base = bsdf.inputs["Base Color"]
    texture = _texture(base) if base.is_linked else None
    if base.is_linked and texture is None:
        return "Base colour is not a texture or a value"
    vector = texture.inputs["Vector"].links if texture is not None else ()
    vector = vector[0].from_socket if vector else None
    group = tree.nodes.new('ShaderNodeGroup')
    group.node_tree = _group()
    group.name = group.label = NODE
    group[TAG] = True
    group.location = bsdf.location.x - 260.0, bsdf.location.y
    group.hide = True
    for name, target in (("Base Color", "Base Color"), ("Emission", "Emission Color"),
                         ("Strength", "Emission Strength")):
        socket = bsdf.inputs[target]
        if socket.is_linked:
            tree.links.new(socket.links[0].from_socket, group.inputs[name])
        else:
            group.inputs[name].default_value = socket.default_value
    tree.links.new(group.outputs["Base Color"], base)
    tree.links.new(group.outputs["Emission"], bsdf.inputs["Emission Color"])
    strength = bsdf.inputs["Emission Strength"]
    if strength.is_linked:
        tree.links.remove(strength.links[0])
    strength.default_value = 1.0
    for index, (role, name) in enumerate(IMAGE_NODES.items()):
        node = tree.nodes.new('ShaderNodeTexImage')
        node.name = node.label = name
        node[TAG] = True
        node.hide = True
        node.location = group.location.x - 260.0, group.location.y - 40.0 * index
        node.extension = 'REPEAT'
        if vector is not None:
            tree.links.new(vector, node.inputs["Vector"])
        colour, alpha = {"colour": ("Frame", "Frame Alpha"), "emissive": ("Lit Emission", None),
                         "lit_base": ("Lit Base", "Lit Base Alpha")}[role]
        tree.links.new(node.outputs["Color"], group.inputs[colour])
        if alpha:
            tree.links.new(node.outputs["Alpha"], group.inputs[alpha])
    return None


def _remove(material):
    """Removes the preview's nodes from `material`, giving its BSDF back its
    inputs as they were (or as edited on the node group meanwhile)."""
    tree = material.node_tree
    if tree is None:
        return
    group = tree.nodes.get(NODE)
    bsdf = _bsdf(tree)
    if group is not None and group.get(TAG) and bsdf is not None:
        for name, target in (("Base Color", "Base Color"), ("Emission", "Emission Color"),
                             ("Strength", "Emission Strength")):
            socket, given = bsdf.inputs[target], group.inputs[name]
            if given.is_linked:
                tree.links.new(given.links[0].from_socket, socket)
            else:
                for link in socket.links:
                    tree.links.remove(link)
                socket.default_value = given.default_value
    for node in [node for node in tree.nodes if node.get(TAG)]:
        tree.nodes.remove(node)


def _has_nodes(material):
    tree = material.node_tree
    return tree is not None and any(node.get(TAG) for node in tree.nodes)


def _complete(material):
    nodes = material.node_tree.nodes
    return all(nodes.get(name) is not None and nodes[name].get(TAG) for name in (NODE, *IMAGE_NODES.values()))


def remove_all():
    """Removes everything the preview added to the file."""
    for material in bpy.data.materials:
        if _has_nodes(material):
            _remove(material)
    for group in [group for group in bpy.data.node_groups if group.get(TAG)]:
        bpy.data.node_groups.remove(group)
    for image in [image for image in bpy.data.images if image.get(TAG)]:
        bpy.data.images.remove(image)
    _images.clear()
    skipped.clear()


# ---- frames ------------------------------------------------------------------------

def _image(path):
    """The image of the PNG `path`: one already open (a frame being painted),
    or loaded for the preview once."""
    name = _images.get(path)
    image = bpy.data.images.get(name) if name else None
    if image is None:
        image = next((image for image in bpy.data.images
                      if image.filepath and os.path.normpath(bpy.path.abspath(image.filepath)) == path), None)
        if image is None:
            image = bpy.data.images.load(path, check_existing=False)
            image[TAG] = True
            image.name = ".Aeron Frame " + os.path.basename(path)
        _images[path] = image.name
    return image


def _frame_image(loaded, library, key, kind, input_name):
    """The image of input `input_name` of frame `key`, or None when the
    library lacks the key (the game shows no frame), it is of another kind,
    or the file is missing."""
    entry = loaded.libraries.get(library, {}).get("images", {}).get(key)
    if entry is None or entry.get("kind") != kind or not entry.get(input_name):
        return None
    path = os.path.normpath(os.path.join(os.path.dirname(loaded.libraries[library]["path"]), entry[input_name]))
    return _image(path) if os.path.isfile(path) else None


def _set(target, name, value):
    """Sets a property only when it changes (a write updates the material's
    shaders). Floats are compared at their 32-bit precision."""
    current = getattr(target, name)
    if current != value if not isinstance(value, float) else abs(current - value) > 1e-6:
        setattr(target, name, value)


def _show(material, loaded, states, t):
    """Shows `material`'s playing frames."""
    nodes = material.node_tree.nodes
    group = nodes.get(NODE)
    now = playing(material, states, t)
    colour = lit = lit_base = None
    if "baseColor" in now:
        book, key = now["baseColor"]
        colour = _frame_image(loaded, book.library, key, "baseColor", "baseColor")
    if "emissive" in now:
        book, key = now["emissive"]
        lit = _frame_image(loaded, book.library, key, "emissive", "emissive")
        lit_base = _frame_image(loaded, book.library, key, "emissive", "baseColor") if lit else None
    unlit = round(now["baseColor"][0].unlit * 255.0) / 255.0 if colour and not lit else 0.0
    for role, image in (("colour", colour), ("emissive", lit), ("lit_base", lit_base)):
        if image is not None:
            _set(nodes[IMAGE_NODES[role]], "image", image)
    for name, value in (("Frame On", 1.0 if colour else 0.0), ("Unlit", unlit),
                        ("Lit Base On", 1.0 if lit_base else 0.0),
                        ("Lit Strength", now["emissive"][0].emissive_strength if lit else 0.0),
                        ("Replace", 1.0 if lit or unlit > 0.0 else 0.0)):
        _set(group.inputs[name], "default_value", value)


def _trees():
    """{material: its model's loaded source tree, or why there is none}."""
    result = {}
    for obj in bpy.data.objects:
        if common.role(obj) != 'model':
            continue
        if not sources.library_sources(bpy.path.abspath(obj.aeron_flight.source_path)):
            loaded = "Not in a model of a source tree"
        else:
            loaded = libraries.tree_of(obj) or "Libraries not loaded"
        for material in sources.model_materials(obj):
            if not isinstance(result.get(material), libraries.Tree):
                result[material] = loaded
    return result


def enabled():
    managers = bpy.data.window_managers
    return bool(len(managers)) and managers[0].aeron_flipbook_preview


def refresh(scene=None):
    """Brings the file in line with the preview: the playing frames when it
    is on, nothing of it when it is off."""
    if not enabled():
        if any(group.get(TAG) for group in bpy.data.node_groups) or skipped:
            remove_all()
        return
    scene = scene or bpy.context.scene
    states, t = active_states(scene), seconds(scene)
    trees = _trees()
    skipped.clear()
    for material in bpy.data.materials:
        if not len(material.aeron_flight.flipbooks):
            if _has_nodes(material):
                _remove(material)
            continue
        loaded = trees.get(material, "Not in a model of a source tree")
        if not isinstance(loaded, libraries.Tree):
            if _has_nodes(material):
                _remove(material)
            skipped[material.name] = loaded
            continue
        if not _has_nodes(material) or not _complete(material):
            _remove(material)
            reason = _insert(material)
            if reason:
                skipped[material.name] = reason
                continue
        _show(material, loaded, states, t)


def load_trees(context):
    """Loads the source trees of the scene's models that are not loaded yet.
    Returns an error message, or None."""
    from . import preferences

    wanted = set()
    for obj in bpy.data.objects:
        if common.role(obj) == 'model' and libraries.tree_of(obj) is None:
            effects = sources.library_sources(bpy.path.abspath(obj.aeron_flight.source_path))
            if effects:
                wanted.add(effects)
    if not wanted:
        return None
    cooker = preferences.cooker_path(context)
    if not cooker:
        return "aeron_gltf_cook is not set: see the add-on's preferences"
    for effects in sorted(wanted):
        try:
            libraries.load(cooker, effects)
        except (OSError, RuntimeError, ValueError) as error:
            return "reading the libraries: {}".format(error)
    return None


def _deferred():
    refresh()
    return None


def schedule():
    """Refreshes the preview once Blender is idle: animation edits call it
    (Blender's depsgraph does not report them)."""
    if enabled() and not bpy.app.timers.is_registered(_deferred):
        bpy.app.timers.register(_deferred, first_interval=0.0)


@contextlib.contextmanager
def suspended():
    """Without the preview's data in the file (the exports), then with it again."""
    remove_all()
    try:
        yield
    finally:
        refresh()


# ---- properties and handlers ------------------------------------------------------

def _enabled_update(self, context):
    global message
    message = (load_trees(context) or "") if self.aeron_flipbook_preview else ""
    refresh(context.scene)


def _states_update(self, context):
    refresh(context.scene)


AeronFlipbookStates = type("AeronFlipbookStates", (PropertyGroup,), {"__annotations__": {
    state_property(state): BoolProperty(name=name, description=description, update=_states_update)
    for state, name, description in animation.STATES[1:]}})


@persistent
def _on_frame(scene, *_args):
    if enabled():
        refresh(scene)


@persistent
def _on_undo(*_args):
    refresh()


_saved = []


@persistent
def _on_save_pre(*_args):
    _saved[:] = [enabled()]
    if any(group.get(TAG) for group in bpy.data.node_groups):
        remove_all()


@persistent
def _on_save_post(*_args):
    if _saved and _saved[0]:
        refresh()


@persistent
def _on_load(*_args):
    # Nothing of a preview stays in a file opened (an autosave may hold it).
    managers = bpy.data.window_managers
    if len(managers) and managers[0].aeron_flipbook_preview:
        managers[0]["aeron_flipbook_preview"] = False
    remove_all()


_HANDLERS = ((bpy.app.handlers.frame_change_post, _on_frame), (bpy.app.handlers.undo_post, _on_undo),
             (bpy.app.handlers.redo_post, _on_undo), (bpy.app.handlers.save_pre, _on_save_pre),
             (bpy.app.handlers.save_post, _on_save_post), (bpy.app.handlers.load_post, _on_load))


def register():
    bpy.utils.register_class(AeronFlipbookStates)
    bpy.types.WindowManager.aeron_flipbook_preview = BoolProperty(
        name="Flipbook Preview", update=_enabled_update,
        description="Show the materials' flipbooks in the viewport (Material Preview or Rendered shading) at "
                    "the timeline's frame, with the checked states active. Play or scrub the timeline to "
                    "animate them")
    for handlers, handler in _HANDLERS:
        handlers.append(handler)


def unregister():
    for handlers, handler in _HANDLERS:
        if handler in handlers:
            handlers.remove(handler)
    if bpy.app.timers.is_registered(_deferred):
        bpy.app.timers.unregister(_deferred)
    remove_all()
    del bpy.types.WindowManager.aeron_flipbook_preview
    bpy.utils.unregister_class(AeronFlipbookStates)
