#include "xwau_weapon_models.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "util.h"

bool XwauWeaponModels_HookReads(const char* game) {
	return Xwau_DllHasText(game, "hook_mission_tie.dll", "OptReplace");
}

/* Reads the value of one weapon into `out`. Returns false when the game
   can't use the value. */
static bool read_value(const char* game, const XwauSection* section, const char* key, const char* value,
					   bool report, char* out, size_t cap) {
	char path[512], found[1100];
	if (strlen(value) >= sizeof path) {
		if (report)
			Report(section->source, "%s: the path is too long. The shots keep their own model", key);
		return false;
	}
	/* The hook loads the path as it is, from the game folder. */
	size_t length = 0;
	for (; value[length]; ++length)
		path[length] = value[length] == '\\' ? '/' : value[length];
	path[length] = '\0';
	const char* name = path + strlen("FlightModels/");
	if (strncasecmp(path, "FlightModels/", strlen("FlightModels/")) != 0 || strchr(name, '/') ||
		!EndsWithNoCase(name, ".opt")) {
		if (report)
			Report(section->source,
				   "%s = %s: the import only converts the models of FlightModels. The shots keep their own "
				   "model",
				   key, value);
		return false;
	}
	if (!Xwau_FindFile(game, path, found, sizeof found)) {
		if (report)
			Report(section->source, "%s = %s: the file doesn't exist. The shots keep their own model", key,
				   value);
		return false;
	}
	if (!Xwau_ModelKey(path, out, cap)) {
		if (report)
			Report(section->source, "%s = %s: the model name is too long. The shots keep their own model", key,
				   value);
		return false;
	}
	return true;
}

bool XwauWeaponModels_Read(const char* game, const char* model, bool report, XwauWeaponModels* out) {
	char         keys[XWAU_WEAPON_MODELS_COUNT][16];
	const char*  known[XWAU_WEAPON_MODELS_COUNT + 1];
	XwauSection  section;
	memset(out, 0, sizeof *out);
	if (!XwauSection_ReadModel(game, model, "OptReplace", XWAU_RULES_CPP, &section))
		return false;
	for (int weapon = 0; weapon < XWAU_WEAPON_MODELS_COUNT; ++weapon) {
		char value[1024];
		snprintf(keys[weapon], sizeof keys[weapon], "Weapon%d", XWAU_WEAPON_MODELS_FIRST + weapon);
		known[weapon] = keys[weapon];
		XwauSection_Text(&section, keys[weapon], value, sizeof value);
		if (value[0] && read_value(game, &section, keys[weapon], value, report, out->key[weapon],
								   sizeof out->key[weapon]))
			out->has = true;
	}
	known[XWAU_WEAPON_MODELS_COUNT] = NULL;
	if (report)
		XwauSection_ReportUnknownKeys(&section, known, NULL);
	XwauSection_Free(&section);
	return true;
}

/* Whether `keys` holds `key`. */
static bool has_key(const XwauLines* keys, const char* key) {
	for (size_t i = 0; i < keys->count; ++i)
		if (strcmp(keys->items[i], key) == 0)
			return true;
	return false;
}

bool XwauWeaponModels_FindAll(const char* game, XwauLines* keys) {
	char dir[1024];
	if (!XwauWeaponModels_HookReads(game) || !Xwau_FindDir(game, "FlightModels", dir, sizeof dir))
		return true;
	size_t count = 0;
	char** names = ListDir(dir, true, &count);
	bool   ok    = true;
	for (size_t i = 0; ok && i < count; ++i) {
		/* A model can have the section in its .ini file or a file of its
		   own. */
		static const char* const suffixes[] = { ".ini", "OptReplace.txt" };
		char                     model[256];
		const size_t             length = strlen(names[i]);
		bool                     found  = false;
		for (size_t s = 0; !found && s < sizeof suffixes / sizeof suffixes[0]; ++s) {
			const size_t suffix = strlen(suffixes[s]);
			if (EndsWithNoCase(names[i], suffixes[s]) && length > suffix && length - suffix < sizeof model) {
				memcpy(model, names[i], length - suffix);
				model[length - suffix] = '\0';
				found                  = true;
			}
		}
		if (!found || strcasecmp(model, "default") == 0)
			continue;
		XwauWeaponModels models;
		if (!XwauWeaponModels_Read(game, model, false, &models)) {
			ok = false;
			break;
		}
		for (int weapon = 0; ok && weapon < XWAU_WEAPON_MODELS_COUNT; ++weapon) {
			const char* key = models.key[weapon];
			if (key[0] && !has_key(keys, key))
				ok = XwauLines_Add(keys, key, strlen(key));
		}
	}
	FreeList(names, count);
	return ok;
}
