"""Tests Import XWA OPT in a headless Blender.

    python3 opt_import.py --blender BLENDER --cooker AERON_GLTF_COOK --opt2gltf OPT2GLTF
                          --stock OPT [--xwau OPT] [--keep]

In a headless Blender with this add-on, it imports an OPT of the original
game (Original Game) into a scratch project. With --xwau, it also imports
an OPT from the FlightModels folder of an XWA Upgrade installation
(Auto) into another project. For each import, it checks that:
- the scene has the model root and one component per OPT mesh, as many as
  the model's .gltf has;
- the project has models/<key>/, identical to what opt2gltf converts;
- the project's libraries have every image that the model's materials use;
- the project becomes the first recent project;
- a second import without Replace fails and keeps the model;
- a second import with Replace and no project goes into the scene's OPT
  project.
The XWA Upgrade import also sets the installation in the preferences. A
copy of the OPT outside the installation must then convert the same way
(Auto).
"""

import argparse
import filecmp
import json
import os
import shutil
import subprocess
import sys
import tempfile

ADDON_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The add-on is loaded from its source directory, so do not write bytecode caches there.
sys.dont_write_bytecode = True
MODULE = "bl_ext.user_default.aeron_flight_model"


def _key(opt):
    return os.path.splitext(os.path.basename(opt))[0].lower()


# ---- inside Blender ------------------------------------------------------------

def blender_main(cooker, opt2gltf, projects, stock, xwau, outside):
    import addon_utils
    import bpy

    addon_utils.enable(MODULE, default_set=True, handle_error=None)
    prefs = bpy.context.preferences.addons[MODULE].preferences
    prefs.cooker = cooker
    prefs.opt2gltf = opt2gltf
    from bl_ext.user_default.aeron_flight_model import common

    failures = []

    def check(condition, message):
        print("OPT", "ok" if condition else "FAILED", message)
        if not condition:
            failures.append(message)

    for opt, data in ((stock, 'STOCK'), (xwau, 'AUTO')):
        if not opt:
            continue
        key = _key(opt)
        project = os.path.join(projects, data.lower())
        for obj in list(bpy.data.objects):
            bpy.data.objects.remove(obj)
        try:
            result = bpy.ops.import_scene.xwa_opt(filepath=opt, project=project, data=data)
        except RuntimeError as error:
            result = {str(error)}
        check('FINISHED' in result, "{}: imported ({})".format(key, ", ".join(sorted(result))))
        check(len(prefs.recent_projects) > 0 and prefs.recent_projects[0].path == project,
              "{}: first recent project".format(key))
        roots_found = [obj for obj in bpy.data.objects if common.role(obj) == 'model']
        gltf = os.path.join(project, "models", key, key + ".gltf")
        check(len(roots_found) == 1 and os.path.isfile(gltf), "{}: one model root, from {}".format(key, gltf))
        if roots_found and os.path.isfile(gltf):
            with open(gltf, encoding="utf-8") as stream:
                document = json.load(stream)
            expected = sum(1 for node in document["nodes"]
                           if (node.get("extensions") or {}).get(common.EXTENSION_NAME, {}).get("role") == "component")
            check(len(common.components(roots_found[0])) == expected,
                  "{}: {} components".format(key, len(common.components(roots_found[0]))))
        try:
            bpy.ops.import_scene.xwa_opt(filepath=opt, project=project, data=data)
            check(False, "{}: imported again without Replace".format(key))
        except RuntimeError:
            check(os.path.isfile(gltf), "{}: refused without Replace, model kept".format(key))
        check('FINISHED' in bpy.ops.import_scene.xwa_opt(filepath=opt, data=data, replace=True)
              and prefs.recent_projects[0].path == project,
              "{}: imported again with Replace, into the scene model's project".format(key))
    if xwau:
        installation = os.path.dirname(os.path.dirname(os.path.abspath(xwau)))
        check(prefs.xwau_folder == installation, "the preferences' installation: " + prefs.xwau_folder)
        result = bpy.ops.import_scene.xwa_opt(filepath=outside, project=os.path.join(projects, "outside"))
        check('FINISHED' in result, "{}: imported from outside the installation".format(_key(outside)))
    sys.exit(1 if failures else 0)


# ---- outside -------------------------------------------------------------------

def _library_keys(cooker, project):
    """Returns the keys of each library in the project, as {library: {key}}."""
    keys = {}
    effects = os.path.join(project, "effects")
    for name in sorted(os.listdir(effects)) if os.path.isdir(effects) else []:
        path = os.path.join(effects, name, name + ".yaml")
        if os.path.isfile(path):
            out = subprocess.run([cooker, "--library-json", path], capture_output=True, text=True, check=True).stdout
            keys[name] = set(json.loads(out)["images"])
    return keys


def _named_keys(gltf):
    """Returns the keys that the model's materials use, as {library: {key}}."""
    with open(gltf, encoding="utf-8") as stream:
        document = json.load(stream)
    names = {}
    for material in document.get("materials", []):
        extension = (material.get("extensions") or {}).get("AERON_flight_model") or {}
        for book in extension.get("flipbooks", []):
            names.setdefault(book.get("library"), set()).update(frame[0] for frame in book.get("frames", []))
        damage = extension.get("damage")
        if damage:
            names.setdefault(damage["library"], set()).update(
                stage for alternative in damage["alternatives"] for stage in alternative["stages"] if stage)
    return names


def _same_tree(a, b):
    compared = filecmp.dircmp(a, b)
    if compared.left_only or compared.right_only or compared.diff_files or compared.funny_files:
        return False
    return all(_same_tree(os.path.join(a, sub), os.path.join(b, sub)) for sub in compared.common_dirs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--blender", required=True)
    parser.add_argument("--cooker", required=True)
    parser.add_argument("--opt2gltf", required=True)
    parser.add_argument("--stock", required=True, help="an OPT of the original game")
    parser.add_argument("--xwau", help="an OPT of an XWA Upgrade installation's FlightModels folder")
    parser.add_argument("--keep", action="store_true", help="keep the scratch directory")
    parser.add_argument("--inside", nargs=2, help=argparse.SUPPRESS)
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    args = parser.parse_args(argv)
    if args.inside:
        blender_main(args.cooker, args.opt2gltf, args.inside[0], args.stock, args.xwau, args.inside[1])
        return

    scratch = tempfile.mkdtemp(prefix="aeron_opt_import_")
    projects = os.path.join(scratch, "projects")
    os.makedirs(os.path.join(scratch, "ext", "user_default"))
    os.symlink(ADDON_DIR, os.path.join(scratch, "ext", "user_default", "aeron_flight_model"))
    env = dict(os.environ, BLENDER_USER_EXTENSIONS=os.path.join(scratch, "ext"),
               BLENDER_USER_CONFIG=os.path.join(scratch, "config"),
               BLENDER_USER_SCRIPTS=os.path.join(scratch, "scripts"))
    inside = ["--blender", args.blender, "--cooker", args.cooker, "--opt2gltf", args.opt2gltf, "--stock", args.stock]
    outside = os.path.join(scratch, "outside", os.path.basename(args.xwau or ""))
    if args.xwau:
        inside += ["--xwau", args.xwau]
        os.makedirs(os.path.dirname(outside))
        shutil.copyfile(args.xwau, outside)
    completed = subprocess.run([args.blender, "-b", "--factory-startup", "--python-exit-code", "1",
                                "--python", os.path.abspath(__file__), "--"] + inside +
                               ["--inside", projects, outside], env=env, capture_output=True, text=True)
    failures = []
    for line in completed.stdout.splitlines():
        if line.startswith(("OPT", "Error:")):
            print(line)
    if completed.returncode != 0:
        failures.append("Blender: " + " | ".join(completed.stderr.strip().splitlines()[-5:]))

    # Check that each model in the project is what opt2gltf converts on its
    # own, and that the libraries have every image the model uses.
    for opt, mode, data in ((args.stock, ["--stock"], "stock"), (args.xwau, None, "auto")):
        if not opt:
            continue
        key = _key(opt)
        project = os.path.join(projects, data)
        direct = os.path.join(scratch, "direct", data)
        if mode is None:
            mode = ["--xwau-root", os.path.dirname(os.path.dirname(os.path.abspath(opt)))]
        subprocess.run([args.opt2gltf] + mode + ["--project", direct, opt], capture_output=True, check=True)
        same = _same_tree(os.path.join(direct, "models", key), os.path.join(project, "models", key))
        print("OPT", "ok" if same else "FAILED", "{}: the project's model is opt2gltf's".format(key))
        if not same:
            failures.append(key + " differs")
        present = _library_keys(args.cooker, project)
        missing = sorted("{} {}".format(library, image) for library, images in
                         _named_keys(os.path.join(project, "models", key, key + ".gltf")).items()
                         for image in images if image not in present.get(library, set()))
        print("OPT", "FAILED" if missing else "ok", "{}: library images named: {}".format(
            key, ", ".join(missing[:5]) if missing else "all present"))
        if missing:
            failures.append(key + " lacks library images")
    if args.xwau:
        key = _key(args.xwau)
        same = _same_tree(os.path.join(projects, "auto", "models", key),
                          os.path.join(projects, "outside", "models", key))
        print("OPT", "ok" if same else "FAILED", "{}: from outside, the same as from the installation".format(key))
        if not same:
            failures.append(key + " differs from outside")
    for failure in failures:
        print("FAILED", failure)
    if args.keep or failures:
        print("scratch:", scratch)
    else:
        shutil.rmtree(scratch, ignore_errors=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
