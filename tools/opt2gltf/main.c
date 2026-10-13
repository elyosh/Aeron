/*
 * opt2gltf converts an OPT model to glTF 2.0 (a .gltf file, a .bin file and PNG
 * textures). It also writes the batch outputs of an XWA Upgrade conversion.
 *
 * Usage:
 *   opt2gltf (--stock | --xwau-root DIR) [--smooth-angle DEG]
 *            (--project DIR [--replace] | <output_dir>) FILE.opt
 *   opt2gltf effects --xwau-root DIR --project DIR <project|model.gltf>...
 *   opt2gltf missions --xwau-root DIR --stock-game DIR <output_dir>
 *
 * The model is written into <output_dir>, or into a project with --project.
 * A project is a folder that holds models (models/<key>/) and their effect
 * libraries (effects/<library>/). The key is the OPT name in lower case, and
 * the OPT becomes models/<key>/<key>.gltf with its .bin and a textures/ folder.
 * In XWAU mode, the effect library images that the materials name and that the
 * project does not have yet are added (xwau_library.h). If the model folder
 * already exists, this is an error unless --replace is given.
 *
 * `effects` writes the effect libraries that the given models use into the
 * effects/ folder of --project, replacing them, together with the game's
 * effects.yaml (xwau_library.h). Each input is a model .gltf or a project, which
 * stands for all its models.
 *
 * `missions` writes the selections of XWAU's missions for the missions of the
 * original game, installed in --stock-game. Each mission gets a file
 * <output_dir>/<dir>/<mission>.yaml (xwau_mission_files.h).
 *
 * Replacement workflow (both tools are built with AERON_BUILD_TOOLS):
 *
 *   opt2gltf --stock FLIGHTMODELS/XWING.OPT out
 *   aeron_gltf_cook out/XWING.gltf --out <asset root>/remaster/models/xwing.glb
 *
 * The cooked GLB replaces the OPT that has the same base name in lower case.
 * --stock converts an OPT of the original game the way the game draws it.
 * --xwau-root DIR converts an X-Wing Alliance Upgrade model with everything XWAU
 * adds to it, read from the XWAU installation in DIR (model_conversion.h).
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model_conversion.h"
#include "util.h"
#include "xwau_config.h"
#include "xwau_library.h"
#include "xwau_mission_files.h"
#include "xwau_missions.h"

/* Converts the OPT into models/<key>/ in the project. In XWAU mode, it then
 * adds the library images that the model names. */
static bool convert_into_project(const ModelConversion* mc, const char* opt_path, const char* basename,
								 const char* project, bool replace) {
	char key[256], models[1024], final_dir[1300], scratch[1300];
	LowerCopy(key, sizeof key, basename);
	if (!PathFormat(models, sizeof models, "%s/models", project) ||
		!PathFormat(final_dir, sizeof final_dir, "%s/%s", models, key) ||
		!PathFormat(scratch, sizeof scratch, "%s/.%s.opt2gltf", models, key))
		return false;
	if ((IsDir(final_dir) || IsFile(final_dir)) && !replace) {
		Report(opt_path, "%s already exists (use --replace to replace it)", final_dir);
		return false;
	}
	if (!MakeDirs(project) || !MakeDirs(models) || !RemoveTree(scratch)) {
		Report(opt_path, "cannot write into %s", models);
		return false;
	}
	if (!ModelConversion_Convert(mc, opt_path, basename, scratch)) {
		RemoveTree(scratch);
		return false;
	}
	char from[1600], to[1600];
	if (!PathFormat(from, sizeof from, "%s/%s.gltf", scratch, basename) ||
		!PathFormat(to, sizeof to, "%s/%s.gltf", scratch, key)) {
		RemoveTree(scratch);
		return false;
	}
	if ((strcmp(from, to) != 0 && rename(from, to) != 0) || !RemoveTree(final_dir) ||
		rename(scratch, final_dir) != 0) {
		Report(opt_path, "cannot create %s: %s", final_dir, strerror(errno));
		RemoveTree(scratch);
		return false;
	}
	if (!PathFormat(to, sizeof to, "%s/%s.gltf", final_dir, key))
		return false;
	Report(opt_path, "wrote %s", to);
	return !mc->xwau || XwauLibrary_AddModel(mc->game, project, to);
}

static int usage(void) {
	fprintf(stderr, "usage: opt2gltf (--stock | --xwau-root DIR) [--smooth-angle DEG]\n"
					"                (--project DIR [--replace] | <output_dir>) FILE.opt\n"
					"       opt2gltf effects --xwau-root DIR --project DIR <project|model.gltf>...\n"
					"       opt2gltf missions --xwau-root DIR --stock-game DIR <output_dir>\n");
	return 2;
}

/* ===== Arguments ======================================================== */

enum {
	FLAG_STOCK        = 1 << 0,
	FLAG_XWAU_ROOT    = 1 << 1,
	FLAG_SMOOTH_ANGLE = 1 << 2,
	FLAG_PROJECT      = 1 << 3,
	FLAG_REPLACE      = 1 << 4,
	FLAG_STOCK_GAME   = 1 << 5,
};

static const struct {
	const char* name;
	unsigned    flag;
	bool        value;
} kFlags[] = {
	{ "--stock", FLAG_STOCK, false },
	{ "--xwau-root", FLAG_XWAU_ROOT, true },
	{ "--smooth-angle", FLAG_SMOOTH_ANGLE, true },
	{ "--project", FLAG_PROJECT, true },
	{ "--replace", FLAG_REPLACE, false },
	{ "--stock-game", FLAG_STOCK_GAME, true },
};

typedef struct Args {
	const char* xwau_root;
	bool        stock;
	float       smooth_angle; /* negative when not given */
	const char* project;
	bool        replace;
	const char* stock_game;
	char**      inputs; /* the arguments after the flags */
	int         input_count;
} Args;

/* Parses the flags listed in `allowed`, then the inputs. Returns false, after
 * reporting it, when a flag is unknown, not allowed, or has no value. */
static bool parse_args(int argc, char** argv, unsigned allowed, Args* out) {
	memset(out, 0, sizeof *out);
	out->smooth_angle = -1.0f;
	int ai            = 0;
	for (; ai < argc && strncmp(argv[ai], "--", 2) == 0; ++ai) {
		size_t f = 0;
		while (f < sizeof kFlags / sizeof kFlags[0] && strcmp(argv[ai], kFlags[f].name) != 0)
			++f;
		if (f == sizeof kFlags / sizeof kFlags[0] || !(allowed & kFlags[f].flag)) {
			Report(NULL, "unknown option '%s'", argv[ai]);
			return false;
		}
		const char* value = kFlags[f].value ? argv[ai + 1] : NULL;
		if (kFlags[f].value && (ai + 1 == argc || !value)) {
			Report(NULL, "%s needs a value", argv[ai]);
			return false;
		}
		ai += kFlags[f].value;
		switch (kFlags[f].flag) {
			case FLAG_STOCK:
				out->stock = true;
				break;
			case FLAG_XWAU_ROOT:
				if (out->xwau_root) {
					Report(NULL, "--xwau-root can only be given once");
					return false;
				}
				out->xwau_root = value;
				break;
			case FLAG_SMOOTH_ANGLE: {
				char* end         = NULL;
				out->smooth_angle = strtof(value, &end);
				if (end == value || *end || !isfinite(out->smooth_angle) || out->smooth_angle < 0.0f ||
					out->smooth_angle > 180.0f) {
					Report(NULL, "--smooth-angle needs an angle from 0 to 180 degrees");
					return false;
				}
				break;
			}
			case FLAG_PROJECT:
				out->project = value;
				break;
			case FLAG_REPLACE:
				out->replace = true;
				break;
			case FLAG_STOCK_GAME:
				out->stock_game = value;
				break;
		}
	}
	out->inputs      = argv + ai;
	out->input_count = argc - ai;
	return true;
}

/* ===== Commands ========================================================= */

/* opt2gltf (--stock | --xwau-root DIR) [--smooth-angle DEG]
 *          (--project DIR [--replace] | <output_dir>) FILE.opt */
static int run_convert(int argc, char** argv) {
	Args a;
	if (!argc)
		return usage();
	if (!parse_args(argc, argv, FLAG_STOCK | FLAG_XWAU_ROOT | FLAG_SMOOTH_ANGLE | FLAG_PROJECT | FLAG_REPLACE,
					&a))
		return 2;
	if (a.stock == (a.xwau_root != NULL)) {
		Report(NULL, "give exactly one of --stock and --xwau-root");
		return usage();
	}
	if (a.smooth_angle >= 0.0f && !a.stock) {
		Report(NULL, "--smooth-angle only works with --stock");
		return 2;
	}
	if (a.replace && !a.project) {
		Report(NULL, "--replace needs --project");
		return 2;
	}
	if (a.input_count != (a.project ? 1 : 2))
		return usage();
	const char* in = a.inputs[0];
	if (!IsFile(in)) {
		Report(in, "not an OPT file");
		return 1;
	}

	ModelConversion mc = { .smooth_angle = a.smooth_angle };
	if (!a.stock && !ModelConversion_InitXwau(&mc, a.xwau_root)) {
		ModelConversion_Free(&mc);
		return 1;
	}

	/* The OPT name is the part after the last '/' or '\\', without the
	 * extension. */
	const char* fname = in;
	for (const char* c = in; *c; ++c)
		if (*c == '/' || *c == '\\')
			fname = c + 1;
	const char* dot = strrchr(fname, '.');
	char        base[256];
	if (!PathFormat(base, sizeof base, "%.*s", dot ? (int)(dot - fname) : (int)strlen(fname), fname)) {
		ModelConversion_Free(&mc);
		return 1;
	}
	const bool ok = a.project ? convert_into_project(&mc, in, base, a.project, a.replace)
							  : ModelConversion_Convert(&mc, in, base, a.inputs[1]);
	ModelConversion_Free(&mc);
	return ok ? 0 : 1;
}

/* opt2gltf effects --xwau-root DIR --project DIR <project|model.gltf>... */
static int run_effects(int argc, char** argv) {
	Args a;
	if (!parse_args(argc, argv, FLAG_XWAU_ROOT | FLAG_PROJECT, &a))
		return 2;
	if (!a.xwau_root || !a.project || !a.input_count) {
		Report(NULL, "effects needs --xwau-root, --project and at least one input");
		return 2;
	}
	return XwauLibrary_Rebuild(a.xwau_root, a.project, (const char* const*)a.inputs, (size_t)a.input_count)
			   ? 0
			   : 1;
}

/* opt2gltf missions --xwau-root DIR --stock-game DIR <output_dir> */
static int run_missions(int argc, char** argv) {
	Args a;
	if (!parse_args(argc, argv, FLAG_XWAU_ROOT | FLAG_STOCK_GAME, &a))
		return 2;
	if (!a.xwau_root || !a.stock_game || a.input_count != 1) {
		Report(NULL, "missions needs --xwau-root, --stock-game and an output folder");
		return 2;
	}
	XwauMissions* missions = XwauMissions_Load(a.xwau_root, true);
	if (!missions) {
		Report(NULL, "out of memory (missions)");
		return 1;
	}
	const bool ok = XwauMissionFiles_Write(a.xwau_root, missions, a.stock_game, a.inputs[0]);
	XwauMissions_Free(missions);
	return ok ? 0 : 1;
}

int main(int argc, char** argv) {
	if (argc > 1 && strcmp(argv[1], "effects") == 0)
		return run_effects(argc - 2, argv + 2);
	if (argc > 1 && strcmp(argv[1], "missions") == 0)
		return run_missions(argc - 2, argv + 2);
	return run_convert(argc - 1, argv + 1);
}
