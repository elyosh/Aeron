/*
 * The hooks' file rules opt2gltf reproduces (xwau_config.h) and its skin
 * selection (xwau_skins.h), on fixed texts and skin folders written under a
 * scratch folder. Built with the tools as opt2gltf_xwau_test; prints each
 * check, exits non-zero on a failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opt.h"
#include "util.h"
#include "xwau_config.h"
#include "xwau_missions.h"
#include "xwau_skins.h"

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
		  "sections whole and without case ([CockpitPov;] is another), the first key wins");
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

/* A case: the global skin lines, the OPT's skins ("" for a file directly in
 * its skin folder; NULL for no skin folder), its states, and the expected
 * default markings (NULL: unskinned; none when no variant). */
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
		XwauRoots     roots    = { { root }, 1 };
		XwauMissions* missions = XwauMissions_Load(&roots, false);
		/* An OPT with one face group of `states` states, no texture. */
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
		int       ok = missions && XwauSkins_Resolve(&roots, missions, k->model, &opt, "test", &skins);
		if (ok && k->markings < 0) {
			ok = skins.skins.variant_count == 0;
		} else if (ok) {
			/* The default markings, unskinned values NULL. */
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

int main(void) {
	char        scratch[1024];
	const char* tmp = getenv("TMPDIR");
	snprintf(scratch, sizeof scratch, "%s/opt2gltf_xwau_test", tmp && tmp[0] ? tmp : "/tmp");
	RemoveTree(scratch);
	MakeDirs(scratch);
	test_rules(scratch);
	test_skins(scratch);
	RemoveTree(scratch);
	printf("%s\n", g_failures ? "FAILED" : "passed");
	return g_failures ? 1 : 0;
}
