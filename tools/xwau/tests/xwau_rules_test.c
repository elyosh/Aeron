/*
 * Tests the hook file rules that the XWAU readers reproduce (xwau_config.h),
 * the skin selection (xwau_skins.h), the skybox conversion (xwau_skyboxes.h),
 * the model settings (xwau_craft.h), the text overrides (xwau_text.h), the
 * object type and craft type files (xwau_objects.h), the files copied into
 * the mod folder's game/ folder (xwau_game_files.h), the mission files
 * (xwau_mission_files.h), the flight settings (xwau_flight_settings.h), the
 * [mission_tie] lines (xwau_mission_tie.h) and the model files
 * (xwau_model.h). The inputs are fixed texts and folders written to a
 * scratch folder. The test is built with the tools as xwau_rules_test. It
 * prints each check and exits with a non-zero status when a check fails.
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opt.h"
#include "util.h"
#include "xwau_config.h"
#include "xwau_craft.h"
#include "xwau_flight_settings.h"
#include "xwau_game_files.h"
#include "xwau_gunner_seats.h"
#include "xwau_mission_files.h"
#include "xwau_mission_sounds.h"
#include "xwau_mission_tie.h"
#include "xwau_missions.h"
#include "xwau_model.h"
#include "xwau_objects.h"
#include "xwau_skins.h"
#include "xwau_skyboxes.h"
#include "xwau_sound_banks.h"
#include "xwau_stats_profiles.h"
#include "xwau_text.h"
#include "xwau_warhead_pools.h"
#include "xwau_weapon_stats.h"

static int g_failures;

static void check(int ok, const char* what) {
	printf("%s %s\n", ok ? "ok" : "FAILED", what);
	g_failures += !ok;
}

static void write_file(const char* path, const char* text) {
	FILE* f = fopen(path, "wb");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

/* ---- Rules ------------------------------------------------------------------ */

static void test_rules(const char* scratch) {
	long long v = 0;
	check(Xwau_Stoi(" -12x", &v) && v == -12, "stoi: sign and digits up to another character");
	check(!Xwau_Stoi("x1", &v) && !Xwau_Stoi("99999999999", &v), "stoi: no digits, or out of range");
	check(Xwau_StoiAuto("0x1F", &v) && v == 31 && Xwau_StoiAuto("010", &v) && v == 8 &&
			  Xwau_StoiAuto("08", &v) && v == 0,
		  "stoi(text, 0, 0): hexadecimal, octal");

	XwauLines tokens;
	Xwau_Tokenize(" 1, ;2 ;,,3", XWAU_RULES_CPP, &tokens);
	check(tokens.count == 4 && strcmp(tokens.items[1], "") == 0 && strcmp(tokens.items[2], "2") == 0 &&
			  strcmp(tokens.items[3], "3") == 0,
		  "C++ tokens: a token of spaces is kept as an empty one, and empty text between separators is "
		  "skipped");
	XwauLines_Free(&tokens);
	Xwau_Tokenize("Ace;", XWAU_RULES_CS, &tokens);
	check(tokens.count == 2 && tokens.items[1][0] == 0, "C# tokens: empty ones kept");
	XwauLines_Free(&tokens);

	char encoded[64];
	check(Xwau_EncodeName("Con-L+K", encoded, sizeof encoded) && strcmp(encoded, "con.2dl.2bk") == 0,
		  "names encoded as tags");

	char path[1024], value[64];
	snprintf(path, sizeof path, "%s/rules.ini", scratch);
	write_file(path, "; comment\n[CockpitPov;]\nCockpitPovX = 1\n[cockpitpov]\n  CockpitPovX=2  \n"
					 "cockpitpovx = 3\n[Other]\nCockpitPovX = 4\n");
	XwauLines lines;
	XwauLines_Read(path, "CockpitPov", XWAU_RULES_CPP, &lines);
	XwauLines_Value(&lines, "COCKPITPOVX", XWAU_RULES_CPP, value, sizeof value);
	check(lines.count == 2 && strcmp(value, "2") == 0,
		  "section names match whole and in any case ([CockpitPov;] differs), and the first key wins");
	XwauLines_Free(&lines);

	snprintf(path, sizeof path, "%s/cs.txt", scratch);
	write_file(path, "\xa0Opt\x85=\xa0"
					 "Ace\xa0\rB = 1\r\nC = 2");
	XwauLines_Read(path, "", XWAU_RULES_CS, &lines);
	XwauLines_Value(&lines, "opt", XWAU_RULES_CS, value, sizeof value);
	check(lines.count == 3 && strcmp(value, "Ace") == 0, "C#: U+0085 and U+00A0 trimmed, lines end at CR");
	XwauLines_Free(&lines);
	XwauLines_Read(path, "", XWAU_RULES_CPP, &lines);
	check(lines.count == 2, "C++: lines end at LF only");
	XwauLines_Free(&lines);
}

/* ---- Skins ------------------------------------------------------------------ */

/* A test case: the global skin lines, the OPT's skins, its state count and
 * the expected default markings. In `skins`, "" puts a file directly in the
 * skin folder, and a NULL first entry means no skin folder. In `expected`,
 * NULL means unskinned, and nothing is expected when there is no variant. */
typedef struct Case {
	const char*       what;
	const char*       lines;
	const char*       model;
	const char* const skins[8];
	int               states;
	int               markings; /* expected count, -1 for no variant */
	const char* const expected[8];
} Case;

#define DEFAULTS { "", "default_0", "default_1", "default_2", "default_3", "ace", "jake", NULL }

static const Case kCases[] = {
	{ "Default_i fallback",
	  "",
	  "Awing",
	  DEFAULTS,
	  1,
	  4,
	  { "0|default_0", "0|default_1", "0|default_2", "0|default_3" } },
	{ "Default when Default_i is missing",
	  "",
	  "Opt",
	  { "default", "default_0" },
	  2,
	  2,
	  { "0|default_0", "1|default" } },
	{ "base + fgc",
	  "Awing = Ace\nAwing_fgc_1 = Jake\n",
	  "Awing",
	  DEFAULTS,
	  1,
	  4,
	  { "0|ace", "0|ace,jake", "0|ace", "0|ace" } },
	{ "fgc alone",
	  "Awing_fgc_0 = Ace\n",
	  "Awing",
	  DEFAULTS,
	  1,
	  4,
	  { "0|ace", "0|default_1", "0|default_2", "0|default_3" } },
	{ "count from fgc",
	  "Opt_fgc_5 = Ace\n",
	  "Opt",
	  { "ace" },
	  1,
	  6,
	  { NULL, NULL, NULL, NULL, NULL, "0|ace" } },
	{ "count from states", "Opt = Ace\n", "Opt", { "ace" }, 3, 3, { "0|ace", "1|ace", "2|ace" } },
	{ "count from Default_N",
	  "",
	  "Opt",
	  { "default_6" },
	  1,
	  7,
	  { NULL, NULL, NULL, NULL, NULL, NULL, "0|default_6" } },
	{ "only fgc lines skin", "Opt_fgc_1 = Ace\n", "Opt", { "ace" }, 1, 2, { NULL, "0|ace" } },
	{ "no skin folder", "Opt = Ace\n", "Opt", { NULL }, 1, -1, { NULL } },
	{ "no skin exists", "Opt = Ace\n", "Opt", { "readme" }, 1, -1, { NULL } },
	{ "nothing selects a skin", "", "Opt", { "ace" }, 1, -1, { NULL } },
	{ "missing skins dropped", "Opt = Ace, Gone\n", "Opt", { "ace" }, 1, 1, { "0|ace" } },
	{ "an empty list is unskinned", "Opt = Gone\n", "Opt", { "ace" }, 1, -1, { NULL } },
	{ "an empty token keeps the list", "Opt_fgc_0 = Gone;\n", "Opt", { "default_0" }, 1, -1, { NULL } },
	{ "opacity",
	  "Opt = Ace-50, Jake-150, Ace-x, Jake-100, Ace-+7, Jake- 20\n",
	  "Opt",
	  { "ace", "jake" },
	  1,
	  1,
	  { "0|ace-50,jake,ace,jake,ace-7,jake-20" } },
	{ "opacity overflow", "Opt = Ace-99999999999\n", "Opt", { "ace" }, 1, 1, { "0|ace" } },
	{ "case-insensitive keys",
	  "XWING = Ace\nxwing_FGC_1 = Jake\n",
	  "Xwing",
	  DEFAULTS,
	  1,
	  4,
	  { "0|ace", "0|ace,jake", "0|ace", "0|ace" } },
	{ "the first key wins", "Opt = Ace\nOPT = Jake\n", "Opt", { "ace", "jake" }, 1, 1, { "0|ace" } },
	{ "state", "Opt_fgc_3 = Ace\n", "Opt", { "ace" }, 2, 4, { NULL, NULL, NULL, "0|ace" } },
};

static void test_skins(const char* scratch) {
	for (size_t c = 0; c < sizeof kCases / sizeof kCases[0]; ++c) {
		const Case* k = &kCases[c];
		char        root[1024], dir[1100], path[1300];
		snprintf(root, sizeof root, "%s/skins%zu", scratch, c);
		snprintf(dir, sizeof dir, "%s/FlightModels", root);
		MakeDirs(dir);
		snprintf(path, sizeof path, "%s/Skins.txt", dir);
		write_file(path, k->lines);
		for (size_t s = 0; s < 8 && k->skins[s]; ++s) {
			if (!k->skins[s][0]) {
				snprintf(path, sizeof path, "%s/Skins/%s", dir, k->model);
				MakeDirs(path);
				snprintf(path, sizeof path, "%s/Skins/%s/readme.txt", dir, k->model);
			} else if (strcmp(k->skins[s], "readme") == 0) {
				snprintf(path, sizeof path, "%s/Skins/%s/empty", dir, k->model);
				MakeDirs(path);
				continue;
			} else {
				snprintf(path, sizeof path, "%s/Skins/%s/%s", dir, k->model, k->skins[s]);
				MakeDirs(path);
				snprintf(path, sizeof path, "%s/Skins/%s/%s/a.txt", dir, k->model, k->skins[s]);
			}
			write_file(path, "x");
		}
		XwauMissions* missions = XwauMissions_Load(root, false);
		/* An OPT with one face group that has `states` states and no texture. */
		opt_face_group_t group = { 0 };
		group.state_count      = k->states;
		opt_lod_t  lod         = { 0 };
		opt_mesh_t mesh        = { 0 };
		lod.group_count        = 1;
		lod.groups             = &group;
		mesh.lod_count         = 1;
		mesh.lods              = &lod;
		opt_file_t opt         = { 0 };
		opt.mesh_count         = 1;
		opt.meshes             = &mesh;
		XwauSkins skins;
		int       ok = missions && XwauSkins_Resolve(root, missions, k->model, &opt, "test", &skins);
		if (ok && k->markings < 0) {
			ok = skins.skins.variant_count == 0;
		} else if (ok) {
			/* Compare the default markings. Unskinned values are NULL. */
			ok = skins.skins.markings_count == (size_t)k->markings;
			for (int i = 0; ok && i < k->markings; ++i) {
				const char* got = skins.markings[i];
				ok = got && k->expected[i] ? strcmp(got, k->expected[i]) == 0 : !got && !k->expected[i];
			}
		}
		check(ok, k->what);
		XwauSkins_Free(&skins);
		XwauMissions_Free(missions);
	}
}

/* ---- Skyboxes ---------------------------------------------------------------- */

static void mat_mul(const double a[9], const double b[9], double out[9]) {
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

static void rotation(int axis, double degrees, double out[9]) {
	const double a = degrees * 3.14159265358979323846 / 180.0, c = cos(a), s = sin(a);
	const double m[3][9] = { { 1, 0, 0, 0, c, -s, 0, s, c },
							 { c, 0, s, 0, 1, 0, -s, 0, c },
							 { c, -s, 0, s, c, 0, 0, 0, 1 } };
	memcpy(out, m[axis], sizeof m[axis]);
}

/* The converted [yaw, pitch, roll] must give XWAU's M Rot^T M as
 * Ry(yaw) Rx(pitch) Rz(roll), with Rot = Rz Ry Rx for a sky and Rx Ry Rz for
 * an overlay (xwau_skyboxes_plan.md, 4.2). */
static void test_skybox_rotation(void) {
	uint32_t seed  = 7;
	double   worst = 0.0;
	for (int i = 0; i < 2000; ++i) {
		float xyz[3];
		for (int k = 0; k < 3; ++k) {
			seed   = seed * 1664525u + 1013904223u;
			xyz[k] = (float)(seed >> 8) / (float)(1u << 24) * 720.0f - 360.0f;
		}
		if (i < 4) /* pitch +-90 */
			xyz[0] = i < 2 ? 90.0f : -90.0f;
		const bool overlay = i % 2 != 0;
		double     rx[9], ry[9], rz[9], t[9], rot[9];
		rotation(0, xyz[0], rx);
		rotation(1, xyz[1], ry);
		rotation(2, xyz[2], rz);
		mat_mul(overlay ? rx : rz, ry, t);
		mat_mul(t, overlay ? rz : rx, rot);
		const double m[3] = { 1.0, 1.0, -1.0 };
		float        ypr[3];
		XwauSkyboxes_Rotation(xyz, overlay, ypr);
		double y[9], x[9], z[9], q[9];
		rotation(1, ypr[0], y);
		rotation(0, ypr[1], x);
		rotation(2, ypr[2], z);
		mat_mul(y, x, t);
		mat_mul(t, z, q);
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				worst = fmax(worst, fabs(q[r * 3 + c] - m[r] * m[c] * rot[c * 3 + r]));
	}
	printf("skybox rotation: largest difference %.3g\n", worst);
	check(worst < 1e-4, "skybox rotations give XWAU's cube orientation");
}

/* Whether `text` occurs in the mission entry that starts at `entry` and ends
 * at `next`. */
static bool in_entry(const char* entry, const char* next, const char* text) {
	const char* found = strstr(entry, text);
	return found && found < next;
}

static void make_cube_folder(const char* root, const char* name, int files) {
	char dir[1200], path[1400];
	snprintf(dir, sizeof dir, "%s/Effects/CubeMaps/%s", root, name);
	MakeDirs(dir);
	for (int f = 0; f < files; ++f) {
		snprintf(path, sizeof path, "%s/%s.%d.jpg", dir, name, f);
		write_file(path, "");
	}
}

/* Whether the environment `name` of `manifest` holds `text`. */
static bool definition_has(const char* manifest, const char* name, const char* text) {
	char header[128];
	snprintf(header, sizeof header, "\n  %s:\n", name);
	const char* entry = strstr(manifest, header);
	if (!entry)
		return false;
	const char* next = strstr(entry + strlen(header), "\n  xwau.");
	const char* end  = strstr(entry, "\nxwa:");
	return in_entry(entry, next && (!end || next < end) ? next : end, text);
}

/* The overlay lines of a definition whose overlay has XWAU angles `xyz`. */
static void overlay_lines(const char* folder, const float xyz[3], char* out, size_t size) {
	int written = snprintf(
		out, size, "    overlay:\n      cube: \"flight/environments/xwau/%s/overlay.ktx2\"\n", folder);
	if (xyz[0] != 0.0f || xyz[1] != 0.0f || xyz[2] != 0.0f) {
		float ypr[3];
		XwauSkyboxes_Rotation(xyz, true, ypr);
		snprintf(out + written, size - (size_t)written, "      rotation_degrees: [%g, %g, %g]\n",
				 (double)ypr[0], (double)ypr[1], (double)ypr[2]);
	}
}

static void test_skyboxes(const char* scratch) {
	char game[1024], stock[1024], dir[1200], path[1300];
	snprintf(game, sizeof game, "%s/sky_game", scratch);
	snprintf(stock, sizeof stock, "%s/sky_stock", scratch);
	make_cube_folder(game, "Alpha", 6);
	make_cube_folder(game, "Beta", 6);
	make_cube_folder(game, "Gamma", 5); /* does not load */
	make_cube_folder(game, "Delta", 6);
	make_cube_folder(game, "Delta_overlay", 6);
	make_cube_folder(game, "Rocks", 6);
	snprintf(dir, sizeof dir, "%s/Missions", game);
	MakeDirs(dir);
	snprintf(dir, sizeof dir, "%s/Missions", stock);
	MakeDirs(dir);
	static const char* const kMissions[][2] = {
		{ "m1",
		  "[SkyBoxes]\nDisabledBackdrops = 9001-1100;All;6025-1\nAllRegions = Effects\\CubeMaps\\Alpha\n"
		  "AllRegionsAmbientInt = 0.3abc\nAllRegionsRotationY = 5\nRegion0 = Effects\\CubeMaps\\Beta\n"
		  "Region0AmbientMin = 0.05\nRegion1 = Effects\\CubeMaps\\Gamma\nAllRegionsTypo = 1\n" },
		{ "m2", "[SkyBoxes]\nAllRegions = Effects\\CubeMaps\\Gamma\n" },
		{ "m3", "[SkyBoxes]\nEnabledBackdrops = all\nAllRegions = Effects\\CubeMaps\\Alpha\n"
				"AllRegionsAmbientInt = 0.3\n" },
		/* Regions 0 and 3 use AllRegions and its default overlay Delta_overlay;
		 * region 3's own overlay is not tried without Region3. Region 1 has
		 * its own overlay, region 2 the AllRegions one with its own angles. */
		{ "m4", "[SkyBoxes]\nAllRegions = Effects\\CubeMaps\\Delta\nAllRegionsRotationX = 10\n"
				"Region1 = Effects\\CubeMaps\\Beta\nRegion1Overlay = Effects\\CubeMaps\\Rocks\n"
				"Region1RotationZ = 20\nRegion1OverlayRotationY = 30\nRegion2 = Effects\\CubeMaps\\Beta\n"
				"Region3Overlay = Effects\\CubeMaps\\Rocks\n" },
		{ "m5",
		  "[SkyBoxes]\nRegion0 = Effects\\CubeMaps\\Gamma\nRegion0Overlay = Effects\\CubeMaps\\Rocks\n" },
	};
	for (size_t m = 0; m < sizeof kMissions / sizeof kMissions[0]; ++m) {
		snprintf(path, sizeof path, "%s/Missions/%s.ini", game, kMissions[m][0]);
		write_file(path, kMissions[m][1]);
		snprintf(path, sizeof path, "%s/Missions/%s.tie", stock, kMissions[m][0]);
		write_file(path, "");
	}
	/* m6 is only in the mod, and m7 is in neither. */
	snprintf(path, sizeof path, "%s/Missions/m6.ini", game);
	write_file(path, "[SkyBoxes]\nAllRegions = Effects\\CubeMaps\\Alpha\nAllRegionsAmbientInt = 0.3\n");
	snprintf(path, sizeof path, "%s/Missions/m6.tie", game);
	write_file(path, "");
	snprintf(path, sizeof path, "%s/Missions/m7.ini", game);
	write_file(path, "[SkyBoxes]\nAllRegions = Effects\\CubeMaps\\Alpha\n");
	char manifest[1200], folders[1200];
	snprintf(manifest, sizeof manifest, "%s/sky.yaml", scratch);
	snprintf(folders, sizeof folders, "%s/sky_folders.txt", scratch);
	check(XwauSkyboxes_Write(game, stock, manifest, folders), "skyboxes written");
	size_t      size = 0;
	char*       text = ReadWholeFile(manifest, &size);
	char*       list = ReadWholeFile(folders, &size);
	const char* m1   = text ? strstr(text, "\"missions/m1.tie\"") : NULL;
	const char* m2   = text ? strstr(text, "\"missions/m2.tie\"") : NULL;
	const char* m3   = text ? strstr(text, "\"missions/m3.tie\"") : NULL;
	const char* m4   = text ? strstr(text, "\"missions/m4.tie\"") : NULL;
	const char* m5   = text ? strstr(text, "\"missions/m5.tie\"") : NULL;
	check(m1 && m2 && m3 && m4 && m5, "skyboxes: one entry per mission");
	check(text && strstr(text, "\"missions/m6.tie\"") && !strstr(text, "\"missions/m7.tie\""),
		  "skyboxes: a mission that only the mod has is kept, one that neither has is left out");
	if (m4 && m5) {
		char               lines[256];
		static const float kAll[3] = { 10, 0, 0 }, kRegion1[3] = { 0, 30, 20 }, kNone[3] = { 0, 0, 0 };
		overlay_lines("delta_overlay", kAll, lines, sizeof lines);
		check(definition_has(text, "xwau.delta.delta_overlay", lines) &&
				  strstr(m4, "\"0\":\n          environment: xwau.delta.delta_overlay\n") &&
				  strstr(m4, "\"3\":\n          environment: xwau.delta.delta_overlay\n"),
			  "skyboxes: AllRegions has its default overlay, with the AllRegions angles");
		overlay_lines("rocks", kRegion1, lines, sizeof lines);
		check(definition_has(text, "xwau.beta.rocks", lines) &&
				  strstr(m4, "\"1\":\n          environment: xwau.beta.rocks\n"),
			  "skyboxes: a region's own overlay, with angles defaulting to the region's");
		overlay_lines("delta_overlay", kNone, lines, sizeof lines);
		check(definition_has(text, "xwau.beta.delta_overlay", lines) &&
				  !definition_has(text, "xwau.beta.delta_overlay", "      rotation_degrees") &&
				  strstr(m4, "\"2\":\n          environment: xwau.beta.delta_overlay\n") &&
				  !definition_has(text, "xwau.beta", "overlay:"),
			  "skyboxes: a region with its own cube takes the AllRegions overlay with its own angles");
		check(strstr(m5, "\"0\": none\n") != NULL, "skyboxes: an overlay without a cube is left out");
	}
	if (m1 && m2 && m3 && m4) {
		check(strstr(text, "  xwau.beta:\n") && strstr(text, "diffuse_strength: 0.15\n") &&
				  strstr(text, "ambient_fill: [0.05, 0.05, 0.05]"),
			  "skyboxes: a region with its own folder takes the region defaults");
		check(strstr(text, "  xwau.alpha:\n") && strstr(text, "diffuse_strength: 0.3\n") &&
				  !strstr(text, "xwau.alpha.2"),
			  "skyboxes: AllRegions values read as stof reads them, one environment for equal values");
		check(
			strstr(m1, "\"1\":\n          environment: xwau.alpha\n          rotation_degrees: [5, 0, 0]") &&
				strstr(m1, "\"0\":\n          environment: xwau.beta\n          backdrops: keep\n"
						   "          backdrop_light_scale: 0.25\n"),
			"skyboxes: a region whose folder does not load uses AllRegions, with its rotation");
		check(strstr(m1, "\"6104-0\"") && strstr(m1, "\"9001-1100\"") && strstr(m1, "\"6025-1\"") &&
				  !in_entry(m1, m2, "show_backdrops"),
			  "skyboxes: hidden images are the starfield ones and DisabledBackdrops, 'All' is ignored");
		check(strstr(m2, "\"0\": none\n        \"1\": none") != NULL, "skyboxes: no folder loads, no sky");
		check(in_entry(m3, m4, "show_backdrops: all") && !in_entry(m3, m4, "hide_backdrops"),
			  "skyboxes: EnabledBackdrops = all turns hiding off");
	}
	check(list && strstr(list, "/Effects/CubeMaps/Alpha\txwau.alpha\tflight/environments/xwau/alpha\n") &&
			  strncmp(list, "sky\t", 4) == 0 && strstr(list, "\nsky\t") && strstr(list, "\txwau.beta\t") &&
			  !strstr(list, "gamma") && strstr(list, "\noverlay\t") &&
			  strstr(list, "/Effects/CubeMaps/Rocks\t-\tflight/environments/xwau/rocks\n") &&
			  !strstr(list, "\txwau.rocks"),
		  "skyboxes: the folder list names the sky and overlay folders that load");
	free(text);
	free(list);

	/* An existing manifest keeps its other entries and its comments, and the
	 * importer's block between its two lines is replaced. */
	write_file(
		manifest,
		"# My manifest\nversion: 1\n\nenvironments:\n  deathstar.interior:\n    intensity: 1\n"
		"  # Begin of the entries that xwau_import missions --skyboxes writes. Each import replaces them.\n"
		"  xwau.old:\n"
		"    intensity: 2\n  # End of the entries that xwau_import missions --skyboxes writes.\n\n"
		"xwa:\n  default: deathstar.interior\n  missions:\n    \"skirmish/temp.tie\":\n"
		"      regions:\n        \"0\": deathstar.interior\n");
	check(XwauSkyboxes_Write(game, stock, manifest, folders), "skyboxes written into an existing manifest");
	text = ReadWholeFile(manifest, &size);
	check(text &&
			  strncmp(text, "# My manifest\nversion: 1\n\nenvironments:\n  deathstar.interior:\n", 62) == 0 &&
			  strstr(text, "xwa:\n  default: deathstar.interior\n  missions:\n    \"skirmish/temp.tie\":\n"),
		  "skyboxes: the manifest's other entries and comments are kept");
	check(text && !strstr(text, "xwau.old") && strstr(text, "  xwau.alpha:\n") &&
			  strstr(text, "    # Begin of the entries") && strstr(text, "    # End of the entries"),
		  "skyboxes: the importer's block is replaced, and the mission map gets one");
	char* first = text;
	text        = NULL;
	check(XwauSkyboxes_Write(game, stock, manifest, folders), "skyboxes written again");
	text = ReadWholeFile(manifest, &size);
	check(first && text && strcmp(first, text) == 0, "skyboxes: a second import changes nothing");
	free(first);
	free(text);
}

/* ---- Craft settings ----------------------------------------------------------- */

static void test_craft(const char* scratch) {
	check(XwauCraft_HudColor(0.1f, 0.1f, 0.5f) == 0xff19197fu &&
			  XwauCraft_HudColor(1.0f, 0.0f, 0.36f) == 0xffff005bu,
		  "HUD colours: 0.1 becomes 0x19, and the products are truncated");

	char         path[1024], txt[1024];
	XwauCraftHud hud;
	snprintf(path, sizeof path, "%s/colors.ini", scratch);
	write_file(path, "[HUDColor]\n;HUDInnerColor = 0.1, 0.1, 0.1\nHUDBorderColor = 0.2, 0.2\n"
					 "[hudcolor]\nHUDInnerColor = 0.3, 0.3, 0.3\n");
	memset(&hud, 0, sizeof hud);
	XwauCraft_ReadHudColors(path, &hud);
	check(!hud.has_inner && !hud.has_border,
		  "HUD colours: commented lines, two numbers and a lower-case [hudcolor] set nothing");
	write_file(path, "[Size]\n[HUDColor]  ; colours\nHUDInnerColor = 0.1, 0.1, 0.1\n"
					 "hudinnercolor = 1, 1, 1\nHUDBorderColor=0.5, 0.5, 0.5\n");
	memset(&hud, 0, sizeof hud);
	XwauCraft_ReadHudColors(path, &hud);
	check(hud.has_inner && hud.inner == 0xffffffffu && !hud.has_border,
		  "HUD colours: the last value wins, keys in any case, and key=value without spaces is ignored");

	snprintf(path, sizeof path, "%s/Model.ini", scratch);
	snprintf(txt, sizeof txt, "%s/ModelReticle.txt", scratch);
	write_file(path, "[Reticle]\nReticle_5 = 9\nReticle_6 = 136 ;lock\nReticle_7 = -1\nReticle_8 = x\n");
	memset(&hud, 0, sizeof hud);
	XwauCraft_ReadReticle(NULL, path, &hud);
	check(hud.has_reticle[5] && hud.reticle[5] == 9 && hud.has_reticle[6] && hud.reticle[6] == 136 &&
			  !hud.has_reticle[7] && !hud.has_reticle[8] && !hud.has_reticle[9],
		  "reticle: a comment after the value, -1 and a value that isn't a number");
	write_file(txt, "; comment\nReticle_5 = 7\n");
	memset(&hud, 0, sizeof hud);
	XwauCraft_ReadReticle(txt, path, &hud);
	check(hud.has_reticle[5] && hud.reticle[5] == 7 && !hud.has_reticle[6],
		  "reticle: <model>Reticle.txt is read instead of the [Reticle] section");

	XwauCraftEngineGlows glows;
	write_file(path, "[EngineGlows]\n0, 1, -1\n1;100;-1\n2, 5\n0, 50, -1\n3, -1, 40\n4, x, 1\n"
					 "300, 1, 1\n5, 0, 0\n0x6, 1, 1\n");
	memset(&glows, 0, sizeof glows);
	XwauCraft_ReadEngineGlows(NULL, path, &glows);
	check(glows.has[0] && glows.min_percent[0] == 1 && glows.min_mglt[0] == 0 && glows.min_percent[1] == 100,
		  "engine glows: ',' and ';' separate the values, -1 is no limit, and the first line of a glow wins");
	check(!glows.has[2] && glows.min_mglt[3] == 40 && glows.min_percent[3] == 0 && !glows.has[4],
		  "engine glows: a short line and a value that isn't a number are skipped");
	check(glows.has[5] && glows.min_percent[5] == 0 && glows.min_mglt[5] == 0 && glows.min_percent[6] == 1 &&
			  glows.min_mglt[6] == 1,
		  "engine glows: 0 is no limit, and a hexadecimal glow is read");
	snprintf(txt, sizeof txt, "%s/ModelEngineGlows.txt", scratch);
	write_file(txt, "; comment\n2, 1, -1\n");
	memset(&glows, 0, sizeof glows);
	XwauCraft_ReadEngineGlows(txt, path, &glows);
	check(glows.has[2] && !glows.has[0],
		  "engine glows: <model>EngineGlows.txt is read instead of the section");

	uint32_t hex = 0;
	check(Xwau_StoulHex(" 0x1F;x", &hex) && hex == 0x1f && Xwau_StoulHex("-1", &hex) && hex == 0xffffffffu &&
			  !Xwau_StoulHex("1FFFFFFFF", &hex) && !Xwau_StoulHex("z", &hex),
		  "colours: std::stoul in base 16, with 0x, a sign and 32 bits");
	check(XwauCraft_ImpactColor(0x80ff0000u) == 0xff0000 && XwauCraft_ImpactColor(0xff040204u) == 0 &&
			  XwauCraft_ImpactColor(0xff050000u) == 0x050000,
		  "impact colours: the alpha is dropped, and 0 when the RGB565 value is 0");
	XwauCraftWeapons* weapons = (XwauCraftWeapons*)calloc(1, sizeof *weapons);
	write_file(path, "[WeaponColor]\n//FLIGHTGROUP 0 = RED\nWeaponColor = 0\t\t;0 = Default\n"
					 "WeaponColor282 = 1\nweaponcolor282 = 5\nWeaponColor284 = -1\nWeaponColor285 = 0x2\n"
					 "WeaponImpactColor_3100 = BF5FD6FF\t\t;Light Blue\nWeaponImpactColor_3200 = 0\n"
					 "WeaponImpactColor280 = 0x80FF0000\nWeaponImpactColor281 = 0\n"
					 "WeaponImpactColor283 = 00000003\nEnergyBarColorLaserHigh = FFFF0000\n"
					 "EnergyBarColorIonLow = zz\nWeaponColor_fg1_ = 3\nWeaponColor_fg1_284 = 1\n"
					 "WeaponImpactColor_3300_fg1_ = 0\nWeaponImpactColor_fg1_280 = BFFFE700\n"
					 "EnergyBarColorLaserHigh_fg1_ = FFFFFF00\nWeaponLightColor280 = FF0000\n"
					 "WeaponLightColor_fg2_280 = FFE700\nWeaponColor_fg07_ = 4\n");
	XwauCraft_ReadWeapons(NULL, path, weapons);
	const XwauCraftWeaponColors* colors = &weapons->colors;
	check(colors->has_default && colors->default_variant == 0 && colors->variant[2] == 1 &&
			  !colors->has_variant[4] && colors->variant[5] == 2,
		  "weapon colours: WeaponColor = 0 is a value, the first key wins, and -1 isn't set");
	check(colors->impact[4] == 0x5fd6ff && colors->impact[16] == 0x5fd6ff && colors->impact[0] == 0xff0000 &&
			  colors->has_impact[1] && colors->impact[1] == 0 && colors->has_impact[3] &&
			  colors->impact[3] == 0 && !colors->has_impact[2],
		  "weapon colours: the groups are expanded, a group set to 0 is the stock mark, and a weapon's own "
		  "colour wins");
	check(colors->has_bar[XWAU_CRAFT_LASER_HIGH] && colors->bar[XWAU_CRAFT_LASER_HIGH] == 0xffff0000u &&
			  !colors->has_bar[XWAU_CRAFT_ION_LOW],
		  "weapon colours: the energy bars, and a value that isn't a number");
	const XwauCraftWeaponColors* fg1 = &weapons->by_markings[1];
	check(weapons->has_markings[1] && fg1->default_variant == 3 && fg1->variant[4] == 1 &&
			  fg1->impact[0] == 0xffe700 && !fg1->has_impact[2] &&
			  fg1->bar[XWAU_CRAFT_LASER_HIGH] == 0xffffff00u,
		  "weapon colours: the _fg1_ keys, where a group set to 0 isn't set");
	check(!weapons->has_markings[2] && !weapons->has_markings[7] && weapons->light_colors,
		  "weapon colours: markings with only light colours, _fg07_, and the WeaponLightColor keys");
	check(weapons->switch_by_iff == -1, "weapon switch: not set without WeaponSwitchBasedOnIff");
	write_file(path, "[WeaponColor]\nWeaponSwitchBasedOnIff = 2\t;1 = Based on IFF color\n");
	memset(weapons, 0, sizeof *weapons);
	XwauCraft_ReadWeapons(NULL, path, weapons);
	check(weapons->switch_by_iff == 1, "weapon switch: a value other than 0 and -1 turns it on");
	write_file(path, "[WeaponColor]\nWeaponSwitchBasedOnIff = -1\n");
	memset(weapons, 0, sizeof *weapons);
	XwauCraft_ReadWeapons(NULL, path, weapons);
	check(weapons->switch_by_iff == -1, "weapon switch: -1 is the global value");
	free(weapons);

	uint16_t* list  = NULL;
	size_t    count = 0;
	snprintf(path, sizeof path, "%s/IconList.txt", scratch);
	write_file(path, "0\n; comment\n\n100\t\t;X-wing\n  200\n");
	check(XwauCraft_ReadIconList(path, &list, &count) && count == 3 && list[1] == 100 && list[2] == 200,
		  "icon lists: comment and empty lines aren't counted");
	free(list);

	char root[1024], out[1024], models[1100];
	snprintf(root, sizeof root, "%s/xwau", scratch);
	snprintf(models, sizeof models, "%s/FlightModels", root);
	snprintf(out, sizeof out, "%s/out", scratch);
	MakeDirs(models);
	snprintf(path, sizeof path, "%s/Awing.ini", models);
	write_file(path, "[Reticle]\nReticle_5 = 135\n[HUDColor]\nHUDInnerColor = 0.1, 0.1, 0.5\n");
	snprintf(path, sizeof path, "%s/Empty.ini", models);
	write_file(path, "[Size]\nX = 1\n");
	snprintf(path, sizeof path, "%s/Isd.ini", models);
	write_file(path, "[EngineGlows]\n0, 1, -1\n6, 100, 30\n7, -1, -1\n");
	snprintf(path, sizeof path, "%s/Slave.ini", models);
	write_file(path, "[WeaponColor]\nWeaponColor = 0\nWeaponColor282 = 1\nWeaponImpactColor280 = FF0000\n"
					 "WeaponColor_fg1_ = 3\nEnergyBarColorIonHigh_fg1_ = FF0000FF\n");
	snprintf(path, sizeof path, "%s/Corvette.ini", models);
	write_file(path, "[WeaponColor]\nWeaponSwitchBasedOnIff = 0\n");
	snprintf(path, sizeof path, "%s/NoIniEngineGlows.txt", models);
	write_file(path, "1, -1, 20\n");
	snprintf(path, sizeof path, "%s/HullIconList.txt", root);
	write_file(path, "0\n100\n");
	check(XwauCraft_Write(root, out, NULL), "craft: the files are written");
	size_t size = 0;
	snprintf(path, sizeof path, "%s/models/awing.yaml", out);
	char* text = ReadWholeFile(path, &size);
	check(text && strstr(text, "reticle: {5: 135}") && strstr(text, "colors: {inner: 0xff19197f}"),
		  "craft: awing.yaml has the reticle and the inner colour");
	free(text);
	snprintf(path, sizeof path, "%s/models/isd.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && !strstr(text, "hud:") &&
			  strstr(text, "engine_glows:\n  0: {min_speed_percent: 1}\n"
						   "  6: {min_speed_percent: 100, min_speed_mglt: 30}\n") &&
			  !strstr(text, "  7:"),
		  "craft: isd.yaml has the engine glows with a limit, and no hud group");
	free(text);
	snprintf(path, sizeof path, "%s/models/slave.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "weapons:\n  colors:\n    variant: {default: 0, 282: 1}\n"
							   "    impact: {280: 0xff0000}\n  by_markings:\n    1:\n"
							   "      variant: {default: 3}\n      energy_bars: {ion_high: 0xff0000ff}\n"),
		  "craft: slave.yaml has the weapon colours and the changes for markings 1");
	free(text);
	snprintf(path, sizeof path, "%s/models/corvette.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "weapons:\n  switch_by_iff: false\n") && !strstr(text, "colors"),
		  "craft: corvette.yaml has the weapon switch alone");
	free(text);
	snprintf(path, sizeof path, "%s/models/noini.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "  1: {min_speed_mglt: 20}"),
		  "craft: <model>EngineGlows.txt counts for a model without an INI file");
	free(text);
	snprintf(path, sizeof path, "%s/models/empty.yaml", out);
	check(!IsFile(path), "craft: no file for a model without settings");
	snprintf(path, sizeof path, "%s/hud.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "hull_icons: [\n  0, 100]") && !strstr(text, "map_icons"),
		  "craft: hud.yaml has the hull icon list, and no map icon list without its file");
	free(text);
}

/* ---- Model files ------------------------------------------------------------ */

/* Returns the members of set `name`, or NULL when the model has no such set. */
static const uint32_t* model_set(const XwauModel* model, const char* name) {
	for (size_t i = 0; i < model->set_count; ++i)
		if (strcmp(model->sets[i].name, name) == 0)
			return model->sets[i].members;
	return NULL;
}

/* Returns whether `members` holds exactly the meshes of `meshes`. */
static bool set_is(const uint32_t* members, const int* meshes, size_t count) {
	uint32_t expected[8] = { 0 };
	for (size_t i = 0; i < count; ++i)
		expected[meshes[i] / 32] |= 1u << (meshes[i] % 32);
	return members && memcmp(members, expected, sizeof expected) == 0;
}

static void test_model(const char* scratch) {
	char game[1024], models[1100], path[1200];
	snprintf(game, sizeof game, "%s/model_game", scratch);
	snprintf(models, sizeof models, "%s/FlightModels", game);
	MakeDirs(models);
	snprintf(path, sizeof path, "%s/Fields.ini", models);
	write_file(path, "[MeshesCollisions]\nMeshCollision_3 = 0 ;Hangar Forcefield\nMeshCollision_3 = 1\n"
					 "MeshCollision_7 = 0x0\nMeshCollision_8 = x\n;MeshCollision_9 = 0\n"
					 "[ObjectProfiles]\nDefault = 1\nTargetableMeshes = 0; 3,4 , x, 300, 5\n"
					 "TargetableMeshes = 9\n");
	snprintf(path, sizeof path, "%s/AllOff.ini", models);
	write_file(path, "[MeshesCollisions]\nMeshCollision = 0\nMeshCollision_5 = 1\n");
	snprintf(path, sizeof path, "%s/Text.ini", models);
	write_file(path,
			   "[MeshesCollisions]\nMeshCollision_4 = 0\n[ObjectProfiles]\nTargetableMeshes = 300, x\n");
	snprintf(path, sizeof path, "%s/TextMeshesCollisions.txt", models);
	write_file(path, "MeshCollision_2 = 0\n");
	snprintf(path, sizeof path, "%s/Plain.ini", models);
	write_file(path, "[Size]\nX = 1\n[ObjectProfiles]\nTargetableMeshes =\n");

	XwauMissions* missions = XwauMissions_Load(game, false);
	XwauModel*    model    = (XwauModel*)malloc(sizeof *model);
	check(missions && model, "models: setup");
	if (!missions || !model) {
		XwauMissions_Free(missions);
		free(model);
		return;
	}

	check(XwauModel_Read(game, missions, "Fields", model, NULL), "models: Fields is read");
	const int fields_off[] = { 3, 7 };
	check(set_is(model_set(model, "collision.off"), fields_off, 2),
		  "collisions: the first MeshCollision_3 wins, 0x0 is 0, and a commented line or a value that "
		  "isn't a number changes nothing");
	const int fields_target[] = { 0, 3, 4, 5 };
	check(set_is(model_set(model, "targetable"), fields_target, 4),
		  "targetable: tokens are split on ',' and ';', the first key wins, and bad tokens are dropped");
	const int fields_profile[] = { 1 };
	check(set_is(model_set(model, "hidden_by_profile.default"), fields_profile, 1),
		  "targetable: TargetableMeshes isn't a profile");

	check(XwauModel_Read(game, missions, "AllOff", model, NULL), "models: AllOff is read");
	int    all_off[254];
	size_t all_count = 0;
	for (int mesh = 0; mesh <= 254; ++mesh)
		if (mesh != 5)
			all_off[all_count++] = mesh;
	check(set_is(model_set(model, "collision.off"), all_off, all_count),
		  "collisions: MeshCollision is the default of every mesh");

	check(XwauModel_Read(game, missions, "Text", model, NULL), "models: Text is read");
	const int text_off[] = { 2 };
	check(set_is(model_set(model, "collision.off"), text_off, 1),
		  "collisions: <model>MeshesCollisions.txt is read instead of the section");
	check(!model_set(model, "targetable"), "targetable: no set when no listed mesh can be used");

	check(XwauModel_Read(game, missions, "Plain", model, NULL), "models: Plain is read");
	check(!model_set(model, "collision.off") && !model_set(model, "targetable"),
		  "models: no sets without the sections, or with an empty TargetableMeshes");

	XwauMissions_Free(missions);
	free(model);
}

/* ---- Text overrides ----------------------------------------------------------- */

static void test_text(const char* scratch) {
	char stock[1024], mod[1024], path[1100], out[1100];
	snprintf(stock, sizeof stock, "%s/text_stock", scratch);
	snprintf(mod, sizeof mod, "%s/text_mod", scratch);
	snprintf(out, sizeof out, "%s/text_out/strings.yaml", scratch);
	MakeDirs(stock);
	MakeDirs(mod);
	snprintf(path, sizeof path, "%s/STRINGS.TXT", stock);
	write_file(path, "// comment\r\n!MSG_ARMED!\\03Lasers armed\r\n!KSPEC_A!m:A-wing\r\n!SAME!Same\r\n");
	snprintf(path, sizeof path, "%s/Strings.txt", mod);
	write_file(path,
			   "// other comment\r\n!msg_armed!\\03Primary cannons armed\r\n!KSPEC_A!m:Pilot's \xe9 A\r\n"
			   "!SAME!Same\r\n!EXTRA!Extra\r\n");
	snprintf(path, sizeof path, "%s/SHIPLIST.TXT", stock);
	write_file(path, "!SPECIES_1!Xwing,Fighter,\t\t6 ,9 \r\n!SPECIES_2!Ywing\r\n");
	snprintf(path, sizeof path, "%s/SHIPLIST.TXT", mod);
	write_file(path, "!SPECIES_2!Ywing\r\n!SPECIES_1!X-wing,Fighter,\t\t6, 9 \r\n");
	snprintf(path, sizeof path, "%s/FRONTTXT.TXT", stock);
	write_file(path, "!STR_STATS!Pilot Statistics\n");
	snprintf(path, sizeof path, "%s/SPECDESC.TXT", stock);
	write_file(path, "!SPEC_1_NAME!X-wing\n!SPEC_186_NAME!*Sullust\n!SPEC_183_NAME!*Old\n");
	snprintf(path, sizeof path, "%s/SPECDESC.TXT", mod);
	write_file(path, "!SPEC_1_NAME!X-wing\n!SPEC_186_NAME!U-Wing\n!SPEC_183_NAME!New\n");

	check(XwauText_Write(mod, stock, out, NULL), "text: strings.yaml is written without FRONTTXT.TXT");
	size_t size = 0;
	char*  text = ReadWholeFile(out, &size);
	check(text && strstr(text, "  msg_armed: '\\03Primary cannons armed'\n"),
		  "text: IDs match in upper or lower case, and backslashes stay as they are");
	check(text && strstr(text, "  KSPEC_A: 'm:Pilot''s \xc3\xa9 A'\n"),
		  "text: quotes are doubled, and Latin-1 is written as UTF-8");
	check(text && !strstr(text, "SAME") && !strstr(text, "EXTRA"),
		  "text: no entry for a text that didn't change, or for an ID that only the mod has");
	check(text && strstr(text, "  SPECIES_1: 'X-wing,Fighter,\t\t6, 9 '\n"),
		  "text: tabs and the spaces at the end are kept");
	check(text && strstr(text, "  SPEC_183_NAME: 'New'\n") && !strstr(text, "SPEC_186"),
		  "text: no tech room text for a craft type that the stock game has no craft for");
	free(text);

	snprintf(path, sizeof path, "%s/STRINGS.TXT", stock);
	remove(path);
	check(!XwauText_Write(mod, stock, out, NULL) && !IsFile(out),
		  "text: nothing is written when a stock file is missing");
}

/* ---- Object files ------------------------------------------------------------ */

/* A small XWINGALLIANCE.EXE: a PE file whose four sections hold the addresses
   that the object files read, and nothing else. */
typedef struct FakeExe {
	unsigned char* data;
	size_t         size;
} FakeExe;

static const struct {
	unsigned address, size;
} g_fakeSections[] = {
	{ 0x41C000, 0x1000 },  /* the beam test */
	{ 0x43D000, 0x1000 },  /* three sound switches */
	{ 0x458000, 0x2000 },  /* the hangar launch switch */
	{ 0x5B0000, 0x50000 }, /* the craft type table, the craft records and the object type table */
};

static unsigned char* fake_at(FakeExe* exe, unsigned address) {
	size_t raw = 0x400;
	for (size_t i = 0; i < 4; ++i) {
		if (address >= g_fakeSections[i].address &&
			address < g_fakeSections[i].address + g_fakeSections[i].size)
			return exe->data + raw + (address - g_fakeSections[i].address);
		raw += g_fakeSections[i].size;
	}
	return NULL;
}

static void put16(unsigned char* p, unsigned value) {
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char* p, unsigned value) {
	put16(p, value & 0xFFFF);
	put16(p + 2, value >> 16);
}

static void fake_exe_init(FakeExe* exe) {
	exe->size = 0x400;
	for (size_t i = 0; i < 4; ++i)
		exe->size += g_fakeSections[i].size;
	exe->data        = calloc(1, exe->size);
	unsigned char* d = exe->data;
	d[0]             = 'M';
	d[1]             = 'Z';
	put32(d + 0x3C, 0x40);
	memcpy(d + 0x40, "PE\0\0", 4);
	put16(d + 0x40 + 6, 4);
	put16(d + 0x40 + 20, 224);
	put32(d + 0x40 + 24 + 28, 0x400000);
	size_t raw = 0x400;
	for (size_t i = 0; i < 4; ++i) {
		unsigned char* section = d + 0x40 + 24 + 224 + 40 * i;
		put32(section + 8, g_fakeSections[i].size);
		put32(section + 12, g_fakeSections[i].address - 0x400000);
		put32(section + 16, g_fakeSections[i].size);
		put32(section + 20, (unsigned)raw);
		raw += g_fakeSections[i].size;
	}
	/* The sound switches: `jmp [table + case * 4]`, every object type on case 9
	   and the TIE Fighter (5) on case 4. */
	static const unsigned cases[4]       = { 0x43D458, 0x43D4F8, 0x43DCC4, 0x459678 };
	static const unsigned jumps[4]       = { 0x43D117, 0x43D359, 0x43DA0C, 0x458E81 };
	static const unsigned jump_tables[4] = { 0x43D800, 0x43D840, 0x43D880, 0x459800 };
	for (int s = 0; s < 4; ++s) {
		unsigned char* jump = fake_at(exe, jumps[s]);
		jump[0]             = 0xFF;
		jump[1]             = 0x24;
		jump[2]             = 0x85;
		put32(jump + 3, jump_tables[s]);
		memset(fake_at(exe, cases[s]), 9, 65);
		fake_at(exe, cases[s])[5 - 1] = 4;
	}
	/* The stock beam test's `jz`. */
	fake_at(exe, 0x41C6BF)[0] = 0x74;
	fake_at(exe, 0x41C6BF)[1] = 0x1E;
	/* Every object type has no list row. */
	for (int t = 0; t < 557; ++t)
		put16(fake_at(exe, 0x5FB240 + 24 * t + 20), 0xFFFF);
}

/* Gives object type `t` row `row` of Spacecraft0.LST and craft record `record`. */
static void fake_type(FakeExe* exe, int t, int row, int record) {
	unsigned char* type = fake_at(exe, 0x5FB240 + 24 * t);
	type[0]             = 0x02;
	put16(type + 18, (unsigned)record);
	put16(type + 20, 0);
	put16(type + 22, (unsigned)row);
}

/* Gives object type `t` row `row` of Equipment0.LST and no craft record. */
static void fake_equipment(FakeExe* exe, int t, int row) {
	fake_type(exe, t, row, 0xFFFF);
	put16(fake_at(exe, 0x5FB240 + 24 * t + 20), 1);
}

static void fake_exe_write(FakeExe* exe, const char* root) {
	char path[1100];
	snprintf(path, sizeof path, "%s/XWINGALLIANCE.EXE", root);
	WriteWholeFile(path, exe->data, exe->size);
	free(exe->data);
}

/* Writes STRINGS.TXT with the three species tables, and `long_1` as the long
   name of species record 1. */
static void write_strings(const char* root, const char* long_1) {
	char path[1100], line[128];
	snprintf(path, sizeof path, "%s/STRINGS.TXT", root);
	FILE* f = fopen(path, "wb");
	fputs("// species\r\n!KLASTBUOYSTRING!Buoy\r\n", f);
	static const char* const suffixes[3] = { "", "_PLURAL", "_SHORT" };
	for (int table = 0; table < 3; ++table)
		for (int i = 0; i < 221; ++i) {
			if (i == 0)
				snprintf(line, sizeof line, "!KSPECXW%s!%sX-wing", suffixes[table], table ? "" : "m:");
			else if (i == 1 && table == 0)
				snprintf(line, sizeof line, "!KSPEC%d!%s", i, long_1);
			else
				snprintf(line, sizeof line, "!KSPEC%d%s!%sCraft %d", i, suffixes[table],
						 table ? "" : "m:", i);
			fprintf(f, "%s\r\n", line);
		}
	fclose(f);
}

static void test_objects(const char* scratch) {
	char stock[1024], mod[1024], path[1100], out[1100];
	snprintf(stock, sizeof stock, "%s/objects_stock", scratch);
	snprintf(mod, sizeof mod, "%s/objects_mod", scratch);
	snprintf(out, sizeof out, "%s/objects_out", scratch);
	const char* roots[2] = { stock, mod };
	for (int i = 0; i < 2; ++i) {
		snprintf(path, sizeof path, "%s/FlightModels", roots[i]);
		MakeDirs(path);
		snprintf(path, sizeof path, "%s/FlightModels/Equipment0.LST", roots[i]);
		write_file(path, i == 0 ? "FlightModels\\Laser.opt\r\nFlightModels\\LaserRebelTurbo.opt\r\n"
								: "FlightModels\\Laser.opt\r\nFlightModels\\LaserRebelTurbo.opt\r\n"
								  "FlightModels\\RebelTurboShort.opt\r\n");
	}
	snprintf(path, sizeof path, "%s/FlightModels/Spacecraft0.LST", stock);
	write_file(
		path,
		"FlightModels\\XWING.opt\r\n\r\nFlightModels\\Booster.opt\r\nFlightModels\\CorvetteExplode.opt\r\n"
		"FlightModels\\Bwing.opt\r\n");
	snprintf(path, sizeof path, "%s/FlightModels/Spacecraft0.LST", mod);
	write_file(path, "FlightModels\\Xwing.opt\r\nFlightModels\\TieAdvancedx1.opt\r\n"
					 "FlightModels\\CalamariHomeOne.opt\r\nFlightModels\\Bwing.opt\r\n");

	FakeExe exes[2];
	for (int i = 0; i < 2; ++i) {
		fake_exe_init(&exes[i]);
		/* Object types 1 and 2 are on the beam test; 70 and 71 aren't. */
		fake_type(&exes[i], 1, 0, 0);  /* X-wing */
		fake_type(&exes[i], 2, 1, 1);  /* Booster, then TIE Advanced x1 */
		fake_type(&exes[i], 70, 2, 2); /* CorvetteExplode */
		fake_type(&exes[i], 71, 3, 3); /* B-wing */
		put16(fake_at(&exes[i], 0x5B0F70 + 2 * 7), 2);
		put16(fake_at(&exes[i], 0x5B0F70 + 2 * 9), 70);
		fake_equipment(&exes[i], 288, 1); /* two turbolasers on the same row */
		fake_equipment(&exes[i], 297, 1);
		fake_equipment(&exes[i], 280, 0); /* a laser that the mod doesn't change */
	}
	FakeExe* m = &exes[1];
	fake_equipment(m, 288, 2); /* both move to a new row */
	fake_equipment(m, 297, 2);
	fake_type(m, 33, 2, 32); /* a new craft in a reserved slot, on the renamed row */
	fake_type(m, 34, 4, 33); /* a reserved slot on a row that the list doesn't have */
	put16(fake_at(m, 0x5B0F70 + 2 * 200), 33);
	put16(fake_at(m, 0x5B0F70 + 2 * 201), 34);
	put32(fake_at(m, 0x5BB480 + 987 * 0 + 21), 999); /* the X-wing's hull */
	put16(fake_at(m, 0x5FB240 + 24 * 71 + 16), 0x843);
	put16(fake_at(m, 0x5BB480 + 987 * 3 + 306), 280); /* the B-wing's first laser group */
	put32(fake_at(m, 0x5BB480 + 987 * 3 + 324), 1000);
	put16(fake_at(m, 0x5BB480 + 987 * 1 + 306), 282); /* the TIE Advanced x1's first laser group */
	fake_at(m, 0x5BB480 + 987 * 1 + 318)[0] = 2;
	memcpy(fake_at(m, 0x5BB480 + 987 * 1 + 50), "TieAdvancedX1", 13);
	static const unsigned cases[4] = { 0x43D458, 0x43D4F8, 0x43DCC4, 0x459678 };
	for (int s = 0; s < 4; ++s)
		fake_at(m, cases[s])[2 - 1] = 4; /* object type 2 sounds like the TIE Fighter */
	fake_at(m, 0x41C6BF)[0] = 0xEB;
	fake_at(m, 0x41C6BF)[1] = 0x29;
	fake_exe_write(&exes[0], stock);
	fake_exe_write(&exes[1], mod);

	snprintf(path, sizeof path, "%s/FlightModels/BWING.ini", mod);
	write_file(path, "[Size]\nX = 1\n[ExeCraftValues]\nGameOptions_IsHardpointsMirroring = 0\n"
					 "LaserTypeId1 = 283\nLaserSequence1 = 0x5\t; hexadecimal\nWarheadTypeId0 = 286\n"
					 "CounterMeasuresCount = x\nAIHitsTakenToEvade = 3\n");
	write_strings(stock, "m:Craft 1");
	write_strings(mod, "m:TIE Advanced x1");
	snprintf(path, sizeof path, "%s/SHIPLIST.TXT", stock);
	write_file(path,
			   "!SPECIES_7!*Booster,Fighter,Nonflyable,Unknown,NoSkirmish,\t\t1,2,3,4,5,6,7,8\r\n"
			   "!SPECIES_9!B-wing,Fighter,Flyable,Known,Skirmish,\t\t1,2,3,4,5,6,7,8\r\n"
			   "!SPECIES_200!*DontUseMe,Planet/asteroid,Nonflyable,Known,NoSkirmish,\t1,2,3,4,5,6,7,8\r\n"
			   "!SPECIES_201!*DontUseMe,Planet/asteroid,Nonflyable,Known,NoSkirmish,\t1,2,3,4,5,6,7,8\r\n");
	snprintf(path, sizeof path, "%s/SHIPLIST.TXT", mod);
	write_file(
		path,
		"!SPECIES_7!TIE Advanced x1,Fighter,GunnerFlyable,Known,Skirmish,\t\t1,2,3,4, 651, 246,681,279\r\n"
		"!SPECIES_9!B-wing A,Fighter,Flyable,Known,Skirmish,\t\t1,2,3,4,5,6,7,8\r\n"
		"!SPECIES_200!Home One,Starship,Nonflyable,Known,Skirmish,\t1,2,3,4,5,6,7,8\r\n"
		"!SPECIES_201!Nothing,Starship,Nonflyable,Known,Skirmish,\t1,2,3,4,5,6,7,8\r\n");
	snprintf(path, sizeof path, "%s/SPECDESC.TXT", stock);
	write_file(path, "!SPEC_7_NAME!Booster\r\n!SPEC_7_CREW!0\r\n");
	snprintf(path, sizeof path, "%s/SPECDESC.TXT", mod);
	write_file(path, "!SPEC_7_NAME!TIE Advanced x1\r\n!SPEC_7_CREW!0\r\n");
	snprintf(path, sizeof path, "%s/FRONTTXT.TXT", stock);
	write_file(path, "!STR_A!A\r\n");

	XwauTextIds moved = { 0 };
	char        strings[1200];
	snprintf(strings, sizeof strings, "%s/strings.yaml", out);
	check(XwauObjects_Write(mod, stock, out, &moved) && XwauText_Write(mod, stock, strings, &moved),
		  "objects: the object type files, the craft type files and strings.yaml are written");

	size_t size = 0;
	snprintf(path, sizeof path, "%s/object_types/tieadvancedx1.yaml", out);
	char* text = ReadWholeFile(path, &size);
	check(text && strstr(text, "object_type: 2\nmodel: tieadvancedx1\n"),
		  "objects: a row that the mod renames gives `model`");
	check(text && strstr(text, "    - {type: 282, count: 2, kind: 0, range: 0, cooldown: 0}\n") &&
			  strstr(text, "  cockpit_name: 'TieAdvancedX1'\n") && strstr(text, "sounds_like: 5\n"),
		  "objects: the laser groups, the cockpit name and the engine sound case");
	check(text && strstr(text, "names: {long: 'TIE Advanced x1', gender: m}\n") && !strstr(text, "ship_list"),
		  "objects: the species name of the craft record goes into `names`, and the craft type's texts don't "
		  "go into the object type file");
	free(text);
	snprintf(path, sizeof path, "%s/craft_types/tie_advanced_x1.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "craft_type: 7\nobject_type: 2  # tieadvancedx1\n") &&
			  strstr(text, "ship_list: {name: 'TIE Advanced x1', category: 'Fighter', flyable: gunner, "
						   "known: true, skirmish: true, hidden: false, icon: [651, 246, 681, 279]}\n") &&
			  strstr(text, "tech_room: {name: 'TIE Advanced x1'}\n"),
		  "objects: the craft type file has the object type with its model, the ship list line and the tech "
		  "room text, and is named after the ship list name");
	free(text);
	snprintf(path, sizeof path, "%s/craft_types/b_wing_a.yaml", out);
	text = ReadWholeFile(path, &size);
	check(
		text &&
			strstr(text, "craft_type: 9\nobject_type: 70  # corvetteexplode\nship_list: {name: 'B-wing A'"),
		"objects: a craft type gets a file for a changed ship list line even when its object type has none");
	free(text);
	snprintf(path, sizeof path, "%s/object_types/xwing.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && !strstr(text, "model:") && strstr(text, "  hull: 999\n") &&
			  strstr(text, "beam_allowed: true\n"),
		  "objects: a change of case isn't a new model; the hull and the beam");
	free(text);
	snprintf(path, sizeof path, "%s/object_types/bwing.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "object_type: 71\nflags: 0x43\n"),
		  "objects: [ExeCraftValues] sets the object type's flags");
	check(text && strstr(text, "  ai_hits_to_evade: 3\n") && !strstr(text, "countermeasures") &&
			  strstr(text, "  lasers:\n    - {type: 280, count: 0, kind: 0, range: 1000, cooldown: 0}\n"
						   "    - {type: 283, count: 0, kind: 5, range: 0, cooldown: 0}\n") &&
			  strstr(text, "  warheads:\n    - {type: 286, count: 0, capacity: 0}\n"),
		  "objects: [ExeCraftValues] sets the craft record; missing keys and values that aren't numbers keep "
		  "the record's values");
	free(text);
	snprintf(path, sizeof path, "%s/object_types/corvetteexplode.yaml", out);
	check(!IsFile(path),
		  "objects: a row renamed for a new craft keeps the stock model, and the type gets no file");
	snprintf(path, sizeof path, "%s/object_types/calamarihomeone.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "object_type: 33\nmodel: calamarihomeone\n"),
		  "objects: a reserved slot that the mod fills gets its model");
	free(text);
	snprintf(path, sizeof path, "%s/craft_types/home_one.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "craft_type: 200\nobject_type: 33  # calamarihomeone\n") &&
			  strstr(text, "ship_list: {name: 'Home One', category: 'Starship', flyable: false"),
		  "objects: the craft type of a new craft gets a file with its object type and its ship list line");
	free(text);
	snprintf(path, sizeof path, "%s/object_types/rebelturboshort_297.yaml", out);
	text = ReadWholeFile(path, &size);
	check(
		text && strstr(text, "object_type: 297\nmodel: rebelturboshort\nlist: equipment\n"),
		"objects: an equipment type that the mod moves to another model gets `model` and `list: equipment`, "
		"and two types with one model get a file each");
	free(text);
	snprintf(path, sizeof path, "%s/object_types", out);
	size_t count = 0;
	char** names = ListDir(path, true, &count);
	check(count == 6,
		  "objects: a reserved slot that the mod gives an empty row gets no file, and neither does "
		  "an equipment type that the mod doesn't change");
	FreeList(names, count);
	snprintf(path, sizeof path, "%s/craft_types", out);
	names = ListDir(path, true, &count);
	check(count == 3, "objects: a craft type whose object type loads nothing gets no file");
	FreeList(names, count);
	text = ReadWholeFile(strings, &size);
	check(
		text && !strstr(text, "SPECIES_9") && !strstr(text, "SPECIES_7") && !strstr(text, "SPEC_7_") &&
			!strstr(text, "KSPEC1:") && !strstr(text, "SPECIES_200") && !strstr(text, "SPECIES_201"),
		"objects: the texts in the object type and craft type files aren't in strings.yaml, and neither are "
		"the texts of a craft type without a craft");
	free(text);
	XwauTextIds_Free(&moved);
}

/* ---- Game files and mission files ----------------------------------------------- */

static bool same_file(const char* path, const char* text) {
	size_t     size = 0;
	char*      data = ReadWholeFile(path, &size);
	const bool same = data && size == strlen(text) && memcmp(data, text, size) == 0;
	free(data);
	return same;
}

static void test_game_files(const char* scratch) {
	char game[1024], stock[1024], out[1024], path[1300];
	snprintf(game, sizeof game, "%s/files_game", scratch);
	snprintf(stock, sizeof stock, "%s/files_stock", scratch);
	snprintf(out, sizeof out, "%s/files_out", scratch);
	static const char* const kDirs[] = { "files_game/Missions",           "files_game/Combat",
										 "files_game/Wave/FRONTEND/B1M1", "files_game/Wave/Other",
										 "files_game/FRONTRES/Cutscene",  "files_stock/MISSIONS",
										 "files_stock/WAVE/FRONTEND",     "files_stock/FrontRes/CUTSCENE",
										 "files_game/Wave/FE_HIGH_RES",   "files_stock/WAVE/FE_HIGH_RES",
										 "files_game/Wave/Custom" };
	for (size_t i = 0; i < sizeof kDirs / sizeof kDirs[0]; ++i) {
		snprintf(path, sizeof path, "%s/%s", scratch, kDirs[i]);
		MakeDirs(path);
	}
	static const char* const kFiles[][2] = {
		{ "files_game/Missions/a.tie", "mod a" },
		{ "files_game/Missions/MISSION.LST", "same list" },
		{ "files_game/Missions/Email.txt", "mod email" },
		{ "files_game/Missions/a.ini", "settings" },
		{ "files_game/Combat/c.TIE", "mod c" },
		{ "files_game/Wave/FRONTEND/same.wav", "same voice" },
		{ "files_game/Wave/FRONTEND/B1M1/new.wav", "new voice" },
		{ "files_game/Wave/FRONTEND/changed.wav", "mod voice" },
		{ "files_game/Wave/Other/x.wav", "other sound" },
		{ "files_game/FRONTRES/Cutscene/cutscene.txt", "same table" },
		{ "files_stock/MISSIONS/A.TIE", "stock a" },
		{ "files_stock/MISSIONS/mission.lst", "same list" },
		{ "files_stock/MISSIONS/EMAIL.TXT", "stock email" },
		{ "files_stock/WAVE/FRONTEND/SAME.WAV", "same voice" },
		{ "files_stock/WAVE/FRONTEND/CHANGED.WAV", "stock voice" },
		{ "files_stock/FrontRes/CUTSCENE/CUTSCENE.TXT", "same table" },
		{ "files_game/Wave/SfxBlastNew.lst", "mod list" },
		{ "files_stock/WAVE/SFXBLASTNEW.LST", "stock list" },
		{ "files_game/Wave/OverDrive.lst", "new list" },
		{ "files_game/Wave/FE_HIGH_RES/Alarm.wav", "mod sound" },
		{ "files_stock/WAVE/FE_HIGH_RES/ALARM.WAV", "stock sound" },
		{ "files_game/hook_engine_sound.dll", "" },
		{ "files_game/Missions/b.ini",
		  "[Sounds]\nWave\\FE_High_Res\\Alarm.wav = Wave\\Custom\\Evacuate.wav\n" },
		{ "files_game/Wave/Custom/Evacuate.wav", "evacuate" },
		{ "files_game/Wave/Custom/Unused.wav", "not loaded" },
	};
	for (size_t i = 0; i < sizeof kFiles / sizeof kFiles[0]; ++i) {
		snprintf(path, sizeof path, "%s/%s", scratch, kFiles[i][0]);
		write_file(path, kFiles[i][1]);
	}
	check(XwauGameFiles_Copy(game, stock, out), "game files: copied");
	snprintf(path, sizeof path, "%s/Missions/a.tie", out);
	const bool tie = same_file(path, "mod a");
	snprintf(path, sizeof path, "%s/Combat/c.TIE", out);
	check(tie && same_file(path, "mod c"), "game files: every .tie is copied, from each mission folder");
	snprintf(path, sizeof path, "%s/Missions/Email.txt", out);
	const bool email = same_file(path, "mod email");
	snprintf(path, sizeof path, "%s/Missions/MISSION.LST", out);
	const bool list = IsFile(path);
	snprintf(path, sizeof path, "%s/FRONTRES/Cutscene/cutscene.txt", out);
	check(email && !list && !IsFile(path),
		  "game files: a list is copied only when it differs from the stock one");
	snprintf(path, sizeof path, "%s/Missions/a.ini", out);
	check(!IsFile(path), "game files: the mission settings aren't copied");
	snprintf(path, sizeof path, "%s/Wave/FRONTEND/B1M1/new.wav", out);
	const bool added = same_file(path, "new voice");
	snprintf(path, sizeof path, "%s/Wave/FRONTEND/changed.wav", out);
	const bool changed = same_file(path, "mod voice");
	snprintf(path, sizeof path, "%s/Wave/FRONTEND/same.wav", out);
	const bool same = IsFile(path);
	snprintf(path, sizeof path, "%s/Wave/Other/x.wav", out);
	check(added && changed && !same && !IsFile(path),
		  "game files: only the voices that are new or changed are copied, and no folder the game lacks");
	snprintf(path, sizeof path, "%s/Wave/SfxBlastNew.lst", out);
	const bool blast = same_file(path, "mod list");
	snprintf(path, sizeof path, "%s/Wave/FE_HIGH_RES/Alarm.wav", out);
	const bool alarm = same_file(path, "mod sound");
	snprintf(path, sizeof path, "%s/Wave/OverDrive.lst", out);
	check(blast && alarm && !IsFile(path),
		  "game files: the changed stock sounds and lists are copied, and no file the game lacks");
	snprintf(path, sizeof path, "%s/Wave/Custom/Evacuate.wav", out);
	const bool target = same_file(path, "evacuate");
	snprintf(path, sizeof path, "%s/Wave/Custom/Unused.wav", out);
	check(target && !IsFile(path), "game files: the files of the missions' sound replacements are copied");

	/* A mission's sound replacements. */
	snprintf(path, sizeof path, "%s/WAVE/HANGARVOICESFX.LST", stock);
	write_file(path, "Rebel_Pilot8/G4P8001.wav\r\nRebel_Pilot8/G4P8024.wav\r\n");
	snprintf(path, sizeof path, "%s/Missions/b.ini", game);
	write_file(path, "[Sounds]\n; comment\nWave\\FE_High_Res\\Alarm.wav = Wave\\Custom\\Evacuate.wav\n"
					 "wave\\fe_high_res\\ALARM.WAV = Wave\\Custom\\Unused.wav\n"
					 "Wave\\FE_High_Res\\Other.wav = Wave\\Custom\\Missing.wav\n"
					 "Wave\\Rebel_Pilot8\\G4P8024.wav = Wave\\Custom\\Evacuate.wav\n");
	XwauMissionSounds sounds;
	check(XwauMissionSounds_Read(game, stock, "Missions/b", true, &sounds) && sounds.count == 1 &&
			  strcmp(sounds.items[0].to, "Wave\\Custom\\Evacuate.wav") == 0,
		  "mission sounds: the first line of a path wins; a missing file and a hangar voice are left out");
	snprintf(path, sizeof path, "%s/mission_sounds.yaml", scratch);
	FILE* f = fopen(path, "wb");
	if (f) {
		XwauMissionSounds_Write(f, &sounds);
		fclose(f);
	}
	size_t size = 0;
	char*  text = ReadWholeFile(path, &size);
	check(text && strcmp(text, "sounds:\n  - [\"Wave\\\\FE_High_Res\\\\Alarm.wav\", "
							   "\"Wave\\\\Custom\\\\Evacuate.wav\"]\n") == 0,
		  "mission sounds: the mission file's list");
	free(text);
	XwauMissionSounds_Free(&sounds);
}

/* Writes a .TIE file with `count` flight groups, named `names`, of craft
 * types `crafts`, with 2 craft in each wave. */
static void write_tie(const char* path, size_t count, const char* const* names, const int* crafts) {
	enum { kHeader = 0x23F0, kFlightGroup = 0xE3E, kCraft = 0x6B, kCraftCount = 0x6C };

	const size_t   size = kHeader + count * kFlightGroup;
	unsigned char* data = (unsigned char*)calloc(1, size);
	if (!data)
		return;
	data[2] = (unsigned char)count;
	for (size_t i = 0; i < count; ++i) {
		memcpy(data + kHeader + i * kFlightGroup, names[i], strlen(names[i]));
		data[kHeader + i * kFlightGroup + kCraft]      = (unsigned char)crafts[i];
		data[kHeader + i * kFlightGroup + kCraftCount] = 2;
	}
	WriteWholeFile(path, data, size);
	free(data);
}

static void test_mission_files(const char* scratch) {
	char game[1024], stock[1024], out[1024], path[1300];
	snprintf(game, sizeof game, "%s/mf_game", scratch);
	snprintf(stock, sizeof stock, "%s/mf_stock", scratch);
	snprintf(out, sizeof out, "%s/mf_out", scratch);
	snprintf(path, sizeof path, "%s/Missions", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/MISSIONS", stock);
	MakeDirs(path);
	/* Mission a has its own .TIE, b uses the stock one, and c has none. */
	static const char* const kModNames[]    = { "Red", "Blue", "Gold" };
	static const int         kModCrafts[]   = { 1, 2, 3 };
	static const char* const kStockNames[]  = { "Other", "Red" };
	static const int         kStockCrafts[] = { 9, 1 };
	snprintf(path, sizeof path, "%s/Missions/a.tie", game);
	write_tie(path, 3, kModNames, kModCrafts);
	snprintf(path, sizeof path, "%s/MISSIONS/A.TIE", stock);
	write_tie(path, 2, kStockNames, kStockCrafts);
	snprintf(path, sizeof path, "%s/MISSIONS/B.TIE", stock);
	write_tie(path, 2, kStockNames, kStockCrafts);
	snprintf(path, sizeof path, "%s/Missions/a.ini", game);
	write_file(path, "[Objects]\nObjectProfile_fg_1 = Ace\nObjectProfile_fg_5 = Lost\n[Resdata]\n"
					 "Resdata\\Planet2.dat\nResdata\\Missing.dat\nResdata\\BACKDROP_Special.DAT\n"
					 "resdata\\planet2.DAT\n");
	/* Mission d only adds art, from its own file. */
	snprintf(path, sizeof path, "%s/Resdata", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/Resdata/Planet2.dat", game);
	write_file(path, "");
	snprintf(path, sizeof path, "%s/Resdata/BACKDROP_Special.DAT", game);
	write_file(path, "");
	snprintf(path, sizeof path, "%s/Missions/d.ini", game);
	write_file(path, "[Resdata]\nResdata\\Planet2.dat\n");
	snprintf(path, sizeof path, "%s/Missions/d_Resdata.txt", game);
	write_file(path, "Resdata\\BACKDROP_Special.DAT\n");
	snprintf(path, sizeof path, "%s/MISSIONS/D.TIE", stock);
	write_tie(path, 2, kStockNames, kStockCrafts);
	snprintf(path, sizeof path, "%s/Missions/b.ini", game);
	write_file(path, "[Objects]\nObjectProfile_fg_1 = Stock\n");
	snprintf(path, sizeof path, "%s/Missions/c.ini", game);
	write_file(path, "[Objects]\nObjectProfile_fg_0 = None\n");
	XwauMissions* missions = XwauMissions_Load(game, false);
	check(missions && XwauMissionFiles_Write(game, missions, stock, out), "mission files: written");
	XwauMissions_Free(missions);
	size_t size = 0;
	snprintf(path, sizeof path, "%s/missions/a.yaml", out);
	char* a = ReadWholeFile(path, &size);
	check(a && strstr(a, "    1: { profile: \"Ace\", name: \"Blue\", craft: 2 }\n") && !strstr(a, "Lost"),
		  "mission files: a selection keeps its position in the mod's .TIE, and one past its end is dropped");
	snprintf(path, sizeof path, "%s/missions/b.yaml", out);
	char* b = ReadWholeFile(path, &size);
	check(b && strstr(b, "    1: { profile: \"Stock\", name: \"Red\", craft: 1 }\n"),
		  "mission files: a mission without its own .TIE uses the game's");
	snprintf(path, sizeof path, "%s/missions/c.yaml", out);
	check(!IsFile(path), "mission files: a mission without a .TIE gets no file");
	check(a && strstr(a, "\nresdata: [\"planet2\", \"backdrop_special\"]\n"),
		  "mission files: resdata lists the art files that exist, once, by their mission art names");
	snprintf(path, sizeof path, "%s/missions/d.yaml", out);
	char* d = ReadWholeFile(path, &size);
	check(d && strstr(d, "\nresdata: [\"backdrop_special\"]\n") && !strstr(d, "object_profiles"),
		  "mission files: <mission>_Resdata.txt wins over the .ini, and a mission with only art gets a file");
	free(d);
	free(a);
	free(b);
}

static void test_mission_tie(const char* scratch) {
	char game[1024], stock[1024], out[1024], path[1300];
	snprintf(game, sizeof game, "%s/tie_game", scratch);
	snprintf(stock, sizeof stock, "%s/tie_stock", scratch);
	snprintf(out, sizeof out, "%s/tie_out", scratch);
	snprintf(path, sizeof path, "%s/Missions", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/MISSIONS", stock);
	MakeDirs(path);
	/* The .txt file wins over the .ini. */
	snprintf(path, sizeof path, "%s/Missions/t.ini", game);
	write_file(path, "[mission_tie]\nfg, 1, markings, 9\n");
	snprintf(path, sizeof path, "%s/Missions/t.txt", game);
	write_file(path, "fg, 3 markings, 6\n"
					 "fg, 3 markings, 6			;Grey 5\n"
					 "fg; 1; markings; 4\n"
					 "fg, 1, markings, 5\n"
					 "// fg, 1, markings, 8\n"
					 "fg, 2, iff, 2 // trailing comment\n"
					 "fg, 2, pilotvoice, Rebel_Pilot3\n"
					 "fg, 0, index, 1, markings, 7\n"
					 "fg, 0, index, 1, markings, 8\n"
					 "fg, 1, index, 3, markings, 300\n"
					 "fg, 1, index, 0, markings, 2\n"
					 "fg, 1, index, -1, markings, 2\n"
					 "fg, 9, index, 0, markings, 2\n"
					 "fg, 4, markings, abc\n"
					 "fg, 0, open_LandingGears, 1\n"
					 "fg, 0, close_SFoils, 1\n"
					 "fg, 59, markings, 1\n"
					 "craft, 5, name, Debris\n"
					 "SkipHyperspacedMessages = 1\n");
	XwauMissionTie tie;
	check(XwauMissionTie_Read(game, "Missions/t", true, &tie), "mission_tie: read");
	const XwauMissionTieFlightGroup* fg0 = XwauMissionTie_FlightGroup(&tie, 0);
	const XwauMissionTieFlightGroup* fg1 = XwauMissionTie_FlightGroup(&tie, 1);
	const XwauMissionTieFlightGroup* fg2 = XwauMissionTie_FlightGroup(&tie, 2);
	check(fg1 && fg1->has_markings && fg1->markings == 5,
		  "mission_tie: ';' splits words too, the last line wins, and comment lines are skipped");
	check(!XwauMissionTie_FlightGroup(&tie, 3) && !XwauMissionTie_FlightGroup(&tie, 4),
		  "mission_tie: a line with fewer than 4 words and a value that isn't a number are skipped");
	check(fg2 && fg2->has_iff && fg2->iff == 2 && fg2->has_pilot_voice &&
			  strcmp(fg2->pilot_voice, "Rebel_Pilot3") == 0 && !fg2->has_markings,
		  "mission_tie: a number is read as std::stoi reads it, so a trailing comment is ignored");
	check(tie.craft_marking_count == 5 && tie.craft_markings[0].fg == 0 && tie.craft_markings[0].craft == 1 &&
			  tie.craft_markings[0].markings == 8,
		  "mission_tie: the markings of one craft, the last line winning");
	check(fg0 && fg0->open_landing_gear && fg0->close_sfoils && fg0->open_landing_gear < fg0->close_sfoils,
		  "mission_tie: the start lines keep their order");
	check(tie.craft_lines.count == 1 && tie.settings.count == 1,
		  "mission_tie: craft lines and settings are kept for their readers");
	XwauMissionTie_Free(&tie);

	/* The mission file: values by position in the mod's .TIE, which has 3
	   flight groups, so flight group 59 is dropped. */
	static const char* const kNames[]  = { "Red", "Blue", "Gold" };
	static const int         kCrafts[] = { 1, 2, 3 };
	snprintf(path, sizeof path, "%s/Missions/t.tie", game);
	write_tie(path, 3, kNames, kCrafts);
	XwauMissions* missions = XwauMissions_Load(game, false);
	check(missions && XwauMissionFiles_Write(game, missions, stock, out),
		  "mission_tie: mission file written");
	XwauMissions_Free(missions);
	size_t size = 0;
	snprintf(path, sizeof path, "%s/missions/t.yaml", out);
	char* text = ReadWholeFile(path, &size);
	check(text &&
			  strstr(text, "flight_groups:\n  0: { name: \"Red\", craft: 1, craft_markings: { 1: 8 } }\n"
						   "  1: { name: \"Blue\", craft: 2, markings: 5, craft_markings: { 0: 2, 3: 44 } }\n"
						   "  2: { name: \"Gold\", craft: 3, iff: 2, pilot_voice: \"Rebel_Pilot3\" }\n") &&
			  !strstr(text, "59:") && !strstr(text, "9:") && !strstr(text, "-1") &&
			  strstr(text, "start: [landing_gear, sfoils]"),
		  "mission_tie: the values are written by position, the markings of each craft by its number, cut to "
		  "a byte, without the flight groups that the .TIE doesn't have and negative craft numbers, and the "
		  "start lines keep their order");
	free(text);
}

static void test_gunner_seats(const char* scratch) {
	char game[1024], path[1300], txt[1300], out[1300];
	snprintf(game, sizeof game, "%s/gunner_game", scratch);
	snprintf(path, sizeof path, "%s/FlightModels", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/FlightModels/FalconGunner.opt", game);
	write_file(path, "");
	snprintf(path, sizeof path, "%s/FlightModels/Falcon.ini", game);
	write_file(path, "[Turrets]\n"
					 "0, 19, 250, -16384, 16384, 12040, 17308, FalconGunner, 0\n"
					 "0; 19; 250; 16384; -16384; 12040; 17308; MissingGunner; 1\n"
					 "1, 2, 3\n"
					 "x, 19, 250, 0, 0, 1, 1, FalconGunner, 0\n"
					 "0, 0, 0, 0, 0, 0, 0, FalconGunner, 0\n"
					 "TurretForwardFire = 0\n");
	XwauGunnerSeats seats;
	XwauGunnerSeats_Read(game, NULL, "Falcon", NULL, path, &seats);
	check(seats.has && !seats.forward_fire && seats.count == 2 && seats.seats[0].x == 19 &&
			  seats.seats[0].y == 0 && seats.seats[0].z == 250 && seats.seats[0].mount_angle_a == 16384 &&
			  seats.seats[0].mount_angle_b == -16384 && seats.seats[0].aim_limit_a == 12040 &&
			  seats.seats[0].aim_limit_b == 17308 && strcmp(seats.seats[0].cockpit, "FalconGunner") == 0 &&
			  !seats.seats[0].inverted && seats.seats[1].inverted &&
			  strcmp(seats.seats[1].cockpit, "CorellianTransportGunner") == 0,
		  "gunner seats: the hook's field order, ';' splits too, short lines and lines with words are left "
		  "out, "
		  "a missing cockpit is the stock one, and the third seat doesn't fit");
	snprintf(out, sizeof out, "%s/gunner.yaml", scratch);
	XwauLines cockpits = { 0 };
	FILE*     f        = fopen(out, "wb");
	XwauGunnerSeats_Write(f, &seats, &cockpits);
	fclose(f);
	check(same_file(out,
					"gunner_seats:\n  forward_fire: false\n  seats:\n"
					"    - {x: 19, y: 0, z: 250, mount_angle_a: 16384, mount_angle_b: -16384, aim_limit_a: "
					"12040, aim_limit_b: 17308, cockpit: falcongunner, inverted: false}\n"
					"    - {x: 19, y: 0, z: 250, mount_angle_a: -16384, mount_angle_b: 16384, aim_limit_a: "
					"12040, aim_limit_b: 17308, cockpit: corelliantransportgunner, inverted: true}\n") &&
			  cockpits.count == 2,
		  "gunner seats: the group and its cockpits");
	XwauLines_Free(&cockpits);
	snprintf(txt, sizeof txt, "%s/FlightModels/FalconTurrets.txt", game);
	write_file(txt, "TurretForwardFire = 1\n");
	XwauGunnerSeats_Read(game, NULL, "Falcon", txt, path, &seats);
	check(seats.has && seats.forward_fire && seats.count == 0,
		  "gunner seats: <model>Turrets.txt wins, and a section without seat lines gives no seat");
	write_file(path, "[Size]\nX = 1\n");
	XwauGunnerSeats_Read(game, NULL, "Falcon", NULL, path, &seats);
	check(!seats.has, "gunner seats: no section and no record, no group");
}

static void test_weapon_stats(const char* scratch) {
	char game[1024], path[1300], out[1300];
	snprintf(game, sizeof game, "%s/weapon_stats_game", scratch);
	snprintf(path, sizeof path, "%s/FlightModels", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/Missions", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/hook_weapon_rate.dll", game);
	write_file(path, "");
	snprintf(path, sizeof path, "%s/FlightModels/default.ini", game);
	write_file(path, "[WeaponHardpointTypes]\n1\n1\n[WeaponStats]\nWeapon280_Range = 65536\n");
	snprintf(path, sizeof path, "%s/FlightModels/Model.ini", game);
	write_file(path, "[WeaponStats]\n"
					 "My_Profile_Weapon282_Power = 900\n"
					 "Weapon282_FireRate = 47\n"
					 "Weapon282_Player_Easy_FireRate = 39\n"
					 "weapon282_player_easy_power = 350\n"
					 "Weapon282_FireRate = 50\n"
					 "Weapon282_Speed = -1\n"
					 "Weapon289_IsPrecise = 0\n"
					 "Weapon289_DegreeOfSpreadMask = 63\n"
					 "Default_Weapon280_Power = 5\n"
					 "Weapon280_Side = 1\n"
					 "Weapon280_Foo = 1\n"
					 "[WarheadStats]\n"
					 "Warhead1_CapacityPercent = 50\n"
					 "Warhead2_CapacityPercent = -5\n"
					 "[EnergyStats]\n"
					 "SpeedFactorForPlayer = 12\n"
					 "[LinkingStats]\n"
					 "LaserLink = 2\n"
					 "PreventPlayerLinkedLasersIon = 1\n"
					 "[WeaponHardpointTypes]\n0\n1\n2\n");
	XwauWeaponStats stats;
	check(XwauWeaponStats_Read(game, "Model", &stats) && XwauWeaponStats_Any(&stats),
		  "weapon profiles: a model's sections are read");
	snprintf(out, sizeof out, "%s/weapon_stats.yaml", scratch);
	FILE* f = fopen(out, "wb");
	XwauWeaponStats_Write(f, "  ", &stats);
	fclose(f);
	check(
		same_file(out, "  stats:\n"
					   "    \"my_profile\":\n"
					   "      282: {power: 900}\n"
					   "    default:\n"
					   "      282: {fire_rate: 47, player_easy: {power: 350, fire_rate: 39}}\n"
					   "      289: {precise: false, spread_mask: 63}\n"
					   "  warhead_capacity_percent:\n"
					   "    default: {1: 50, 2: 0}\n"
					   "  energy_speed_q16:\n"
					   "    default: {player: 7864}\n"
					   "  linking:\n"
					   "    default: {laser_link: 2, prevent_player_linked_lasers_ion: true}\n"
					   "  hardpoint_kinds: [0, 1, 2, 1, 1]\n"),
		"weapon profiles: profile names with '_', the player and difficulty forms, the first of two keys, "
		"-1, Default and other keys left out, the energy in Q16, and the model's hardpoint kinds before the "
		"global ones");
	XwauWeaponStats_Free(&stats);
	check(XwauWeaponStats_Read(game, NULL, &stats) && stats.count == 1 && stats.items[0].index == 280 &&
			  stats.hardpoint_kind_count == 2,
		  "weapon profiles: the global sections");
	XwauWeaponStats_Free(&stats);
	snprintf(path, sizeof path, "%s/Missions/m1.ini", game);
	write_file(path, "[WeaponProfiles]\n"
					 "WeaponProfile_fg_1 = My_Profile\n"
					 "WeaponProfile_fg_1 = Other\n"
					 "WeaponProfile_fg_2 = Default\n"
					 "WeaponProfile_fg_03 = X\n"
					 "[EnergyProfiles]\n"
					 "EnergyProfile_fg_2 = Fast\n");
	XwauWeaponProfileChoices choices;
	snprintf(path, sizeof path, "Missions/m1");
	check(XwauWeaponStats_ReadChoices(game, path, &choices) && choices.count == 2 &&
			  strcmp(XwauWeaponStats_FindChoice(&choices, 1)->name[XWAU_WEAPON_STATS_STATS], "my_profile") ==
				  0 &&
			  !XwauWeaponStats_FindChoice(&choices, 2)->name[XWAU_WEAPON_STATS_STATS][0] &&
			  strcmp(XwauWeaponStats_FindChoice(&choices, 2)->name[XWAU_WEAPON_STATS_ENERGY], "fast") == 0 &&
			  !XwauWeaponStats_FindChoice(&choices, 3),
		  "weapon profiles: a mission's choices, the first of two lines, Default and keys the hook doesn't "
		  "read");
	check(XwauWeaponStats_HasProfile(game, "Model", XWAU_WEAPON_STATS_STATS, "my_profile") &&
			  !XwauWeaponStats_HasProfile(game, "Model", XWAU_WEAPON_STATS_ENERGY, "fast"),
		  "weapon profiles: the profiles that a model defines");
	f = fopen(out, "wb");
	XwauWeaponStats_WriteChoice(f, XwauWeaponStats_FindChoice(&choices, 2));
	fclose(f);
	check(same_file(out, ", energy_profile: \"fast\""), "weapon profiles: a flight group's choices");
	XwauWeaponStats_FreeChoices(&choices);
}

static void test_warhead_pools(const char* scratch) {
	char game[1024], stock[1024], out[1024], path[1300];
	snprintf(game, sizeof game, "%s/pool_game", scratch);
	snprintf(stock, sizeof stock, "%s/pool_stock", scratch);
	snprintf(out, sizeof out, "%s/pool_out", scratch);
	snprintf(path, sizeof path, "%s/Missions", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/FlightModels", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/MISSIONS", stock);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/Missions/w.ini", game);
	write_file(path, "[WarheadTypeCount]\n"
					 "WarheadTypeCount_fg_0_Missiles = 80\n"
					 "warheadtypecount_FG_0_missiles = 5\n"
					 "WarheadTypeCount_fg_0_ProtonTorpedos = 0 ;none\n"
					 "WarheadTypeCount_fg_0_SpaceBombs = -1\n"
					 "WarheadTypeCount_fg_1_HeavyRockets = -5\n"
					 "WarheadTypeCount_fg_1_MagPulseCount = -1\n"
					 "WarheadTypeCount_fg_01_Missiles = 3\n"
					 "WarheadTypeCount_fg_7_Missiles = 4\n");
	XwauWarheadPools pools;
	check(XwauWarheadPools_Read(game, "Missions/w", false, &pools) && pools.count == 0,
		  "warhead pools: not read without hook_weapon_rate");
	snprintf(path, sizeof path, "%s/hook_weapon_rate.dll", game);
	write_file(path, "MZ WarheadTypeCount_fg_");
	check(XwauWarheadPools_Read(game, "Missions/w", true, &pools), "warhead pools: read");
	const XwauWarheadPool* fg0 = XwauWarheadPools_Find(&pools, 0);
	const XwauWarheadPool* fg1 = XwauWarheadPools_Find(&pools, 1);
	check(fg0 && fg0->count[2] == 80 && fg0->count[3] == 0 && fg0->count[0] == -1 && fg1 &&
			  !XwauWarheadPools_Any(fg1) && !XwauWarheadPools_Find(&pools, 2),
		  "warhead pools: keys in any case, the first line wins, -1 and other negative values are left out, "
		  "and a flight group with a leading 0 is never read");
	XwauWarheadPools_Free(&pools);
	snprintf(path, sizeof path, "%s/FlightModels/default.ini", game);
	write_file(path, "[WarheadTypeCount]\nWarheadTypeCount_fg_2_IonPulse = 3\n");
	check(XwauWarheadPools_Read(game, "Missions/v", false, &pools) && pools.count == 1 &&
			  pools.items[0].fg == 2 && pools.items[0].count[7] == 3,
		  "warhead pools: a mission without lines uses the global ones");
	XwauWarheadPools_Free(&pools);
	write_file(path, "");

	static const char* const kNames[]  = { "Red", "Blue", "Gold" };
	static const int         kCrafts[] = { 1, 2, 3 };
	snprintf(path, sizeof path, "%s/Missions/w.tie", game);
	write_tie(path, 3, kNames, kCrafts);
	XwauMissions* missions = XwauMissions_Load(game, false);
	check(missions && XwauMissionFiles_Write(game, missions, stock, out),
		  "warhead pools: mission file written");
	XwauMissions_Free(missions);
	size_t size = 0;
	snprintf(path, sizeof path, "%s/missions/w.yaml", out);
	char* text = ReadWholeFile(path, &size);
	check(text &&
			  strstr(text, "flight_groups:\n  0: { name: \"Red\", craft: 1, warheads: { missiles: 80, "
						   "torpedoes: 0 } }\n") &&
			  !strstr(text, "  1: {") && !strstr(text, "  7: {"),
		  "warhead pools: written in the flight group's entry, without the flight groups that the .TIE "
		  "doesn't have");
	free(text);
}

static void test_mission_settings(const char* scratch) {
	char game[1024], stock[1024], out[1024], path[1300], settings[1300];
	snprintf(game, sizeof game, "%s/ms_game", scratch);
	snprintf(stock, sizeof stock, "%s/ms_stock", scratch);
	snprintf(out, sizeof out, "%s/ms_out", scratch);
	snprintf(path, sizeof path, "%s/Missions", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/FlightModels", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/MISSIONS", stock);
	MakeDirs(path);
	static const char* const kNames[]   = { "Red", "Blue" };
	static const int         kCrafts[]  = { 1, 2 };
	const char* const        files[][2] = {
		{ "hook_mission_tie.dll", "" },
		{ "hook_engine_sound.dll", "" },
		/* The .cfg wins over Hooks.ini. */
		{ "Hook_Mission_Tie.cfg", "IsWarheadCollisionDamagesEnabled = 0\nTargetCraftKeyMethod = 1\n" },
		{ "Hooks.ini", "[hook_mission_tie]\nCanShootThroughtShieldOnHardDifficulty = 1\n" },
		{ "FlightModels/default.ini", "[Interdiction]\nRegion = 3\n" },
		{ "Missions/MISSION.LST", "//\n!BATTLE_0!"
								  "[Battle]\n//\n1\n* m1.tie\n!D1!Desc\n49\n* m49.tie\n"
								  "!D49!Desc\n50\nm50.tie\n!D50!Desc\n" },
		{ "Missions/m1.txt", "IsRedAlertEnabled = 1\nSkipObjectsMessagesIff = 255\n"
							 "ForcePlayerInTurretSeconds = 12\nKEY_O_TargetCraftFGs = 0x1, 5, abc\n"
							 "CampaignCraftsListLines = A, ,B\nCampaignCraftsListLinesTop = 140\t\t;top\n"
							 "isredalertenabled = 0\n" },
		{ "Missions/m1_Interdiction.txt", "Region = 1;2\n" },
	};
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		snprintf(path, sizeof path, "%s/%s", game, files[i][0]);
		write_file(path, files[i][1]);
	}
	snprintf(path, sizeof path, "%s/Missions/m1.tie", game);
	write_tie(path, 2, kNames, kCrafts);
	snprintf(path, sizeof path, "%s/Missions/m50.tie", game);
	write_tie(path, 2, kNames, kCrafts);
	/* m49 isn't in the mod, only in its list. */
	snprintf(path, sizeof path, "%s/MISSIONS/m49.tie", stock);
	write_tie(path, 2, kNames, kCrafts);

	snprintf(settings, sizeof settings, "%s/flight.yaml", scratch);
	check(XwauFlightSettings_Write(game, settings) &&
			  same_file(settings, "# Made by xwau_import missions.\nversion: 1\nrules:\n"
								  "  reset_combat_music_at_mission_load: true\n"
								  "  hangar_red_alert_any_mothership: true\nmission_settings:\n"
								  "  warhead_collision_damage: false\n  interdiction_regions: [3]\n"),
		  "mission settings: the hook's rules, and the global settings as defaults");

	XwauMissions* missions = XwauMissions_Load(game, false);
	check(missions && XwauMissionFiles_Write(game, missions, stock, out), "mission settings: files written");
	XwauMissions_Free(missions);
	size_t size = 0;
	snprintf(path, sizeof path, "%s/missions/m1.yaml", out);
	char* m1 = ReadWholeFile(path, &size);
	snprintf(path, sizeof path, "%s/missions/m49.yaml", out);
	char* m49 = ReadWholeFile(path, &size);
	snprintf(path, sizeof path, "%s/missions/m50.yaml", out);
	char* m50 = ReadWholeFile(path, &size);
	check(m1 && strstr(m1, "settings:\n  red_alert: true\n  skip_arrival_messages_iff: all\n"
						   "  force_player_in_turret: \"0:00:12\"\n"
						   "  target_key: { flight_groups: [1], method: cycle, only_not_inspected: false }\n"
						   "  order_of_battle:\n    lines:\n      - \"A\"\n      - \"\"\n      - \"B\"\n"
						   "    top: 140\n  interdiction_regions: [1, 2]\n"),
		  "mission settings: the first line wins, numbers are read as std::stoi reads them, a word of "
		  "spaces is an empty line, and flight groups that the .TIE doesn't have are left out");
	check(m49 && strstr(m49, "settings:\n  skip_hyperspaced_messages: true\n"),
		  "mission settings: mission 49 skips the \"entering area\" messages too, even when the mod doesn't "
		  "change it");
	check(m50 && strstr(m50, "settings:\n  damage_through_shields_on_hard: true\n"),
		  "mission settings: in mission 50, shots damage components through the shields on Hard");
	free(m1);
	free(m49);
	free(m50);
}

static void test_model_swaps(const char* scratch) {
	char game[1024], stock[1024], out[1024], path[1300];
	snprintf(game, sizeof game, "%s/sw_game", scratch);
	snprintf(stock, sizeof stock, "%s/sw_stock", scratch);
	snprintf(out, sizeof out, "%s/sw_mod/flight/missions", scratch);
	snprintf(path, sizeof path, "%s/Missions", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/FlightModels", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/sw_mod/flight/models", scratch);
	MakeDirs(path);
	MakeDirs(stock);
	const char* const files[][2] = {
		{ "FlightModels/Spacecraft0.LST", "FlightModels\\SuperStarDestroyer.opt\nFlightModels\\Tug.opt\n" },
		{ "FlightModels/SuperStarDestroyerRecentered.opt", "" },
		{ "FlightModels/OnlyOpt.opt", "" },
		{ "Missions/s.ini",
		  "[Objects]\nObjectProfile_fg_1 = Aeron\n"
		  "FlightModels\\SuperStarDestroyer.opt = FlightModels\\SuperStarDestroyerRecentered.opt\n"
		  "flightmodels\\superstardestroyer.OPT = FlightModels\\OnlyOpt.opt\n"
		  "FlightModels\\Unknown.opt = FlightModels\\SuperStarDestroyerRecentered.opt\n"
		  "FlightModels\\Tug.opt = FlightModels\\OnlyOpt.opt\n"
		  "FlightModels\\TugCockpit.opt = FlightModels\\Missing.opt\n"
		  "Models/X.opt = FlightModels\\SuperStarDestroyerRecentered.opt\n" },
	};
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		snprintf(path, sizeof path, "%s/%s", game, files[i][0]);
		write_file(path, files[i][1]);
	}
	snprintf(path, sizeof path, "%s/sw_mod/flight/models/superstardestroyerrecentered.glb", scratch);
	write_file(path, "");
	static const char* const kNames[]  = { "Red", "Blue" };
	static const int         kCrafts[] = { 1, 2 };
	snprintf(path, sizeof path, "%s/Missions/s.tie", game);
	write_tie(path, 2, kNames, kCrafts);
	XwauMissions* missions = XwauMissions_Load(game, false);
	check(missions && XwauMissionFiles_Write(game, missions, stock, out),
		  "model swaps: mission file written");
	XwauMissions_Free(missions);
	size_t size = 0;
	snprintf(path, sizeof path, "%s/missions/s.yaml", out);
	char* text = ReadWholeFile(path, &size);
	check(text &&
			  strstr(text, "model_swaps:\n  superstardestroyer: superstardestroyerrecentered\n"
						   "  unknown: superstardestroyerrecentered\n") &&
			  !strstr(text, "tug"),
		  "model swaps: the first line for a model wins, in any case, and swaps whose target doesn't exist "
		  "or has no GLB, or whose source the game never loads, are dropped");
	free(text);
}

static void test_craft_names(const char* scratch) {
	char game[1024], stock[1024], out[1024], path[1300];
	snprintf(game, sizeof game, "%s/cn_game", scratch);
	snprintf(stock, sizeof stock, "%s/cn_stock", scratch);
	snprintf(out, sizeof out, "%s/cn_mod/flight/missions", scratch);
	snprintf(path, sizeof path, "%s/Missions", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/FlightModels", game);
	MakeDirs(path);
	MakeDirs(stock);
	/* Records 70 and 71 have the models of rows 0 and 1, and craft type 26 is
	   object type 10, record 70. Record 72 has no object type. */
	FakeExe exe;
	fake_exe_init(&exe);
	fake_type(&exe, 10, 0, 70);
	fake_type(&exe, 11, 1, 71);
	put16(fake_at(&exe, 0x5B0F70 + 2 * 26), 10);
	fake_exe_write(&exe, game);
	const char* const files[][2] = {
		{ "FlightModels/Spacecraft0.LST",
		  "FlightModels\\ContainerBrick.opt\r\nFlightModels\\ContainerHexBox.opt\r\n" },
		{ "FlightModels/Equipment0.LST", "" },
		{ "Missions/c.ini", "[mission_tie]\ncraft, 70, name, ISD A\ncraft, 70, name, ISD B\n"
							"craft, 70, shortname, ISD\ncraft, 70, mapicon, 1, 2, 3, 4\n"
							"craft, 71, specname, Spec\ncraft, 72, name, Nothing\ncraft, 300, name, Far\n"
							"craft, 70, bogus, 5\n" },
	};
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		snprintf(path, sizeof path, "%s/%s", game, files[i][0]);
		write_file(path, files[i][1]);
	}
	static const char* const kNames[]  = { "Brick" };
	static const int         kCrafts[] = { 26 };
	snprintf(path, sizeof path, "%s/Missions/c.tie", game);
	write_tie(path, 1, kNames, kCrafts);
	XwauMissions* missions = XwauMissions_Load(game, false);
	check(missions && XwauMissionFiles_Write(game, missions, stock, out),
		  "craft names: mission file written");
	XwauMissions_Free(missions);
	size_t size = 0;
	snprintf(path, sizeof path, "%s/missions/c.yaml", out);
	char* text = ReadWholeFile(path, &size);
	check(
		text &&
			strstr(text,
				   "craft_names:\n"
				   "  containerbrick: { ship_list: \"ISD B\", short_name: \"ISD\", map_icon: [1, 2, 3, 4] }\n"
				   "  containerhexbox: { name: \"Spec\" }\n") &&
			!strstr(text, "Nothing") && !strstr(text, "Far"),
		"craft names: by model, the last line winning, without records that no object type uses or that "
		"XWAU ignores");
	free(text);
}

static void test_front_planet(const char* scratch) {
	char game[1024], stock[1024], out[1024], path[1300];
	snprintf(game, sizeof game, "%s/fp_game", scratch);
	snprintf(stock, sizeof stock, "%s/fp_stock", scratch);
	snprintf(out, sizeof out, "%s/fp_mod/flight/missions", scratch);
	snprintf(path, sizeof path, "%s/Missions", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/Resdata", game);
	MakeDirs(path);
	MakeDirs(stock);
	const char* const files[][2] = {
		{ "hook_concourse.dll", "" },
		{ "RESDATA.TXT", "Resdata\\FrontPlanet.dat\nResdata\\Missing.dat\n" },
		{ "Missions/a.ini",
		  "[concourse]\nfrontplanetindex = 15010\nFrontPlanetPositionX = -150\nFrontPlanetPositionY = 0x4E\n"
		  "FrontPlanetPositionY = 5\n" },
		{ "Missions/b_concourse.txt", "FrontPlanetIndex = 15115\n" },
		{ "Missions/b.ini", "[concourse]\nFrontPlanetPositionX = 5\n" },
		{ "Missions/c.ini", "[concourse]\nFrontPlanetIndex = -1\nFrontPlanetPositionX = abc\n" },
	};
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		snprintf(path, sizeof path, "%s/%s", game, files[i][0]);
		write_file(path, files[i][1]);
	}
	/* A DAT with groups 15010 (one image) and 15011 (none): the file header,
	   4 bytes, then a 12-byte header for each group. */
	uint8_t dat[22 + 2 * 12] = { 0x57, 0x23, 0x06, 0x57, 0x56, 0x23, 0x02, 0x56, 0, 0, 2 };
	put16(dat + 22, 15010);
	put16(dat + 24, 1);
	put16(dat + 34, 15011);
	snprintf(path, sizeof path, "%s/Resdata/FrontPlanet.dat", game);
	FILE* f = fopen(path, "wb");
	check(f && fwrite(dat, 1, sizeof dat, f) == sizeof dat && fclose(f) == 0, "front planet: DAT written");
	static const char* const kNames[]  = { "Red" };
	static const int         kCrafts[] = { 1 };
	for (const char* m = "abc"; *m; ++m) {
		snprintf(path, sizeof path, "%s/Missions/%c.tie", game, *m);
		write_tie(path, 1, kNames, kCrafts);
	}
	XwauMissions* missions = XwauMissions_Load(game, false);
	check(missions && XwauMissionFiles_Write(game, missions, stock, out),
		  "front planet: mission files written");
	XwauMissions_Free(missions);
	size_t size = 0;
	snprintf(path, sizeof path, "%s/missions/a.yaml", out);
	char* text = ReadWholeFile(path, &size);
	check(text && strstr(text, "concourse:\n  front_planet: { x: -150, y: 78, image: 15010 }\n"),
		  "front planet: the keys are found in any case, the first line wins and numbers are read in base 0");
	free(text);
	snprintf(path, sizeof path, "%s/missions/b.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "concourse:\n  front_planet: { image: 15115 }\n"),
		  "front planet: <mission>_concourse.txt wins over the .ini, and a group that no DAT has is kept");
	free(text);
	snprintf(path, sizeof path, "%s/missions/c.yaml", out);
	check(!IsFile(path), "front planet: -1 and values that aren't numbers aren't written");
}

static void test_flight_settings(const char* scratch) {
	char game[1024], path[1300], settings[1300];
	snprintf(game, sizeof game, "%s/fs_game", scratch);
	MakeDirs(game);
	snprintf(settings, sizeof settings, "%s/flight.yaml", scratch);
	check(XwauFlightSettings_Write(game, settings) &&
			  same_file(settings, "# Made by xwau_import missions.\nversion: 1\n"),
		  "flight settings: without the backdrop hook, only the version");
	snprintf(path, sizeof path, "%s/Hook_Backdrops.dll", game);
	write_file(path, "");
	check(XwauFlightSettings_Write(game, settings) &&
			  same_file(
				  settings,
				  "# Made by xwau_import missions.\nversion: 1\nbackdrops:\n  extra_planets:\n    - { ids: "
				  "[104, 255], first_group: 6304 }\n"),
		  "flight settings: the backdrop hook makes backdrop ids 104 to 255 planets");
	remove(path);
	snprintf(path, sizeof path, "%s/ddraw.dll", game);
	write_file(path, "");
	snprintf(path, sizeof path, "%s/SSAO.cfg", game);
	write_file(path, "ssao_enabled = 1\n;headlights_angle = 40\n headlights_col = 0.85, 0.85, 0.9\n"
					 "HEADLIGHTS_ANGLE = 25\nheadlights_pos = 1, 2\n");
	check(XwauFlightSettings_Write(game, settings) &&
			  same_file(settings,
						"# Made by xwau_import missions.\nversion: 1\nheadlights:\n  position_m: [0, 0, "
						"20]\n  color: [0.944, 0.944, 1]\n  power_w: 17671\n  angle_degrees: 25\n"
						"  auto_turn_on: true\n"),
		  "flight settings: the headlights of SSAO.cfg, as a tint and a power in watts that gives ddraw's "
		  "light at 50 m");
	write_file(path, "headlights_col = 0.85, 0.85, 0.9\n");
	check(XwauFlightSettings_Write(game, settings) &&
			  same_file(settings, "# Made by xwau_import missions.\nversion: 1\n"),
		  "flight settings: no headlights without ssao_enabled");
	snprintf(path, sizeof path, "%s/hook_opt_limit.dll", game);
	write_file(path, "MZ");
	check(XwauFlightSettings_Write(game, settings) &&
			  same_file(settings, "# Made by xwau_import missions.\nversion: 1\n"),
		  "flight settings: no engine glow rule from a hook_opt_limit.dll without the feature");
	write_file(path, "MZ EngineGlows.txt");
	check(XwauFlightSettings_Write(game, settings) &&
			  same_file(settings, "# Made by xwau_import missions.\nversion: 1\nrules:\n"
								  "  classic_hide_destroyed_engine_glows: true\n"),
		  "flight settings: hook_opt_limit.dll with [EngineGlows] hides the glows of destroyed meshes");
	remove(path);
	char hooks[1300];
	snprintf(path, sizeof path, "%s/hook_weapon_color.dll", game);
	write_file(path, "");
	snprintf(hooks, sizeof hooks, "%s/Hooks.ini", game);
	write_file(hooks, "[hook_weapon_color]\nWeaponImpactColor_3100 = 0xFF112233\n");
	size_t size = 0;
	char*  text = XwauFlightSettings_Write(game, settings) ? ReadWholeFile(settings, &size) : NULL;
	check(text && strstr(text, "weapons:\n  impact: {280: 0xff0000, 281: 0xff0000, 282: 0x00ff00, ") &&
			  strstr(text, "284: 0x112233") && strstr(text, "307: 0xffff00}\n"),
		  "flight settings: hook_weapon_color tints every impact, with the hook's defaults and Hooks.ini");
	check(text && !strstr(text, "weapon_iff_swap"),
		  "flight settings: no weapon switch rule from a hook_weapon_color.dll without the feature");
	free(text);
	write_file(path, "MZ WeaponSwitchBasedOnIff");
	text = XwauFlightSettings_Write(game, settings) ? ReadWholeFile(settings, &size) : NULL;
	check(text && strstr(text, "rules:\n  weapon_iff_swap: false\n"),
		  "flight settings: hook_weapon_color turns the weapon switch off by default");
	free(text);
	write_file(hooks, "[hook_weapon_color]\nWeaponSwitchBasedOnIff = 1\n");
	text = XwauFlightSettings_Write(game, settings) ? ReadWholeFile(settings, &size) : NULL;
	check(text && strstr(text, "rules:\n  weapon_iff_swap: true\n"),
		  "flight settings: WeaponSwitchBasedOnIff = 1 in Hooks.ini swaps every shot");
	free(text);
	remove(path);
	remove(hooks);
}

static void test_headlights(const char* scratch) {
	char game[1024], stock[1024], out[1024], path[1300];
	snprintf(game, sizeof game, "%s/hl_game", scratch);
	snprintf(stock, sizeof stock, "%s/hl_stock", scratch);
	snprintf(out, sizeof out, "%s/hl_mod/flight/missions", scratch);
	snprintf(path, sizeof path, "%s/Missions", game);
	MakeDirs(path);
	MakeDirs(stock);
	const char* const files[][2] = {
		{ "ddraw.dll", "" },
		{ "Missions/a.ini", "[Headlights]\nenabled = 0x1\n" },
		{ "Missions/b.ini", "[Headlights]\nEnabled = 0\n" },
		{ "Missions/c_Headlights.txt", "Enabled = 1\n" },
	};
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		snprintf(path, sizeof path, "%s/%s", game, files[i][0]);
		write_file(path, files[i][1]);
	}
	static const char* const kNames[]  = { "Red" };
	static const int         kCrafts[] = { 1 };
	for (const char* m = "abc"; *m; ++m) {
		snprintf(path, sizeof path, "%s/Missions/%c.tie", game, *m);
		write_tie(path, 1, kNames, kCrafts);
	}
	XwauMissions* missions = XwauMissions_Load(game, false);
	check(missions && XwauMissionFiles_Write(game, missions, stock, out),
		  "headlights: mission files written");
	XwauMissions_Free(missions);
	size_t size = 0;
	snprintf(path, sizeof path, "%s/missions/a.yaml", out);
	char* text = ReadWholeFile(path, &size);
	check(text && strstr(text, "\nheadlights: true\n"),
		  "headlights: [Headlights] Enabled, in any case and base 0");
	free(text);
	snprintf(path, sizeof path, "%s/missions/b.yaml", out);
	check(!IsFile(path), "headlights: Enabled = 0 writes nothing");
	snprintf(path, sizeof path, "%s/missions/c.yaml", out);
	check(!IsFile(path), "headlights: ddraw reads only the .ini");
}

static void test_stats_profiles(const char* scratch) {
	char game[1024], path[1300];
	snprintf(game, sizeof game, "%s/sp_game", scratch);
	snprintf(path, sizeof path, "%s/FlightModels", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/Missions", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/FlightModels/Frigate.ini", game);
	write_file(path,
			   "[StatsProfile_Default;]\nSpeed = 99\n[;StatsProfile_Default]\nSpeed = 99\n"
			   "[STATSPROFILE_Default]\nSpeed = 38\nAcceleration = 4\nPitch = 2\nExplosionStrength = 10\n"
			   "HullStrength = 29\nShieldStrength = 60\nSystemStrength = 60\nHasShieldGenerator = 2\n"
			   "HasHyperdrive = 0\nIsWarheadCollisionDamagesEnabled = 0\nSpeed = 50\nTurbo = 1\n"
			   "[StatsProfile_Default_Easy]\nHullStrength = 10\n"
			   "[StatsProfile_Fast]\nSpeed = 99\n[StatsModifiers]\nSpeed = 2\n");
	snprintf(path, sizeof path, "%s/FlightModels/FrigateStatsProfile_Fast.txt", game);
	write_file(path, "Speed = 55\n");
	size_t count = 0;
	snprintf(path, sizeof path, "%s/FlightModels", game);
	char**            names = ListDir(path, true, &count);
	XwauStatsProfiles profiles;
	check(XwauStatsProfiles_Read(game, "Frigate", names, count, 0, &profiles) && profiles.count == 3 &&
			  profiles.disabled == 2,
		  "stats profiles: three profiles, and the two disabled sections are counted");
	const XwauStatsProfile* d = &profiles.items[0];
	check(!strcmp(d->name, "default") && d->value[0] == 86 && d->value[1] == 9 && d->value[3] == 512 &&
			  d->value[6] == 1050 && d->value[7] == 3045 && d->value[9] == 3000 && d->value[8] == 60 &&
			  d->value[12] == 1 && d->value[13] == 0 && d->set[14] && d->value[14] == 0 && !d->set[10],
		  "stats profiles: the units of genus class 0, ExplosionStrength, the first line of a key, and the "
		  "flags");
	check(!strcmp(profiles.items[1].name, "default_easy") && profiles.items[1].set[7] &&
			  !profiles.items[1].set[0],
		  "stats profiles: a variant is a whole profile");
	check(!strcmp(profiles.items[2].name, "fast") && profiles.items[2].value[0] == 124,
		  "stats profiles: the .txt file wins over the section");
	XwauStatsProfiles_Free(&profiles);
	check(XwauStatsProfiles_Read(game, "Frigate", names, count, 2, &profiles) && profiles.count == 3 &&
			  profiles.items[0].value[7] == 190 && profiles.items[0].value[9] == 187 &&
			  profiles.items[0].value[6] == 65,
		  "stats profiles: hull, shields and the threshold / 16 for starships");
	XwauStatsProfiles_Free(&profiles);
	check(XwauStatsProfiles_Read(game, "Frigate", names, count, 1, &profiles) &&
			  profiles.items[0].value[7] == 761,
		  "stats profiles: / 4 for freighters");
	XwauStatsProfiles_Free(&profiles);
	check(XwauStatsProfiles_Read(game, "Frigate", names, count, -1, &profiles) && profiles.count == 1 &&
			  !strcmp(profiles.items[0].name, "fast"),
		  "stats profiles: without a genus, only the profiles without hull or shields are kept");
	char out[1300];
	snprintf(out, sizeof out, "%s/sp_model.yaml", scratch);
	FILE* f = fopen(out, "wb");
	XwauStatsProfiles_Write(f, &profiles);
	fclose(f);
	check(same_file(out, "stats_profiles:\n  \"fast\": {speed: 124}\n"), "stats profiles: the model group");
	XwauStatsProfiles_Free(&profiles);
	FreeList(names, count);

	XwauCraftShield shield;
	snprintf(path, sizeof path, "%s/FlightModels/Frigate.ini", game);
	write_file(path, "[Shield]\nUseGenerator = 1\nPerGeneratorRechargeRate = 5\nRechargeRate = 40\n"
					 "RechargeRateDelay = 30\n");
	XwauCraft_ReadShield(NULL, path, &shield);
	check(shield.has && shield.per_generator && shield.rate == 5 && shield.delay_seconds == 30 &&
			  shield.player_rate == 0,
		  "shield: UseGenerator = 1 takes the rate per generator, and the delay");
	char txt[1300];
	snprintf(txt, sizeof txt, "%s/FlightModels/FrigateShield.txt", game);
	write_file(txt, "UseGenerator = 2\nRechargeRate = 40\nPlayerRechargeRate = 60\n");
	XwauCraft_ReadShield(txt, path, &shield);
	check(
		shield.has && !shield.per_generator && shield.rate == 40 && shield.player_rate == 60 &&
			shield.delay_seconds == 0,
		"shield: <model>Shield.txt wins over the section, and UseGenerator other than 1 takes RechargeRate");
	write_file(path, "[Size]\nX = 1\n");
	XwauCraft_ReadShield(NULL, path, &shield);
	check(!shield.has, "shield: no section, no rate");

	snprintf(path, sizeof path, "%s/Missions/m1.ini", game);
	write_file(path,
			   "[StatsProfiles]\nFrigate_fgc_2 = Fast\nfrigate_FGC_2 = Default\nFrigate_fg_player = Fast\n"
			   "Frigate = Missing\t;comment\nFrigate_fg_0 = Fast\nFrigate_fgc_02 = Fast\n"
			   "ExplosionStrengthPercent = 50\nPlayerHullStrengthPercent = 400\nSpeedPercent = -5\n");
	XwauMissionStats stats;
	check(XwauMissionStats_Read(game, "Missions/m1", &stats) && stats.model_count == 1 &&
			  !strcmp(stats.models[0].key, "frigate") && stats.models[0].has_markings[2] &&
			  !strcmp(stats.models[0].markings[2], "fast") && !strcmp(stats.models[0].player, "fast") &&
			  !strcmp(stats.models[0].all, "missing ;comment") && !stats.models[0].has_markings[0],
		  "stats profiles: _fgc_N, _fg_player and the model's line; the first line of a key wins, and _fg_0, "
		  "_fgc_02 and a name with no profile");
	check(stats.percent[6] == 50 && stats.player_percent[7] == 400 && stats.percent[0] == -1,
		  "stats profiles: the percents, ExplosionStrengthPercent, and a negative percent left out");
	snprintf(out, sizeof out, "%s/sp_mission.yaml", scratch);
	f = fopen(out, "wb");
	XwauMissionStats_Write(f, &stats);
	fclose(f);
	check(
		same_file(out,
				  "stats_profiles:\n  models:\n    frigate: {player: \"fast\", markings: {2: \"fast\"}, all: "
				  "\"missing ;comment\"}\n  percent: {system_damage_hull_threshold: 50}\n"
				  "  player_percent: {hull: 400}\n"),
		"stats profiles: the mission section");
	XwauMissionStats_Free(&stats);

	char settings[1300];
	snprintf(path, sizeof path, "%s/hook_mission_tie.dll", game);
	write_file(path, "MZ StatsProfiles");
	snprintf(settings, sizeof settings, "%s/sp_flight.yaml", scratch);
	size_t size = 0;
	char*  text = XwauFlightSettings_Write(game, settings) ? ReadWholeFile(settings, &size) : NULL;
	check(text && strstr(text, "  ai_attack_uses_max_speed_cache: true\n") &&
			  !strstr(text, "shield_recharge"),
		  "stats profiles: hook_mission_tie with stats profiles compares the craft's own top speeds");
	free(text);
	snprintf(path, sizeof path, "%s/hook_shield.dll", game);
	write_file(path, "MZ RechargeRatePercent");
	snprintf(path, sizeof path, "%s/Hooks.ini", game);
	write_file(path, "[hook_shield]\nRechargeRatePercent = 50\n");
	snprintf(path, sizeof path, "%s/hook_shield.cfg", game);
	write_file(path, "IsShieldRechargeForStarshipsEnabled = 0\nIsShieldStrengthForStarfighterDoubled = 1\n");
	text = XwauFlightSettings_Write(game, settings) ? ReadWholeFile(settings, &size) : NULL;
	check(text && strstr(text, "shield_recharge:\n  starships: false\n  starships_on_easy: false\n"
							   "  double_starfighter_shields: true\n  rate_percent: 100\n"),
		  "shield: hook_shield's rules, from its .cfg over Hooks.ini");
	free(text);

	XwauCraftWeaponRates rates;
	snprintf(path, sizeof path, "%s/FlightModels/Frigate.ini", game);
	write_file(path, "[WeaponRate]\nDechargeRate = 2\nRechargeRate = 3\nCooldownTimeFactor = 94   ;47 = 1 s\n"
					 "DechargeRatePercent = -1\nRechargeRatePercent = -5\nDechargeRate = 9\n");
	XwauCraft_ReadWeaponRates(NULL, path, &rates);
	snprintf(out, sizeof out, "%s/wr_model.yaml", scratch);
	f = fopen(out, "wb");
	XwauCraft_WriteWeaponRates(f, "", &rates);
	fclose(f);
	check(
		same_file(out, "rates: {decharge: 2, recharge: 3, cooldown: 94}\n"),
		"weapon rates: the model's keys; the first line wins, and -1 and other negative values are left out");
	snprintf(txt, sizeof txt, "%s/FlightModels/FrigateWeaponRate.txt", game);
	write_file(txt, "RechargeRatePercent = 50\n");
	XwauCraft_ReadWeaponRates(txt, path, &rates);
	check(rates.value[0] == -1 && rates.value[4] == 50,
		  "weapon rates: <model>WeaponRate.txt wins over the section");
	write_file(path, "[Size]\nX = 1\n");
	XwauCraft_ReadWeaponRates(NULL, path, &rates);
	check(!XwauCraft_HasWeaponRates(&rates), "weapon rates: no section, no rates");
	write_file(path,
			   "[WeaponRate]\nEnergyTransferRate = 32\nIsImpactSpinningEnabled = 2\nMaxSystemDamages = 500\n"
			   "Weapon296_MagPulse_Damages = 10\nMaxTorpedoCountPerPass = -3\n");
	XwauCraft_ReadWeaponRates(NULL, path, &rates);
	f = fopen(out, "wb");
	XwauCraft_WriteWeaponRates(f, "  ", &rates);
	fclose(f);
	check(same_file(out, "  energy_transfer: {rate: 32}\n  impact_spin: {enabled: true}\n"
						 "  system_damage: {max: 500, 296: 10}\n"),
		  "weapon rates: the other keys go into their groups, and any spin value but 0 turns it on");
	write_file(path, "[WeaponRate]\nEnergyTransferRate = 32\n");
	XwauCraft_ReadWeaponRates(NULL, path, &rates);
	check(XwauCraft_HasWeaponRates(&rates) && !XwauCraft_HasDefaultFirstRates(&rates),
		  "weapon rates: XWAU reads the energy transfer keys of a model first");

	XwauCraftComponents components;
	write_file(path,
			   "[SubcomponentsStrength]\n;All 32 lines\n-1 ;Default\n-1\n-1\n-1\n18 ;GunTurret\n300\n0\n");
	XwauCraft_ReadComponents(NULL, path, &components);
	check(
		components.has && components.strength[0] == -1 && components.strength[4] == 18 &&
			components.strength[5] == -1 && components.strength[6] == 0,
		"component strength: one line by mesh type; -1 keeps the game's value, and a value past 255 is left "
		"out");
	write_file(path, "[SubcomponentsStrength]\n18\nGunTurret = 4\n");
	XwauCraft_ReadComponents(NULL, path, &components);
	check(!components.has, "component strength: a line that isn't a number leaves out the section");

	XwauCraftWarheadLock lock;
	write_file(path, "[WarheadLock]\n;Lock time\nLockTime = 354\t\t;1.5 seconds\nLockStrengthDivisor = 118\n"
					 "//Player range\nlockdistance0 = 102400 ;2.5km\nLockDistance2 = 0x18DAD\n"
					 "AILockDistance2 = 101805\nAILockTimeFactor = 400\n");
	XwauCraft_ReadWarheadLock(NULL, path, &lock);
	check(lock.has && lock.value[0] == 102400 && lock.value[1] == -1 && lock.value[2] == 101805 &&
			  lock.value[3] == 354 && lock.value[4] == 118 && lock.value[5] == -1 &&
			  lock.value[7] == 101805 && lock.value[8] == 400,
		  "warhead lock: each key, in any case, with comments after the values");
	f = fopen(out, "wb");
	XwauCraft_WriteWarheadLock(f, &lock);
	fclose(f);
	check(same_file(out,
					"warhead_lock:\n  lock_range: {small: 102400, transport: 101805}\n  lock_ticks: 354\n"
					"  ticks_per_homing_level: 118\n  ai_lock_ticks_per_tier: 400\n"
					"  ai_lock_range: {big_top_tier: 101805}\n"),
		  "warhead lock: the keys go into the warhead_lock group");
	write_file(path, "[WarheadLock]\nLockDistance0 = 90000\nLockStrengthDivisor = 0\nLockTime = 150\n"
					 "LockTime_Fast = 300\n");
	XwauCraft_ReadWarheadLock(NULL, path, &lock);
	check(lock.has && lock.value[0] == 90000 && lock.value[3] == 708 && lock.value[4] == -1,
		  "warhead lock: values that would crash XWAU are left out, and a section without a usable LockTime "
		  "gets 708, even for the Missile Boat");
	write_file(path, "[Shield]\nRechargeRate = 3\n");
	XwauCraft_ReadWarheadLock(NULL, path, &lock);
	check(!lock.has && lock.value[3] == -1, "warhead lock: no section keeps the game's values");
	write_file(txt, "LockStrengthDivisor = 59\n");
	XwauCraft_ReadWarheadLock(txt, path, &lock);
	check(lock.has && lock.value[4] == 59 && lock.value[3] == 708,
		  "warhead lock: <model>WarheadLock.txt wins over the section");

	snprintf(path, sizeof path, "%s/FlightModels/default.ini", game);
	write_file(path, "[WeaponRate]\nIsWarheadLockHookEnabled = 0\n");
	check(
		XwauCraft_ReadGlobalWeaponRates(game, &rates) && !XwauCraft_HasWeaponRates(&rates),
		"weapon rates: a global section without values still has lines, so XWAU ignores the models' values");
	write_file(path, "[WeaponRate]\nDechargeRate = 3\n");
	snprintf(path, sizeof path, "%s/hook_weapon_rate.dll", game);
	write_file(path, "MZ DechargeRatePercent _EnergyLowHighSeparation Craft_Offset_292");
	snprintf(path, sizeof path, "%s/Hooks.ini", game);
	write_file(path,
			   "[hook_shield]\nRechargeRatePercent = 50\n[hook_weapon_rate]\nDechargeRatePercent = 50\n");
	text = XwauFlightSettings_Write(game, settings) ? ReadWholeFile(settings, &size) : NULL;
	check(text && strstr(text, "  rates: {decharge: 3}\n  decharge_percent: 50\n  recharge_percent: 100\n") &&
			  strstr(text,
					 "  fire_needs_decharge_rate: true\n  tie_fighter_energy_step_always_high: true\n"
					 "  player_decharge_lasers_only: true\n  warhead_collision_bonus_any_target: true\n"
					 "  gun_turret_hardpoints: true\n  no_starship_fire_from_destroyed_racks: true\n") &&
			  !strstr(text, "no_fire_from_destroyed_hardpoints") && !strstr(text, "fire_ratio_by_rank"),
		  "weapon rates: the global rates, hook_weapon_rate's percents from Hooks.ini, and its rules");
	free(text);
	snprintf(path, sizeof path, "%s/hook_weapon_rate.dll", game);
	write_file(path, "MZ _EnergyLowHighSeparation EnableFireMeshFilter WeaponHardpointTypes "
					 "WarheadCapacity_00460904_Hook NoviceRankFireratioMultiplicator "
					 "DamagesMultiplicatorForDifficulty0 PreventAILasersIonWhenDisabling "
					 "PreventAIIonWhenNotDisabling PreventPlayerLinkedLasersIon");
	snprintf(path, sizeof path, "%s/Hooks.ini", game);
	write_file(path,
			   "[hook_weapon_rate]\nEnableFireMeshFilter = 0\nNoviceRankFireratioMultiplicator = 2.5 ;x\n"
			   "DamagesMultiplicatorForDifficulty2 = 0.15\nPreventAIIonWhenNotDisabling = 1\n");
	text = XwauFlightSettings_Write(game, settings) ? ReadWholeFile(settings, &size) : NULL;
	check(text && !strstr(text, "no_fire_from_destroyed_hardpoints") &&
			  strstr(text, "  turbolaser_hardpoint_types: true\n  rearm_follows_both_statuses: true\n") &&
			  strstr(text, "  fire_ratio_by_rank: [2.5, 3, 2, 2, 1, 1]\n"
						   "  damage_by_difficulty: [0.5, 0.25, 0.150000006]\n"
						   "  linking_rules: {prevent_ai_ion_when_not_disabling: true}\n"),
		  "weapon profiles: the hook's rules and settings, with the float32 factors");
	free(text);
	snprintf(path, sizeof path, "%s/FlightModels/default.ini", game);
	write_file(path, "[WeaponRate]\nDechargeRate = 3\nMaxTorpedoCountPerTarget = 9\n");
	snprintf(path, sizeof path, "%s/hook_weapon_rate.dll", game);
	write_file(path, "MZ MaxTorpedoCountPerPass MaxTorpedoCountPerTarget WarheadTypeCount_fg_");
	snprintf(path, sizeof path, "%s/Hooks.ini", game);
	write_file(path, "[hook_weapon_rate]\nMaxTorpedoCountPerPass = 2\nMaxTorpedoCountPerTarget = 6\n");
	text = XwauFlightSettings_Write(game, settings) ? ReadWholeFile(settings, &size) : NULL;
	check(text && strstr(text, "  rates: {decharge: 3}\n  ai_torpedoes: {per_pass: 2, per_target: 9}\n") &&
			  strstr(text, "  no_player_warhead_slot_cap: true\n") &&
			  !strstr(text, "fire_needs_decharge_rate"),
		  "weapon rates: the hook's torpedo limits when the global [WeaponRate] doesn't set them, and the "
		  "rule of the warhead pools");
	free(text);
	snprintf(path, sizeof path, "%s/FlightModels/WeaponRate.txt", game);
	write_file(path, "CooldownTimeFactor = 30\n");
	check(XwauCraft_ReadGlobalWeaponRates(game, &rates) && rates.value[0] == -1 && rates.value[2] == 30,
		  "weapon rates: FlightModels/WeaponRate.txt wins over default.ini");
}

/* ---- Sound banks ----------------------------------------------------------- */

/* A WAV file: a fmt chunk of `fmt_size` bytes and a data chunk. */
static size_t make_wav(uint8_t* out, const uint8_t* fmt, size_t fmt_size, const void* data, size_t size) {
	const size_t total = 12 + 8 + fmt_size + 8 + size;
	memcpy(out, "RIFF", 4);
	const uint32_t riff = (uint32_t)(total - 8), f = (uint32_t)fmt_size, d = (uint32_t)size;
	memcpy(out + 4, &riff, 4);
	memcpy(out + 8, "WAVEfmt ", 8);
	memcpy(out + 16, &f, 4);
	memcpy(out + 20, fmt, fmt_size);
	memcpy(out + 20 + fmt_size, "data", 4);
	memcpy(out + 24 + fmt_size, &d, 4);
	memcpy(out + 28 + fmt_size, data, size);
	return total;
}

/* A fmt chunk: tag, channels, rate, block align and bits. */
static void make_fmt(uint8_t* fmt, uint16_t tag, uint16_t channels, uint16_t align, uint16_t bits) {
	const uint32_t rate = 22050, bytes = rate * align;
	memcpy(fmt, &tag, 2);
	memcpy(fmt + 2, &channels, 2);
	memcpy(fmt + 4, &rate, 4);
	memcpy(fmt + 8, &bytes, 4);
	memcpy(fmt + 12, &align, 2);
	memcpy(fmt + 14, &bits, 2);
}

/* The 16-bit samples of a converted file. */
static bool wav_samples(const uint8_t* wav, size_t size, uint16_t channels, const int16_t* expected,
						size_t count) {
	uint16_t tag, wav_channels, bits;
	memcpy(&tag, wav + 20, 2);
	memcpy(&wav_channels, wav + 22, 2);
	memcpy(&bits, wav + 34, 2);
	if (size != 44 + count * 2 || tag != 1 || wav_channels != channels || bits != 16)
		return false;
	for (size_t i = 0; i < count; ++i) {
		int16_t sample;
		memcpy(&sample, wav + 44 + i * 2, 2);
		if (sample != expected[i])
			return false;
	}
	return true;
}

static void test_sound_banks(const char* scratch) {
	XwauSoundLine line;
	check(XwauSoundBanks_ParseLine("Unused\t\t\t76 2000 ; Unused\r\n", &line) &&
			  strcmp(line.name, "Unused") == 0 && line.volume == 76 && line.distance == 8192,
		  "sound banks: a line's file name, decimal volume and hex distance, cut at ';'");
	check(XwauSoundBanks_ParseLine("CanopyOpen.wav\r\n", &line) && strcmp(line.name, "CanopyOpen.wav") == 0 &&
			  line.volume == 127 && line.distance == 8192,
		  "sound banks: a line without a volume or a distance takes 127 and 8192");
	check(XwauSoundBanks_ParseLine("a.wav  300\t1F0a", &line) && line.volume == 44 &&
			  line.distance == 0x1f0a && XwauSoundBanks_ParseLine("a.wav 90 1E00", &line) &&
			  line.distance == 1,
		  "sound banks: the volume keeps its low byte, and the hex reader stops at A to E");
	check(XwauSoundBanks_ParseLine("a.wav ; comment", &line) && line.volume == 0 && line.distance == 8192,
		  "sound banks: a space before ';' gives the volume 0, as in XWAU");
	check(!XwauSoundBanks_ParseLine("", &line) && !XwauSoundBanks_ParseLine("\r\n", &line) &&
			  !XwauSoundBanks_ParseLine("  a.wav 10", &line) && !XwauSoundBanks_ParseLine("; a.wav", &line) &&
			  XwauSoundBanks_ParseLine("NoExtension 10", &line) && strcmp(line.name, "NoExtension") == 0,
		  "sound banks: a line without a file name takes no slot; a name needs no extension");

	/* The WAV files. */
	uint8_t fmt[64], wav[512], *out = NULL;
	size_t  out_size = 0;
	uint8_t pcm24[]  = { 0x00, 0x34, 0x12, 0xff, 0xff, 0xff };
	uint8_t quad[16];
	int16_t quad_samples[4] = { 100, 200, 300, 400 };
	memcpy(quad, quad_samples, 8);
	memcpy(quad + 8, quad_samples, 8);
	make_fmt(fmt, 1, 1, 2, 16);
	check(XwauSoundBanks_ConvertWav(wav, make_wav(wav, fmt, 16, pcm24, 4), &out, &out_size) ==
				  XWAU_WAV_PLAYABLE &&
			  !out,
		  "sound banks: a 16-bit PCM file is copied as it is");
	make_fmt(fmt, 1, 2, 6, 24);
	const int16_t stereo[] = { 0x1234, -1 };
	check(XwauSoundBanks_ConvertWav(wav, make_wav(wav, fmt, 16, pcm24, 6), &out, &out_size) ==
				  XWAU_WAV_CONVERTED &&
			  wav_samples(out, out_size, 2, stereo, 2),
		  "sound banks: a 24-bit file is converted to 16 bits");
	free(out);
	make_fmt(fmt, 1, 4, 8, 16);
	const int16_t mixed[] = { 200, 300, 200, 300 };
	check(XwauSoundBanks_ConvertWav(wav, make_wav(wav, fmt, 16, quad, 16), &out, &out_size) ==
				  XWAU_WAV_CONVERTED &&
			  wav_samples(out, out_size, 2, mixed, 4),
		  "sound banks: channels past the second are mixed into the first two");
	free(out);
	/* MS ADPCM, mono: 4 samples by block, the first coefficient pair. */
	make_fmt(fmt, 2, 1, 8, 4);
	const uint16_t extra[] = {
		32, 4, 7, 256, 0, 512, (uint16_t)-256, 0, 0, 192, 64, 240, 0, 460, (uint16_t)-208, 392, (uint16_t)-232
	};
	memcpy(fmt + 16, extra, sizeof extra);
	const uint8_t adpcm[]   = { 0, 16, 0, 100, 0, 50, 0, 0x10 };
	const int16_t decoded[] = { 50, 100, 116, 116 };
	check(XwauSoundBanks_ConvertWav(wav, make_wav(wav, fmt, 16 + sizeof extra, adpcm, sizeof adpcm), &out,
									&out_size) == XWAU_WAV_CONVERTED &&
			  wav_samples(out, out_size, 1, decoded, 4),
		  "sound banks: an MS ADPCM file is decoded");
	free(out);

	/* An installation with two banks. */
	char game[1024], path[1200], files[1024], flight[1024];
	snprintf(game, sizeof game, "%s/banks", scratch);
	snprintf(files, sizeof files, "%s/banks_game", scratch);
	snprintf(flight, sizeof flight, "%s/banks_flight", scratch);
	snprintf(path, sizeof path, "%s/Wave/Sfx_Interior", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/Wave/Sfx_Interior.lst", game);
	write_file(path,
			   "Unused\t76 2000\r\n\r\nEngine.wav 76 2000 ; X-wing\r\nMissing.wav\r\nDeep.WAV\r\n   \r\n");
	make_fmt(fmt, 1, 1, 2, 16);
	const size_t engine_size = make_wav(wav, fmt, 16, pcm24, 4);
	snprintf(path, sizeof path, "%s/Wave/Sfx_Interior/ENGINE.wav", game);
	WriteWholeFile(path, wav, engine_size);
	make_fmt(fmt, 1, 1, 3, 24);
	snprintf(path, sizeof path, "%s/Wave/Sfx_Interior/deep.wav", game);
	WriteWholeFile(path, wav, make_wav(wav, fmt, 16, pcm24, 6));
	snprintf(path, sizeof path, "%s/Wave/Sfx_Interior/Unused.wav", game);
	write_file(path, "not referenced");
	snprintf(path, sizeof path, "%s/Wave/Sfx_Weapon", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/Wave/Sfx_Weapon/Laser.wav", game);
	make_fmt(fmt, 1, 1, 2, 16);
	WriteWholeFile(path, wav, make_wav(wav, fmt, 16, pcm24, 4));
	snprintf(path, sizeof path, "%s/Wave/Sfx_Weapon.lst", game);
	write_file(path, "Unused\nUnused\nUnused\nUnused\nUnused\nUnused\nUnused\nUnused\nUnused\nUnused\n\n"
					 "Unused\nLaser.wav 70 2800\n");
	snprintf(path, sizeof path, "%s/Wave/Sfx_Other.lst", game);
	write_file(path, "Laser.wav\n");
	snprintf(path, sizeof path, "%s/Wave/Sfx_SFoil.lst", game);
	write_file(path, "Laser.wav\n");
	check(XwauSoundBanks_Write(game, flight, files) && !IsFile(flight),
		  "sound banks: nothing is written without hook_sounds_count.dll");
	snprintf(path, sizeof path, "%s/hook_sounds_count.dll", game);
	write_file(path, "");
	snprintf(path, sizeof path, "%s/hook_engine_sound.dll", game);
	write_file(path, "");
	snprintf(path, sizeof path, "%s/sounds.yaml", flight);
	size_t size = 0;
	char*  text = XwauSoundBanks_Write(game, flight, files) ? ReadWholeFile(path, &size) : NULL;
	check(text &&
			  strstr(text,
					 "  interior: # Wave/Sfx_Interior.lst, by object type\n    count: 4\n    sounds:\n"
					 "      1: {file: \"Wave\\\\Sfx_Interior\\\\Engine.wav\", volume: 76, distance: 8192}\n"
					 "      3: {file: \"Wave\\\\Sfx_Interior\\\\Deep.WAV\", volume: 127, distance: 8192}\n"),
		  "sound banks: blank lines take no slot; Unused and missing files are silent; count is XWAU's line "
		  "count");
	check(text &&
			  strstr(text, "  weapon: # Wave/Sfx_Weapon.lst, 10 by object type\n    count: 12\n    sounds:\n"
						   "      1:\n        1: {file: \"Wave\\\\Sfx_Weapon\\\\Laser.wav\", volume: 70, "
						   "distance: 10240}\n") &&
			  !strstr(text, "other") && !strstr(text, "s_foil"),
		  "sound banks: 10 entries by object type; a bank that no hook reads, or whose hook isn't installed, "
		  "is "
		  "left out");
	free(text);
	snprintf(path, sizeof path, "%s/Wave/Sfx_Interior/ENGINE.wav", files);
	const bool engine = IsFile(path);
	snprintf(path, sizeof path, "%s/Wave/Sfx_Interior/deep.wav", files);
	text = ReadWholeFile(path, &size);
	check(engine && text && size == 44 + 4 && text[34] == 16,
		  "sound banks: the files keep their own names; a 24-bit file is converted");
	free(text);
	snprintf(path, sizeof path, "%s/Wave/Sfx_Interior/Unused.wav", files);
	check(!IsFile(path), "sound banks: only the files that entries use are copied");

	/* A model's [Sound] section. */
	XwauCraftSound sound;
	char           sound_txt[1100];
	snprintf(path, sizeof path, "%s/Sound.ini", scratch);
	write_file(path, "[Sound]\nEngineSoundInterior = 3\t\t\t\nEngineSoundFlyBy = 28\nEngineSoundWash = 5\n"
					 "EngineSoundTakeOff = 2\nWeaponSoundBehavior = 0\nHyperSoundBehavior = 0\n"
					 "WeaponSoundRangeStart_283 = 2\nWeaponSoundRangeCount_283 = 3\n"
					 "WeaponSoundRangeStart_289 = 10\nWeaponSoundRangeCount_289 = 3\n"
					 "WeaponSoundRangeStart_290 = 0\nWeaponSoundRangeCount_290 = 0\n"
					 "WeaponSoundRangeStart_303 = 1\nEngineSlowing = 0\n");
	XwauCraft_ReadSound(NULL, path, 12, &sound);
	check(sound.interior == 3 && sound.fly_by == 8 && sound.fly_by_mode == 2 && sound.wash == 1 &&
			  sound.take_off == 2 && sound.weapons == 0 && sound.hyper == 0 && sound.engine_slowing,
		  "craft sounds: the engine types, a 3D fly-by, WeaponSoundBehavior 0 as plain and "
		  "HyperSoundBehavior 0");
	check(sound.range_first[3] == 2 && sound.range_count[3] == 3 && sound.range_count[9] == 0 &&
			  sound.range_count[10] == 0 && sound.range_count[23] == 0,
		  "craft sounds: a range past the bank, a count of 0 and a range without its count are left out");
	snprintf(sound_txt, sizeof sound_txt, "%s/SoundSound.txt", scratch);
	write_file(sound_txt, "; comment\nEngineSoundInterior = 0\nWeaponSoundBehavior = TieFighterWithIon\n"
						  "HyperSoundBehavior = -1\n");
	XwauCraft_ReadSound(sound_txt, path, 12, &sound);
	check(sound.interior == 0 && sound.fly_by == 0 && sound.weapons == 3 && sound.hyper == -1 &&
			  XwauCraft_HasSound(&sound),
		  "craft sounds: <model>Sound.txt wins; 0 and -1 mean the bank");
	write_file(sound_txt, "; only a comment\n");
	write_file(path, "[Sound]\n; EngineSoundInterior = 3\n[Other]\nEngineSoundInterior = 3\n");
	XwauCraft_ReadSound(sound_txt, path, 12, &sound);
	check(!XwauCraft_HasSound(&sound), "craft sounds: a section with only comments sets nothing");

	/* The model file of an installation with hook_engine_sound.dll. */
	snprintf(path, sizeof path, "%s/Wave/Sfx_WeaponRange.lst", game);
	write_file(path, "a.wav\nb.wav\n\nc.wav\n");
	check(XwauSoundBanks_LineCount(game, "WeaponRange") == 3 &&
			  XwauSoundBanks_LineCount(game, "Missing") == 0,
		  "craft sounds: the WeaponRange bank's count");
	snprintf(path, sizeof path, "%s/FlightModels", game);
	MakeDirs(path);
	snprintf(path, sizeof path, "%s/FlightModels/Tug.ini", game);
	write_file(path, "[Sound]\nEngineSoundInterior = 9\nWeaponSoundBehavior = Empire\n"
					 "WeaponSoundRangeStart_283 = 1\nWeaponSoundRangeCount_283 = 2\n");
	snprintf(path, sizeof path, "%s/hook_engine_sound.dll", game);
	remove(path);
	snprintf(path, sizeof path, "%s/models/tug.yaml", flight);
	check(XwauCraft_Write(game, flight, NULL) && !IsFile(path),
		  "craft sounds: nothing without hook_engine_sound.dll");
	snprintf(path, sizeof path, "%s/hook_engine_sound.dll", game);
	write_file(path, "");
	snprintf(path, sizeof path, "%s/models/tug.yaml", flight);
	text = XwauCraft_Write(game, flight, NULL) ? ReadWholeFile(path, &size) : NULL;
	check(text && strstr(text, "sound:\n  weapons: empire\n  weapon_ranges: {283: {first: 1, count: 2}}\n") &&
			  !strstr(text, "interior"),
		  "craft sounds: the model file's sound group; an interior type that plays nothing is left out");
	free(text);

	/* The ambient sound. */
	snprintf(path, sizeof path, "%s/FlightModels/Tug.ini", game);
	write_file(path,
			   "[Sound]\nAmbientSoundOffsetX = 0\nAmbientSoundOffsetY = 300\nAmbientSoundDistance = 1000\n");
	snprintf(path, sizeof path, "%s/models/tug.yaml", flight);
	text = XwauCraft_Write(game, flight, NULL) ? ReadWholeFile(path, &size) : NULL;
	check(text && strstr(text, "  ambient: {offset_m: [0, 300, 0], distance_m: 1000}\n"),
		  "ambient sounds: the offset (right, up, forward) and one distance");
	free(text);
	snprintf(path, sizeof path, "%s/FlightModels/Tug.ini", game);
	write_file(path, "[Sound]\nAmbientSoundDistanceX = 4000\nAmbientSoundDistanceY = 11000\n"
					 "AmbientSoundDistanceZ = 0\n");
	snprintf(path, sizeof path, "%s/models/tug.yaml", flight);
	text = XwauCraft_Write(game, flight, NULL) ? ReadWholeFile(path, &size) : NULL;
	check(text && strstr(text, "  ambient: {distance_m: [4000, null, 11000]}\n") && !strstr(text, "offset_m"),
		  "ambient sounds: DistanceX, Z and Y are right, up and forward; a distance of 0 without "
		  "AmbientSoundDistance is the craft's size");
	free(text);
	snprintf(path, sizeof path, "%s/FlightModels/Tug.ini", game);
	write_file(path, "[Sound]\nAmbientSoundDistance = 0\nAmbientSoundDistanceY = 500\n");
	snprintf(path, sizeof path, "%s/models/tug.yaml", flight);
	text = XwauCraft_Write(game, flight, NULL) ? ReadWholeFile(path, &size) : NULL;
	check(text && strstr(text, "  ambient: {distance_m: [1, 1, 500]}\n"),
		  "ambient sounds: an AmbientSoundDistance of 0 is 1, and the other axes take it");
	free(text);

	/* The starship_ambient_sounds rule. */
	char settings[1100];
	snprintf(settings, sizeof settings, "%s/ambient_flight.yaml", scratch);
	snprintf(path, sizeof path, "%s/hook_engine_sound.dll", game);
	write_file(path, "AreStarshipAmbientEnabled");
	text = XwauFlightSettings_Write(game, settings) ? ReadWholeFile(settings, &size) : NULL;
	check(text && strstr(text, "  starship_ambient_sounds: true\n"),
		  "ambient sounds: hook_engine_sound.dll with them gives the rule");
	free(text);
	snprintf(path, sizeof path, "%s/Hooks.ini", game);
	write_file(path, "[hook_engine_sound]\nAreStarshipAmbientEnabled = 0\n");
	text = XwauFlightSettings_Write(game, settings) ? ReadWholeFile(settings, &size) : NULL;
	check(!text || !strstr(text, "starship_ambient_sounds"),
		  "ambient sounds: AreStarshipAmbientEnabled = 0 in Hooks.ini turns the rule off");
	free(text);
}

int main(void) {
	char        scratch[1024];
	const char* tmp = getenv("TMPDIR");
	snprintf(scratch, sizeof scratch, "%s/xwau_rules_test", tmp && tmp[0] ? tmp : "/tmp");
	RemoveTree(scratch);
	MakeDirs(scratch);
	test_rules(scratch);
	test_skins(scratch);
	test_skybox_rotation();
	test_skyboxes(scratch);
	test_craft(scratch);
	test_model(scratch);
	test_text(scratch);
	test_objects(scratch);
	test_game_files(scratch);
	test_mission_files(scratch);
	test_flight_settings(scratch);
	test_mission_settings(scratch);
	test_model_swaps(scratch);
	test_craft_names(scratch);
	test_front_planet(scratch);
	test_headlights(scratch);
	test_mission_tie(scratch);
	test_warhead_pools(scratch);
	test_gunner_seats(scratch);
	test_weapon_stats(scratch);
	test_stats_profiles(scratch);
	test_sound_banks(scratch);
	RemoveTree(scratch);
	printf("%s\n", g_failures ? "FAILED" : "passed");
	return g_failures ? 1 : 0;
}
