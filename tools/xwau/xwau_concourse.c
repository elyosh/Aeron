#include "xwau_concourse.h"

#include <string.h>

#include "util.h"
#include "xwau_config.h"

/* The value of `key` from `min` to `max`, read as std::stoi(value, 0, 0)
 * reads it, or -1 when the key is missing, when its value isn't a number,
 * which XWAU fails on, or when the import fails on it. */
static int planet_value(const XwauLines* lines, const char* key, long long min, long long max,
						const char* base) {
	char      text[256];
	long long value = 0;
	XwauLines_Value(lines, key, XWAU_RULES_CPP, text, sizeof text);
	if (!text[0])
		return -1;
	if (!Xwau_StoiAuto(text, &value))
		return -1;
	if (value != -1 && (value < min || value > max)) {
		ReportFatal(base, "[concourse]: %s = %lld, and the importer only takes %lld to %lld", key, value, min,
					max);
		return -1;
	}
	return (int)value;
}

bool XwauFrontPlanet_Read(const char* game, const char* base, const int design_scale[2],
						  XwauFrontPlanet* out) {
	out->image        = -1;
	out->has_position = false;
	char       path[1024];
	const bool hook = Xwau_FindFile(game, "hook_concourse.dll", path, sizeof path);
	char       rel[1100];
	XwauLines lines;
	if (!PathFormat(rel, sizeof rel, "%s_concourse.txt", base) ||
		!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP,
						&lines))
		return false;
	if (!lines.count && (!PathFormat(rel, sizeof rel, "%s.ini", base) ||
						 !XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL,
										 "concourse", XWAU_RULES_CPP, &lines)))
		return false;
	/* Without hook_concourse.dll, XWAU never reads these keys. */
	if (!hook) {
		XwauLines_Free(&lines);
		return true;
	}
	/* The game reads image groups as 16-bit numbers. */
	out->image = planet_value(&lines, "FrontPlanetIndex", 1, 32767, base);
	const int x = planet_value(&lines, "FrontPlanetPositionX", -32768, 32767, base);
	const int y = planet_value(&lines, "FrontPlanetPositionY", -32768, 32767, base);
	if ((x == -1) != (y == -1))
		ReportFatal(base,
					"[concourse]: only one of FrontPlanetPositionX and FrontPlanetPositionY is set, and "
					"the importer can only write both");
	else if (x != -1) {
		out->has_position = true;
		out->position[0]  = x * design_scale[0];
		out->position[1]  = y * design_scale[1];
	}
	XwauLines_Free(&lines);
	return true;
}

bool XwauFrontPlanet_Any(const XwauFrontPlanet* planet) {
	return planet->image != -1 || planet->has_position;
}

void XwauFrontPlanet_Write(FILE* f, const XwauFrontPlanet* planet) {
	if (!XwauFrontPlanet_Any(planet))
		return;
	fputs("concourse:\n  front_planet: {", f);
	const char* separator = " ";
	if (planet->has_position) {
		fprintf(f, "%sposition: [%d, %d]", separator, planet->position[0], planet->position[1]);
		separator = ", ";
	}
	if (planet->image != -1)
		fprintf(f, "%simage: %d", separator, planet->image);
	fputs(" }\n", f);
}
