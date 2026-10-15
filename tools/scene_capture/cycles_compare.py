"""Compares scene_capture's captures of a cycles_match.py scene with its Cycles reference.

    blender -b --python cycles_compare.py -- DIR [--box NAME X0 Y0 X1 Y1 ...] [--near-floor 2.5]

Reads DIR/cycles.exr (cycles_render.py), DIR/captures/{full,diffuse,reflect}
(scene_capture's runs of the configs cycles_match.py wrote) and DIR/scene.json.
Without the diffuse and reflect captures, only the full images are compared.
Runs in Blender's Python for numpy and OpenImageIO.

Writes DIR/compare/:
- cycles_<term>.png, game_<term>.png: the full image, the diffuse light and
  the reflections, at one exposure (Cycles' 90th percentile at 0.5), linear
  values through the sRGB curve. The game's diffuse and reflections are the
  diffuse-only and reflect-only captures less the emission they share.
- ratio_<term>.png: log2(game / Cycles) over 9 x 9 pixels; red where the game
  is brighter, blue where darker, two stops at full colour.
- align_edges.png: the edges of both full images, the game's red, Cycles'
  green: yellow where they line up.
- regions.txt: the mean luminance of each term per region, both renders and
  their ratio. Regions come from Cycles' passes: the floor (the lowest
  up-facing surface), split by the models' footprint; the models' surfaces
  facing up, sideways and down, and those within --near-floor metres of the
  floor; each --box (pixels, from the top left) adds the open floor inside it."""

import json
import os
import sys

import numpy as np
import OpenImageIO as oiio

LUMINANCE = np.array([0.2126, 0.7152, 0.0722])
# Pass names: Blender 5's, then the older short ones.
PASSES = {
    "combined": ("Combined",), "position": ("Position",), "normal": ("Normal",),
    "index": ("Object Index", "IndexOB"), "emit": ("Emission", "Emit"),
    "diffuse_direct": ("Diffuse Direct", "DiffDir"), "diffuse_indirect": ("Diffuse Indirect", "DiffInd"),
    "diffuse_color": ("Diffuse Color", "DiffCol"), "glossy_direct": ("Glossy Direct", "GlossDir"),
    "glossy_indirect": ("Glossy Indirect", "GlossInd"), "glossy_color": ("Glossy Color", "GlossCol"),
}


def parse_args():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not args:
        sys.exit(__doc__)
    out = {"dir": os.path.abspath(args[0]), "boxes": [], "near_floor": 2.5}
    i = 1
    while i < len(args):
        if args[i] == "--box":
            out["boxes"].append((args[i + 1], *map(int, args[i + 2:i + 6])))
            i += 6
        elif args[i] == "--near-floor":
            out["near_floor"] = float(args[i + 1])
            i += 2
        else:
            sys.exit(f"cycles_compare.py: unknown argument {args[i]}")
    return out


def read_exr(path):
    """Every channel of every part (Blender 5 writes each pass as its own part)."""
    channels = {}
    image = oiio.ImageInput.open(path)
    if image is None:
        sys.exit(f"cycles_compare.py: cannot read {path}")
    part = 0
    while image.seek_subimage(part, 0):
        names = list(image.spec().channelnames)
        pixels = image.read_image(part, 0, 0, len(names), "float").astype(np.float64)
        for c, name in enumerate(names):
            channels[name] = pixels[:, :, c]
        part += 1
    image.close()
    return channels


def get_pass(channels, key, components):
    for alias in PASSES[key]:
        for prefix in {name.rsplit(".", 2)[0] for name in channels if name.count(".") >= 2}:
            names = [f"{prefix}.{alias}.{c}" for c in components]
            if all(n in channels for n in names):
                return np.stack([channels[n] for n in names], -1)
    return None


def read_pfm(path):
    with open(path, "rb") as stream:
        if stream.readline().strip() != b"PF":
            sys.exit(f"cycles_compare.py: {path} is not an RGB PFM")
        width, height = map(int, stream.readline().split())
        scale = float(stream.readline())
        data = np.frombuffer(stream.read(), dtype="<f4" if scale < 0 else ">f4").reshape(height, width, 3)
    return np.flipud(data).astype(np.float64)


def save(path, rgb):
    v = np.clip(rgb, 0, 1)
    v = np.where(v <= 0.0031308, v * 12.92, 1.055 * v ** (1 / 2.4) - 0.055)
    image = oiio.ImageOutput.create(path)
    image.open(path, oiio.ImageSpec(rgb.shape[1], rgb.shape[0], 3, "uint8"))
    image.write_image((v * 255 + 0.5).astype(np.uint8))
    image.close()


def box_mean(a, radius=4):
    c = np.cumsum(np.cumsum(np.pad(a, radius + 1, mode="edge"), 0), 1)
    n = 2 * radius + 1
    return (c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n])[:a.shape[0], :a.shape[1]] / (n * n)


def ratio_image(game, cycles):
    floor = 0.002
    g, c = box_mean(game), box_mean(cycles)
    x = np.clip(np.log2((g + floor) / (c + floor)) / 2, -1, 1)
    image = np.stack([1 - np.maximum(-x, 0), 1 - np.abs(x), 1 - np.maximum(x, 0)], -1)
    image[(g < floor) & (c < floor)] = 0.5
    return image ** 2.2  # save() applies the sRGB curve


def edges(rgb):
    l = np.log(rgb @ LUMINANCE + 1e-3)
    gx, gy = np.zeros_like(l), np.zeros_like(l)
    gx[:, 1:-1] = l[:, 2:] - l[:, :-2]
    gy[1:-1] = l[2:] - l[:-2]
    e = np.hypot(gx, gy)
    return np.clip(e / max(np.percentile(e, 97), 1e-6), 0, 1)


def regions(channels, scene, root, boxes, near_floor):
    position = get_pass(channels, "position", "XYZ")
    normal = get_pass(channels, "normal", "XYZ")
    index = get_pass(channels, "index", "X")
    if position is None or normal is None or index is None:
        sys.exit("cycles_compare.py: cycles.exr lacks the position, normal or object index pass")
    height, width = position.shape[:2]
    model = index[..., 0] > 0.5
    valid = np.isfinite(position).all(-1) & (np.abs(position).sum(-1) > 0)
    up = valid & ~model & (normal[..., 2] > 0.95)
    floor_z = np.percentile(position[..., 2][up], 5)
    floor = up & (np.abs(position[..., 2] - floor_z) < 0.05)
    under = np.zeros((height, width), bool)
    info = scene.get("footprint")
    if info:
        covered = np.load(os.path.join(root, info["file"]))
        i = np.floor((position[..., 0] - info["origin"][0]) / info["cell"]).astype(int)
        j = np.floor((position[..., 1] - info["origin"][1]) / info["cell"]).astype(int)
        inside = floor & (i >= 0) & (i < covered.shape[1]) & (j >= 0) & (j < covered.shape[0])
        under[inside] = covered[j[inside], i[inside]] > 0
    rows, cols = np.mgrid[0:height, 0:width]
    out = {"floor, open": floor & ~under, "floor, under the models": floor & under}
    for name, x0, y0, x1, y1 in boxes:
        out[f"floor, open, in {name}"] = floor & ~under & (cols >= x0) & (cols < x1) & (rows >= y0) & (rows < y1)
    out.update({
        "models, facing up": model & valid & (normal[..., 2] > 0.7),
        "models, sideways": model & valid & (np.abs(normal[..., 2]) < 0.3),
        "models, facing down": model & valid & (normal[..., 2] < -0.5),
        f"models, within {near_floor:g} m of the floor": model & valid & (position[..., 2] < floor_z + near_floor),
    })
    return out, floor_z


def main():
    args = parse_args()
    root = args["dir"]
    scene = json.load(open(os.path.join(root, "scene.json")))
    channels = read_exr(os.path.join(root, "cycles.exr"))
    cycles = {"full": get_pass(channels, "combined", "RGB")}
    parts = [get_pass(channels, k, "RGB") for k in ("diffuse_direct", "diffuse_indirect", "diffuse_color",
                                                     "glossy_direct", "glossy_indirect", "glossy_color")]
    if all(p is not None for p in parts):
        cycles["diffuse"] = (parts[0] + parts[1]) * parts[2]
        cycles["reflect"] = (parts[3] + parts[4]) * parts[5]

    captures = os.path.join(root, "captures")
    frame = lambda name: os.path.join(captures, name, "frame_000_sharp.pfm")
    game = {"full": read_pfm(frame("full"))}
    if os.path.exists(frame("diffuse")) and os.path.exists(frame("reflect")) and "diffuse" in cycles:
        diffuse, reflect = read_pfm(frame("diffuse")), read_pfm(frame("reflect"))
        emission = np.maximum(diffuse + reflect - game["full"], 0)
        game["diffuse"] = np.maximum(diffuse - emission, 0)
        game["reflect"] = np.maximum(reflect - emission, 0)
    if game["full"].shape != cycles["full"].shape:
        sys.exit(f"cycles_compare.py: sizes differ: game {game['full'].shape}, Cycles {cycles['full'].shape}")

    out = os.path.join(root, "compare")
    os.makedirs(out, exist_ok=True)
    exposure = 0.5 / max(np.percentile(cycles["full"] @ LUMINANCE, 90), 1e-6)
    terms = [t for t in ("full", "diffuse", "reflect") if t in game]
    for term in terms:
        save(os.path.join(out, f"cycles_{term}.png"), cycles[term] * exposure)
        save(os.path.join(out, f"game_{term}.png"), game[term] * exposure)
        save(os.path.join(out, f"ratio_{term}.png"), ratio_image(game[term] @ LUMINANCE, cycles[term] @ LUMINANCE))
    overlay = np.stack([edges(game["full"]), edges(cycles["full"]), np.zeros(game["full"].shape[:2])], -1)
    save(os.path.join(out, "align_edges.png"), overlay ** 2.2)

    masks, floor_z = regions(channels, scene, root, args["boxes"], args["near_floor"])
    lines = [f"exposure {exposure:.4g}, floor at z {floor_z:.2f} (Blender)",
             f"{'region':40s} {'term':8s} {'Cycles':>9s} {'game':>9s} {'game/Cycles':>12s} {'pixels':>8s}"]
    for name, mask in masks.items():
        if not mask.any():
            continue
        for term in terms:
            c, g = (cycles[term] @ LUMINANCE)[mask].mean(), (game[term] @ LUMINANCE)[mask].mean()
            lines.append(f"{name:40s} {term:8s} {c:9.4f} {g:9.4f} {g / max(c, 1e-6):12.2f} {int(mask.sum()):8d}")
    text = "\n".join(lines) + "\n"
    open(os.path.join(out, "regions.txt"), "w").write(text)
    print(text, end="")


main()
