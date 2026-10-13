#include "xwau_mission_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "opt.h"
#include "util.h"
#include "xwau_skins.h"

static const char* const kMissionDirs[] = { "Missions", "Melee" };

static void lower_copy(char* out, size_t cap, const char* text) {
	size_t n = 0;
	for (; text[n] && n + 1 < cap; ++n)
		out[n] = Xwau_Lower(text[n]);
	out[n] = 0;
}

/* Writes ISO-8859-1 `text` as a quoted YAML string, in UTF-8. */
static void write_string(FILE* f, const char* text) {
	fputc('"', f);
	for (const unsigned char* c = (const unsigned char*)text; *c; ++c) {
		if (*c == '"' || *c == '\\') {
			fputc('\\', f);
			fputc(*c, f);
		} else if (*c < 0x80) {
			fputc(*c, f);
		} else {
			fputc(0xC0 | (*c >> 6), f);
			fputc(0x80 | (*c & 0x3F), f);
		}
	}
	fputc('"', f);
}

/* ---- Flight groups -------------------------------------------------------- */

/* The length of a flight group's name without its trailing white space. */
static size_t name_length(const char* name) {
	size_t n = strlen(name);
	while (n && strchr(" \t\n\r\v\f\x1c\x1d\x1e\x1f\x85\xa0", name[n - 1]))
		--n;
	return n;
}

static bool same_flight_group(const XwauTieFlightGroup* a, const XwauTieFlightGroup* b) {
	const size_t n = name_length(a->name);
	return a->craft == b->craft && name_length(b->name) == n && memcmp(a->name, b->name, n) == 0;
}

typedef struct Tie {
	XwauTieFlightGroup* groups;
	size_t              count;
} Tie;

/* The original mission's flight group that XWAU's flight group `index` is:
 * the same index with the same name and craft type, else the only such
 * flight group when it is the only one in both missions; -1 otherwise. */
static int stock_index(long long index, const Tie* xwau, const Tie* stock) {
	if (index < 0 || (size_t)index >= xwau->count)
		return -1;
	const XwauTieFlightGroup* wanted = &xwau->groups[index];
	if ((size_t)index < stock->count && same_flight_group(&stock->groups[index], wanted))
		return (int)index;
	int    match         = -1;
	size_t stock_matches = 0, xwau_matches = 0;
	for (size_t i = 0; i < stock->count; ++i) {
		if (same_flight_group(&stock->groups[i], wanted)) {
			match = (int)i;
			++stock_matches;
		}
	}
	for (size_t i = 0; i < xwau->count; ++i)
		xwau_matches += same_flight_group(&xwau->groups[i], wanted);
	return stock_matches == 1 && xwau_matches == 1 ? match : -1;
}

/* Why XWAU's flight group `index` has no original one, for a report. */
static void why_dropped(const Tie* xwau, long long index, char* out, size_t cap) {
	if (index >= 0 && (size_t)index < xwau->count)
		snprintf(out, cap, "XWAU's '%.*s' (craft %d) has no unique original match",
				 (int)name_length(xwau->groups[index].name), xwau->groups[index].name,
				 xwau->groups[index].craft);
	else
		snprintf(out, cap, "XWAU's mission has no such flight group");
}

/* ---- Object profiles ------------------------------------------------------ */

typedef struct Profile {
	long long fg;
	char*     profile;
} Profile;

static int compare_profiles(const void* a, const void* b) {
	const long long x = ((const Profile*)a)->fg, y = ((const Profile*)b)->fg;
	return x < y ? -1 : x > y;
}

/* The `ObjectProfile_fg_<N> = <profile>` selections of mission `base`, by
 * flight group. */
static bool profiles(const XwauRoots* roots, const char* base, Profile** out, size_t* count) {
	*out   = NULL;
	*count = 0;
	char      rel[1100], path[1024];
	XwauLines lines;
	snprintf(rel, sizeof rel, "%s_Objects.txt", base);
	if (!XwauLines_Read(XwauRoots_FindFile(roots, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP,
						&lines))
		return false;
	bool ok = true;
	if (!lines.count) {
		snprintf(rel, sizeof rel, "%s.ini", base);
		ok = XwauLines_Read(XwauRoots_FindFile(roots, rel, path, sizeof path) ? path : NULL, "Objects",
							XWAU_RULES_CPP, &lines);
	}
	static const char kPrefix[] = "objectprofile_", kFgPrefix[] = "objectprofile_fg_";
	for (size_t i = 0; ok && i < lines.count; ++i) {
		char  line[4096];
		char* equals;
		snprintf(line, sizeof line, "%s", lines.items[i]);
		if (!(equals = strchr(line, '=')))
			continue;
		*equals           = 0;
		const char* key   = Xwau_Trim(line, XWAU_RULES_CPP);
		const char* value = Xwau_Trim(equals + 1, XWAU_RULES_CPP);
		if (strncasecmp(key, kPrefix, sizeof kPrefix - 1) != 0)
			continue;
		const char* digits = key + sizeof kFgPrefix - 1;
		bool        by_fg  = strncasecmp(key, kFgPrefix, sizeof kFgPrefix - 1) == 0 && *digits &&
							 strspn(digits, "0123456789") == strlen(digits);
		if (!by_fg) {
			Report(base, "'%s' does not select by flight group; not imported", key);
			continue;
		}
		long long fg = 0;
		for (const char* d = digits; *d; ++d)
			if (fg < 1000000000)
				fg = fg * 10 + (*d - '0');
		bool seen = false;
		for (size_t k = 0; !seen && k < *count; ++k)
			seen = (*out)[k].fg == fg;
		if (seen || !*value)
			continue;
		Profile* grown = (Profile*)realloc(*out, (*count + 1) * sizeof *grown);
		ok             = grown && (grown[*count].profile = Xwau_Strdup(value)) != NULL;
		if (grown)
			*out = grown;
		if (ok)
			grown[(*count)++].fg = fg;
	}
	XwauLines_Free(&lines);
	if (*count)
		qsort(*out, *count, sizeof **out, compare_profiles);
	return ok;
}

static void free_profiles(Profile* list, size_t count) {
	for (size_t i = 0; i < count; ++i)
		free(list[i].profile);
	free(list);
}

/* ---- Skins ---------------------------------------------------------------- */

typedef struct SkinModel {
	char          key[256]; /* the model's name in lower case */
	int           states;
	XwauSkinSet   skins;
	XwauMarkings* defaults; /* the global context's */
	size_t        default_count;
} SkinModel;

typedef struct SkinModels {
	SkinModel*    models;
	size_t        count;
	XwauMarkings* scratch; /* XWAU_SKINS_MAX_MARKINGS */
} SkinModels;

static int compare_skin_models(const void* a, const void* b) {
	return strcmp(((const SkinModel*)a)->key, ((const SkinModel*)b)->key);
}

static void free_skin_models(SkinModels* s) {
	for (size_t i = 0; i < s->count; ++i) {
		XwauSkins_FreeNames(&s->models[i].skins);
		free(s->models[i].defaults);
	}
	free(s->models);
	free(s->scratch);
	memset(s, 0, sizeof *s);
}

/* The models with skins: the OPTs of the roots' FlightModels, the first
 * root's for a name, by name. */
static bool load_skin_models(const XwauRoots* roots, const XwauMissions* missions, SkinModels* out) {
	memset(out, 0, sizeof *out);
	out->scratch   = (XwauMarkings*)malloc(XWAU_SKINS_MAX_MARKINGS * sizeof *out->scratch);
	bool      ok   = out->scratch != NULL;
	XwauLines seen = { 0 };
	for (int r = 0; ok && r < roots->count; ++r) {
		char dir[1024];
		if (!XwauRoots_FindDir(roots, r, "FlightModels", dir, sizeof dir))
			continue;
		size_t count   = 0;
		char** entries = Xwau_ListDir(dir, true, &count);
		for (size_t i = 0; ok && i < count; ++i) {
			const size_t len = strlen(entries[i]);
			char         key[256];
			if (len <= 4 || strcasecmp(entries[i] + len - 4, ".opt") != 0 || len - 4 >= sizeof key)
				continue;
			lower_copy(key, len - 3, entries[i]);
			bool known = false;
			for (size_t k = 0; !known && k < seen.count; ++k)
				known = strcmp(seen.items[k], key) == 0;
			char path[2048];
			snprintf(path, sizeof path, "%s/%s", dir, entries[i]);
			FILE* f = known ? NULL : fopen(path, "rb");
			if (!f)
				continue;
			fclose(f);
			char** grown = (char**)realloc(seen.items, (seen.count + 1) * sizeof *grown);
			ok           = grown && (grown[seen.count] = Xwau_Strdup(key)) != NULL;
			if (grown)
				seen.items = grown;
			seen.count += ok;
			SkinModel model = { 0 };
			snprintf(model.key, sizeof model.key, "%s", key);
			ok = ok && XwauSkins_Names(roots, key, &model.skins);
			if (!ok || !model.skins.exists) {
				XwauSkins_FreeNames(&model.skins);
				continue;
			}
			opt_error_t err;
			opt_file_t* opt = opt_load_file(path, &err);
			if (!opt) {
				Report(path, "%s; its skins are left out", err.msg);
				XwauSkins_FreeNames(&model.skins);
				continue;
			}
			model.states = XwauSkins_StateCount(opt);
			opt_free(opt);
			ok = XwauSkins_Markings(XwauMissions_SkinContext(missions, 0), key, model.states, &model.skins,
									out->scratch, &model.default_count);
			model.defaults = ok ? (XwauMarkings*)malloc((model.default_count ? model.default_count : 1) *
														sizeof *model.defaults)
								: NULL;
			SkinModel* models =
				model.defaults ? (SkinModel*)realloc(out->models, (out->count + 1) * sizeof *models) : NULL;
			ok = models != NULL;
			if (!ok) {
				XwauSkins_FreeNames(&model.skins);
				free(model.defaults);
				break;
			}
			memcpy(model.defaults, out->scratch, model.default_count * sizeof *model.defaults);
			out->models               = models;
			out->models[out->count++] = model;
		}
		Xwau_FreeList(entries, count);
	}
	XwauLines_Free(&seen);
	if (ok && out->count)
		qsort(out->models, out->count, sizeof *out->models, compare_skin_models);
	return ok;
}

/* A markings value whose variant differs from its model's default. */
typedef struct SkinChoice {
	size_t model;
	size_t markings;
	char   variant[1100]; /* "" for the model as modelled */
} SkinChoice;

/* The markings values the skin lines `lines` change, by model then
 * markings value. */
static bool skin_choices(const SkinModels* s, const XwauLines* lines, const char* base, SkinChoice** out,
						 size_t* count) {
	*out    = NULL;
	*count  = 0;
	bool ok = true;
	for (size_t m = 0; ok && m < s->count; ++m) {
		const SkinModel* model = &s->models[m];
		size_t           n     = 0;
		ok = XwauSkins_Markings(lines, model->key, model->states, &model->skins, s->scratch, &n);
		for (size_t i = 0; ok && i < n; ++i) {
			const XwauMarkings  none   = { false, 0, "" };
			const XwauMarkings* now    = &s->scratch[i];
			const XwauMarkings* before = i < model->default_count ? &model->defaults[i] : &none;
			if ((now->skinned == before->skinned && now->state == before->state &&
				 strcmp(now->list, before->list) == 0) ||
				(!now->skinned && !before->skinned))
				continue;
			SkinChoice* grown = (SkinChoice*)realloc(*out, (*count + 1) * sizeof *grown);
			if (!(ok = grown != NULL))
				break;
			*out              = grown;
			SkinChoice* entry = &grown[(*count)++];
			entry->model      = m;
			entry->markings   = i;
			entry->variant[0] = 0;
			if (now->skinned) {
				snprintf(entry->variant, sizeof entry->variant, "%d|%s", now->state, now->list);
			} else {
				/* State 0 is the model as modelled; the others are variants. */
				if (now->state)
					snprintf(entry->variant, sizeof entry->variant, "State %d", now->state);
				Report(base, "%s markings %zu is unskinned against a skinned default; using %s", model->key,
					   i, entry->variant[0] ? entry->variant : "the model as modelled");
			}
		}
	}
	return ok;
}

/* ---- Mission files -------------------------------------------------------- */

typedef struct Totals {
	size_t files, profiles, profiles_dropped, skins, articulations, articulations_dropped;
} Totals;

/* The original flight group of each selection (-1 when dropped, reported). */
static int* match_profiles(const Profile* list, size_t count, const Tie* xwau, const Tie* stock,
						   const char* base, Totals* totals) {
	int* targets = (int*)malloc((count ? count : 1) * sizeof *targets);
	for (size_t i = 0; targets && i < count; ++i) {
		int target = stock_index(list[i].fg, xwau, stock);
		for (size_t k = 0; target >= 0 && k < i; ++k)
			if (targets[k] == target)
				target = -1;
		targets[i] = target;
		char why[128];
		if (target < 0) {
			why_dropped(xwau, list[i].fg, why, sizeof why);
			Report(base, "flight group %lld (%s) dropped: %s", list[i].fg, list[i].profile, why);
			++totals->profiles_dropped;
		} else if (target != list[i].fg) {
			Report(base, "flight group %lld (%s) is original flight group %d", list[i].fg, list[i].profile,
				   target);
		}
	}
	return targets;
}

static int* match_articulations(const XwauFlightGroupArticulation* fgs, size_t count, const Tie* xwau,
								const Tie* stock, const char* base, Totals* totals) {
	int* targets = (int*)malloc((count ? count : 1) * sizeof *targets);
	for (size_t i = 0; targets && i < count; ++i) {
		int target = stock_index(fgs[i].flight_group, xwau, stock);
		for (size_t k = 0; target >= 0 && k < i; ++k)
			if (targets[k] == target)
				target = -1;
		targets[i] = target;
		char why[128];
		if (target < 0) {
			why_dropped(xwau, fgs[i].flight_group, why, sizeof why);
			Report(base, "flight group %d articulations dropped: %s", fgs[i].flight_group, why);
			++totals->articulations_dropped;
		} else if (target != fgs[i].flight_group) {
			Report(base, "flight group %d articulations are original flight group %d's", fgs[i].flight_group,
				   target);
		}
	}
	return targets;
}

/* The order of `targets` by original flight group (the dropped ones
 * first, to be skipped). */
static size_t* by_target(const int* targets, size_t count) {
	size_t* order = (size_t*)malloc((count ? count : 1) * sizeof *order);
	for (size_t i = 0; order && i < count; ++i) {
		size_t at = i;
		for (; at > 0 && targets[order[at - 1]] > targets[i]; --at)
			order[at] = order[at - 1];
		order[at] = i;
	}
	return order;
}

static bool write_mission(const char* path, const char* base, const Profile* profile_list,
						  const int* profile_targets, size_t profile_count, const SkinModels* skin_models,
						  const SkinChoice* skins, size_t skin_count, const XwauFlightGroupArticulation* fgs,
						  const int* fg_targets, size_t fg_count, const Tie* stock) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(NULL, "cannot write %s", path);
		return false;
	}
	fprintf(f, "# Imported from XWAU's %s selections by opt2gltf.\nversion: 1\n", base);
	size_t* order = by_target(profile_targets, profile_count);
	bool    any   = false;
	for (size_t k = 0; order && k < profile_count; ++k) {
		const size_t i = order[k];
		if (profile_targets[i] < 0)
			continue;
		if (!any)
			fputs("object_profiles:\n  flight_groups:\n", f);
		any                          = true;
		const XwauTieFlightGroup* fg = &stock->groups[profile_targets[i]];
		fprintf(f, "    %d: { profile: ", profile_targets[i]);
		write_string(f, profile_list[i].profile);
		fputs(", name: ", f);
		write_string(f, fg->name);
		fprintf(f, ", craft: %d }\n", fg->craft);
	}
	free(order);
	for (size_t i = 0; i < skin_count; ++i) {
		if (i == 0)
			fputs("skins:\n", f);
		if (i == 0 || skins[i].model != skins[i - 1].model) {
			fputs("  ", f);
			write_string(f, skin_models->models[skins[i].model].key);
			fputs(":\n", f);
		}
		fprintf(f, "    %zu: ", skins[i].markings);
		if (skins[i].variant[0])
			write_string(f, skins[i].variant);
		else
			fputc('~', f);
		fputc('\n', f);
	}
	order = by_target(fg_targets, fg_count);
	any   = false;
	for (size_t k = 0; order && k < fg_count; ++k) {
		const size_t i = order[k];
		if (fg_targets[i] < 0)
			continue;
		if (!any)
			fputs("articulations:\n  flight_groups:\n", f);
		any                          = true;
		const XwauTieFlightGroup* fg = &stock->groups[fg_targets[i]];
		fprintf(f, "    %d: { name: ", fg_targets[i]);
		write_string(f, fg->name);
		fprintf(f, ", craft: %d", fg->craft);
		if (fgs[i].variant[0]) {
			fputs(", variant: ", f);
			write_string(f, fgs[i].variant);
		}
		for (size_t s = 0; s < fgs[i].start_count; ++s)
			fprintf(f, "%s%s", s ? ", " : ", start: [", fgs[i].start[s]);
		fputs(fgs[i].start_count ? "] }\n" : " }\n", f);
	}
	free(order);
	const bool ok = !ferror(f) && fclose(f) == 0;
	if (!ok)
		Report(NULL, "cannot write %s", path);
	return ok;
}

/* The selections of mission `name` of folder `dir`, written when it has
 * some. */
static bool import_mission(const XwauRoots* roots, const XwauMissions* missions, const XwauRoots* stock_roots,
						   const char* dir, const char* name, const char* out, SkinModels* skin_models,
						   bool* skins_loaded, Totals* totals) {
	char base[1100];
	snprintf(base, sizeof base, "%s/%s", dir, name);
	Profile*    profile_list  = NULL;
	size_t      profile_count = 0;
	SkinChoice* skins         = NULL;
	size_t      skin_count    = 0;
	XwauLines   lines         = { 0 };
	bool        ok            = profiles(roots, base, &profile_list, &profile_count) &&
								XwauMissions_OwnSkinLines(roots, base, &lines);
	if (ok && lines.count && !*skins_loaded) {
		ok            = load_skin_models(roots, missions, skin_models);
		*skins_loaded = true;
	}
	ok = ok && (!lines.count || skin_choices(skin_models, &lines, base, &skins, &skin_count));
	XwauLines_Free(&lines);
	const XwauFlightGroupArticulation* fgs      = NULL;
	const size_t                       fg_count = XwauMissions_FlightGroupArticulations(missions, base, &fgs);
	size_t                             skinned_models = 0;
	for (size_t i = 0; i < skin_count; ++i)
		skinned_models += i == 0 || skins[i].model != skins[i - 1].model;
	if (!ok || (!profile_count && !skin_count && !fg_count)) {
		free_profiles(profile_list, profile_count);
		free(skins);
		return ok;
	}

	char rel[1200], xwau_path[1024], stock_path[1024];
	snprintf(rel, sizeof rel, "%s.tie", base);
	const bool has_xwau  = XwauRoots_FindFile(roots, rel, xwau_path, sizeof xwau_path);
	const bool has_stock = XwauRoots_FindFile(stock_roots, rel, stock_path, sizeof stock_path);
	Tie        xwau = { 0 }, stock = { 0 };
	if (has_xwau)
		xwau.groups = XwauTie_FlightGroups(xwau_path, &xwau.count);
	if (has_stock)
		stock.groups = XwauTie_FlightGroups(stock_path, &stock.count);
	if (!xwau.groups || !stock.groups) {
		Report(base,
			   "%zu selections, %zu skinned models and %zu articulation flight groups dropped: no "
			   "%s .TIE",
			   profile_count, skinned_models, fg_count, xwau.groups ? "original" : "XWAU");
		totals->profiles_dropped += profile_count;
		totals->articulations_dropped += fg_count;
		free_profiles(profile_list, profile_count);
		free(skins);
		free(xwau.groups);
		free(stock.groups);
		return true;
	}
	int* profile_targets = match_profiles(profile_list, profile_count, &xwau, &stock, base, totals);
	if (skin_count) {
		for (size_t i = 0; i < xwau.count; ++i) {
			const int target = stock_index((long long)i, &xwau, &stock);
			if (target >= 0 && xwau.groups[i].markings != stock.groups[target].markings)
				Report(base,
					   "flight group %zu '%.*s' has markings %d in XWAU's mission, %d in the "
					   "original one",
					   i, (int)name_length(xwau.groups[i].name), xwau.groups[i].name, xwau.groups[i].markings,
					   stock.groups[target].markings);
		}
	}
	int*   fg_targets    = match_articulations(fgs, fg_count, &xwau, &stock, base, totals);
	size_t profiles_kept = 0, fgs_kept = 0;
	for (size_t i = 0; profile_targets && i < profile_count; ++i)
		profiles_kept += profile_targets[i] >= 0;
	for (size_t i = 0; fg_targets && i < fg_count; ++i)
		fgs_kept += fg_targets[i] >= 0;
	ok = profile_targets && fg_targets;
	if (ok && (profiles_kept || skin_count || fgs_kept)) {
		char dir_path[1200], lower_dir[64], path[1400];
		lower_copy(lower_dir, sizeof lower_dir, dir);
		snprintf(dir_path, sizeof dir_path, "%s/%s", out, lower_dir);
		snprintf(path, sizeof path, "%s/%s.yaml", dir_path, name);
		ok = Xwau_MakeDir(dir_path) &&
			 write_mission(path, base, profile_list, profile_targets, profile_count, skin_models, skins,
						   skin_count, fgs, fg_targets, fg_count, &stock);
		if (ok) {
			++totals->files;
			totals->profiles += profiles_kept;
			totals->skins += skin_count;
			totals->articulations += fgs_kept;
		}
	}
	free(profile_targets);
	free(fg_targets);
	free_profiles(profile_list, profile_count);
	free(skins);
	free(xwau.groups);
	free(stock.groups);
	return ok;
}

bool XwauMissionFiles_Write(const XwauRoots* roots, const XwauMissions* missions, const char* stock_game,
							const char* out) {
	const XwauRoots stock_roots  = { { stock_game }, 1 };
	SkinModels      skin_models  = { 0 };
	bool            skins_loaded = false;
	Totals          totals       = { 0 };
	bool            ok           = true;
	for (size_t d = 0; ok && d < 2; ++d) {
		XwauLines names;
		ok = XwauMissions_Names(roots, kMissionDirs[d], &names);
		for (size_t n = 0; ok && n < names.count; ++n)
			ok = import_mission(roots, missions, &stock_roots, kMissionDirs[d], names.items[n], out,
								&skin_models, &skins_loaded, &totals);
		XwauLines_Free(&names);
	}
	free_skin_models(&skin_models);
	if (!ok)
		return false;
	Report(NULL,
		   "%zu mission files: %zu object profile selections (%zu dropped), %zu skin selections, %zu "
		   "articulation flight groups (%zu dropped)",
		   totals.files, totals.profiles, totals.profiles_dropped, totals.skins, totals.articulations,
		   totals.articulations_dropped);
	return true;
}
