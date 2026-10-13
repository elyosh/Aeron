"""Checks the cooker's skin layers (tools/extensions/AERON_flight_model, "Skins").

    python3 skin_layers.py --cooker AERON_GLTF_COOK

A copy of the example model (../example) gets skin layers on its two
materials and three skin variants, one starting from a variant of the file.
The test cooks it and checks:
- the cooked variants, the mappings and the composites that variants share;
- the composite materials: their emissive strength, their animations, and
  no skins or skin variants left;
- the errors: an unknown skin, an unknown base, a repeated name and a layer
  of the wrong size.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

EXAMPLE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "example")

# ---- PNG ------------------------------------------------------------------


def write_png(path, width, height, rgba):
    rows = b"".join(b"\0" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as stream:
        stream.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


# ---- The model ----------------------------------------------------------------

SIZE = 64


def image(fill):
    return bytearray(c for y in range(SIZE) for x in range(SIZE) for c in fill(x, y))


def red(x, y):  # opaque on the left half
    return (220, 30, 30, 255 if x < SIZE // 2 else 0)


def red_screen(x, y):  # half covered everywhere
    return (200, 20, 20, 128)


def glow_base(x, y):  # an 8x8 block
    return (250, 250, 250, 255 if 8 <= x < 16 and 8 <= y < 16 else 0)


def glow_emissive(x, y):
    return (255, 200, 40, 255)


def build(folder, layer_size=SIZE):
    shutil.copytree(EXAMPLE, folder)
    textures = os.path.join(folder, "textures", "skins")
    os.makedirs(textures)
    files = {
        "red_lights": (red, SIZE), "red_screen": (red_screen, SIZE),
        "glow_base": (glow_base, layer_size), "glow_emissive": (glow_emissive, layer_size),
    }
    for name, (fill, size) in files.items():
        pixels = bytearray(c for y in range(size) for x in range(size) for c in fill(x * SIZE // size, y * SIZE // size))
        write_png(os.path.join(textures, name + ".png"), size, size, pixels)

    path = os.path.join(folder, "panel.gltf")
    doc = json.load(open(path))
    first = len(doc["images"])
    for k, name in enumerate(files):
        doc["images"].append({"name": name, "uri": "textures/skins/%s.png" % name, "mimeType": "image/png"})
        doc["textures"].append({"source": first + k, "sampler": 0})
    red_lights, red_screen_t, glow_b, glow_e = range(first, first + 4)
    lights, screen = doc["materials"]
    lights["extensions"]["AERON_flight_model"]["skins"] = {
        "red": {"baseColorTexture": {"index": red_lights}},
        "glow": {"baseColorTexture": {"index": glow_b}, "emissiveTexture": {"index": glow_e}, "emissiveStrength": 2},
    }
    screen["extensions"]["AERON_flight_model"]["skins"] = {"red": {"baseColorTexture": {"index": red_screen_t}}}
    # A variant of the file: Lights shows a copy with another roughness.
    alt = json.loads(json.dumps(lights))
    alt["name"] = "LightsAlt"
    alt["pbrMetallicRoughness"]["roughnessFactor"] = 0.3
    del alt["extensions"]
    doc["materials"].append(alt)
    doc["extensions"] = {"KHR_materials_variants": {"variants": [{"name": "State 1"}]}}
    doc["extensionsUsed"].append("KHR_materials_variants")
    doc["meshes"][0]["primitives"][0]["extensions"] = {
        "KHR_materials_variants": {"mappings": [{"material": 2, "variants": [0]}]}}
    doc["nodes"][0]["extensions"]["AERON_flight_model"].update({
        "skinVariants": [
            {"name": "Red", "layers": [{"skin": "red"}]},
            {"name": "Glow", "base": "State 1", "layers": [{"skin": "glow"}]},
            {"name": "Red glow", "base": "State 1", "layers": [{"skin": "red"}, {"skin": "glow", "opacity": 50}]},
        ],
        "markings": [None, "Red", "Glow"],
    })
    json.dump(doc, open(path, "w"), indent=1)
    return path, doc

# ---- Checks ---------------------------------------------------------------------


def mappings(doc, mesh):
    out = {}
    for mapping in doc["meshes"][mesh]["primitives"][0].get("extensions", {}).get(
            "KHR_materials_variants", {}).get("mappings", []):
        for v in mapping["variants"]:
            out[v] = mapping["material"]
    return out


def check_cook(cooker, scratch, failures):
    src = os.path.join(scratch, "cook")
    path, _ = build(src)
    glb = os.path.join(scratch, "panel.glb")
    run = subprocess.run([cooker, path, "--out", glb, "--quality", "fast"], capture_output=True, text=True)
    if run.returncode:
        failures.append("cook failed: " + (run.stderr.strip().splitlines() or ["?"])[-1])
        return
    data = open(glb, "rb").read()
    length = struct.unpack("<I", data[12:16])[0]
    doc = json.loads(data[20:20 + length])
    names = [v["name"] for v in doc["extensions"]["KHR_materials_variants"]["variants"]]
    if names != ["State 1", "Red", "Glow", "Red glow"]:
        failures.append("variants: %s" % names)
    root = next(node["extensions"]["AERON_flight_model"] for node in doc["nodes"]
                if node.get("extensions", {}).get("AERON_flight_model", {}).get("role") == "model")
    if root.get("markings") != [None, "Red", "Glow"] or "skinVariants" in root:
        failures.append("model root: markings %s, skin variants left: %s" % (root.get("markings"),
                                                                              "skinVariants" in root))
    if any("skins" in m.get("extensions", {}).get("AERON_flight_model", {}) for m in doc["materials"]):
        failures.append("skins left in the cooked file")
    if len(doc["materials"]) != 3 + 4:
        failures.append("composite materials: %d, expected 4" % (len(doc["materials"]) - 3))
    lights, screen = mappings(doc, 0), mappings(doc, 1)
    m = doc["materials"]
    if m[lights.get(0, 0)].get("name") != "LightsAlt" or not all(v in lights for v in (1, 2, 3)) or \
            len({lights[v] for v in (1, 2, 3)}) != 3:
        failures.append("Lights mappings: %s" % lights)
    if set(screen) != {1, 3} or screen[1] != screen[3]:
        failures.append("Screen mappings (Red and Red glow share one composite, Glow keeps the own): %s" % screen)
    if not lights.get(2) or m[lights[2]].get("extensions", {}).get(
            "KHR_materials_emissive_strength", {}).get("emissiveStrength") != 2:
        failures.append("Glow: emissive strength is not the layer's (2)")
    if lights.get(1) and "KHR_materials_emissive_strength" in m[lights[1]].get("extensions", {}):
        failures.append("Red: emissive strength set")
    if lights.get(1) and "flipbooks" not in m[lights[1]].get("extensions", {}).get("AERON_flight_model", {}):
        failures.append("the composite lost the material's flipbooks")


def check_errors(cooker, scratch, failures):
    def case(label, edit, expect, layer_size=SIZE):
        folder = os.path.join(scratch, "error_" + label.replace(" ", "_"))
        path, doc = build(folder, layer_size)
        edit(doc)
        json.dump(doc, open(path, "w"))
        run = subprocess.run([cooker, path, "--out", os.path.join(folder, "out.glb"), "--quality", "fast"],
                             capture_output=True, text=True)
        if run.returncode == 0 or expect not in run.stderr:
            failures.append("%s: expected a failure with '%s', got %d: %s" % (
                label, expect, run.returncode, run.stderr.strip()[-200:]))

    def variants(doc):
        return doc["nodes"][0]["extensions"]["AERON_flight_model"]["skinVariants"]

    case("unknown skin", lambda d: variants(d)[0]["layers"].append({"skin": "blue"}), "no material has skin 'blue'")
    case("unknown base", lambda d: variants(d)[1].update(base="State 9"), "base 'State 9' is not a variant")
    case("repeated name", lambda d: variants(d)[2].update(name="Red"), "skin variant 'Red' is repeated")
    case("file variant name", lambda d: variants(d)[2].update(name="State 1"), "has the name of a variant of the file")
    case("layer size", lambda d: None, "a layer is 32x32, not 64x64", layer_size=32)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cooker", required=True)
    args = parser.parse_args()
    cooker = os.path.abspath(args.cooker)
    scratch = tempfile.mkdtemp(prefix="aeron_skin_layers_")
    failures = []
    try:
        check_cook(cooker, scratch, failures)
        check_errors(cooker, scratch, failures)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    for failure in failures:
        print("FAIL " + failure)
    print("skin layers: %s" % ("ok" if not failures else "%d problem(s)" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
