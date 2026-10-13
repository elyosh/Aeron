# Skin layers (tools/extensions/AERON_flight_model, "Skins"). A material's
# skins are images painted over it; the model root's skin variants list
# skins in order. The cooker composites each combination when it builds the
# .glb. This module holds the data, reads and writes it, checks it, and
# shows a skin variant in the viewport.
#
# The preview puts a copy of each material that the variant changes in its
# slots. The copy mixes each layer over the channels that the layer has
# (base color, emissive, metallic-roughness), with the layer's base color
# alpha times its opacity as the factor. It mixes in Blender's linear light,
# where the cooker mixes stored values, so partly covered texels can look
# slightly different. The copies record their own material, which takes the
# slot back when the preview ends, and before any export.

import os
import re
import shutil
import urllib.parse

import bpy
from bpy.props import BoolProperty, CollectionProperty, FloatProperty, IntProperty, PointerProperty, StringProperty
from bpy.types import PropertyGroup

from . import common

SKIN_NAME = re.compile(r"^[a-z0-9._-]{1,63}$")
CHANNELS = ("base_color", "emissive", "metallic_roughness")
JSON_KEYS = {"base_color": "baseColorTexture", "emissive": "emissiveTexture",
             "metallic_roughness": "metallicRoughnessTexture"}
PREVIEW_TAG = "aeron_skin_preview"
# On the images that add_material_skins adds, until Save to Project places
# them (exporting._place_images): the skin, whose folder they go in.
SKIN_IMAGE_KEY = "_aeronSkin"


# ---- data ---------------------------------------------------------------------------

def _root_of(owner):
    return common.model_root(owner) if isinstance(owner, bpy.types.Object) else None


def own_material(material):
    """The material that a preview copy stands for, or the material itself."""
    if material is not None and material.aeron_flight.preview_of is not None:
        return material.aeron_flight.preview_of
    return material


def model_materials(root):
    """The own materials of the model's components, in slot order, once each."""
    out = []
    for component in common.components(root):
        for slot in component.material_slots:
            material = own_material(slot.material)
            if material is not None and material not in out:
                out.append(material)
    return out


def skin_names(root):
    names = []
    for material in model_materials(root):
        for skin in material.aeron_flight.skins:
            if skin.name not in names:
                names.append(skin.name)
    return names


def khr_variant_names():
    return [variant.name for variant in getattr(bpy.data.scenes[0], "gltf2_KHR_materials_variants_variants", ())]


def variant_names(root):
    """Every variant a markings entry can name: the glTF ones, then the skin variants."""
    return khr_variant_names() + [variant.name for variant in root.aeron_flight.skin_variants]


def _search_skins(self, context, edit_text):
    root = _root_of(self.id_data)
    return [name for name in (skin_names(root) if root else []) if edit_text.lower() in name.lower()]


def _search_khr(self, context, edit_text):
    return [name for name in khr_variant_names() if edit_text.lower() in name.lower()]


def _preview_update(self, context):
    root = _root_of(self.id_data)
    if root is not None and root.aeron_flight.preview_markings >= 0:
        from . import display
        display.apply_skin(root)


class AeronSkinLayer(PropertyGroup):
    """A skin on a material: its images, in the UV layout of the base color texture."""
    # name: the skin name (PropertyGroup.name).
    base_color: PointerProperty(
        type=bpy.types.Image, name="Base Color",
        description="The skin's colors. Its alpha says where the skin covers the material, for every channel")
    emissive: PointerProperty(
        type=bpy.types.Image, name="Emissive",
        description="The skin's glow. Without it, the skin keeps the material's glow")
    metallic_roughness: PointerProperty(
        type=bpy.types.Image, name="Metallic-Roughness",
        description="The skin's roughness (G) and metallic (B). Without it, the skin keeps the material's")
    emissive_strength_given: BoolProperty(
        name="Own Strength", description="Give the skin's glow its own strength instead of the material's")
    emissive_strength: FloatProperty(name="Strength", min=0.0, default=1.0,
                                     description="Strength of the skin's glow")


class AeronSkinVariantLayer(PropertyGroup):
    skin: StringProperty(name="Skin", search=_search_skins, update=_preview_update,
                         description="A skin of the model's materials")
    opacity: IntProperty(name="Opacity", min=0, max=100, default=100, subtype='PERCENTAGE',
                         update=_preview_update, description="How much of the skin's coverage shows")


class AeronSkinVariant(PropertyGroup):
    """A material variant made of skins (PropertyGroup.name is its name)."""
    base: StringProperty(name="Base", search=_search_khr, update=_preview_update,
                         description="A glTF material variant the skin variant starts from, such as a paint "
                                     "state. Empty: the materials as modeled")
    layers: CollectionProperty(type=AeronSkinVariantLayer)
    active_layer: IntProperty()


def skin_variant(root, name):
    for variant in root.aeron_flight.skin_variants:
        if variant.name == name:
            return variant
    return None


def chain(material, variant):
    """The layers of `variant` that `material` has, as [(skin, opacity)]."""
    skins = {skin.name: skin for skin in material.aeron_flight.skins}
    return [(skins[layer.skin], layer.opacity) for layer in variant.layers if layer.skin in skins]


# ---- import ---------------------------------------------------------------------------

def _gltf_image_path(gltf, texture_index):
    data = gltf.data
    try:
        source = data.textures[texture_index].source
        uri = data.images[source].uri
    except (IndexError, TypeError, AttributeError):
        return None
    if not uri:
        return None
    return os.path.join(os.path.dirname(gltf.filename), urllib.parse.unquote(uri))


def restore_material(material, extension, gltf):
    """Reads the material's `skins` from its AERON_flight_model object."""
    skins = material.aeron_flight.skins
    skins.clear()
    for name, entry in (extension.get("skins") or {}).items():
        skin = skins.add()
        skin.name = name
        for channel in CHANNELS:
            info = entry.get(JSON_KEYS[channel])
            path = _gltf_image_path(gltf, info["index"]) if isinstance(info, dict) and "index" in info else None
            if path:
                image = bpy.data.images.load(path, check_existing=True)
                if channel == "metallic_roughness":
                    image.colorspace_settings.name = 'Non-Color'
                setattr(skin, channel, image)
        if "emissiveStrength" in entry:
            skin.emissive_strength_given = True
            skin.emissive_strength = entry["emissiveStrength"]


def restore_variants(root, extension):
    variants = root.aeron_flight.skin_variants
    variants.clear()
    for item in extension.get("skinVariants", []):
        variant = variants.add()
        variant.name = item.get("name", "")
        variant.base = item.get("base", "")
        for layer in item.get("layers", []):
            entry = variant.layers.add()
            entry.skin = layer.get("skin", "")
            entry.opacity = layer.get("opacity", 100)


# ---- export ---------------------------------------------------------------------------

def variants_json(root):
    out = []
    for variant in root.aeron_flight.skin_variants:
        item = {"name": variant.name}
        if variant.base:
            item["base"] = variant.base
        item["layers"] = [dict({"skin": layer.skin}, **({"opacity": layer.opacity} if layer.opacity != 100 else {}))
                          for layer in variant.layers]
        out.append(item)
    return out


def _image_file(image):
    path = bpy.path.abspath(image.filepath) if image.filepath else ""
    return os.path.normpath(path) if path and os.path.isfile(path) else None


def _write_png(image, target):
    """Saves a copy of `image` as a PNG at `target` (for images without a file)."""
    copy = image.copy()
    try:
        copy.filepath_raw = target
        copy.file_format = 'PNG'
        copy.save()
    finally:
        bpy.data.images.remove(copy)


def add_material_skins(document, gltf_path, keep_originals):
    """Adds the skins of the exported materials to `document`: a texture and
    an image for each skin image, and `skins` in each material's
    AERON_flight_model object. With `keep_originals` (Save to Project), an
    image with a file is referenced where it is, as Blender's exporter does;
    otherwise it is copied next to the .gltf. Images without a file are
    written there as PNG. Returns whether anything was added."""
    directory = os.path.dirname(gltf_path)
    images = document.setdefault("images", [])
    textures = document.setdefault("textures", [])
    index_of = {}
    added = False

    def texture(image, skin_name):
        key = image.name
        if key in index_of:
            return index_of[key]
        source = _image_file(image)
        if source and keep_originals:
            target = source
        else:
            name = os.path.basename(source) if source else bpy.path.clean_name(image.name) + ".png"
            target = os.path.join(directory, "textures", "skins", skin_name, os.path.splitext(name)[0] + ".png")
            os.makedirs(os.path.dirname(target), exist_ok=True)
            if source and source.lower().endswith(".png"):
                if os.path.normpath(source) != os.path.normpath(target):
                    shutil.copyfile(source, target)
            else:
                _write_png(image, target)
        uri = urllib.parse.quote(os.path.relpath(target, directory).replace(os.sep, "/"))
        entry = {"name": image.name, "uri": uri, "mimeType": "image/png"}
        if keep_originals:
            entry[SKIN_IMAGE_KEY] = skin_name
        images.append(entry)
        textures.append({"source": len(images) - 1})
        index_of[key] = len(textures) - 1
        return index_of[key]

    for material in document.get("materials", []):
        blender_material = bpy.data.materials.get(material.get("name", ""))
        if blender_material is None or not len(blender_material.aeron_flight.skins):
            continue
        skins = {}
        for skin in blender_material.aeron_flight.skins:
            if skin.base_color is None:
                continue
            entry = {}
            for channel in CHANNELS:
                image = getattr(skin, channel)
                if image is not None:
                    entry[JSON_KEYS[channel]] = {"index": texture(image, skin.name)}
            if skin.emissive_strength_given:
                entry["emissiveStrength"] = skin.emissive_strength
            skins[skin.name] = entry
        if skins:
            extension = material.setdefault("extensions", {}).setdefault(common.EXTENSION_NAME, {})
            extension["skins"] = skins
            added = True
    return added


def skin_images(root):
    """Every image of the model's skins, with its skin name."""
    out = {}
    for material in model_materials(root):
        for skin in material.aeron_flight.skins:
            for channel in CHANNELS:
                image = getattr(skin, channel)
                if image is not None:
                    out[image] = skin.name
    return out


# ---- checks ------------------------------------------------------------------------------

def _own_image(material, channel):
    """The image of the material's own texture for a channel, or None."""
    from .flipbook_preview import _bsdf, _texture
    tree = material.node_tree
    bsdf = _bsdf(tree) if tree is not None else None
    if bsdf is None:
        return None
    socket = {"base_color": "Base Color", "emissive": "Emission Color", "metallic_roughness": "Roughness"}[channel]
    node = _texture(bsdf.inputs[socket]) if bsdf.inputs[socket].is_linked else None
    return node.image if node is not None else None


def check(problems, root):
    materials = model_materials(root)
    names = set()
    for material in materials:
        seen = set()
        for skin in material.aeron_flight.skins:
            label = "material {}, skin {!r}".format(material.name, skin.name)
            if not SKIN_NAME.match(skin.name):
                problems.error(root, "{}: use 1-63 lower-case letters, digits, '.', '_' or '-'".format(label))
            if skin.name in seen:
                problems.error(root, "{}: the material has this skin twice".format(label))
            seen.add(skin.name)
            names.add(skin.name)
            if skin.base_color is None:
                problems.error(root, "{}: it needs a base color image".format(label))
                continue
            for channel in CHANNELS:
                image = getattr(skin, channel)
                own = _own_image(material, channel)
                size = tuple(own.size) if own is not None else tuple(skin.base_color.size)
                if image is not None and tuple(image.size) != size:
                    problems.error(root, "{}: its {} image is {}x{}, not {}x{}".format(
                        label, channel.replace("_", " "), image.size[0], image.size[1], size[0], size[1]))
    khr = khr_variant_names()
    used = set()
    seen = set()
    for variant in root.aeron_flight.skin_variants:
        label = "skin variant {!r}".format(variant.name)
        if not variant.name:
            problems.error(root, "a skin variant has no name")
        elif variant.name in seen or variant.name in khr:
            problems.error(root, "{}: another variant has this name".format(label))
        seen.add(variant.name)
        if variant.base and variant.base not in khr:
            problems.error(root, "{}: base {!r} is not a material variant".format(label, variant.base))
        if not len(variant.layers):
            problems.error(root, "{}: it has no layers".format(label))
        for layer in variant.layers:
            used.add(layer.skin)
            if layer.skin not in names:
                problems.error(root, "{}: no material has skin {!r}".format(label, layer.skin))
    for name in sorted(names - used):
        problems.warning(root, "no skin variant uses skin {!r}".format(name))


# ---- preview ------------------------------------------------------------------------------

def _key(material, layers):
    return material.name + "|" + ",".join("{}:{}".format(skin.name, opacity) for skin, opacity in layers)


def _source(tree, socket, value_node):
    """The socket feeding `socket`, or a new node holding its value."""
    if socket.is_linked:
        return socket.links[0].from_socket
    node = tree.nodes.new(value_node)
    node[PREVIEW_TAG] = True
    if value_node == 'ShaderNodeRGB':
        node.outputs[0].default_value = tuple(socket.default_value)[:3] + (1.0,)
    return node.outputs[0]


def _mix_layers(tree, start, layers, channel, vector, x, y):
    """Mixes the layers' images of `channel` over `start`. Returns the result socket."""
    current = start
    for skin, opacity in layers:
        image = getattr(skin, channel)
        if image is None:
            continue
        texture = tree.nodes.new('ShaderNodeTexImage')
        texture.image = image
        texture.hide = True
        texture.location = x, y
        if vector is not None:
            tree.links.new(vector, texture.inputs["Vector"])
        if channel == "base_color":
            cover = texture
        else:
            cover = tree.nodes.new('ShaderNodeTexImage')
            cover.image = skin.base_color
            cover.hide = True
            cover.location = x, y - 40.0
            if vector is not None:
                tree.links.new(vector, cover.inputs["Vector"])
        factor = tree.nodes.new('ShaderNodeMath')
        factor.operation = 'MULTIPLY'
        factor.inputs[1].default_value = opacity / 100.0
        factor.hide = True
        factor.location = x + 200.0, y
        tree.links.new(cover.outputs["Alpha"], factor.inputs[0])
        mix = tree.nodes.new('ShaderNodeMix')
        mix.data_type = 'RGBA'
        mix.blend_type = 'MIX'
        mix.hide = True
        mix.location = x + 360.0, y
        tree.links.new(factor.outputs[0], mix.inputs[0])
        tree.links.new(current, mix.inputs[6])
        tree.links.new(texture.outputs["Color"], mix.inputs[7])
        current = mix.outputs[2]
        y -= 80.0
    return current


def _build(material, layers):
    """A copy of `material` with `layers` mixed over its channels."""
    from .flipbook_preview import _bsdf, _texture
    copy = material.copy()
    copy.name = material.name + " (skin preview)"
    copy.aeron_flight.preview_of = material
    copy.aeron_flight.preview_key = _key(material, layers)
    copy[PREVIEW_TAG] = True
    tree = copy.node_tree
    bsdf = _bsdf(tree) if tree is not None else None
    if bsdf is None:
        return copy
    base = bsdf.inputs["Base Color"]
    own = _texture(base) if base.is_linked else None
    vector = own.inputs["Vector"].links[0].from_socket if own is not None and own.inputs["Vector"].links else None
    x, y = bsdf.location.x - 700.0, bsdf.location.y + 300.0
    if any(skin.base_color is not None for skin, _ in layers):
        tree.links.new(_mix_layers(tree, _source(tree, base, 'ShaderNodeRGB'), layers, "base_color", vector, x, y),
                       base)
    if any(skin.emissive is not None for skin, _ in layers):
        socket = bsdf.inputs["Emission Color"]
        tree.links.new(_mix_layers(tree, _source(tree, socket, 'ShaderNodeRGB'), layers, "emissive", vector, x,
                                   y - 400.0), socket)
        strength = bsdf.inputs["Emission Strength"]
        if not strength.is_linked and (strength.default_value == 0.0 or not any(socket.default_value[:3])):
            given = [skin.emissive_strength for skin, _ in layers if skin.emissive is not None and
                     skin.emissive_strength_given]
            strength.default_value = given[0] if given else 1.0
    if any(skin.metallic_roughness is not None for skin, _ in layers):
        roughness, metallic = bsdf.inputs["Roughness"], bsdf.inputs["Metallic"]
        separate = roughness.links[0].from_node if roughness.is_linked else None
        if separate is not None and separate.type == 'SEPARATE_COLOR' and separate.inputs[0].is_linked:
            start = separate.inputs[0].links[0].from_socket
        else:
            combine = tree.nodes.new('ShaderNodeCombineColor')
            combine.inputs[0].default_value = 1.0
            combine.inputs[1].default_value = roughness.default_value
            combine.inputs[2].default_value = metallic.default_value
            start = combine.outputs[0]
            separate = tree.nodes.new('ShaderNodeSeparateColor')
            tree.links.new(separate.outputs[1], roughness)
            tree.links.new(separate.outputs[2], metallic)
        tree.links.new(_mix_layers(tree, start, layers, "metallic_roughness", vector, x, y - 800.0),
                       separate.inputs[0])
    return copy


def _preview(material, layers):
    key = _key(material, layers)
    for existing in bpy.data.materials:
        if existing.get(PREVIEW_TAG) and existing.aeron_flight.preview_key == key and \
                existing.aeron_flight.preview_of == material:
            return existing
    return _build(material, layers)


def restore(root):
    """Puts the own materials back in the slots of the model's components."""
    for component in common.components(root):
        for slot in component.material_slots:
            if slot.material is not None and slot.material.aeron_flight.preview_of is not None:
                slot.material = slot.material.aeron_flight.preview_of


def remove_unused():
    for material in [m for m in bpy.data.materials if m.get(PREVIEW_TAG) and m.users == 0]:
        bpy.data.materials.remove(material)


def show(root, variant, base_material):
    """Shows skin variant `variant`. `base_material(component, slot index)`
    gives a slot's material in the base variant, or None for the own one."""
    for component in common.components(root):
        for index, slot in enumerate(component.material_slots):
            material = own_material(slot.material)
            if material is None:
                continue
            layers = chain(material, variant)
            if layers:
                target = _preview(material, layers)
            else:
                target = base_material(component, index) or material
            if slot.material != target:
                slot.material = target


classes = (AeronSkinLayer, AeronSkinVariantLayer, AeronSkinVariant)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
