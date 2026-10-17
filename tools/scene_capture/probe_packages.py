"""Bakes the test probes packages of the hangar-probes and flight-probes presets.

    python3 probe_packages.py --baker ENVIRONMENT_BAKE --out DIR

Writes DIR/probe_tests.yaml and the packages it names, under DIR. Each probe
reflects one flat colour, so the captures show which probe applies where;
every grid point sees light from the model's +X side, so the grid's lighting
direction shows too. Packages:

- probes.hangar4, probes.hangar16, probes.hangar42: 2x2, 4x4 and 7x6 probes
  tiling the calamaricruisernewhangar interior (one ellipsoid in the first),
  reflecting onto the interior's box, with an 8x12x4 grid over it;
- probes.isd: four probes, the quadrants of imperialstardestroyer and the
  space above it, with an 8x12x4 grid;
- test.hangar: a dim neutral environment without a sky, to check probes over
  an environment.
"""

import argparse
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


def vec(v):
    return "[%s]" % ", ".join("%g" % c for c in v)


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
    with open(os.path.join(job_dir, "job.yaml"), "w") as stream:
        stream.write("probes:\n")
        for k, (centre, size) in enumerate(boxes):
            shape = "ellipsoid" if k in ellipsoid else "box"
            # The room's box as the parallax volume, as Fill with Probes does; else the probe's own.
            parallax = "parallax_center: %s, parallax_size: %s, parallax_shape: box" % room if room else \
                "parallax_size: %s, parallax_shape: %s" % (vec(size), shape)
            stream.write("  - {center: %s, size: %s, shape: %s, falloff: 0.2, %s}\n" % (
                vec(centre), vec(size), shape, parallax))
        stream.write("probe_panoramas: [%s]\n" % ", ".join("probe_%d.hdr" % k for k in range(len(boxes))))
        centre = [(a + b) / 2 for a, b in zip(grid_lo, grid_hi)]
        size = [b - a for a, b in zip(grid_lo, grid_hi)]
        stream.write("grid_box: {center: %s, size: %s}\ngrid_resolution: %s\n" % (vec(centre), vec(size),
                                                                                  vec(resolution)))
        stream.write("grid_panoramas: [%s]\n" % ", ".join(["grid.hdr"] * points))
    target = os.path.join(out, "models", name)
    os.makedirs(target)
    package = os.path.join(target, "package.yaml")
    run = subprocess.run([baker, "--probes", os.path.join(job_dir, "job.yaml"), "--name", name,
                          "--cubes-out", os.path.join(target, "probes.ktx2"),
                          "--grid-out", os.path.join(target, "grid.ktx2"), "--package-out", package,
                          "--manifest-root", out, "--specular-samples", "64"],
                         capture_output=True, text=True)
    if run.returncode:
        sys.exit("%s: %s" % (name, run.stderr.strip()))
    return package


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--baker", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    baker = os.path.abspath(args.baker)
    out = os.path.abspath(args.out)
    jobs_dir = os.path.join(out, "jobs")
    os.makedirs(jobs_dir)
    packages = []

    # The hangar interior, model frame (metres): wall to wall, floor to
    # ceiling, the open side to the back wall. The grid's box is the zone it
    # lights; the probes reflect onto it as a room.
    lo, hi = (-60.0, 0.0, -21.0), (60.0, 138.0, 13.0)
    room = (vec([(a + b) / 2 for a, b in zip(lo, hi)]), vec([b - a for a, b in zip(lo, hi)]))
    for name, nx, ny in (("probes.hangar4", 2, 2), ("probes.hangar16", 4, 4), ("probes.hangar42", 7, 6)):
        packages.append(bake_probes(baker, out, name, tiles(lo, hi, nx, ny), lo, hi, (8, 12, 4), jobs_dir,
                                    ellipsoid=(3,) if nx == 2 else (), room=room))
    # The Star Destroyer and 100 m of space above it.
    lo, hi = (-460.0, -840.0, -120.0), (460.0, 780.0, 420.0)
    packages.append(bake_probes(baker, out, "probes.isd", tiles(lo, hi, 2, 2), lo, hi, (8, 12, 4), jobs_dir))

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
    packages.append(package)

    with open(os.path.join(out, "probe_tests.yaml"), "w") as stream:
        stream.write("packages:\n")
        for path in packages:
            stream.write(open(path).read().split("\n", 1)[1])
    shutil.rmtree(jobs_dir)
    print("wrote %s" % os.path.join(out, "probe_tests.yaml"))


if __name__ == "__main__":
    main()
