#include "xwa_mat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fgets buffer of LoadIndividualMATParams. */
#define XWA_MAT_LINE_MAX 3000

typedef struct XwaMatEntry {
	char         name[XWA_MAT_NAME_MAX];
	XwaMatParams params;
} XwaMatEntry;

struct XwaMatFile {
	XwaMatParams slot0;
	XwaMatEntry* entries;
	size_t       count;
	size_t       cap;
};

void XwaMat_CodeDefaults(XwaMatParams* out) {
	memset(out, 0, sizeof *out);
	out->metallic     = 0.30f; /* DEFAULT_METALLIC */
	out->intensity    = 0.25f; /* DEFAULT_SPEC_INT */
	out->glossiness   = 0.02f; /* DEFAULT_GLOSSINESS */
	out->nm_intensity = 0.5f;
	out->specular_val = 0.0f; /* DEFAULT_SPEC_VALUE */
}

static int ascii_casecmp(const char* a, const char* b) {
	for (;; ++a, ++b) {
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb || !ca)
			return ca - cb;
	}
}

/* sscanf(line, "%s = %s", param, value): returns the number of fields
 * converted (0 for a blank line). The value is "" unless 2 fields convert. */
static int scan_key_line(const char* line, char* param, size_t param_cap, char* value, size_t value_cap) {
	const char* p = line;
	param[0] = value[0] = 0;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (!*p)
		return 0;
	size_t n = 0;
	while (*p && !isspace((unsigned char)*p)) {
		if (n + 1 < param_cap)
			param[n++] = *p;
		p++;
	}
	param[n] = 0;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (*p != '=')
		return 1;
	p++;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (!*p)
		return 1;
	n = 0;
	while (*p && !isspace((unsigned char)*p)) {
		if (n + 1 < value_cap)
			value[n++] = *p;
		p++;
	}
	value[n] = 0;
	return 2;
}

/* ReadMaterialLine for the shading keys; every other key is ignored. */
static void read_material_line(const char* line, XwaMatParams* m) {
	char param[256], value[XWA_MAT_LINE_MAX];
	if (line[0] == ';' || line[0] == '#')
		return;
	if (scan_key_line(line, param, sizeof param, value, sizeof value) <= 0)
		return;
	const float f = (float)atof(value);
	if (ascii_casecmp(param, "Metallic") == 0)
		m->metallic = f;
	else if (ascii_casecmp(param, "Intensity") == 0)
		m->intensity = f;
	else if (ascii_casecmp(param, "Glossiness") == 0)
		m->glossiness = f;
	else if (ascii_casecmp(param, "NMIntensity") == 0)
		m->nm_intensity = f;
	else if (ascii_casecmp(param, "SpecularVal") == 0)
		m->specular_val = f;
	else if (ascii_casecmp(param, "Shadeless") == 0)
		m->shadeless = f != 0.0f;
	else if (ascii_casecmp(param, "Ambient") == 0)
		m->ambient = f;
	else if (ascii_casecmp(param, "AlphaIsntGlass") == 0)
		m->alpha_isnt_glass = f != 0.0f;
	else if (ascii_casecmp(param, "AlphaToBloom") == 0)
		m->alpha_to_bloom = f != 0.0f;
	else if (ascii_casecmp(param, "NoColorAlpha") == 0)
		m->no_color_alpha = f != 0.0f;
	else if (ascii_casecmp(param, "NormalMap") == 0)
		snprintf(m->normal_map, sizeof m->normal_map, "%s", value);
}

bool XwaMat_ReadGlobalDefault(const char* path, XwaMatParams* io) {
	FILE* f = fopen(path, "rt");
	if (!f)
		return false;
	char line[XWA_MAT_LINE_MAX];
	while (fgets(line, sizeof line, f))
		read_material_line(line, io);
	fclose(f);
	return true;
}

static bool push_entry(XwaMatFile* file, const char* name, const XwaMatParams* params) {
	if (file->count == file->cap) {
		size_t       cap   = file->cap ? file->cap * 2 : 32;
		XwaMatEntry* grown = (XwaMatEntry*)realloc(file->entries, cap * sizeof *grown);
		if (!grown)
			return false;
		file->entries = grown;
		file->cap     = cap;
	}
	XwaMatEntry* e = &file->entries[file->count++];
	snprintf(e->name, sizeof e->name, "%s", name);
	e->params = *params;
	return true;
}

/* Close a section: every listed name receives the section's material;
 * "Default" replaces slot 0 instead. */
static bool save_section(XwaMatFile* file, char (*names)[XWA_MAT_NAME_MAX], size_t name_count,
						 const XwaMatParams* cur) {
	for (size_t i = 0; i < name_count; ++i) {
		if (ascii_casecmp(names[i], "default") == 0)
			file->slot0 = *cur;
		else if (!push_entry(file, names[i], cur))
			return false;
	}
	return true;
}

/* "[A,B,...]" -> names. A token counts only when ',' or ']' ends it; no
 * trimming, as the renderer does. */
static size_t parse_section_names(const char* line, char (*names)[XWA_MAT_NAME_MAX], size_t cap) {
	size_t      count = 0;
	const char* start = line + 1;
	while (*start && *start != ']') {
		const char* end = start;
		while (*end && *end != ',' && *end != ']')
			end++;
		if (!*end)
			break;
		if (count < cap) {
			size_t len = (size_t)(end - start);
			if (len >= XWA_MAT_NAME_MAX)
				len = XWA_MAT_NAME_MAX - 1;
			memcpy(names[count], start, len);
			names[count][len] = 0;
			count++;
		}
		start = end + 1;
	}
	return count;
}

XwaMatFile* XwaMat_Load(const char* path, const XwaMatParams* global) {
	FILE* f = fopen(path, "rt");
	if (!f)
		return NULL;
	XwaMatFile* file = (XwaMatFile*)calloc(1, sizeof *file);
	if (!file) {
		fclose(f);
		return NULL;
	}
	file->slot0 = *global;

	enum { MAX_SECTION_NAMES = 256 };

	char         names[MAX_SECTION_NAMES][XWA_MAT_NAME_MAX];
	size_t       name_count = 0;
	bool         saved      = true; /* lines before the first section are dropped */
	XwaMatParams cur        = file->slot0;
	char         line[XWA_MAT_LINE_MAX];
	bool         ok = true;
	while (ok && fgets(line, sizeof line, f)) {
		if (line[0] == ';' || line[0] == '#')
			continue;
		char param[256], value[XWA_MAT_LINE_MAX];
		if (scan_key_line(line, param, sizeof param, value, sizeof value) <= 0)
			continue;
		if (line[0] == '[') {
			if (!saved)
				ok = save_section(file, names, name_count, &cur);
			name_count = parse_section_names(line, names, MAX_SECTION_NAMES);
			cur        = file->slot0;
			saved      = false;
		} else {
			read_material_line(line, &cur);
		}
	}
	if (ok && !saved)
		ok = save_section(file, names, name_count, &cur);
	fclose(f);
	if (!ok) {
		XwaMat_Free(file);
		return NULL;
	}
	return file;
}

/* Case-insensitive strstr for "_fg_". */
static const char* find_skin_suffix(const char* s) {
	for (; *s; ++s) {
		if (s[0] == '_' && tolower((unsigned char)s[1]) == 'f' && tolower((unsigned char)s[2]) == 'g' &&
			s[3] == '_')
			return s;
	}
	return NULL;
}

const XwaMatParams* XwaMat_Find(const XwaMatFile* file, const char* texture_name) {
	char        name[XWA_MAT_NAME_MAX];
	const char* skin = find_skin_suffix(texture_name);
	size_t      len  = skin ? (size_t)(skin - texture_name) : strlen(texture_name);
	if (len >= sizeof name)
		len = sizeof name - 1;
	memcpy(name, texture_name, len);
	name[len] = 0;
	if (!name[0])
		return &file->slot0;
	for (size_t i = 0; i < file->count; ++i) {
		if (ascii_casecmp(name, file->entries[i].name) == 0)
			return &file->entries[i].params;
	}
	return &file->slot0;
}

void XwaMat_Free(XwaMatFile* file) {
	if (!file)
		return;
	free(file->entries);
	free(file);
}
