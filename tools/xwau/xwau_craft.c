#include "xwau_craft.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"

/* The slots of XWAU's reticle hook (g_imageIndices in reticle.cpp). */
static const int g_reticleSlots[] = { 5, 6, 7, 8, 9, 10, 15, 16, 17, 18, 19, 20, 21, 22 };

uint32_t XwauCraft_HudColor(float r, float g, float b) {
	return 0xffu << 24 | (uint32_t)(r * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(b * 255);
}

void XwauCraft_ReadReticle(const char* reticle_txt, const char* ini, XwauCraftHud* hud) {
	XwauLines   lines  = { 0 };
	const char* source = reticle_txt;
	if (reticle_txt)
		XwauLines_Read(reticle_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "Reticle", XWAU_RULES_CPP, &lines);
	}
	for (size_t i = 0; i < sizeof g_reticleSlots / sizeof g_reticleSlots[0]; ++i) {
		const int slot = g_reticleSlots[i];
		char      key[32], value[64];
		long long number;
		snprintf(key, sizeof key, "Reticle_%d", slot);
		XwauLines_Value(&lines, key, XWAU_RULES_CPP, value, sizeof value);
		if (!value[0])
			continue;
		/* The hook reads the value with std::stoi(value, 0, 0), which throws
		   when there is no number. */
		if (!Xwau_StoiAuto(value, &number)) {
			Report(source, "%s = %s isn't a number; the slot keeps its own image", key, value);
			continue;
		}
		if (number == -1)
			continue;
		hud->has_reticle[slot] = true;
		hud->reticle[slot]     = (int)number;
	}
	XwauLines_Free(&lines);
}

/* Reads "<key> = r, g, b" as LoadGeneric3DCoords does: three numbers after
 * the '='. */
static bool read_hud_color(const char* ini, const char* line, const char* key, uint32_t* out) {
	const char* equals = strchr(line, '=');
	float       rgb[3];
	if (!equals || sscanf(equals + 1, "%f, %f, %f", &rgb[0], &rgb[1], &rgb[2]) < 3) {
		Report(ini, "%s needs three numbers; it is ignored", key);
		return false;
	}
	/* uint32_t(x * 255) has no meaning for a negative value. */
	if (rgb[0] < 0 || rgb[1] < 0 || rgb[2] < 0) {
		Report(ini, "%s has a negative value; it is ignored", key);
		return false;
	}
	*out = XwauCraft_HudColor(rgb[0], rgb[1], rgb[2]);
	return true;
}

void XwauCraft_ReadHudColors(const char* ini, XwauCraftHud* hud) {
	FILE* f = ini ? fopen(ini, "r") : NULL;
	if (!f)
		return;
	/* The wrapper reads lines into a 256-byte buffer. */
	char line[256];
	bool in_section = false;
	while (fgets(line, sizeof line, f)) {
		if (line[0] == ';' || line[0] == '#' || line[0] == '\0')
			continue;
		if (line[0] == '[')
			in_section = strstr(line, "[HUDColor]") != NULL;
		char param[128], value[128];
		if (!in_section || sscanf(line, "%127s = %127s", param, value) < 1)
			continue;
		uint32_t color;
		if (strcasecmp(param, "HUDBorderColor") == 0 && read_hud_color(ini, line, param, &color)) {
			hud->has_border = true;
			hud->border     = color;
		} else if (strcasecmp(param, "HUDInnerColor") == 0 && read_hud_color(ini, line, param, &color)) {
			hud->has_inner = true;
			hud->inner     = color;
		}
	}
	fclose(f);
}

/* The largest MGLT limit that the file holds. XWAU's limit is mglt * 2.25
 * in raw speed units, and a speed is at most 65535, so a larger limit hides
 * the glow all the same. */
#define XWAU_CRAFT_MAX_MGLT 100000000

/* Reads one line of engine glows. Returns false, with a report, when a value
 * isn't a number that fits in an int. */
static bool read_engine_glow(const char* source, const char* line, XwauCraftEngineGlows* glows) {
	XwauLines tokens = { 0 };
	long long values[3];
	bool      ok = true;
	if (!Xwau_Tokenize(line, XWAU_RULES_CPP, &tokens)) {
		Report(source, "out of memory");
		return false;
	}
	if (tokens.count < 3) {
		XwauLines_Free(&tokens);
		return true;
	}
	for (int i = 0; ok && i < 3; ++i)
		ok = Xwau_StoiAuto(tokens.items[i], &values[i]) && values[i] >= INT_MIN && values[i] <= INT_MAX;
	XwauLines_Free(&tokens);
	if (!ok) {
		/* The hook reads the values with std::stoi(value, 0, 0), which throws. */
		Report(source, "engine glow line '%s' has a value that isn't a number; it is ignored", line);
		return false;
	}
	if (values[0] < 0 || values[0] >= XWAU_CRAFT_ENGINE_GLOWS) {
		Report(source, "engine glow line '%s' names a glow outside 0 to 255; it is ignored", line);
		return false;
	}
	const int glow = (int)values[0];
	if (glows->has[glow])
		return true;
	glows->has[glow]         = true;
	glows->min_percent[glow] = values[1] > 0 ? (int)values[1] : 0;
	glows->min_mglt[glow] =
		values[2] > 0 ? (int)(values[2] < XWAU_CRAFT_MAX_MGLT ? values[2] : XWAU_CRAFT_MAX_MGLT) : 0;
	return true;
}

void XwauCraft_ReadEngineGlows(const char* glows_txt, const char* ini, XwauCraftEngineGlows* glows) {
	XwauLines   lines  = { 0 };
	const char* source = glows_txt;
	if (glows_txt)
		XwauLines_Read(glows_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "EngineGlows", XWAU_RULES_CPP, &lines);
	}
	for (size_t i = 0; i < lines.count; ++i)
		read_engine_glow(source, lines.items[i], glows);
	XwauLines_Free(&lines);
}

bool XwauCraft_ReadIconList(const char* path, uint16_t** values, size_t* count) {
	XwauLines lines = { 0 };
	*values         = NULL;
	*count          = 0;
	if (!XwauLines_Read(path, "", XWAU_RULES_CPP, &lines)) {
		Report(path, "out of memory");
		return false;
	}
	uint16_t* list = (uint16_t*)calloc(lines.count ? lines.count : 1, sizeof *list);
	bool      ok   = list != NULL;
	for (size_t i = 0; ok && i < lines.count; ++i) {
		long long number;
		/* The hook reads each line with std::stoi, base 10. */
		ok = Xwau_Stoi(lines.items[i], &number);
		if (!ok)
			Report(path, "line '%s' isn't a number", lines.items[i]);
		else
			list[i] = (uint16_t)number;
	}
	if (ok) {
		*values = list;
		*count  = lines.count;
	} else {
		free(list);
	}
	XwauLines_Free(&lines);
	return ok;
}

static bool has_reticle(const XwauCraftHud* hud) {
	for (int slot = 0; slot < XWAU_CRAFT_RETICLE_SLOTS; ++slot) {
		if (hud->has_reticle[slot])
			return true;
	}
	return false;
}

static bool has_colors(const XwauCraftHud* hud) { return hud->has_inner || hud->has_border; }

static bool has_glow_limit(const XwauCraftEngineGlows* glows, int glow) {
	return glows->min_percent[glow] > 0 || glows->min_mglt[glow] > 0;
}

static bool has_engine_glows(const XwauCraftEngineGlows* glows) {
	for (int glow = 0; glow < XWAU_CRAFT_ENGINE_GLOWS; ++glow) {
		if (has_glow_limit(glows, glow))
			return true;
	}
	return false;
}

static void write_hud(FILE* f, const XwauCraftHud* hud) {
	fprintf(f, "hud:\n");
	bool first = true;
	for (int slot = 0; slot < XWAU_CRAFT_RETICLE_SLOTS; ++slot) {
		if (!hud->has_reticle[slot])
			continue;
		fprintf(f, "%s%d: %d", first ? "  reticle: {" : ", ", slot, hud->reticle[slot]);
		first = false;
	}
	if (!first)
		fprintf(f, "}\n");
	if (hud->has_inner || hud->has_border) {
		fprintf(f, "  colors: {");
		if (hud->has_inner)
			fprintf(f, "inner: 0x%08x", hud->inner);
		if (hud->has_border)
			fprintf(f, "%sborder: 0x%08x", hud->has_inner ? ", " : "", hud->border);
		fprintf(f, "}\n");
	}
}

static void write_engine_glows(FILE* f, const XwauCraftEngineGlows* glows) {
	fprintf(f, "engine_glows:\n");
	for (int glow = 0; glow < XWAU_CRAFT_ENGINE_GLOWS; ++glow) {
		if (!has_glow_limit(glows, glow))
			continue;
		fprintf(f, "  %d: {", glow);
		if (glows->min_percent[glow] > 0)
			fprintf(f, "min_speed_percent: %d", glows->min_percent[glow]);
		if (glows->min_mglt[glow] > 0)
			fprintf(f, "%smin_speed_mglt: %d", glows->min_percent[glow] > 0 ? ", " : "",
					glows->min_mglt[glow]);
		fprintf(f, "}\n");
	}
}

static bool write_craft_file(const char* path, const char* sources, const XwauCraftHud* hud,
							 const XwauCraftEngineGlows* glows) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "can't be written");
		return false;
	}
	fprintf(f, "# Written by xwau_import craft from %s.\nversion: 1\n", sources);
	if (has_reticle(hud) || has_colors(hud))
		write_hud(f, hud);
	if (has_engine_glows(glows))
		write_engine_glows(f, glows);
	const bool ok = fclose(f) == 0;
	if (!ok)
		Report(path, "can't be written");
	return ok;
}

static void write_list(FILE* f, const char* key, const uint16_t* values, size_t count) {
	fprintf(f, "%s: [", key);
	for (size_t i = 0; i < count; ++i)
		fprintf(f, "%s%s%u", i ? "," : "", i % 16 ? " " : "\n  ", values[i]);
	fprintf(f, "]\n");
}

static bool write_icon_lists(const char* xwau_root, const char* output_dir) {
	char       hull_path[1024], map_path[1024], path[1024];
	const bool has_hull = Xwau_FindFile(xwau_root, "HullIconList.txt", hull_path, sizeof hull_path);
	const bool has_map  = Xwau_FindFile(xwau_root, "MapIconList.txt", map_path, sizeof map_path);
	if (!has_hull && !has_map)
		return true;
	uint16_t* hull       = NULL;
	uint16_t* map        = NULL;
	size_t    hull_count = 0, map_count = 0;
	bool      ok = (!has_hull || XwauCraft_ReadIconList(hull_path, &hull, &hull_count)) &&
				   (!has_map || XwauCraft_ReadIconList(map_path, &map, &map_count)) &&
				   PathFormat(path, sizeof path, "%s/hud.yaml", output_dir) && MakeDirs(output_dir);
	FILE*     f  = ok ? fopen(path, "wb") : NULL;
	if (ok && !f) {
		Report(path, "can't be written");
		ok = false;
	}
	if (f) {
		fprintf(f,
				"# Written by xwau_import craft from %s%s%s.\n"
				"# Each list gives the image id for each object type.\nversion: 1\n",
				has_hull ? "HullIconList.txt" : "", has_hull && has_map ? " and " : "",
				has_map ? "MapIconList.txt" : "");
		if (has_hull)
			write_list(f, "hull_icons", hull, hull_count);
		if (has_map)
			write_list(f, "map_icons", map, map_count);
		ok = fclose(f) == 0;
		if (!ok)
			Report(path, "can't be written");
		else
			Report(NULL, "hud.yaml: %zu hull icons, %zu map icons", hull_count, map_count);
	}
	free(hull);
	free(map);
	return ok;
}

/* Whether `name` is "<model><suffix>" for a model that has no INI file, in
 * which case `model` gets the model's name. */
static bool side_file_without_ini(char* const* names, size_t count, const char* name, const char* suffix,
								  char* model, size_t cap) {
	const size_t length        = strlen(name);
	const size_t suffix_length = strlen(suffix);
	if (length <= suffix_length || !EndsWithNoCase(name, suffix) || length - suffix_length >= cap)
		return false;
	memcpy(model, name, length - suffix_length);
	model[length - suffix_length] = '\0';
	for (size_t i = 0; i < count; ++i) {
		const size_t other = strlen(names[i]);
		if (other == length - suffix_length + 4 && EndsWithNoCase(names[i], ".ini") &&
			strncasecmp(names[i], model, length - suffix_length) == 0)
			return false;
	}
	return true;
}

/* Whether the folder has "<model><suffix>". */
static bool has_side_file(char* const* names, size_t count, const char* model, const char* suffix) {
	const size_t length = strlen(model);
	for (size_t i = 0; i < count; ++i) {
		if (strncasecmp(names[i], model, length) == 0 && Xwau_SameName(names[i] + length, suffix))
			return true;
	}
	return false;
}

/* Appends `name` to the list of source files. */
static void add_source(char* sources, size_t cap, const char* name) {
	const size_t length = strlen(sources);
	snprintf(sources + length, cap - length, "%s%s", length ? " and " : "", name);
}

bool XwauCraft_Write(const char* xwau_root, const char* output_dir) {
	char models_dir[1024], craft_dir[1024];
	if (!Xwau_FindDir(xwau_root, "FlightModels", models_dir, sizeof models_dir)) {
		Report(xwau_root, "has no FlightModels folder");
		return false;
	}
	if (!PathFormat(craft_dir, sizeof craft_dir, "%s/models", output_dir))
		return false;
	size_t count   = 0;
	char** names   = ListDir(models_dir, true, &count);
	bool   ok      = true;
	int    written = 0, reticles = 0, colors = 0, engine_glows = 0;
	for (size_t i = 0; i < count; ++i) {
		char model[256], ini[1100], reticle_name[300], reticle_txt[1100], glows_name[300], glows_txt[1100],
			key[256], path[1100], sources[1000];
		const size_t length  = strlen(names[i]);
		bool         has_ini = false;
		if (EndsWithNoCase(names[i], ".ini") && length > 4 && length - 4 < sizeof model) {
			memcpy(model, names[i], length - 4);
			model[length - 4] = '\0';
			has_ini           = true;
		} else if (!side_file_without_ini(names, count, names[i], "Reticle.txt", model, sizeof model) &&
				   !(side_file_without_ini(names, count, names[i], "EngineGlows.txt", model, sizeof model) &&
					 !has_side_file(names, count, model, "Reticle.txt"))) {
			continue;
		}
		snprintf(ini, sizeof ini, "%s/%s.ini", models_dir, model);
		snprintf(reticle_name, sizeof reticle_name, "FlightModels/%sReticle.txt", model);
		snprintf(glows_name, sizeof glows_name, "FlightModels/%sEngineGlows.txt", model);
		const bool has_reticle_txt = Xwau_FindFile(xwau_root, reticle_name, reticle_txt, sizeof reticle_txt);
		const bool has_glows_txt   = Xwau_FindFile(xwau_root, glows_name, glows_txt, sizeof glows_txt);

		XwauCraftHud         hud;
		XwauCraftEngineGlows glows;
		memset(&hud, 0, sizeof hud);
		memset(&glows, 0, sizeof glows);
		XwauCraft_ReadReticle(has_reticle_txt ? reticle_txt : NULL, has_ini ? ini : NULL, &hud);
		if (has_ini)
			XwauCraft_ReadHudColors(ini, &hud);
		XwauCraft_ReadEngineGlows(has_glows_txt ? glows_txt : NULL, has_ini ? ini : NULL, &glows);
		if (!has_reticle(&hud) && !has_colors(&hud) && !has_engine_glows(&glows))
			continue;

		LowerCopy(key, sizeof key, model);
		sources[0] = '\0';
		if (has_reticle_txt)
			add_source(sources, sizeof sources, reticle_name);
		if (has_glows_txt)
			add_source(sources, sizeof sources, glows_name);
		if (has_ini) {
			char ini_name[300];
			snprintf(ini_name, sizeof ini_name, "FlightModels/%s.ini", model);
			add_source(sources, sizeof sources, ini_name);
		}
		if (!PathFormat(path, sizeof path, "%s/%s.yaml", craft_dir, key) || !MakeDirs(craft_dir) ||
			!write_craft_file(path, sources, &hud, &glows)) {
			ok = false;
			continue;
		}
		written++;
		reticles += has_reticle(&hud);
		colors += has_colors(&hud);
		engine_glows += has_engine_glows(&glows);
	}
	FreeList(names, count);
	Report(NULL, "model settings: %d files, %d with a reticle, %d with HUD colours, %d with engine glows",
		   written, reticles, colors, engine_glows);
	return write_icon_lists(xwau_root, output_dir) && ok;
}
