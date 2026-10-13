"""Checks environment_bake --kind lighting.

    python3 lighting.py --baker ENVIRONMENT_BAKE

Bakes a small panorama of an enclosed space (bright above, dim below) and
checks that the baker writes the diffuse and specular cubes, no radiance
cube, and a `kind: lighting` entry with reflections and no fill. The baker
re-reads the entry with the runtime parser, so a zero exit also means the
game accepts it.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile


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


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--baker", required=True)
    args = parser.parse_args()
    scratch = tempfile.mkdtemp(prefix="environment_bake_lighting_")
    failures = []
    try:
        pano = os.path.join(scratch, "space.hdr")
        write_hdr(pano, 256, 128, lambda x, y: (4.0, 4.0, 3.5) if y < 32 else (0.3, 0.3, 0.3))
        out = os.path.join(scratch, "flight", "environments", "hangars", "test")
        os.makedirs(out)
        run = subprocess.run([os.path.abspath(args.baker), "--input", pano, "--kind", "lighting",
                              "--name", "hangar.test", "--diffuse-out", os.path.join(out, "diffuse.ktx2"),
                              "--specular-out", os.path.join(out, "specular.ktx2"),
                              "--package-out", os.path.join(out, "package.yaml"), "--manifest-root", scratch,
                              "--specular-size", "64", "--specular-samples", "64", "--jobs", "2"],
                             capture_output=True, text=True)
        if run.returncode:
            failures.append("bake failed: " + run.stderr.strip()[-300:])
        else:
            files = sorted(os.listdir(out))
            if files != ["diffuse.ktx2", "package.yaml", "specular.ktx2"]:
                failures.append("files: %s" % files)
            entry = open(os.path.join(out, "package.yaml")).read()
            for expected in ("kind: lighting", 'specular: "flight/environments/hangars/test/specular.ktx2"',
                             "specular_strength: 1", "ambient_fill: [0, 0, 0]"):
                if expected not in entry:
                    failures.append("package.yaml lacks %r:\n%s" % (expected, entry))
            if "radiance" in entry:
                failures.append("package.yaml has a radiance cube")
        refused = subprocess.run([os.path.abspath(args.baker), "--input", pano, "--kind", "lighting",
                                  "--name", "hangar.test", "--radiance-out", "x", "--diffuse-out", "y",
                                  "--specular-out", "z"], capture_output=True, text=True)
        if refused.returncode != 2:
            failures.append("--radiance-out with --kind lighting was not refused")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    for failure in failures:
        print("FAIL " + failure)
    print("lighting bake: %s" % ("ok" if not failures else "%d problem(s)" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
