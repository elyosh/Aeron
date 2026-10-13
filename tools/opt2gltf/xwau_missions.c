#include "xwau_missions.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "model_tables.h"
#include "util.h"
#include "xwau_model.h"

#define TAG_MAX 63
#define COMPONENT_MAX 254
#define FLIGHT_GROUP_MAX 192

/* An XWA .TIE: the header, then the flight groups. */
#define TIE_HEADER_SIZE 0x23F0
#define TIE_FLIGHT_GROUP_SIZE 0xE3E
#define TIE_FLIGHT_GROUP_NAME 20
#define TIE_FLIGHT_GROUP_CRAFT 0x6B
#define TIE_FLIGHT_GROUP_MARKINGS 0x73

static const char* const kMissionDirs[] = { "Missions", "Melee" };

typedef struct MeshEntry {
	int mesh, angle, speed, speed_back;
} MeshEntry;

typedef struct GroupList {
	char       group[512];
	MeshEntry* meshes;
	size_t     count;
} GroupList;

typedef struct Lists {
	GroupList* groups;
	size_t     count;
} Lists;

typedef struct Variant {
	char  name[128];
	Lists lists;
} Variant;

typedef struct ModelVariants {
	char     model[256]; /* lower case */
	Variant* variants;
	size_t   count;
} ModelVariants;

/* The flight groups a mission starts deployed or gives a variant. */
typedef struct MissionArticulations {
	char                         base[256]; /* "<dir>/<lower-case name>" */
	XwauFlightGroupArticulation* fgs;
	size_t                       count;
} MissionArticulations;

struct XwauMissions {
	bool                  report;
	XwauLines*            contexts;
	size_t                context_count;
	ModelVariants*        models;
	size_t                model_count;
	MissionArticulations* missions;
	size_t                mission_count;
};

static void lists_free(Lists* lists) {
	for (size_t i = 0; i < lists->count; ++i)
		free(lists->groups[i].meshes);
	free(lists->groups);
	memset(lists, 0, sizeof *lists);
}

static void lower_copy(char* out, size_t cap, const char* text) {
	size_t n = 0;
	for (; text[n] && n + 1 < cap; ++n)
		out[n] = (text[n] >= 'A' && text[n] <= 'Z') ? (char)(text[n] + 32) : text[n];
	out[n] = 0;
}

static bool ends_with(const char* text, const char* suffix) {
	const size_t a = strlen(text), b = strlen(suffix);
	return a >= b && strcasecmp(text + a - b, suffix) == 0;
}

/* Reports a problem of mission `base`, when the missions report. */
static void report(const XwauMissions* m, const char* base, const char* format, ...) {
	if (!m->report)
		return;
	va_list args;
	va_start(args, format);
	ReportV(base, format, args);
	va_end(args);
}

/* ---- Skin contexts ------------------------------------------------------------ */

static bool global_skin_lines(const XwauRoots* roots, XwauLines* out) {
	char path[1024];
	if (!XwauLines_Read(XwauRoots_FindFile(roots, "FlightModels/Skins.txt", path, sizeof path) ? path : NULL,
						"", XWAU_RULES_CS, out))
		return false;
	return out->count ||
		   XwauLines_Read(XwauRoots_FindFile(roots, "FlightModels/default.ini", path, sizeof path) ? path
																								   : NULL,
						  "Skins", XWAU_RULES_CS, out);
}

static bool copy_lines(const XwauLines* from, XwauLines* to) {
	memset(to, 0, sizeof *to);
	to->items = (char**)calloc(from->count ? from->count : 1, sizeof *to->items);
	if (!to->items)
		return false;
	for (; to->count < from->count; ++to->count)
		if (!(to->items[to->count] = Xwau_Strdup(from->items[to->count])))
			return false;
	return true;
}

static int compare_strings(const void* a, const void* b) {
	return strcmp(*(char* const*)a, *(char* const*)b);
}

/* The missions with a .ini or _Skins.txt: "<dir>/<base>" as first found,
 * sorted by its lower case. */
static bool skin_missions(const XwauRoots* roots, XwauLines* out) {
	memset(out, 0, sizeof *out);
	XwauLines keys = { 0 };
	bool      ok   = true;
	for (size_t d = 0; ok && d < 2; ++d) {
		for (int r = 0; ok && r < roots->count; ++r) {
			char dir[1024];
			if (!XwauRoots_FindDir(roots, r, kMissionDirs[d], dir, sizeof dir))
				continue;
			size_t count   = 0;
			char** entries = Xwau_ListDir(dir, true, &count);
			for (size_t i = 0; ok && i < count; ++i) {
				size_t base_len;
				if (ends_with(entries[i], ".ini"))
					base_len = strlen(entries[i]) - 4;
				else if (ends_with(entries[i], "_skins.txt"))
					base_len = strlen(entries[i]) - 10;
				else
					continue;
				char path[1024], key[1024];
				snprintf(path, sizeof path, "%s/%.*s", kMissionDirs[d], (int)base_len, entries[i]);
				lower_copy(key, sizeof key, path);
				bool seen = false;
				for (size_t k = 0; !seen && k < keys.count; ++k)
					seen = strcmp(keys.items[k], key) == 0;
				if (seen)
					continue;
				/* "<key>\n<path>": sorted by the key. */
				char*  entry = (char*)malloc(strlen(key) + strlen(path) + 2);
				char** grown = (char**)realloc(keys.items, (keys.count + 1) * sizeof *grown);
				ok           = entry && grown;
				if (grown)
					keys.items = grown;
				if (!ok) {
					free(entry);
					break;
				}
				snprintf(entry, strlen(key) + strlen(path) + 2, "%s\n%s", key, path);
				keys.items[keys.count++] = entry;
			}
			Xwau_FreeList(entries, count);
		}
	}
	if (ok && keys.count)
		qsort(keys.items, keys.count, sizeof *keys.items, compare_strings);
	for (size_t i = 0; ok && i < keys.count; ++i) {
		char** grown = (char**)realloc(out->items, (out->count + 1) * sizeof *grown);
		ok           = grown && (grown[out->count] = Xwau_Strdup(strchr(keys.items[i], '\n') + 1)) != NULL;
		if (grown)
			out->items = grown;
		out->count += ok;
	}
	XwauLines_Free(&keys);
	return ok;
}

static bool load_skin_contexts(const XwauRoots* roots, XwauMissions* m) {
	XwauLines global, missions;
	if (!global_skin_lines(roots, &global))
		return false;
	if (!skin_missions(roots, &missions)) {
		XwauLines_Free(&global);
		return false;
	}
	m->contexts = (XwauLines*)calloc(missions.count + 1, sizeof *m->contexts);
	bool ok     = m->contexts != NULL;
	if (ok) {
		m->contexts[0]   = global;
		m->context_count = 1;
		memset(&global, 0, sizeof global);
	}
	for (size_t i = 0; ok && i < missions.count; ++i) {
		/* The mission's own lines, else the global ones. */
		XwauLines* lines = &m->contexts[m->context_count];
		ok               = XwauMissions_OwnSkinLines(roots, missions.items[i], lines);
		if (ok && !lines->count)
			ok = copy_lines(&m->contexts[0], lines);
		m->context_count += ok;
	}
	XwauLines_Free(&global);
	XwauLines_Free(&missions);
	return ok;
}

bool XwauMissions_OwnSkinLines(const XwauRoots* roots, const char* base, XwauLines* out) {
	char rel[1100], path[1024];
	snprintf(rel, sizeof rel, "%s_Skins.txt", base);
	if (!XwauLines_Read(XwauRoots_FindFile(roots, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CS,
						out))
		return false;
	if (out->count)
		return true;
	snprintf(rel, sizeof rel, "%s.ini", base);
	return XwauLines_Read(XwauRoots_FindFile(roots, rel, path, sizeof path) ? path : NULL, "Skins",
						  XWAU_RULES_CS, out);
}

size_t XwauMissions_SkinContextCount(const XwauMissions* missions) { return missions->context_count; }

const XwauLines* XwauMissions_SkinContext(const XwauMissions* missions, size_t index) {
	return &missions->contexts[index];
}

/* ---- Articulation lists ------------------------------------------------------- */

bool XwauMissions_Names(const XwauRoots* roots, const char* dir_name, XwauLines* out) {
	memset(out, 0, sizeof *out);
	bool ok = true;
	for (int r = 0; ok && r < roots->count; ++r) {
		char dir[1024];
		if (!XwauRoots_FindDir(roots, r, dir_name, dir, sizeof dir))
			continue;
		size_t count   = 0;
		char** entries = Xwau_ListDir(dir, false, &count);
		for (size_t i = 0; ok && i < count; ++i) {
			char lower[1024];
			lower_copy(lower, sizeof lower, entries[i]);
			const size_t             len = strlen(lower);
			char                     names[4][1024];
			int                      n          = 0;
			static const char* const suffixes[] = { ".ini", "_objects.txt", "_skins.txt" };
			for (size_t s = 0; s < 3; ++s)
				if (ends_with(lower, suffixes[s]))
					snprintf(names[n++], sizeof names[0], "%.*s", (int)(len - strlen(suffixes[s])), lower);
			if (ends_with(lower, ".txt")) {
				const char* sfoils = strstr(lower, "_sfoils");
				if (sfoils) {
					snprintf(names[n++], sizeof names[0], "%.*s", (int)(sfoils - lower), lower);
				} else {
					char tie[1024];
					snprintf(tie, sizeof tie, "%.*s.tie", (int)(len - 4), lower);
					bool has_tie = false;
					for (size_t j = 0; !has_tie && j < count; ++j)
						has_tie = strcasecmp(entries[j], tie) == 0;
					if (has_tie)
						snprintf(names[n++], sizeof names[0], "%.*s", (int)(len - 4), lower);
				}
			}
			for (int k = 0; ok && k < n; ++k) {
				bool seen = false;
				for (size_t j = 0; !seen && j < out->count; ++j)
					seen = strcmp(out->items[j], names[k]) == 0;
				if (seen)
					continue;
				char** grown = (char**)realloc(out->items, (out->count + 1) * sizeof *grown);
				ok           = grown && (grown[out->count] = Xwau_Strdup(names[k])) != NULL;
				if (grown)
					out->items = grown;
				out->count += ok;
			}
		}
		Xwau_FreeList(entries, count);
	}
	if (ok && out->count)
		qsort(out->items, out->count, sizeof *out->items, compare_strings);
	return ok;
}

/* "<section>_fg_<N>" (N without leading zeros): the section and N. */
static bool split_fg_section(const char* section, char* base, size_t cap, int* fg) {
	const char* at = NULL;
	for (const char* p = section; *p; ++p)
		if (strncasecmp(p, "_fg_", 4) == 0)
			at = p;
	if (!at || at == section)
		return false;
	const char* digits = at + 4;
	if (!*digits || (digits[0] == '0' && digits[1]))
		return false;
	long long value = 0;
	for (const char* d = digits; *d; ++d) {
		if (*d < '0' || *d > '9')
			return false;
		if (value < 100000000)
			value = value * 10 + (*d - '0');
	}
	snprintf(base, cap, "%.*s", (int)(at - section), section);
	*fg = (int)value;
	return true;
}

/* The section names of an INI file, in order. */
static bool ini_sections(const char* path, XwauLines* out) {
	memset(out, 0, sizeof *out);
	FILE* f = path ? fopen(path, "rb") : NULL;
	if (!f)
		return true;
	char line[4096];
	bool ok = true;
	while (ok && fgets(line, sizeof line, f)) {
		Xwau_Trim(line, XWAU_RULES_CPP);
		const size_t len = strlen(line);
		if (len >= 2 && line[0] == '[' && line[len - 1] == ']') {
			char** grown = (char**)realloc(out->items, (out->count + 1) * sizeof *grown);
			ok           = grown && (grown[out->count] = Xwau_Strndup(line + 1, len - 2)) != NULL;
			if (grown)
				out->items = grown;
			out->count += ok;
		}
	}
	fclose(f);
	return ok;
}

static GroupList* group_list(Lists* lists, const char* group) {
	for (size_t i = 0; i < lists->count; ++i)
		if (strcmp(lists->groups[i].group, group) == 0)
			return &lists->groups[i];
	GroupList* grown = (GroupList*)realloc(lists->groups, (lists->count + 1) * sizeof *grown);
	if (!grown)
		return NULL;
	lists->groups = grown;
	GroupList* g  = &grown[lists->count++];
	memset(g, 0, sizeof *g);
	snprintf(g->group, sizeof g->group, "%s", group);
	return g;
}

typedef struct FlightGroupLists {
	int         fg;
	const char* start[2]; /* the groups its start lines deploy, in order */
	size_t      start_count;
	Lists       lists;
} FlightGroupLists;

/* Flight group `fg` of `fgs`, added when new; NULL when out of memory. */
static FlightGroupLists* flight_group_lists(FlightGroupLists** fgs, size_t* count, int fg) {
	for (size_t i = 0; i < *count; ++i)
		if ((*fgs)[i].fg == fg)
			return &(*fgs)[i];
	FlightGroupLists* grown = (FlightGroupLists*)realloc(*fgs, (*count + 1) * sizeof *grown);
	if (!grown)
		return NULL;
	*fgs                    = grown;
	FlightGroupLists* entry = &grown[(*count)++];
	memset(entry, 0, sizeof *entry);
	entry->fg = fg;
	return entry;
}

static const struct {
	const char* element;
	const char* group;
} kStartElements[] = { { "close_SFoils", "sfoils" }, { "open_LandingGears", "landing_gear" } };

/* The start lines "fg, N, close_SFoils, 1" and "fg, N, open_LandingGears, 1"
 * of mission `base` (<mission>.txt, else [mission_tie] of <mission>.ini). */
static bool start_lines(const XwauRoots* roots, const XwauMissions* m, const char* base,
						FlightGroupLists** fgs, size_t* count) {
	char      rel[1100], path[1024];
	XwauLines lines;
	snprintf(rel, sizeof rel, "%s.txt", base);
	if (!XwauLines_Read(XwauRoots_FindFile(roots, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP,
						&lines))
		return false;
	bool ok = true;
	if (!lines.count) {
		snprintf(rel, sizeof rel, "%s.ini", base);
		ok = XwauLines_Read(XwauRoots_FindFile(roots, rel, path, sizeof path) ? path : NULL, "mission_tie",
							XWAU_RULES_CPP, &lines);
	}
	for (size_t i = 0; ok && i < lines.count; ++i) {
		XwauLines tokens;
		if (!(ok = Xwau_Tokenize(lines.items[i], XWAU_RULES_CPP, &tokens)))
			break;
		int element = -1;
		for (int e = 0; tokens.count >= 4 && Xwau_SameName(tokens.items[0], "fg") && e < 2; ++e)
			if (Xwau_SameName(tokens.items[2], kStartElements[e].element))
				element = e;
		long long  fg = 0, value = 0;
		const bool numbers =
			element >= 0 && Xwau_Stoi(tokens.items[1], &fg) && Xwau_Stoi(tokens.items[3], &value);
		if (element >= 0 && !numbers)
			report(m, base, "mission_tie: line '%s' skipped: not a number (XWAU would fail)", lines.items[i]);
		if (numbers && fg >= 0 && fg <= FLIGHT_GROUP_MAX && value == 1) {
			FlightGroupLists* entry = flight_group_lists(fgs, count, (int)fg);
			const char*       group = kStartElements[element].group;
			bool              found = false;
			ok                      = entry != NULL;
			for (size_t k = 0; ok && k < entry->start_count; ++k)
				found |= entry->start[k] == group;
			if (ok && !found)
				entry->start[entry->start_count++] = group;
		}
		XwauLines_Free(&tokens);
	}
	XwauLines_Free(&lines);
	return ok;
}

/* The lists of each flight group of mission `base`: its [<Section>_fg_N]
 * sections. */
static bool mission_lists(const XwauRoots* roots, const XwauMissions* m, const char* base,
						  FlightGroupLists** out, size_t* count) {
	char rel[1100], ini[1024];
	snprintf(rel, sizeof rel, "%s.ini", base);
	const bool has_ini = XwauRoots_FindFile(roots, rel, ini, sizeof ini);
	XwauLines  sections;
	if (!ini_sections(has_ini ? ini : NULL, &sections))
		return false;
	/* Then <mission>_<section>.txt files, each root's folder in listing order. */
	const char* slash = strrchr(base, '/');
	char        dir_name[256], prefix[512];
	snprintf(dir_name, sizeof dir_name, "%.*s", (int)(slash - base), base);
	snprintf(prefix, sizeof prefix, "%s_", slash + 1);
	bool ok = true;
	for (int r = 0; ok && r < roots->count; ++r) {
		char dir[1024];
		if (!XwauRoots_FindDir(roots, r, dir_name, dir, sizeof dir))
			continue;
		size_t n       = 0;
		char** entries = Xwau_ListDir(dir, false, &n);
		for (size_t i = 0; ok && i < n; ++i) {
			const size_t len = strlen(entries[i]), plen = strlen(prefix);
			if (len >= plen + 4 && strncasecmp(entries[i], prefix, plen) == 0 &&
				ends_with(entries[i], ".txt")) {
				char** grown = (char**)realloc(sections.items, (sections.count + 1) * sizeof *grown);
				ok = grown &&
					 (grown[sections.count] = Xwau_Strndup(entries[i] + plen, len - plen - 4)) != NULL;
				if (grown)
					sections.items = grown;
				sections.count += ok;
			}
		}
		Xwau_FreeList(entries, n);
	}
	for (size_t s = 0; ok && s < sections.count; ++s) {
		char section_base[512], group[512];
		int  fg;
		if (!split_fg_section(sections.items[s], section_base, sizeof section_base, &fg) ||
			!XwauModel_SectionGroup(section_base, group, sizeof group))
			continue;
		bool seen = false;
		for (size_t j = 0; !seen && j < s; ++j)
			seen = Xwau_SameName(sections.items[j], sections.items[s]);
		if (seen || fg > FLIGHT_GROUP_MAX)
			continue;
		if (strlen(group) > TAG_MAX) {
			report(m, base, "%s: skipped: group '%s' is longer than %d characters", sections.items[s], group,
				   TAG_MAX);
			continue;
		}
		XwauLines lines;
		snprintf(rel, sizeof rel, "%s_%s.txt", base, sections.items[s]);
		char path[1024];
		ok = XwauLines_Read(XwauRoots_FindFile(roots, rel, path, sizeof path) ? path : NULL, "",
							XWAU_RULES_CPP, &lines);
		if (ok && !lines.count)
			ok = XwauLines_Read(has_ini ? ini : NULL, sections.items[s], XWAU_RULES_CPP, &lines);
		/* The hook's settings keys are not flight-group ones. */
		for (size_t i = 0; ok && i < lines.count; ++i) {
			XwauLines tokens;
			ok = Xwau_Tokenize(lines.items[i], XWAU_RULES_CPP, &tokens);
			if (ok && strchr(lines.items[i], '=') && tokens.count < 2)
				report(m, base, "%s: setting '%s' not imported", sections.items[s], lines.items[i]);
			XwauLines_Free(&tokens);
		}
		XwauMeshLine* mesh_lines = NULL;
		size_t        mesh_count = 0;
		ok = ok && XwauModel_MeshLines(&lines, &mesh_lines, &mesh_count, m->report ? base : NULL,
									   sections.items[s]);
		XwauLines_Free(&lines);
		/* The flight group's lists, and its group's meshes. */
		FlightGroupLists* entry = ok ? flight_group_lists(out, count, fg) : NULL;
		GroupList*        list  = entry ? group_list(&entry->lists, group) : NULL;
		ok                      = ok && list;
		for (size_t i = 0; ok && i < mesh_count; ++i) {
			const long long* v       = mesh_lines[i].values;
			const char*      section = sections.items[s];
			if (v[0] < 0 || v[0] > COMPONENT_MAX) {
				report(m, base, "%s: mesh %lld skipped: outside 0..%d", section, v[0], COMPONENT_MAX);
				continue;
			}
			if (v[1] < -128 || v[1] > 255) {
				report(m, base, "%s: mesh %lld skipped: angle %lld outside -128..255", section, v[0], v[1]);
				continue;
			}
			if (v[2] < 0 || v[2] > 255 || v[3] < 0 || v[3] > 255) {
				report(m, base, "%s: mesh %lld skipped: speeds %lld, %lld outside 0..255", section, v[0],
					   v[2], v[3]);
				continue;
			}
			bool listed = false;
			for (size_t k = 0; !listed && k < list->count; ++k)
				listed = list->meshes[k].mesh == (int)v[0];
			if (listed) {
				report(m, base, "%s: mesh %lld listed again; the first line is kept", section, v[0]);
				continue;
			}
			MeshEntry* grown = (MeshEntry*)realloc(list->meshes, (list->count + 1) * sizeof *grown);
			ok               = grown != NULL;
			if (ok) {
				list->meshes                = grown;
				list->meshes[list->count++] = (MeshEntry) { (int)v[0], (int)v[1], (int)v[2], (int)v[3] };
			}
		}
		free(mesh_lines);
		/* A group without meshes is no list. */
		if (ok && list && !list->count) {
			free(list->meshes);
			*list = entry->lists.groups[--entry->lists.count];
		}
	}
	XwauLines_Free(&sections);
	return ok;
}

static bool same_lists(const Lists* a, const Lists* b) {
	if (a->count != b->count)
		return false;
	for (size_t i = 0; i < a->count; ++i) {
		const GroupList* other = NULL;
		for (size_t j = 0; !other && j < b->count; ++j)
			if (strcmp(a->groups[i].group, b->groups[j].group) == 0)
				other = &b->groups[j];
		if (!other || other->count != a->groups[i].count)
			return false;
		for (size_t k = 0; k < other->count; ++k) {
			bool found = false;
			for (size_t l = 0; !found && l < other->count; ++l)
				found = memcmp(&a->groups[i].meshes[k], &other->meshes[l], sizeof(MeshEntry)) == 0;
			if (!found)
				return false;
		}
	}
	return true;
}

/* The craft list rows: the lower-case model names of
 * FlightModels/Spacecraft0.LST. */
static bool craft_list(const XwauRoots* roots, XwauLines* rows) {
	memset(rows, 0, sizeof *rows);
	char      path[1024];
	XwauLines lines;
	if (!XwauRoots_FindFile(roots, "FlightModels/Spacecraft0.LST", path, sizeof path))
		return true;
	FILE* f = fopen(path, "rb");
	if (!f)
		return true;
	char line[1024];
	bool ok = true;
	(void)lines;
	while (ok && fgets(line, sizeof line, f)) {
		Xwau_Trim(line, XWAU_RULES_CS);
		if (!line[0])
			continue;
		const char* name = line;
		for (const char* c = line; *c; ++c)
			if (*c == '/' || *c == '\\')
				name = c + 1;
		char  key[512];
		char* dot = NULL;
		lower_copy(key, sizeof key, name);
		dot = strrchr(key, '.');
		if (dot)
			*dot = 0;
		char** grown = (char**)realloc(rows->items, (rows->count + 1) * sizeof *grown);
		ok           = grown && (grown[rows->count] = Xwau_Strdup(key)) != NULL;
		if (grown)
			rows->items = grown;
		rows->count += ok;
	}
	fclose(f);
	return ok;
}

XwauTieFlightGroup* XwauTie_FlightGroups(const char* path, size_t* count) {
	*count  = 0;
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t header[4];
	int16_t groups = 0;
	if (fread(header, 1, 4, f) == 4)
		groups = (int16_t)(header[2] | (header[3] << 8));
	XwauTieFlightGroup* out = (XwauTieFlightGroup*)calloc(groups > 0 ? (size_t)groups : 1, sizeof *out);
	for (int16_t i = 0; out && i < groups; ++i) {
		uint8_t record[TIE_FLIGHT_GROUP_MARKINGS + 1];
		if (fseek(f, TIE_HEADER_SIZE + (long)i * TIE_FLIGHT_GROUP_SIZE, SEEK_SET) != 0 ||
			fread(record, 1, sizeof record, f) != sizeof record)
			break;
		XwauTieFlightGroup* g = &out[(*count)++];
		memcpy(g->name, record, TIE_FLIGHT_GROUP_NAME);
		g->name[TIE_FLIGHT_GROUP_NAME] = 0;
		g->craft                       = record[TIE_FLIGHT_GROUP_CRAFT];
		g->markings                    = record[TIE_FLIGHT_GROUP_MARKINGS];
	}
	fclose(f);
	return out;
}

static ModelVariants* model_variants(XwauMissions* m, const char* model, bool add) {
	for (size_t i = 0; i < m->model_count; ++i)
		if (strcmp(m->models[i].model, model) == 0)
			return &m->models[i];
	if (!add)
		return NULL;
	ModelVariants* grown = (ModelVariants*)realloc(m->models, (m->model_count + 1) * sizeof *grown);
	if (!grown)
		return NULL;
	m->models        = grown;
	ModelVariants* v = &grown[m->model_count++];
	memset(v, 0, sizeof *v);
	snprintf(v->model, sizeof v->model, "%s", model);
	return v;
}

static int compare_fg(const void* a, const void* b) {
	return ((const FlightGroupLists*)a)->fg - ((const FlightGroupLists*)b)->fg;
}

/* Flight group `fg` of mission `base`'s articulations; NULL when out of
 * memory. */
static XwauFlightGroupArticulation* add_articulation(XwauMissions* m, const char* base, int fg) {
	MissionArticulations* mission = m->mission_count ? &m->missions[m->mission_count - 1] : NULL;
	if (!mission || strcmp(mission->base, base) != 0) {
		MissionArticulations* grown =
			(MissionArticulations*)realloc(m->missions, (m->mission_count + 1) * sizeof *grown);
		if (!grown)
			return NULL;
		m->missions = grown;
		mission     = &grown[m->mission_count++];
		memset(mission, 0, sizeof *mission);
		snprintf(mission->base, sizeof mission->base, "%s", base);
	}
	XwauFlightGroupArticulation* grown =
		(XwauFlightGroupArticulation*)realloc(mission->fgs, (mission->count + 1) * sizeof *grown);
	if (!grown)
		return NULL;
	mission->fgs                       = grown;
	XwauFlightGroupArticulation* entry = &grown[mission->count++];
	memset(entry, 0, sizeof *entry);
	entry->flight_group = fg;
	return entry;
}

static bool load_variants(const XwauRoots* roots, XwauMissions* m) {
	XwauLines rows;
	if (!craft_list(roots, &rows))
		return false;
	bool ok = true;
	for (size_t d = 0; ok && d < 2; ++d) {
		XwauLines names;
		ok = XwauMissions_Names(roots, kMissionDirs[d], &names);
		for (size_t n = 0; ok && n < names.count; ++n) {
			char base[1100];
			snprintf(base, sizeof base, "%s/%s", kMissionDirs[d], names.items[n]);
			FlightGroupLists* fgs   = NULL;
			size_t            count = 0;
			ok = start_lines(roots, m, base, &fgs, &count) && mission_lists(roots, m, base, &fgs, &count);
			size_t              group_count = 0;
			XwauTieFlightGroup* groups      = NULL;
			char                rel[1200], tie[1024];
			snprintf(rel, sizeof rel, "%s.tie", base);
			if (ok && count && XwauRoots_FindFile(roots, rel, tie, sizeof tie))
				groups = XwauTie_FlightGroups(tie, &group_count);
			if (count)
				qsort(fgs, count, sizeof *fgs, compare_fg);
			for (size_t i = 0; ok && i < count; ++i) {
				char variant[128] = "";
				if (fgs[i].lists.count) {
					const int      row   = (size_t)fgs[i].fg < group_count
											   ? ModelTables_CraftListRow(groups[fgs[i].fg].craft)
											   : -1;
					ModelVariants* model = NULL;
					if (row < 0 || (size_t)row >= rows.count)
						report(m, base,
							   "flight group %d: articulation lists dropped: no model for its craft type",
							   fgs[i].fg);
					else
						ok = (model = model_variants(m, rows.items[row], true)) != NULL;
					for (size_t v = 0; ok && model && !variant[0] && v < model->count; ++v)
						if (same_lists(&model->variants[v].lists, &fgs[i].lists))
							snprintf(variant, sizeof variant, "%s", model->variants[v].name);
					if (ok && model && !variant[0]) {
						Variant* grown =
							(Variant*)realloc(model->variants, (model->count + 1) * sizeof *grown);
						ok = grown != NULL;
						if (ok) {
							model->variants = grown;
							Variant* v      = &grown[model->count++];
							memset(v, 0, sizeof *v);
							/* The name: "<mission>_fg<N>", other than [a-z0-9_] as '_'. */
							snprintf(v->name, sizeof v->name, "%s_fg%d", names.items[n], fgs[i].fg);
							for (char* c = v->name; *c; ++c)
								if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
									*c = '_';
							v->lists     = fgs[i].lists;
							fgs[i].lists = (Lists) { 0 };
							snprintf(variant, sizeof variant, "%s", v->name);
						}
					}
				}
				if (!ok || (!fgs[i].start_count && !variant[0]))
					continue;
				XwauFlightGroupArticulation* entry = add_articulation(m, base, fgs[i].fg);
				ok                                 = entry != NULL;
				if (ok) {
					memcpy(entry->start, fgs[i].start, sizeof entry->start);
					entry->start_count = fgs[i].start_count;
					snprintf(entry->variant, sizeof entry->variant, "%s", variant);
				}
			}
			free(groups);
			for (size_t i = 0; i < count; ++i)
				lists_free(&fgs[i].lists);
			free(fgs);
		}
		XwauLines_Free(&names);
	}
	XwauLines_Free(&rows);
	return ok;
}

XwauMissions* XwauMissions_Load(const XwauRoots* roots, bool report_problems) {
	XwauMissions* m = (XwauMissions*)calloc(1, sizeof *m);
	if (m)
		m->report = report_problems;
	if (m && load_skin_contexts(roots, m) && load_variants(roots, m))
		return m;
	XwauMissions_Free(m);
	return NULL;
}

size_t XwauMissions_FlightGroupArticulations(const XwauMissions* m, const char* base,
											 const XwauFlightGroupArticulation** out) {
	for (size_t i = 0; i < m->mission_count; ++i) {
		if (strcmp(m->missions[i].base, base) == 0) {
			*out = m->missions[i].fgs;
			return m->missions[i].count;
		}
	}
	*out = NULL;
	return 0;
}

void XwauMissions_Free(XwauMissions* m) {
	if (!m)
		return;
	for (size_t i = 0; i < m->context_count; ++i)
		XwauLines_Free(&m->contexts[i]);
	free(m->contexts);
	for (size_t i = 0; i < m->model_count; ++i) {
		for (size_t v = 0; v < m->models[i].count; ++v)
			lists_free(&m->models[i].variants[v].lists);
		free(m->models[i].variants);
	}
	free(m->models);
	for (size_t i = 0; i < m->mission_count; ++i)
		free(m->missions[i].fgs);
	free(m->missions);
	free(m);
}

static int compare_variants(const void* a, const void* b) {
	return strcmp((*(const Variant* const*)a)->name, (*(const Variant* const*)b)->name);
}

static int compare_meshes(const void* a, const void* b) {
	return ((const MeshEntry*)a)->mesh - ((const MeshEntry*)b)->mesh;
}

bool XwauMissions_Articulations(const XwauMissions* missions, const char* model,
								OptGltfComponentArticulation* out, size_t* count, size_t cap,
								const char* label) {
	char key[256];
	lower_copy(key, sizeof key, model);
	ModelVariants* entry = model_variants((XwauMissions*)missions, key, false);
	if (!entry || !entry->count)
		return true;
	const Variant** sorted = (const Variant**)malloc(entry->count * sizeof *sorted);
	if (!sorted)
		return false;
	for (size_t i = 0; i < entry->count; ++i)
		sorted[i] = &entry->variants[i];
	qsort(sorted, entry->count, sizeof *sorted, compare_variants);
	bool ok = true;
	for (size_t i = 0; ok && i < entry->count; ++i) {
		for (size_t g = 0; ok && g < sorted[i]->lists.count; ++g) {
			GroupList* list = &sorted[i]->lists.groups[g];
			char       tag[1024];
			snprintf(tag, sizeof tag, "variant.%s.%s", sorted[i]->name, list->group);
			if (strlen(tag) > TAG_MAX) {
				Report(label, "variant %s: skipped: group '%s' is longer than %d characters", sorted[i]->name,
					   tag, TAG_MAX);
				continue;
			}
			qsort(list->meshes, list->count, sizeof *list->meshes, compare_meshes);
			for (size_t k = 0; ok && k < list->count; ++k) {
				if (*count == cap) {
					ok = false;
					break;
				}
				OptGltfComponentArticulation* a = &out[(*count)++];
				memset(a, 0, sizeof *a);
				a->component  = (uint32_t)list->meshes[k].mesh;
				a->angle      = list->meshes[k].angle;
				a->speed      = list->meshes[k].speed;
				a->speed_back = list->meshes[k].speed_back;
				snprintf(a->group, sizeof a->group, "%s", tag);
			}
		}
	}
	free(sorted);
	return ok;
}
