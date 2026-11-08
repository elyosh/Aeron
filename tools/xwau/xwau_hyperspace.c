#include "xwau_hyperspace.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"
#include "xwau_missions.h"

/* The hook's default speed in short mode, in game units. */
#define HOOK_SHORT_SPEED 0x7000
/* A game unit of speed is 1/2.25 MGLT. */
#define GAME_UNITS_PER_MGLT 2.25

static const char* const kMissionDirs[] = { "Missions", "Melee", "Combat" };

/* Returns the ShortHyperspaceEffect value of `section`, or `fallback` when
 * the value is outside `low` to 1. A value that isn't a number would crash
 * XWAU (std::stoi), so it also gives `fallback`. */
static int mode_value(const XwauSection* section, int low, int fallback) {
	const long long value = XwauSection_IntOr(section, "ShortHyperspaceEffect", fallback);
	return value < low || value > 1 ? fallback : (int)value;
}

int XwauHyperspace_MissionMode(const char* game, const char* base, const XwauHyperspaceModes* modes) {
	XwauSection section;
	int         mode = -1;
	if (XwauSection_ReadMission(game, base, "Hyperspace", XWAU_RULES_CPP, &section))
		mode = mode_value(&section, -1, -1);
	XwauSection_Free(&section);
	return mode == -1 ? modes->global_mode : mode;
}

bool XwauHyperspace_ReadModes(const char* game, XwauHyperspaceModes* out) {
	char path[1024];
	memset(out, 0, sizeof *out);
	out->interdictor       = Xwau_FindFile(game, "hook_interdictor.dll", path, sizeof path);
	out->hyperspace        = Xwau_FindFile(game, "hook_hyperspace.dll", path, sizeof path);
	out->interdiction_look = Xwau_DllHasText(game, "ddraw_effects.dll", "[Interdiction]");
	if (!out->hyperspace)
		return true;
	XwauSection hook;
	if (!XwauSection_ReadHook(game, "hook_hyperspace", &hook))
		return false;
	out->global_mode = mode_value(&hook, 0, 0);
	XwauSection_Free(&hook);
	/* A mission without its own value uses the global one. */
	out->any_short = out->global_mode == 1;
	for (size_t d = 0; !out->any_short && d < sizeof kMissionDirs / sizeof kMissionDirs[0]; ++d) {
		XwauLines names;
		if (!XwauMissions_Names(game, kMissionDirs[d], &names))
			return false;
		for (size_t i = 0; !out->any_short && i < names.count; ++i) {
			char base[1100];
			snprintf(base, sizeof base, "%s/%s", kMissionDirs[d], names.items[i]);
			out->any_short = XwauHyperspace_MissionMode(game, base, out) == 1;
		}
		XwauLines_Free(&names);
	}
	return true;
}

int XwauHyperspace_MissionUseSpeeds(const char* game, const char* base, const XwauHyperspaceModes* modes) {
	if (!modes->hyperspace || !modes->any_short)
		return -1;
	const int mode = XwauHyperspace_MissionMode(game, base, modes);
	if (modes->global_mode == 1)
		return mode == 0 ? 0 : -1;
	return mode == 1 ? 1 : -1;
}

bool XwauHyperspace_DefaultUseSpeedsOff(const XwauHyperspaceModes* modes) {
	return modes->hyperspace && modes->any_short && modes->global_mode == 0;
}

static void write_mglt(FILE* f, double game_units) { fprintf(f, "%.3f", game_units / GAME_UNITS_PER_MGLT); }

bool XwauHyperspace_WriteFlightSettings(FILE* f, const XwauHyperspaceModes* modes) {
	if (!modes->hyperspace || !modes->any_short)
		return false;
	/* The hook doubles a departing craft's speed on every frame, so it
	   reaches the cap in a few frames. 0.1 s gives the same result without
	   depending on the frame rate. */
	fputs("hyperspace:\n  arrival_speed_mglt: ", f);
	write_mglt(f, HOOK_SHORT_SPEED);
	fputs("\n  departure_speed_mglt: ", f);
	write_mglt(f, HOOK_SHORT_SPEED);
	fputs("\n  departure_acceleration_mglt: ", f);
	write_mglt(f, HOOK_SHORT_SPEED / 0.1);
	fputs("\n  arrival_braking_seconds: 0.5\n", f);
	return true;
}

/* Reads `key`, a speed of the [Hyperspace] section, into `out`. A missing
 * key, and -1, which the hook reads as "use the default", leave `out` at 0. */
static void read_speed(const XwauSection* section, const char* key, int* out) {
	long long       value;
	const XwauValue result = XwauSection_Int(section, key, &value);
	if (result != XWAU_VALUE_SET || value == -1)
		return;
	/* The hook stores the speed in 16 bits, so a larger one wraps. A speed
	   of 0 or less never moves the craft. */
	if (value < 1 || value > 32767) {
		Report(section->source, "%s is %lld, and the hook only works with 1 to 32767; it is left out", key,
			   value);
		return;
	}
	*out = (int)value;
}

void XwauHyperspace_ReadModel(const XwauHyperspaceModes* modes, const char* model,
							  const char* interdictor_txt, const char* hyperspace_txt, const char* ini,
							  XwauCraftHyperspace* out) {
	memset(out, 0, sizeof *out);
	/* default.ini isn't a model. */
	if (strcasecmp(model, "default") == 0)
		return;
	XwauSection section;
	if (modes->interdictor) {
		XwauSection_ReadPaths(interdictor_txt, ini, "Interdictor", XWAU_RULES_CPP, &section);
		if (section.lines.count) {
			long long value;
			/* A value that isn't a number would crash XWAU, so the section
			   is then left out. Without the key, the hook reads 0. */
			const XwauValue result = XwauSection_Int(&section, "IsInterdictor", &value);
			out->has_gravity_well  = result != XWAU_VALUE_BAD;
			out->gravity_well      = result == XWAU_VALUE_SET && value != 0;
		}
		XwauSection_Free(&section);
	}
	if (modes->hyperspace) {
		XwauSection_ReadPaths(hyperspace_txt, ini, "Hyperspace", XWAU_RULES_CPP, &section);
		if (section.lines.count && modes->any_short) {
			read_speed(&section, "OutOfHyperspaceSpeed", &out->arrival_speed);
			read_speed(&section, "IntoHyperspaceSpeed", &out->departure_speed);
		} else if (section.lines.count) {
			ReportRepeated(
				"the [Hyperspace] speeds of models, which XWAU ignores because ShortHyperspaceEffect "
				"is 0 in every mission");
		}
		XwauSection_Free(&section);
	}
}

bool XwauHyperspace_HasModel(const XwauCraftHyperspace* model) {
	return model->has_gravity_well || model->arrival_speed || model->departure_speed;
}

void XwauHyperspace_WriteModel(FILE* f, const XwauCraftHyperspace* model) {
	const char* separator = "";
	fputs("hyperspace: {", f);
	if (model->has_gravity_well) {
		fprintf(f, "gravity_well: %s", model->gravity_well ? "true" : "false");
		separator = ", ";
	}
	if (model->arrival_speed) {
		fprintf(f, "%sarrival_speed_mglt: ", separator);
		write_mglt(f, model->arrival_speed);
		separator = ", ";
	}
	if (model->departure_speed) {
		fprintf(f, "%sdeparture_speed_mglt: ", separator);
		write_mglt(f, model->departure_speed);
	}
	fputs("}\n", f);
}

/* Whether `text` contains `word` in any case, as ddraw's stristr. */
static bool has_text_no_case(const char* text, const char* word) {
	const size_t length = strlen(word);
	for (; *text; ++text)
		if (strncasecmp(text, word, length) == 0)
			return true;
	return false;
}

bool XwauHyperspace_DdrawRegions(const char* game, const char* base, unsigned* regions) {
	char rel[1100], path[1100];
	*regions = 0;
	snprintf(rel, sizeof rel, "%s.ini", base);
	if (!Xwau_FindFile(game, rel, path, sizeof path))
		return false;
	FILE* file = fopen(path, "rt");
	if (!file)
		return false;
	bool in_section = false, found = false;
	char line[256];
	while (fgets(line, sizeof line, file)) {
		if (line[0] == ';' || line[0] == '#')
			continue;
		if (line[0] == '[') {
			const bool was_in = in_section;
			in_section        = has_text_no_case(line, "[Interdiction]");
			found |= in_section;
			if (was_in && !in_section)
				break;
			continue;
		}
		if (!in_section)
			continue;
		/* sscanf("%s = %s") takes the first word, which must be Region. */
		const char* word = line;
		while (isspace((unsigned char)*word))
			++word;
		size_t length = 0;
		while (word[length] && !isspace((unsigned char)word[length]))
			++length;
		if (length != 6 || strncasecmp(word, "Region", 6) != 0)
			continue;
		const char* value = strchr(line, '=');
		if (!value)
			continue;
		/* ReadIntegerList: each piece between commas, read with atoi. */
		for (const char* piece = value + 1; *piece;) {
			const int region = atoi(piece);
			if (region >= 0 && region < 8)
				*regions |= 1u << region;
			const char* comma = strchr(piece, ',');
			if (!comma)
				break;
			piece = comma + 1;
		}
	}
	fclose(file);
	return found;
}
