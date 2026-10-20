/*
 * xwau_import writes the parts of an XWA Upgrade conversion that are not
 * models. opt2gltf converts the models and their effect libraries.
 *
 * Usage:
 *   xwau_import missions --xwau-root DIR --stock-game DIR
 *                        [--skyboxes FILE --skybox-folders FILE] <output_dir>
 *   xwau_import craft --xwau-root DIR [--stock-game DIR --strings FILE] <output_dir>
 *
 * `missions` writes the selections of XWAU's missions for the missions of the
 * original game, installed in --stock-game. Each mission gets a file
 * <output_dir>/<dir>/<mission>.yaml (xwau_mission_files.h). With --skyboxes,
 * it also writes the environments of the missions' [SkyBoxes] sections into
 * FILE, and the sky and overlay folders that environment_bake converts into
 * the --skybox-folders file (xwau_skyboxes.h).
 *
 * `craft` writes the settings of each craft into <output_dir>/craft and the
 * hull and map icon lists into <output_dir>/hud.yaml (xwau_craft.h). With
 * --strings, it also writes into FILE the texts that the installation changes
 * in the game's text files. It compares them with the stock game, installed
 * in --stock-game (xwau_text.h).
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "util.h"
#include "xwau_craft.h"
#include "xwau_mission_files.h"
#include "xwau_missions.h"
#include "xwau_skyboxes.h"
#include "xwau_text.h"

static int usage(void) {
	fprintf(stderr,
			"usage: xwau_import missions --xwau-root DIR --stock-game DIR\n"
			"                            [--skyboxes FILE --skybox-folders FILE] <output_dir>\n"
			"       xwau_import craft --xwau-root DIR [--stock-game DIR --strings FILE] <output_dir>\n");
	return 2;
}

/* xwau_import missions --xwau-root DIR --stock-game DIR
 *                      [--skyboxes FILE --skybox-folders FILE] <output_dir> */
static int run_missions(int argc, char** argv) {
	const char* xwau_root      = NULL;
	const char* stock_game     = NULL;
	const char* skyboxes       = NULL;
	const char* skybox_folders = NULL;
	int         ai             = 0;
	for (; ai < argc && strncmp(argv[ai], "--", 2) == 0; ai += 2) {
		const char** value = strcmp(argv[ai], "--xwau-root") == 0        ? &xwau_root
							 : strcmp(argv[ai], "--stock-game") == 0     ? &stock_game
							 : strcmp(argv[ai], "--skyboxes") == 0       ? &skyboxes
							 : strcmp(argv[ai], "--skybox-folders") == 0 ? &skybox_folders
																		 : NULL;
		if (!value) {
			Report(NULL, "unknown option '%s'", argv[ai]);
			return 2;
		}
		if (ai + 1 == argc) {
			Report(NULL, "%s needs a value", argv[ai]);
			return 2;
		}
		if (*value) {
			Report(NULL, "%s can only be given once", argv[ai]);
			return 2;
		}
		*value = argv[ai + 1];
	}
	if (!xwau_root || !stock_game || argc - ai != 1) {
		Report(NULL, "missions needs --xwau-root, --stock-game and an output folder");
		return 2;
	}
	if (!skyboxes != !skybox_folders) {
		Report(NULL, "--skyboxes and --skybox-folders go together");
		return 2;
	}
	XwauMissions* missions = XwauMissions_Load(xwau_root, true);
	if (!missions) {
		Report(NULL, "out of memory (missions)");
		return 1;
	}
	bool ok = XwauMissionFiles_Write(xwau_root, missions, stock_game, argv[ai]);
	XwauMissions_Free(missions);
	if (ok && skyboxes)
		ok = XwauSkyboxes_Write(xwau_root, stock_game, skyboxes, skybox_folders);
	return ok ? 0 : 1;
}

/* xwau_import craft --xwau-root DIR [--stock-game DIR --strings FILE] <output_dir> */
static int run_craft(int argc, char** argv) {
	const char* xwau_root  = NULL;
	const char* stock_game = NULL;
	const char* strings    = NULL;
	int         ai         = 0;
	for (; ai < argc && strncmp(argv[ai], "--", 2) == 0; ai += 2) {
		const char** value = strcmp(argv[ai], "--xwau-root") == 0    ? &xwau_root
							 : strcmp(argv[ai], "--stock-game") == 0 ? &stock_game
							 : strcmp(argv[ai], "--strings") == 0    ? &strings
																	 : NULL;
		if (!value) {
			Report(NULL, "unknown option '%s'", argv[ai]);
			return 2;
		}
		if (ai + 1 == argc) {
			Report(NULL, "%s needs a value", argv[ai]);
			return 2;
		}
		if (*value) {
			Report(NULL, "%s can only be given once", argv[ai]);
			return 2;
		}
		*value = argv[ai + 1];
	}
	if (!xwau_root || argc - ai != 1) {
		Report(NULL, "craft needs --xwau-root and an output folder");
		return 2;
	}
	if (!stock_game != !strings) {
		Report(NULL, "--stock-game and --strings go together");
		return 2;
	}
	bool ok = XwauCraft_Write(xwau_root, argv[ai]);
	if (strings)
		ok = XwauText_Write(xwau_root, stock_game, strings) && ok;
	return ok ? 0 : 1;
}

int main(int argc, char** argv) {
	SetReportProgram("xwau_import");
	if (argc > 1 && strcmp(argv[1], "missions") == 0)
		return run_missions(argc - 2, argv + 2);
	if (argc > 1 && strcmp(argv[1], "craft") == 0)
		return run_craft(argc - 2, argv + 2);
	return usage();
}
