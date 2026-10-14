"""Builds the Blender add-on's package for one platform, with its tools.

    python3 package_addon.py --platform PLATFORM --cooker AERON_GLTF_COOK --opt2gltf OPT2GLTF
                             [--environment-bake ENVIRONMENT_BAKE] --out DIR
                             [--license NAME=FILE]... [--blender BLENDER]

PLATFORM is windows-x64, macos (universal tools: Apple silicon and Intel)
or linux-x64. The package is blender_addon/ with the cooker, opt2gltf and,
when given, environment_bake in bin/, the licenses of what they link in licenses/ (the third_party ones,
and each --license, e.g. zstd's), and the manifest's platforms set; it is
written as DIR/aeron_flight_model-<version>-<platform>.zip. Blender's own
build sorts only Python wheels between platforms, so it is assembled here.
With --blender, `blender --command extension validate` checks it.
"""

import argparse
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ADDON = os.path.join(HERE, "blender_addon")
THIRD_PARTY = os.path.join(HERE, "..", "..", "third_party")
PLATFORMS = {
    "windows-x64": ["windows-x64"],
    "macos": ["macos-arm64", "macos-x64"],
    "linux-x64": ["linux-x64"],
}
# The third_party code the tools link, and its license file.
LICENSES = {
    "bc7enc": "bc7enc/LICENSE",
    "bcdec": "bcdec/LICENSE",
    "cgltf": "cgltf/LICENSE",
    "cJSON": "cjson/LICENSE",
    "ispc_texcomp": "ispc_texcomp/LICENSE",
    "libyaml": "libyaml/License",
    "lzma": "lzma/LICENSE",
    "mikktspace": "mikktspace/LICENSE",
    "stb": "stb/LICENSE",
    "xxhash": "xxhash/LICENSE",
}
# As the manifest's [build] paths_exclude_pattern: caches, dotfiles, tests.
EXCLUDED = ("__pycache__", "tests")
# A fixed time for every entry, so that the same inputs make the same zip.
DATE = (1980, 1, 1, 0, 0, 0)


def addon_files():
    """(path in the package, path on disk) of the add-on's files, sorted."""
    files = []
    for directory, subdirectories, names in os.walk(ADDON):
        subdirectories[:] = sorted(d for d in subdirectories if d not in EXCLUDED and not d.startswith("."))
        for name in names:
            if not name.startswith(".") and not name.endswith(".pyc"):
                path = os.path.join(directory, name)
                files.append((os.path.relpath(path, ADDON).replace(os.sep, "/"), path))
    return sorted(files)


def manifest(platforms):
    """The add-on's manifest with `platforms` set, and its version."""
    with open(os.path.join(ADDON, "blender_manifest.toml"), encoding="utf-8") as stream:
        text = stream.read()
    if re.search(r"^platforms\s*=", text, re.MULTILINE):
        raise SystemExit("blender_manifest.toml already sets platforms")
    line = "platforms = [{}]\n".format(", ".join('"{}"'.format(p) for p in platforms))
    text, count = re.subn(r"^(blender_version_min\s*=.*\n)", lambda m: m.group(1) + line, text,
                          count=1, flags=re.MULTILINE)
    version = re.search(r'^version\s*=\s*"([^"]+)"', text, re.MULTILINE)
    if count != 1 or not version:
        raise SystemExit("blender_manifest.toml has no blender_version_min or version")
    return text, version.group(1)


def add(archive, name, data, mode=0o644):
    info = zipfile.ZipInfo(name, DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (0o100000 | mode) << 16
    archive.writestr(info, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", required=True, choices=sorted(PLATFORMS))
    parser.add_argument("--cooker", required=True, help="the aeron_gltf_cook built for the platform")
    parser.add_argument("--opt2gltf", required=True, help="the opt2gltf built for the platform")
    parser.add_argument("--environment-bake", help="the environment_bake built for the platform, for "
                                                   "Bake Probes")
    parser.add_argument("--out", required=True, help="the directory the package is written to")
    parser.add_argument("--license", action="append", default=[], metavar="NAME=FILE",
                        help="another license of what the tools link (repeatable)")
    parser.add_argument("--blender", help="a Blender executable to validate the package with")
    args = parser.parse_args()

    licenses = {name: os.path.join(THIRD_PARTY, path) for name, path in LICENSES.items()}
    for entry in args.license:
        name, separator, path = entry.partition("=")
        if not separator or not name:
            parser.error("--license takes NAME=FILE")
        licenses[name] = path
    for path in [args.cooker, args.opt2gltf] + ([args.environment_bake] if args.environment_bake else []) + \
            list(licenses.values()):
        if not os.path.isfile(path):
            parser.error("no such file: " + path)

    text, version = manifest(PLATFORMS[args.platform])
    exe = ".exe" if args.platform.startswith("windows") else ""
    tools = [("bin/aeron_gltf_cook" + exe, args.cooker), ("bin/opt2gltf" + exe, args.opt2gltf)]
    if args.environment_bake:
        tools.append(("bin/environment_bake" + exe, args.environment_bake))
    os.makedirs(args.out, exist_ok=True)
    package = os.path.join(args.out, "aeron_flight_model-{}-{}.zip".format(version, args.platform))
    with zipfile.ZipFile(package, "w") as archive:
        for name, path in addon_files():
            if name == "blender_manifest.toml":
                add(archive, name, text.encode("utf-8"))
            else:
                with open(path, "rb") as stream:
                    add(archive, name, stream.read())
        for name, path in tools:
            with open(path, "rb") as stream:
                add(archive, name, stream.read(), 0o755)
        for name, path in sorted(licenses.items()):
            with open(path, "rb") as stream:
                add(archive, "licenses/{}.txt".format(name), stream.read())
    print(package)

    if args.blender:
        completed = subprocess.run([args.blender, "--factory-startup", "--command", "extension", "validate",
                                    package], capture_output=True, text=True)
        sys.stdout.write(completed.stdout)
        sys.stderr.write(completed.stderr)
        if completed.returncode != 0:
            raise SystemExit("validation failed")


if __name__ == "__main__":
    main()
