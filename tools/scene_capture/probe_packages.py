"""Bakes the test probes of the hangar-probes and flight-probes presets.

    python3 probe_packages.py --baker ENVIRONMENT_BAKE --out DIR

Writes one probes bake folder (bake.json, probes.ktx2, grid.ktx2) per test
set under DIR/probes/. An instance's `probes` field names one of these
folders. Also writes DIR/probe_tests.yaml with the environment package. Each
probe reflects one flat colour, so the captures show which probe applies
where. Every grid point sees light from the model's +X side, so the grid's
lighting direction shows too. Sets:

- hangar4, hangar16, hangar42: 2x2, 4x4 and 7x6 probes
  tiling the calamaricruisernewhangar interior (one ellipsoid in the first),
  reflecting onto the interior's box, with an 8x12x4 grid over it;
- isd: four probes, the quadrants of imperialstardestroyer and the
  space above it, with an 8x12x4 grid;
- test.hangar: a dim neutral environment without a sky, to check probes over
  an environment.
"""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys

COLOURS = [(1.0, 0.1, 0.1), (0.1, 1.0, 0.1), (0.1, 0.2, 1.0), (1.0, 0.9, 0.1),
           (1.0, 0.1, 1.0), (0.1, 1.0, 1.0), (1.0, 0.5, 0.1), (0.6, 0.6, 0.6)]


def write_hdr(path, width, height, pixel):
    """A flat (not run-length encoded) Radiance file; pixel(x, y) gives RGB."""
    def rgbe(rgb):
        top = max(rgb)
        if top < 1e-32:
            return bytes(4)
        exponent = math.frexp(top)[1]
        scale = 256.0 / (2.0 ** exponent)
        return bytes([min(255, int(c * scale)) for c in rgb] + [exponent + 128])
    with open(path, "wb") as stream:
        stream.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (height, width))
        for y in range(height):
            stream.write(b"".join(rgbe(pixel(x, y)) for x in range(width)))


def direction(x, y, width, height):
    """The cube-frame direction of a panorama pixel (envmap.h)."""
    lon = ((x + 0.5) / width - 0.5) * 2.0 * math.pi
    lat = (0.5 - (y + 0.5) / height) * math.pi
    return (math.cos(lat) * math.sin(lon), math.sin(lat), -math.cos(lat) * math.cos(lon))


def tiles(lo, hi, nx, ny):
    """nx x ny boxes tiling lo..hi in x and y, the full z range."""
    out = []
    for j in range(ny):
        for i in range(nx):
            size = ((hi[0] - lo[0]) / nx, (hi[1] - lo[1]) / ny, hi[2] - lo[2])
            centre = (lo[0] + size[0] * (i + 0.5), lo[1] + size[1] * (j + 0.5), (lo[2] + hi[2]) / 2)
            out.append((centre, size))
    return out


def box(lo, hi):
    return [(a + b) / 2 for a, b in zip(lo, hi)], [b - a for a, b in zip(lo, hi)]


def bake_probes(baker, out, name, boxes, grid_lo, grid_hi, resolution, jobs_dir, ellipsoid=(), room=None):
    job_dir = os.path.join(jobs_dir, name)
    os.makedirs(job_dir)
    for k in range(len(boxes)):
        colour = COLOURS[k % len(COLOURS)]
        write_hdr(os.path.join(job_dir, "probe_%d.hdr" % k), 16, 8, lambda x, y, c=colour: c)
    # One panorama for every grid point: lit from +X.
    write_hdr(os.path.join(job_dir, "grid.hdr"), 64, 32,
              lambda x, y: (0.03 + 0.5 * max(0.0, direction(x, y, 64, 32)[0]),) * 3)
    points = resolution[0] * resolution[1] * resolution[2]
    probes = []
    for k, (centre, size) in enumerate(boxes):
        shape = "ellipsoid" if k in ellipsoid else "box"
        probe = {"center": list(centre), "size": list(size), "shape": shape, "falloff": 0.2,
                 "parallaxSize": list(size), "parallaxShape": shape}
        if room:  # use the room's box as the parallax volume, like Fill with Probes does
            probe.update(parallaxCenter=room[0], parallaxSize=room[1], parallaxShape="box")
        probes.append(probe)
    centre, size = box(grid_lo, grid_hi)
    job = {"probes": probes, "probePanoramas": ["probe_%d.hdr" % k for k in range(len(boxes))],
           "gridBox": {"center": centre, "size": size}, "gridResolution": list(resolution),
           "gridPanoramas": ["grid.hdr"] * points}
    with open(os.path.join(job_dir, "job.json"), "w") as stream:
        json.dump(job, stream)
    target = os.path.join(out, "probes", name)
    os.makedirs(target)
    run = subprocess.run([baker, "--probes", os.path.join(job_dir, "job.json"),
                          "--cubes-out", os.path.join(target, "probes.ktx2"),
                          "--grid-out", os.path.join(target, "grid.ktx2"),
                          "--bake-out", os.path.join(target, "bake.json"), "--specular-samples", "64"],
                         capture_output=True, text=True)
    if run.returncode:
        sys.exit("%s: %s" % (name, run.stderr.strip()))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--baker", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    baker = os.path.abspath(args.baker)
    out = os.path.abspath(args.out)
    jobs_dir = os.path.join(out, "jobs")
    os.makedirs(jobs_dir)

    # The hangar interior, model frame (metres): wall to wall, floor to
    # ceiling, the open side to the back wall. The grid's box is the zone it
    # lights; the probes reflect onto it as a room.
    lo, hi = (-60.0, 0.0, -21.0), (60.0, 138.0, 13.0)
    room = box(lo, hi)
    for name, nx, ny in (("hangar4", 2, 2), ("hangar16", 4, 4), ("hangar42", 7, 6)):
        bake_probes(baker, out, name, tiles(lo, hi, nx, ny), lo, hi, (8, 12, 4), jobs_dir,
                    ellipsoid=(3,) if nx == 2 else (), room=room)
    # The Star Destroyer and 100 m of space above it.
    lo, hi = (-460.0, -840.0, -120.0), (460.0, 780.0, 420.0)
    bake_probes(baker, out, "isd", tiles(lo, hi, 2, 2), lo, hi, (8, 12, 4), jobs_dir)

    # A dim neutral environment, brighter above.
    pano = os.path.join(jobs_dir, "environment.hdr")
    write_hdr(pano, 256, 128, lambda x, y: (0.04, 0.04, 0.05) if y < 64 else (0.015, 0.015, 0.015))
    target = os.path.join(out, "environments", "test_hangar")
    os.makedirs(target)
    package = os.path.join(target, "package.yaml")
    run = subprocess.run([baker, "--input", pano, "--name", "test.hangar",
                          "--diffuse-out", os.path.join(target, "diffuse.ktx2"),
                          "--specular-out", os.path.join(target, "specular.ktx2"), "--package-out", package,
                          "--manifest-root", out, "--specular-size", "64", "--specular-samples", "64"],
                         capture_output=True, text=True)
    if run.returncode:
        sys.exit("test.hangar: %s" % run.stderr.strip())

    with open(os.path.join(out, "probe_tests.yaml"), "w") as stream:
        stream.write(open(package).read())
    shutil.rmtree(jobs_dir)
    print("wrote %s" % os.path.join(out, "probe_tests.yaml"))


if __name__ == "__main__":
    main()
