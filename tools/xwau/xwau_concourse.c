#include "xwau_concourse.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"
#include "xwau_dat.h"

/* The value of `key` from `min` to `max`, read as std::stoi(value, 0, 0)
 * reads it, or -1 when the key is missing or its value is reported. */
static int planet_value(const XwauLines* lines, const char* key, long long min, long long max,
						const char* base) {
	char      text[256];
	long long value = 0;
	XwauLines_Value(lines, key, XWAU_RULES_CPP, text, sizeof text);
	if (!text[0])
		return -1;
	if (!Xwau_StoiAuto(text, &value)) {
		Report(base, "[concourse]: %s: '%s' isn't a number, which XWAU would fail on; it is left out", key,
			   text);
		return -1;
	}
	if (value != -1 && (value < min || value > max)) {
		Report(base, "[concourse]: %s = %lld must be from %lld to %lld; it is left out", key, value, min,
			   max);
		return -1;
	}
	return (int)value;
}

bool XwauFrontPlanet_Read(const char* game, const char* base, XwauFrontPlanet* out) {
	out->image = out->x = out->y = -1;
	char path[1024];
	if (!Xwau_FindFile(game, "hook_concourse.dll", path, sizeof path))
		return true;
	char      rel[1100];
	XwauLines lines;
	if (!PathFormat(rel, sizeof rel, "%s_concourse.txt", base) ||
		!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP,
						&lines))
		return false;
	if (!lines.count && (!PathFormat(rel, sizeof rel, "%s.ini", base) ||
						 !XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL,
										 "concourse", XWAU_RULES_CPP, &lines)))
		return false;
	/* The game reads image groups as 16-bit numbers. */
	out->image = planet_value(&lines, "FrontPlanetIndex", 1, 32767, base);
	out->x     = planet_value(&lines, "FrontPlanetPositionX", -32768, 32767, base);
	out->y     = planet_value(&lines, "FrontPlanetPositionY", -32768, 32767, base);
	XwauLines_Free(&lines);
	return true;
}

bool XwauFrontPlanet_Any(const XwauFrontPlanet* planet) {
	return planet->image != -1 || planet->x != -1 || planet->y != -1;
}

void XwauFrontPlanet_Write(FILE* f, const XwauFrontPlanet* planet) {
	if (!XwauFrontPlanet_Any(planet))
		return;
	fputs("concourse:\n  front_planet: {", f);
	const char* separator = " ";
	if (planet->x != -1) {
		fprintf(f, "%sx: %d", separator, planet->x);
		separator = ", ";
	}
	if (planet->y != -1) {
		fprintf(f, "%sy: %d", separator, planet->y);
		separator = ", ";
	}
	if (planet->image != -1)
		fprintf(f, "%simage: %d", separator, planet->image);
	fputs(" }\n", f);
}

bool XwauDatGroups_Read(const char* game, const char* stock_game, XwauDatGroups* out) {
	memset(out, 0, sizeof *out);
	char path[1024];
	if (!Xwau_FindFile(game, "RESDATA.TXT", path, sizeof path) &&
		!Xwau_FindFile(stock_game, "RESDATA.TXT", path, sizeof path)) {
		Report(NULL,
			   "neither the mod nor the game has a RESDATA.TXT, so the concourse planets can't be checked");
		return true;
	}
	XwauLines lines;
	if (!XwauLines_Read(path, "", XWAU_RULES_CPP, &lines))
		return false;
	bool ok = true;
	for (size_t i = 0; ok && i < lines.count; ++i) {
		char rel[1024];
		snprintf(rel, sizeof rel, "%s", lines.items[i]);
		for (char* c = rel; *c; ++c)
			if (*c == '\\')
				*c = '/';
		if (!Xwau_FindFile(game, rel, path, sizeof path) &&
			!Xwau_FindFile(stock_game, rel, path, sizeof path))
			continue;
		int*   ids;
		size_t count;
		char   error[256];
		if (!XwauDat_ReadGroupIds(path, &ids, &count, error, sizeof error)) {
			Report(NULL, "RESDATA.TXT: %s", error);
			continue;
		}
		for (size_t k = 0; ok && k < count; ++k) {
			if (XwauDatGroups_Has(out, ids[k]))
				continue;
			int* added = ARRAY_PUSH(out->ids, out->count);
			if ((ok = added != NULL))
				*added = ids[k];
		}
		free(ids);
	}
	XwauLines_Free(&lines);
	if (!ok)
		XwauDatGroups_Free(out);
	return ok;
}

bool XwauDatGroups_Has(const XwauDatGroups* groups, int group) {
	for (size_t i = 0; i < groups->count; ++i)
		if (groups->ids[i] == group)
			return true;
	return false;
}

void XwauDatGroups_Free(XwauDatGroups* groups) {
	free(groups->ids);
	memset(groups, 0, sizeof *groups);
}
