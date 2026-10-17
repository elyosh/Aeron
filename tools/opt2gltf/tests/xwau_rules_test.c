/*
 * Tests the hook file rules that opt2gltf reproduces (xwau_config.h), its
 * skin selection (xwau_skins.h) and its skybox conversion (xwau_skyboxes.h). The inputs are fixed texts and
 * skin folders written to a scratch folder. The test is built with the tools as opt2gltf_xwau_test. It prints
 * each check and exits with a non-zero status when a check fails.
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opt.h"
#include "util.h"
#include "xwau_config.h"
#include "xwau_missions.h"
#include "xwau_skins.h"
#include "xwau_skyboxes.h"

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

static void test_skyboxes(const char* scratch) {
	char game[1024], stock[1024], dir[1200], path[1300];
	snprintf(game, sizeof game, "%s/sky_game", scratch);
	snprintf(stock, sizeof stock, "%s/sky_stock", scratch);
	make_cube_folder(game, "Alpha", 6);
	make_cube_folder(game, "Beta", 6);
	make_cube_folder(game, "Gamma", 5); /* does not load */
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
	check(m1 && m2 && m3, "skyboxes: one entry per mission");
	if (m1 && m2 && m3) {
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
		check(strstr(m3, "show_backdrops: all") && !strstr(m3, "hide_backdrops"),
			  "skyboxes: EnabledBackdrops = all turns hiding off");
	}
	check(list && strstr(list, "\txwau.alpha\tflight/environments/xwau/alpha\n") &&
			  strstr(list, "\txwau.beta\t") && !strstr(list, "gamma"),
		  "skyboxes: the folder list names the folders that load");
	free(text);
	free(list);
}

int main(void) {
	char        scratch[1024];
	const char* tmp = getenv("TMPDIR");
	snprintf(scratch, sizeof scratch, "%s/opt2gltf_xwau_test", tmp && tmp[0] ? tmp : "/tmp");
	RemoveTree(scratch);
	MakeDirs(scratch);
	test_rules(scratch);
	test_skins(scratch);
	test_skybox_rotation();
	test_skyboxes(scratch);
	RemoveTree(scratch);
	printf("%s\n", g_failures ? "FAILED" : "passed");
	return g_failures ? 1 : 0;
}
