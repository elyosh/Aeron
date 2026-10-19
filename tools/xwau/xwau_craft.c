#include "xwau_craft.h"

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
	XwauLines lines = { 0 };
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
		char key[32], value[64];
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
		hud->reticle[slot] = (int)number;
	}
	XwauLines_Free(&lines);
}

/* Reads "<key> = r, g, b" as LoadGeneric3DCoords does: three numbers after
 * the '='. */
static bool read_hud_color(const char* ini, const char* line, const char* key, uint32_t* out) {
	const char* equals = strchr(line, '=');
	float rgb[3];
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
			hud->border = color;
		} else if (strcasecmp(param, "HUDInnerColor") == 0 && read_hud_color(ini, line, param, &color)) {
			hud->has_inner = true;
			hud->inner = color;
		}
	}
	fclose(f);
}

bool XwauCraft_ReadIconList(const char* path, uint16_t** values, size_t* count) {
	XwauLines lines = { 0 };
	*values = NULL;
	*count = 0;
	if (!XwauLines_Read(path, "", XWAU_RULES_CPP, &lines)) {
		Report(path, "out of memory");
		return false;
	}
	uint16_t* list = (uint16_t*)calloc(lines.count ? lines.count : 1, sizeof *list);
	bool ok = list != NULL;
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
		*count = lines.count;
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

static bool write_craft_file(const char* path, const char* sources, const XwauCraftHud* hud) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "can't be written");
		return false;
	}
	fprintf(f, "# Written by xwau_import craft from %s.\nversion: 1\nhud:\n", sources);
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
	char hull_path[1024], map_path[1024], path[1024];
	const bool has_hull = Xwau_FindFile(xwau_root, "HullIconList.txt", hull_path, sizeof hull_path);
	const bool has_map = Xwau_FindFile(xwau_root, "MapIconList.txt", map_path, sizeof map_path);
	if (!has_hull && !has_map)
		return true;
	uint16_t* hull = NULL;
	uint16_t* map = NULL;
	size_t hull_count = 0, map_count = 0;
	bool ok = (!has_hull || XwauCraft_ReadIconList(hull_path, &hull, &hull_count)) &&
			  (!has_map || XwauCraft_ReadIconList(map_path, &map, &map_count)) &&
			  PathFormat(path, sizeof path, "%s/hud.yaml", output_dir) && MakeDirs(output_dir);
	FILE* f = ok ? fopen(path, "wb") : NULL;
	if (ok && !f) {
		Report(path, "can't be written");
		ok = false;
	}
	if (f) {
		fprintf(f, "# Written by xwau_import craft from %s%s%s.\n"
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

/* Whether `name` is "<model>Reticle.txt" for a model that has no INI file,
 * in which case `model` gets the model's name. */
static bool reticle_file_without_ini(char* const* names, size_t count, const char* name, char* model,
									 size_t cap) {
	const size_t length = strlen(name);
	if (length <= 11 || !EndsWithNoCase(name, "Reticle.txt") || length - 11 >= cap)
		return false;
	memcpy(model, name, length - 11);
	model[length - 11] = '\0';
	for (size_t i = 0; i < count; ++i) {
		const size_t other = strlen(names[i]);
		if (other == length - 11 + 4 && EndsWithNoCase(names[i], ".ini") &&
			strncasecmp(names[i], model, length - 11) == 0)
			return false;
	}
	return true;
}

bool XwauCraft_Write(const char* xwau_root, const char* output_dir) {
	char models_dir[1024], craft_dir[1024];
	if (!Xwau_FindDir(xwau_root, "FlightModels", models_dir, sizeof models_dir)) {
		Report(xwau_root, "has no FlightModels folder");
		return false;
	}
	if (!PathFormat(craft_dir, sizeof craft_dir, "%s/craft", output_dir))
		return false;
	size_t count = 0;
	char** names = ListDir(models_dir, true, &count);
	bool ok = true;
	int written = 0, reticles = 0, colors = 0;
	for (size_t i = 0; i < count; ++i) {
		char model[256], ini[1100], txt_name[300], txt[1100], key[256], path[1100], sources[700];
		const size_t length = strlen(names[i]);
		bool has_ini = false;
		if (EndsWithNoCase(names[i], ".ini") && length > 4 && length - 4 < sizeof model) {
			memcpy(model, names[i], length - 4);
			model[length - 4] = '\0';
			has_ini = true;
		} else if (!reticle_file_without_ini(names, count, names[i], model, sizeof model)) {
			continue;
		}
		snprintf(ini, sizeof ini, "%s/%s.ini", models_dir, model);
		snprintf(txt_name, sizeof txt_name, "FlightModels/%sReticle.txt", model);
		const bool has_txt = Xwau_FindFile(xwau_root, txt_name, txt, sizeof txt);

		XwauCraftHud hud;
		memset(&hud, 0, sizeof hud);
		XwauCraft_ReadReticle(has_txt ? txt : NULL, has_ini ? ini : NULL, &hud);
		if (has_ini)
			XwauCraft_ReadHudColors(ini, &hud);
		if (!has_reticle(&hud) && !has_colors(&hud))
			continue;

		LowerCopy(key, sizeof key, model);
		if (has_txt && has_ini)
			snprintf(sources, sizeof sources, "%s and FlightModels/%s.ini", txt_name, model);
		else if (has_txt)
			snprintf(sources, sizeof sources, "%s", txt_name);
		else
			snprintf(sources, sizeof sources, "FlightModels/%s.ini", model);
		if (!PathFormat(path, sizeof path, "%s/%s.yaml", craft_dir, key) || !MakeDirs(craft_dir) ||
			!write_craft_file(path, sources, &hud)) {
			ok = false;
			continue;
		}
		written++;
		reticles += has_reticle(&hud);
		colors += has_colors(&hud);
	}
	FreeList(names, count);
	Report(NULL, "craft: %d files, %d with a reticle, %d with HUD colours", written, reticles, colors);
	return write_icon_lists(xwau_root, output_dir) && ok;
}
