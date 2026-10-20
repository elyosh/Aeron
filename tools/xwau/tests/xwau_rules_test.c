/*
 * Tests the hook file rules that the XWAU readers reproduce (xwau_config.h),
 * the skin selection (xwau_skins.h), the skybox conversion (xwau_skyboxes.h),
 * the craft settings (xwau_craft.h), the text overrides (xwau_text.h) and the
 * object files (xwau_objects.h). The inputs are fixed texts and folders written
 * to a scratch folder. The test is built with the tools as xwau_rules_test. It
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
#include "xwau_missions.h"
#include "xwau_objects.h"
#include "xwau_skins.h"
#include "xwau_skyboxes.h"
#include "xwau_text.h"

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
	Xwau_Tokenize(" 1, ;2 ;", XWAU_RULES_CPP, &tokens);
	check(tokens.count == 2 && strcmp(tokens.items[1], "2") == 0, "C++ tokens: empty ones dropped");
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
	int written = snprintf(out, size, "    overlay:\n      cube: \"flight/environments/xwau/%s/overlay.ktx2\"\n", folder);
	if (xyz[0] != 0.0f || xyz[1] != 0.0f || xyz[2] != 0.0f) {
		float ypr[3];
		XwauSkyboxes_Rotation(xyz, true, ypr);
		snprintf(out + written, size - (size_t)written, "      rotation_degrees: [%g, %g, %g]\n", (double)ypr[0],
				 (double)ypr[1], (double)ypr[2]);
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
		{ "m5", "[SkyBoxes]\nRegion0 = Effects\\CubeMaps\\Gamma\nRegion0Overlay = Effects\\CubeMaps\\Rocks\n" },
	};
	for (size_t m = 0; m < sizeof kMissions / sizeof kMissions[0]; ++m) {
		snprintf(path, sizeof path, "%s/Missions/%s.ini", game, kMissions[m][0]);
		write_file(path, kMissions[m][1]);
		snprintf(path, sizeof path, "%s/Missions/%s.tie", stock, kMissions[m][0]);
		write_file(path, "");
	}
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
	if (m4 && m5) {
		char              lines[256];
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
				strstr(m1, "\"0\":\n          environment: xwau.beta\n          backdrops: keep"),
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
			  !strstr(list, "gamma") &&
			  strstr(list, "\noverlay\t") &&
			  strstr(list, "/Effects/CubeMaps/Rocks\t-\tflight/environments/xwau/rocks\n") &&
			  !strstr(list, "\txwau.rocks"),
		  "skyboxes: the folder list names the sky and overlay folders that load");
	free(text);
	free(list);
}

/* ---- Craft settings ----------------------------------------------------------- */

static void test_craft(const char* scratch) {
	check(XwauCraft_HudColor(0.1f, 0.1f, 0.5f) == 0xff19197fu && XwauCraft_HudColor(1.0f, 0.0f, 0.36f) == 0xffff005bu,
		  "HUD colours: 0.1 becomes 0x19, and the products are truncated");

	char path[1024], txt[1024];
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

	uint16_t* list = NULL;
	size_t count = 0;
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
	snprintf(path, sizeof path, "%s/HullIconList.txt", root);
	write_file(path, "0\n100\n");
	check(XwauCraft_Write(root, out), "craft: the files are written");
	size_t size = 0;
	snprintf(path, sizeof path, "%s/craft/awing.yaml", out);
	char* text = ReadWholeFile(path, &size);
	check(text && strstr(text, "reticle: {5: 135}") && strstr(text, "colors: {inner: 0xff19197f}"),
		  "craft: awing.yaml has the reticle and the inner colour");
	free(text);
	snprintf(path, sizeof path, "%s/craft/empty.yaml", out);
	check(!IsFile(path), "craft: no file for a model without settings");
	snprintf(path, sizeof path, "%s/hud.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "hull_icons: [\n  0, 100]") && !strstr(text, "map_icons"),
		  "craft: hud.yaml has the hull icon list, and no map icon list without its file");
	free(text);
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
	write_file(path, "// other comment\r\n!msg_armed!\\03Primary cannons armed\r\n!KSPEC_A!m:Pilot's \xe9 A\r\n"
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
	char* text = ReadWholeFile(out, &size);
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
	size_t size;
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
		if (address >= g_fakeSections[i].address && address < g_fakeSections[i].address + g_fakeSections[i].size)
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
	exe->data = calloc(1, exe->size);
	unsigned char* d = exe->data;
	d[0] = 'M';
	d[1] = 'Z';
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
	static const unsigned cases[4] = { 0x43D458, 0x43D4F8, 0x43DCC4, 0x459678 };
	static const unsigned jumps[4] = { 0x43D117, 0x43D359, 0x43DA0C, 0x458E81 };
	static const unsigned jump_tables[4] = { 0x43D800, 0x43D840, 0x43D880, 0x459800 };
	for (int s = 0; s < 4; ++s) {
		unsigned char* jump = fake_at(exe, jumps[s]);
		jump[0] = 0xFF;
		jump[1] = 0x24;
		jump[2] = 0x85;
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
	type[0] = 0x02;
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
				snprintf(line, sizeof line, "!KSPEC%d%s!%sCraft %d", i, suffixes[table], table ? "" : "m:", i);
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
	write_file(path, "FlightModels\\XWING.opt\r\n\r\nFlightModels\\Booster.opt\r\nFlightModels\\CorvetteExplode.opt\r\n"
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
	fake_type(m, 33, 2, 32);                         /* a new craft in a reserved slot, on the renamed row */
	fake_type(m, 34, 4, 33);                         /* a reserved slot on a row that the list doesn't have */
	put16(fake_at(m, 0x5B0F70 + 2 * 200), 33);
	put16(fake_at(m, 0x5B0F70 + 2 * 201), 34);
	put32(fake_at(m, 0x5BB480 + 987 * 0 + 21), 999); /* the X-wing's hull */
	put16(fake_at(m, 0x5FB240 + 24 * 71 + 16), 0x843);
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

	write_strings(stock, "m:Craft 1");
	write_strings(mod, "m:TIE Advanced x1");
	snprintf(path, sizeof path, "%s/SHIPLIST.TXT", stock);
	write_file(path, "!SPECIES_7!*Booster,Fighter,Nonflyable,Unknown,NoSkirmish,\t\t1,2,3,4,5,6,7,8\r\n"
					 "!SPECIES_9!B-wing,Fighter,Flyable,Known,Skirmish,\t\t1,2,3,4,5,6,7,8\r\n"
					 "!SPECIES_200!*DontUseMe,Planet/asteroid,Nonflyable,Known,NoSkirmish,\t1,2,3,4,5,6,7,8\r\n"
					 "!SPECIES_201!*DontUseMe,Planet/asteroid,Nonflyable,Known,NoSkirmish,\t1,2,3,4,5,6,7,8\r\n");
	snprintf(path, sizeof path, "%s/SHIPLIST.TXT", mod);
	write_file(path, "!SPECIES_7!TIE Advanced x1,Fighter,GunnerFlyable,Known,Skirmish,\t\t1,2,3,4, 651, 246,681,279\r\n"
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
	char strings[1200];
	snprintf(strings, sizeof strings, "%s/strings.yaml", out);
	check(XwauObjects_Write(mod, stock, out, &moved) && XwauText_Write(mod, stock, strings, &moved),
		  "objects: the object files and strings.yaml are written");

	size_t size = 0;
	snprintf(path, sizeof path, "%s/objects/tieadvancedx1.yaml", out);
	char* text = ReadWholeFile(path, &size);
	check(text && strstr(text, "object_type: 2\nmodel: tieadvancedx1\n"),
		  "objects: a row that the mod renames gives `model`");
	check(text && strstr(text, "    - {type: 282, count: 2, mount: 0, range: 0, cooldown: 0}\n") &&
			  strstr(text, "  cockpit_name: 'TieAdvancedX1'\n") && strstr(text, "sounds_like: 5\n"),
		  "objects: the laser groups, the cockpit name and the engine sound case");
	check(text && strstr(text, "names: {long: 'TIE Advanced x1', gender: m}\n"),
		  "objects: the species name of the craft record goes into `names`");
	check(text && strstr(text, "  7:\n    ship_list: {name: 'TIE Advanced x1', category: 'Fighter', flyable: gunner, "
							   "known: true, skirmish: true, hidden: false, icon: [651, 246, 681, 279]}\n") &&
			  strstr(text, "    tech_room: {name: 'TIE Advanced x1'}\n"),
		  "objects: the ship list line and the tech room text of the craft type");
	free(text);
	snprintf(path, sizeof path, "%s/objects/xwing.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && !strstr(text, "model:") && strstr(text, "  hull: 999\n") && strstr(text, "beam_allowed: true\n"),
		  "objects: a change of case isn't a new model; the hull and the beam");
	free(text);
	snprintf(path, sizeof path, "%s/objects/bwing.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "model_type: {flags: 0x843}\n") && !strstr(text, "craft_record"),
		  "objects: the model type flags");
	free(text);
	snprintf(path, sizeof path, "%s/objects/corvetteexplode.yaml", out);
	check(!IsFile(path), "objects: a row renamed for a new craft keeps the stock model, and the type gets no file");
	snprintf(path, sizeof path, "%s/objects/calamarihomeone.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "object_type: 33\nmodel: calamarihomeone\n") &&
			  strstr(text, "  200:\n    ship_list: {name: 'Home One', category: 'Starship', flyable: false"),
		  "objects: a reserved slot that the mod fills gets its model, its craft type and its ship list line");
	free(text);
	snprintf(path, sizeof path, "%s/objects/rebelturboshort_297.yaml", out);
	text = ReadWholeFile(path, &size);
	check(text && strstr(text, "object_type: 297\nmodel: rebelturboshort\nlist: equipment\n"),
		  "objects: an equipment type that the mod moves to another model gets `model` and `list: equipment`, "
		  "and two types with one model get a file each");
	free(text);
	snprintf(path, sizeof path, "%s/objects", out);
	size_t count = 0;
	char** names = ListDir(path, true, &count);
	check(count == 6, "objects: a reserved slot that the mod gives an empty row gets no file, and neither does "
					  "an equipment type that the mod doesn't change");
	FreeList(names, count);
	text = ReadWholeFile(strings, &size);
	check(text && strstr(text, "SPECIES_9:") && !strstr(text, "SPECIES_7") && !strstr(text, "SPEC_7_") &&
			  !strstr(text, "KSPEC1:") && !strstr(text, "SPECIES_200") && !strstr(text, "SPECIES_201"),
		  "objects: the texts in the object files aren't in strings.yaml, and the others are, except the texts "
		  "of a craft type without a craft");
	free(text);
	XwauTextIds_Free(&moved);
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
	test_text(scratch);
	test_objects(scratch);
	RemoveTree(scratch);
	printf("%s\n", g_failures ? "FAILED" : "passed");
	return g_failures ? 1 : 0;
}
