/*
 * xwau_import writes the parts of an XWA Upgrade conversion that are not
 * models. opt2gltf converts the models and their effect libraries.
 *
 * Usage:
 *   xwau_import missions --xwau-root DIR --stock-game DIR --game-files DIR
 *                        --flight-settings FILE
 *                        [--skyboxes FILE --skybox-folders FILE] <output_dir>
 *   xwau_import craft --xwau-root DIR [--stock-game DIR [--strings FILE]]
 *                     [--game-files DIR] <output_dir>
 *
 * `missions` copies the files that come with the installation's missions into
 * the --game-files folder, the mod folder's game/ folder (xwau_game_files.h).
 * It then writes the selections of the installation's missions: each
 * mission gets a file <output_dir>/<dir>/<mission>.yaml
 * (xwau_mission_files.h). A mission without its own .TIE in the installation
 * uses the one in --stock-game, the stock game's folder. It writes the
 * settings that apply to every mission into the --flight-settings file,
 * flight/flight.yaml (xwau_flight_settings.h). With --skyboxes, it also
 * writes the environments of the missions' [SkyBoxes] sections into the
 * environment manifest FILE, flight/environments/environments.yaml, keeping
 * the file's other entries, and the sky and overlay folders that
 * environment_bake converts into the --skybox-folders file
 * (xwau_skyboxes.h).
 *
 * `craft` writes the settings of each model into <output_dir>/models and the
 * hull and map icon lists into <output_dir>/hud.yaml (xwau_craft.h). With
 * --stock-game, the stock game's folder, it also writes into
 * <output_dir>/object_types and <output_dir>/craft_types what the
 * installation changes about the stock craft and the craft it adds
 * (xwau_objects.h). With --strings, it writes into FILE the other texts that
 * the installation changes in the game's text files (xwau_text.h). With
 * --game-files, the mod folder's game/ folder, it writes the sound banks into
 * <output_dir>/sounds.yaml and copies their files into DIR
 * (xwau_sound_banks.h).
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "util.h"
#include "xwau_craft.h"
#include "xwau_flight_settings.h"
#include "xwau_game_files.h"
#include "xwau_gunner_seats.h"
#include "xwau_mission_files.h"
#include "xwau_missions.h"
#include "xwau_objects.h"
#include "xwau_skyboxes.h"
#include "xwau_sound_banks.h"
#include "xwau_text.h"

static int usage(void) {
	fprintf(stderr, "usage: xwau_import missions --xwau-root DIR --stock-game DIR --game-files DIR\n"
					"                            --flight-settings FILE\n"
					"                            [--skyboxes FILE --skybox-folders FILE] <output_dir>\n"
					"       xwau_import craft --xwau-root DIR [--stock-game DIR [--strings FILE]]\n"
					"                         [--game-files DIR] <output_dir>\n");
	return 2;
}

/* xwau_import missions --xwau-root DIR --stock-game DIR --game-files DIR
 *                      --flight-settings FILE [--skyboxes FILE --skybox-folders FILE] <output_dir> */
static int run_missions(int argc, char** argv) {
	const char* xwau_root      = NULL;
	const char* stock_game     = NULL;
	const char* skyboxes       = NULL;
	const char* skybox_folders = NULL;
	const char* game_files     = NULL;
	const char* flight         = NULL;
	int         ai             = 0;
	for (; ai < argc && strncmp(argv[ai], "--", 2) == 0; ai += 2) {
		const char** value = strcmp(argv[ai], "--xwau-root") == 0         ? &xwau_root
							 : strcmp(argv[ai], "--stock-game") == 0      ? &stock_game
							 : strcmp(argv[ai], "--skyboxes") == 0        ? &skyboxes
							 : strcmp(argv[ai], "--skybox-folders") == 0  ? &skybox_folders
							 : strcmp(argv[ai], "--game-files") == 0      ? &game_files
							 : strcmp(argv[ai], "--flight-settings") == 0 ? &flight
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
	if (!xwau_root || !stock_game || !game_files || !flight || argc - ai != 1) {
		Report(NULL,
			   "missions needs --xwau-root, --stock-game, --game-files, --flight-settings and an output "
			   "folder");
		return 2;
	}
	if (!skyboxes != !skybox_folders) {
		Report(NULL, "--skyboxes and --skybox-folders go together");
		return 2;
	}
	if (!XwauGameFiles_Copy(xwau_root, stock_game, game_files) ||
		!XwauFlightSettings_Write(xwau_root, flight))
		return 1;
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

/* xwau_import craft --xwau-root DIR [--stock-game DIR [--strings FILE]] [--game-files DIR]
 *                   <output_dir> */
static int run_craft(int argc, char** argv) {
	const char* xwau_root  = NULL;
	const char* stock_game = NULL;
	const char* strings    = NULL;
	const char* game_files = NULL;
	int         ai         = 0;
	for (; ai < argc && strncmp(argv[ai], "--", 2) == 0; ai += 2) {
		const char** value = strcmp(argv[ai], "--xwau-root") == 0    ? &xwau_root
							 : strcmp(argv[ai], "--stock-game") == 0 ? &stock_game
							 : strcmp(argv[ai], "--strings") == 0    ? &strings
							 : strcmp(argv[ai], "--game-files") == 0 ? &game_files
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
	if (strings && !stock_game) {
		Report(NULL, "--strings needs --stock-game");
		return 2;
	}
	XwauLines   cockpits = { 0 };
	bool        ok       = XwauCraft_Write(xwau_root, argv[ai], &cockpits);
	XwauTextIds moved    = { 0 };
	if (stock_game)
		ok = XwauObjects_Write(xwau_root, stock_game, argv[ai], &moved) &&
			 XwauGunnerSeats_WriteCockpits(xwau_root, stock_game, argv[ai], &cockpits) && ok;
	else if (cockpits.count)
		Report(NULL, "the gunner cockpits need --stock-game to get their object types");
	XwauLines_Free(&cockpits);
	if (strings)
		ok = XwauText_Write(xwau_root, stock_game, strings, &moved) && ok;
	if (game_files)
		ok = XwauSoundBanks_Write(xwau_root, argv[ai], game_files) && ok;
	XwauTextIds_Free(&moved);
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
