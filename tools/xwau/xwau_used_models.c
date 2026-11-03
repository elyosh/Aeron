#include "xwau_used_models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

/* The cockpit name of a craft record. */
enum { RECORD_NAME_OFFSET = 50, RECORD_NAME_SIZE = 256 };

/* What the lines of a section or a file give. */
typedef enum Kind {
	KIND_NONE,
	/* "<path> = <path>" lines whose targets are models. */
	KIND_SWAPS,
	/* Like KIND_SWAPS, but the targets can be craft. */
	KIND_CRAFT_SWAPS,
	/* [Turrets] lines, with the cockpit model in column 8. */
	KIND_TURRETS,
} Kind;

bool XwauUsedModels_Has(const XwauLines* keys, const char* key) {
	for (size_t i = 0; i < keys->count; ++i)
		if (strcmp(keys->items[i], key) == 0)
			return true;
	return false;
}

static bool add_key(XwauLines* keys, const char* key) {
	return !key[0] || XwauUsedModels_Has(keys, key) || XwauLines_Add(keys, key, strlen(key));
}

/* Adds the key of the model `name` (a file name or a path) followed by
 * `suffix`. */
static bool add_name(XwauLines* keys, const char* name, const char* suffix) {
	char key[512], full[600];
	if (!Xwau_ModelKey(name, key, sizeof key))
		return true;
	snprintf(full, sizeof full, "%s%s", key, suffix);
	LowerInPlace(full);
	return add_key(keys, full);
}

/* Adds the model of a craft, with the gunner cockpits and the hangar that
 * XWAU builds from its name. */
static bool add_craft(XwauLines* keys, const char* name) {
	return add_name(keys, name, "") && add_name(keys, name, "Gunner") && add_name(keys, name, "Gunner2") &&
		   add_name(keys, name, "Hangar");
}

/* Adds the model at `path`, a swap target or a turret model. A path in
 * another folder than FlightModels is counted as a feature that the import
 * doesn't convert, because the import only converts the models of
 * FlightModels. */
static bool add_target(XwauLines* keys, const char* path, bool craft) {
	const char* slash = NULL;
	for (const char* c = path; *c; ++c)
		if (*c == '\\' || *c == '/')
			slash = c;
	if (slash && !((size_t)(slash - path) == strlen("FlightModels") &&
				   strncasecmp(path, "FlightModels", strlen("FlightModels")) == 0)) {
		ReportUnsupported("swap and turret models outside FlightModels");
		return true;
	}
	return craft ? add_craft(keys, path) : add_name(keys, path, "");
}

static Kind section_kind(const char* name) {
	if (strcasecmp(name, "Objects") == 0)
		return KIND_CRAFT_SWAPS;
	if (strncasecmp(name, "HangarObjects", strlen("HangarObjects")) == 0 ||
		strcasecmp(name, "OptReplace") == 0)
		return KIND_SWAPS;
	if (strcasecmp(name, "Turrets") == 0)
		return KIND_TURRETS;
	return KIND_NONE;
}

/* The kind of the lines of a .txt file, from its name. */
static Kind text_file_kind(const char* name) {
	char lower[512];
	LowerCopy(lower, sizeof lower, name);
	if (strstr(lower, "hangarobjects") || strstr(lower, "optreplace"))
		return KIND_SWAPS;
	if (strstr(lower, "objects"))
		return KIND_CRAFT_SWAPS;
	if (strstr(lower, "turrets"))
		return KIND_TURRETS;
	return KIND_NONE;
}

static void trim(char* text) {
	size_t length = strlen(text);
	while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r'))
		text[--length] = 0;
	size_t start = 0;
	while (text[start] == ' ' || text[start] == '\t')
		++start;
	memmove(text, text + start, length - start + 1);
}

/* Reads one line of a section or a file of kind `kind`. */
static bool read_line(XwauLines* keys, char* line, Kind kind) {
	if (kind == KIND_TURRETS) {
		if (strchr(line, '='))
			return true;
		/* The values are separated by ',' and spaces. */
		int         column = 0;
		const char* token  = strtok(line, ", \t");
		while (token && column < 7) {
			token = strtok(NULL, ", \t");
			++column;
		}
		return !token || add_target(keys, token, false);
	}
	char* equals = strchr(line, '=');
	if (!equals)
		return true;
	char* value   = equals + 1;
	char* comment = strchr(value, ';');
	if (comment)
		*comment = 0;
	trim(value);
	return !EndsWithNoCase(value, ".opt") || add_target(keys, value, kind == KIND_CRAFT_SWAPS);
}

/* Reads the swap, OptReplace and turret lines of the .ini or .txt file
 * `path`. In an .ini file, they are in their sections. A .txt file holds
 * the lines of the kind `kind`. */
static bool read_file(XwauLines* keys, const char* path, Kind kind, bool ini) {
	size_t size = 0;
	char*  text = ReadWholeFile(path, &size);
	if (!text)
		return true;
	bool ok = true;
	for (char* line = text; ok && line;) {
		char* next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		trim(line);
		const size_t length = strlen(line);
		if (ini && line[0] == '[' && length > 1 && line[length - 1] == ']') {
			line[length - 1] = 0;
			kind             = section_kind(line + 1);
		} else if (kind != KIND_NONE && line[0] && line[0] != ';' && line[0] != '#' &&
				   strncmp(line, "//", 2) != 0) {
			ok = read_line(keys, line, kind);
		}
		line = next;
	}
	free(text);
	return ok;
}

/* Reads the .ini and .txt files of the folder `rel` of the game. */
static bool read_folder(XwauLines* keys, const char* root, const char* rel) {
	char dir[1024];
	if (!Xwau_FindDir(root, rel, dir, sizeof dir))
		return true;
	size_t count = 0;
	char** names = ListDir(dir, true, &count);
	bool   ok    = true;
	for (size_t i = 0; ok && i < count; ++i) {
		char       path[2048];
		const bool ini  = EndsWithNoCase(names[i], ".ini");
		const Kind kind = ini ? KIND_NONE : text_file_kind(names[i]);
		if ((ini || (EndsWithNoCase(names[i], ".txt") && kind != KIND_NONE)) &&
			PathFormat(path, sizeof path, "%s/%s", dir, names[i]) && IsFile(path))
			ok = read_file(keys, path, kind, ini);
	}
	FreeList(names, count);
	return ok;
}

/* Adds the proving ground hangar of ProvingGroundHangarModel. The default,
 * "FlightModels\", gives Hangar.opt, which is always added. */
static bool add_proving_ground_hangar(XwauLines* keys, const char* root) {
	static const struct {
		const char* file;
		const char* section;
	} sources[] = { { "Hooks.ini", "hook_hangars" }, { "hook_hangars.cfg", "" } };

	bool ok = true;
	for (size_t i = 0; ok && i < sizeof sources / sizeof sources[0]; ++i) {
		char      path[1024], value[512];
		XwauLines lines;
		if (!Xwau_FindFile(root, sources[i].file, path, sizeof path))
			continue;
		if (!XwauLines_Read(path, sources[i].section, XWAU_RULES_CPP, &lines))
			return false;
		XwauLines_Value(&lines, "ProvingGroundHangarModel", XWAU_RULES_CPP, value, sizeof value);
		XwauLines_Free(&lines);
		if (value[0])
			ok = add_name(keys, value, "Hangar");
	}
	return ok;
}

bool XwauUsedModels_Find(const char* root, const XwauGameTables* tables, XwauLines* keys) {
	static const char* const kFixed[]   = { "tiecockpit", "combatsimcockpit", "corelliantransportgunner",
											"hangar" };
	static const char* const kFolders[] = { "FlightModels", "Missions", "Melee", "Combat", "Skirmish" };
	bool                     ok         = true;
	for (size_t i = 0; ok && i < sizeof kFixed / sizeof kFixed[0]; ++i)
		ok = add_key(keys, kFixed[i]);
	for (int list = 0; ok && list < 2; ++list)
		for (size_t row = 0; ok && row < tables->rowCount[list]; ++row)
			ok = add_craft(keys, tables->rows[list][row]) &&
				 add_name(keys, tables->rows[list][row], "Exterior");
	for (int record = 0; ok && record < XWAU_GAME_RECORDS; ++record) {
		char name[RECORD_NAME_SIZE + 1];
		memcpy(name, tables->records[record] + RECORD_NAME_OFFSET, RECORD_NAME_SIZE);
		name[RECORD_NAME_SIZE] = 0;
		ok                     = add_name(keys, name, "Cockpit") && add_name(keys, name, "Exterior");
	}
	for (size_t i = 0; ok && i < sizeof kFolders / sizeof kFolders[0]; ++i)
		ok = read_folder(keys, root, kFolders[i]);
	return ok && add_proving_ground_hangar(keys, root);
}
