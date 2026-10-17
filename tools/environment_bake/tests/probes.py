"""Checks environment_bake --probes.

    python3 probes.py --baker ENVIRONMENT_BAKE

Bakes a small job (needs Python 3.14 for compression.zstd): five probes over
a 3x2x2 grid whose panoramas are constant, except one point lit from above
and one marked inside geometry. Checks the cube array and grid headers, the
grid's light on the faces of a cube (a constant radiance c gives c on every
face; radiance 1 from the upper half gives 1 up, 0 down, 0.5 on the sides,
and a mean of 0.5), the invalid point filled from
its neighbours, the cell report, the layout written to bake.json, and that
wrong jobs exit 2. The baker re-reads bake.json with the runtime parser, so a
zero exit also means the game accepts it.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

from compression import zstd


def write_hdr(path, width, height, pixel):
    """A flat (not run-length encoded) Radiance file; pixel(x, y) gives RGB."""
    def rgbe(rgb):
        top = max(rgb)
        if top < 1e-32:
            return bytes(4)
        exponent = 0
        mantissa = top
        while mantissa >= 1.0:
            mantissa /= 2.0
            exponent += 1
        while mantissa < 0.5:
            mantissa *= 2.0
            exponent -= 1
        scale = 256.0 / (2.0 ** exponent)
        return bytes([int(c * scale) for c in rgb] + [exponent + 128])
    with open(path, "wb") as stream:
        stream.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (height, width))
        for y in range(height):
            stream.write(b"".join(rgbe(pixel(x, y)) for x in range(width)))


def ktx2(path):
    """Header fields and the decompressed level 0."""
    data = open(path, "rb").read()
    width, height, depth, layers, faces, levels, scheme = struct.unpack_from("<7I", data, 20)
    offset, length = struct.unpack_from("<QQ", data, 80)
    level0 = data[offset:offset + length]
    if scheme == 2:
        level0 = zstd.decompress(level0)
    return {"width": width, "height": height, "layers": layers, "faces": faces, "levels": levels}, level0


def grid_point(level0, width, x, row):
    """The point at (x, row): its texels toward +x, -x, +y, -y, +z, -z, each
    RGB and the mean's luminance."""
    values = struct.unpack_from("<%de" % (width * 4), level0, row * width * 8)
    return [values[(6 * x + f) * 4:(6 * x + f) * 4 + 4] for f in range(6)]


def close(a, b, tolerance=0.03):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


def probe(center, size):
    return {"center": center, "size": [size] * 3, "shape": "box", "falloff": 0.2, "parallaxSize": [size] * 3,
            "parallaxShape": "box"}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--baker", required=True)
    args = parser.parse_args()
    baker = os.path.abspath(args.baker)
    scratch = tempfile.mkdtemp(prefix="environment_bake_probes_")
    failures = []
    try:
        job_dir = os.path.join(scratch, "job")
        os.makedirs(job_dir)
        # Probes: two side by side, three more nested over the first.
        probes = [([-5, 0, 0], 10), ([5, 0, 0], 10), ([-5, 0, 0], 6), ([-5, 0, 0], 4), ([-5, 0, 0], 2)]
        for i in range(len(probes)):
            write_hdr(os.path.join(job_dir, "probe_%d.hdr" % i), 64, 32, lambda x, y, i=i: (i + 1.0, 1.0, 0.5))
        # Grid points: constant 2, except point 1 lit from above (upper half
        # 1, lower half 0) and point 4 inside geometry (no panorama needed).
        for i in range(12):
            if i == 1:
                pixel = lambda x, y: (1.0, 1.0, 1.0) if y < 16 else (0.0, 0.0, 0.0)
            else:
                pixel = lambda x, y: (2.0, 2.0, 2.0)
            if i != 4:
                write_hdr(os.path.join(job_dir, "grid_%d.hdr" % i), 64, 32, pixel)
        base = {"probes": [probe(c, s) for c, s in probes],
                "probePanoramas": ["probe_%d.hdr" % i for i in range(len(probes))],
                "gridBox": {"center": [0, 0, 0], "size": [20, 10, 10]}, "gridResolution": [3, 2, 2],
                "gridPanoramas": ["grid_%d.hdr" % i for i in range(12)], "gridInvalid": [4],
                "keyLightShadow": 0.5}

        def write_job(name, item):
            path = os.path.join(job_dir, name)
            with open(path, "w") as stream:
                json.dump(item, stream)
            return path
        job = write_job("job.json", base)

        out = os.path.join(scratch, "bake")
        os.makedirs(out)
        run = subprocess.run([baker, "--probes", job, "--cubes-out", os.path.join(out, "probes.ktx2"),
                              "--grid-out", os.path.join(out, "grid.ktx2"),
                              "--bake-out", os.path.join(out, "bake.json"),
                              "--specular-size", "16", "--specular-samples", "16", "--jobs", "2"],
                             capture_output=True, text=True)
        if run.returncode:
            failures.append("bake failed: " + run.stderr.strip()[-400:])
        else:
            cubes, _ = ktx2(os.path.join(out, "probes.ktx2"))
            if (cubes["faces"], cubes["layers"], cubes["levels"], cubes["width"]) != (6, 5, 5, 16):
                failures.append("cube array header: %s" % cubes)
            grid, level0 = ktx2(os.path.join(out, "grid.ktx2"))
            if (grid["width"], grid["height"], grid["faces"], grid["layers"]) != (18, 4, 1, 0):
                failures.append("grid header: %s" % grid)
            else:
                constant = grid_point(level0, 18, 0, 0)
                if not all(close(f, (2, 2, 2, 2)) for f in constant):
                    failures.append("constant point: %s" % constant)
                above = grid_point(level0, 18, 1, 0)
                expected = [0.5, 0.5, 0.5, 0.5, 1, 0]
                if not all(close(f, (e, e, e, 0.5)) for f, e in zip(above, expected)):
                    failures.append("point lit from above: %s" % above)
                # Point 4 is (1, 1, 0); its neighbours within 1 include point 1.
                filled = grid_point(level0, 18, 1, 1)
                if not all(1.0 < filled[5][c] < filled[4][c] < 2.0 for c in range(3)):
                    failures.append("invalid point not filled from its neighbours: %s" % filled)
            with open(os.path.join(out, "bake.json")) as stream:
                layout = json.load(stream)
            if (layout["gridResolution"] != [3, 2, 2] or layout["keyLightShadow"] != 0.5 or
                    len(layout["probes"]) != len(probes) or "probePanoramas" in layout):
                failures.append("bake.json: %s" % layout)
            if "at most 4 probes in one" not in run.stdout:
                failures.append("cell report: %s" % run.stdout)
            if "warning" in run.stderr:
                failures.append("unexpected warning: %s" % run.stderr)

        # A sixth probe inside the nested ones: the cell around them holds five
        # (the probe at +5 does not reach it).
        crowded = write_job("crowded.json", dict(base, probes=[probe([-5, 0, 0], 1)] + base["probes"],
                                                 probePanoramas=["probe_0.hdr"] + base["probePanoramas"]))
        run = subprocess.run([baker, "--probes", crowded, "--cubes-out", os.path.join(scratch, "c.ktx2"),
                              "--grid-out", os.path.join(scratch, "g.ktx2"), "--bake-out",
                              os.path.join(scratch, "b.json"), "--specular-size", "16", "--specular-samples", "16"],
                             capture_output=True, text=True)
        if run.returncode or "at most 5 probes in one" not in run.stdout or "overlap more than 4" not in run.stderr:
            failures.append("crowded cells not reported: %s %s" % (run.stdout, run.stderr))

        # Wrong jobs.
        for name, change in (("panorama count", {"probePanoramas": ["x"] + base["probePanoramas"]}),
                             ("invalid index", {"gridInvalid": [12]})):
            wrong = write_job("wrong.json", dict(base, **change))
            run = subprocess.run([baker, "--probes", wrong, "--cubes-out", "c", "--grid-out", "g",
                                  "--bake-out", "b"], capture_output=True, text=True)
            if run.returncode != 2:
                failures.append("%s: exit %d, expected 2" % (name, run.returncode))
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    for failure in failures:
        print("FAIL " + failure)
    print("probes bake: %s" % ("FAILED" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
