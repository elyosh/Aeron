#include "xwau_mat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Size of the fgets buffer in LoadIndividualMATParams. */
#define XWAU_MAT_LINE_MAX 3000

typedef struct XwauMatEntry {
	char          name[XWAU_MAT_NAME_MAX];
	XwauMatParams params;
	int           section; /* index into XwauMatFile.sections */
} XwauMatEntry;

/* The sequences of one section. */
typedef struct XwauMatSection {
	XwauSequence* items;
	size_t        count;
	int           order; /* 0 for [Default], otherwise the 1-based order of the section */
} XwauMatSection;

struct XwauMatFile {
	XwauMatParams   slot0;
	int             slot0_section; /* -1 without a [Default] section */
	XwauMatEntry*   entries;
	size_t          count;
	size_t          cap;
	XwauMatSection* sections;
	size_t          section_count;
};

void XwauMat_CodeDefaults(XwauMatParams* out) {
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

/* Does sscanf(line, "%s = %s", param, value) and returns the number of fields
 * converted (0 for a blank line). The value is "" unless both fields are
 * converted. */
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

/* ReadMaterialLine for the shading keys. Other keys are ignored. */
static void read_material_line(const char* line, XwauMatParams* m) {
	char param[256], value[XWAU_MAT_LINE_MAX];
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
	else if (ascii_casecmp(param, "Light") == 0) {
		/* LoadLightColor reads the three numbers after the '='. */
		const char* equals = strchr(line, '=');
		float       rgb[3];
		if (equals && sscanf(equals + 1, "%f, %f, %f", &rgb[0], &rgb[1], &rgb[2]) == 3) {
			m->has_light = true;
			memcpy(m->light, rgb, sizeof rgb);
		}
	}
}

bool XwauMat_ReadGlobalDefault(const char* path, XwauMatParams* io) {
	FILE* f = fopen(path, "rt");
	if (!f)
		return false;
	char line[XWAU_MAT_LINE_MAX];
	while (fgets(line, sizeof line, f))
		read_material_line(line, io);
	fclose(f);
	return true;
}

/* ---- Image sequences ------------------------------------------------- */

/* Case-insensitive strstr (the renderer's stristr). */
static const char* stristr_c(const char* haystack, const char* needle) {
	const size_t n = strlen(needle);
	for (; *haystack; ++haystack) {
		size_t i = 0;
		while (i < n && haystack[i] &&
			   tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i]))
			i++;
		if (i == n)
			return haystack;
	}
	return NULL;
}

static char* stristr_m(char* haystack, const char* needle) { return (char*)stristr_c(haystack, needle); }

/* Event names, in the order of GameEventEnum and InstEventType (Materials.cpp). */
static const char* const kGameEvents[] = {
	"EVT_NONE",
	"EVT_TARGET_SEL",
	"EVT_LASER_LOCKED",
	"EVT_WARHEAD_LOCKING",
	"EVT_WARHEAD_LOCKED",
	"EVT_BROKEN_CMD",
	"EVT_BROKEN_LASER_ION",
	"EVT_BROKEN_BEAM_WEAPON",
	"EVT_BROKEN_SHIELDS",
	"EVT_BROKEN_THROTTLE",
	"EVT_BROKEN_SENSORS",
	"EVT_BROKEN_LASER_RECHARGE",
	"EVT_BROKEN_ENGINE_POWER",
	"EVT_BROKEN_SHIELD_RECHARGE",
	"EVT_BROKEN_BEAM_RECHARGE",
	"EVT_DAMAGE_75",
	"EVT_DAMAGE_50",
	"EVT_DAMAGE_25",
	"EVT_WLIGHT_LEFT",
	"EVT_WLIGHT_MID_LEFT",
	"EVT_WLIGHT_MID_RIGHT",
	"EVT_WLIGHT_RIGHT_YELLOW",
	"EVT_WLIGHT_RIGHT_RED",
	"EVT_CANNON_1_READY",
	"EVT_CANNON_2_READY",
	"EVT_CANNON_3_READY",
	"EVT_CANNON_4_READY",
	"EVT_CANNON_5_READY",
	"EVT_CANNON_6_READY",
	"EVT_CANNON_7_READY",
	"EVT_CANNON_8_READY",
};
static const char* const kInstanceEvents[] = {
	"IEVT_NONE",         "IEVT_DAMAGE_75",    "IEVT_DAMAGE_50",    "IEVT_DAMAGE_25",
	"IEVT_SHIELDS_DOWN", "IEVT_TRACTOR_BEAM", "IEVT_JAMMING_BEAM",
};

/* ParseEvent: reads the first token of `s`, up to white space. A name that
 * matches no event is EVT_NONE. */
static void parse_event(const char* s, XwauSequence* q) {
	char token[80];
	while (*s && isspace((unsigned char)*s))
		s++;
	size_t n = 0;
	while (*s && !isspace((unsigned char)*s) && n + 1 < sizeof token)
		token[n++] = *s++;
	token[n]         = 0;
	q->event[0]      = 0;
	q->instance      = false;
	q->event_unknown = n > 0;
	for (size_t i = 0; i < sizeof kGameEvents / sizeof kGameEvents[0]; ++i) {
		if (ascii_casecmp(token, kGameEvents[i]) == 0) {
			if (i)
				snprintf(q->event, sizeof q->event, "%s", kGameEvents[i]);
			q->event_unknown = false;
		}
	}
	for (size_t i = 0; i < sizeof kInstanceEvents / sizeof kInstanceEvents[0]; ++i) {
		if (ascii_casecmp(token, kInstanceEvents[i]) == 0) {
			if (i)
				snprintf(q->event, sizeof q->event, "%s", kInstanceEvents[i]);
			q->instance      = true;
			q->event_unknown = false;
		}
	}
}

/* SKIP_WHITESPACES: skips spaces. The parse fails at the end of the line. */
#define SKIP_SPACES(s)                                                                                       \
	do {                                                                                                     \
		while (*(s) == ' ')                                                                                  \
			(s)++;                                                                                           \
		if (!*(s))                                                                                           \
			return false;                                                                                    \
	} while (0)

/* Moves `t` to the next `c`. Returns false at the end of the line. */
static bool skip_to(char** t, char c) {
	while (**t && **t != c)
		(*t)++;
	return **t != 0;
}

/* ParseOptionalModifiers. Returns the rest of the line, or NULL when it ends
 * inside a modifier. */
static char* parse_modifiers(char* s, XwauSequence* q) {
	for (;;) {
		char* t;
		if (stristr_m(s, "UV_AREA")) {
			/* UV_AREA has no effect on what is drawn. It is parsed only to skip it. */
			t = s;
			if (!skip_to(&t, '='))
				return NULL;
			t++;
			for (int i = 0; i < 4; ++i) {
				if (!skip_to(&t, ','))
					return NULL;
				t++;
			}
			*t = 0;
			t++;
			s = t;
		} else if (stristr_m(s, "FIXED_LOC")) {
			q->loc = XWAU_SEQ_LOC_FIXED;
			t      = s;
			if (!skip_to(&t, ','))
				return NULL;
			t++;
			s = t;
		} else if (stristr_m(s, "RAND_LOC")) {
			q->loc = XWAU_SEQ_LOC_INSIDE;
			t      = s;
			while (*t && *t != ',' && *t != '=')
				t++;
			if (!*t)
				return NULL;
			const char sep = *t++;
			if (sep == '=') {
				s = t;
				for (int i = 0; i < 4; ++i) {
					if (!skip_to(&t, ','))
						return NULL;
					t++;
				}
				*t         = 0;
				float v[4] = { 0, 0, 0, 0 };
				sscanf(s, "%f, %f, %f, %f", &v[0], &v[1], &v[2], &v[3]);
				memcpy(q->loc_range, v, sizeof v);
				q->loc = XWAU_SEQ_LOC_RANGE;
				t++;
			}
			s = t;
		} else if (stristr_m(s, "SCALE") && !stristr_m(s, "RAND_SCALE")) {
			/* SCALE has no effect on what is drawn. It is parsed only to skip it. */
			t = s;
			if (!skip_to(&t, '='))
				return NULL;
			t++;
			if (!skip_to(&t, ','))
				return NULL;
			t++;
			*t = 0;
			t++;
			q->rand_scale = false;
			s             = t;
		} else if (stristr_m(s, "RAND_SCALE")) {
			t = s;
			if (!skip_to(&t, '='))
				return NULL;
			t++;
			s = t;
			for (int i = 0; i < 2; ++i) {
				if (!skip_to(&t, ','))
					return NULL;
				t++;
			}
			*t      = 0;
			float a = 1, b = 1;
			sscanf(s, "%f, %f", &a, &b);
			q->rand_scale = true;
			q->scale[0]   = a;
			q->scale[1]   = b;
			t++;
			s = t;
		} else {
			return s;
		}
		while (*s == ' ')
			s++;
		if (!*s)
			return NULL;
	}
}

static void remove_parentheses(const char* in, char* out, size_t cap) {
	size_t j = 0;
	for (size_t i = 0; in[i] && j + 1 < cap; ++i)
		if (in[i] != '(' && in[i] != ')')
			out[j++] = in[i];
	out[j] = 0;
}

static bool push_frame(XwauSequence* q, int group, int image, float seconds, float intensity) {
	XwauSeqFrame* frame = ARRAY_PUSH(q->frames, q->frame_count);
	if (!frame)
		return false;
	*frame = (XwauSeqFrame) { group, image, seconds, intensity };
	return true;
}

/* Copies the text from `s` up to the next comma into `out`, and replaces the
 * comma with NUL. Returns false at the end of the line. */
static bool take_to_comma(char** s, char* out, size_t cap) {
	char* t = *s;
	if (!skip_to(&t, ','))
		return false;
	*t = 0;
	snprintf(out, cap, "%s", *s);
	*s = t + 1;
	return true;
}

/* Like sscanf("%s"): copies the first token, up to white space. */
static void first_token(const char* s, char* out, size_t cap) {
	while (*s && isspace((unsigned char)*s))
		s++;
	size_t n = 0;
	while (*s && !isspace((unsigned char)*s) && n + 1 < cap)
		out[n++] = *s++;
	out[n] = 0;
}

/* LoadTextureSequence, for lines of the form
 * [event,] [LYR_x,] [modifiers,] <file>.dat, [{alpha, [r,g,b]},] G-I, seconds, intensity, ...
 * The elements read before a syntax error are kept. */
static bool parse_listed(const char* line, XwauSequence* q) {
	char buf[XWAU_MAT_LINE_MAX];
	remove_parentheses(line, buf, sizeof buf);
	char* s = strchr(buf, '=');
	if (!s)
		return false;
	s++;
	char token[XWAU_MAT_PATH_MAX];
	if (stristr_m(s, "EVT_")) {
		if (!take_to_comma(&s, token, sizeof token))
			return false;
		parse_event(token, q);
		SKIP_SPACES(s);
	}
	if (stristr_m(s, "LYR_")) {
		if (!take_to_comma(&s, token, sizeof token))
			return false;
		if (ascii_casecmp(token, "LYR_MULT") == 0)
			q->overlay = XWAU_SEQ_MULT;
		else if (ascii_casecmp(token, "LYR_SCREEN") == 0)
			q->overlay = XWAU_SEQ_SCREEN;
		SKIP_SPACES(s);
	}
	s = parse_modifiers(s, q);
	if (!s)
		return false;
	bool dat_file = false;
	if (stristr_m(s, ".dat") || stristr_m(s, ".zip")) {
		dat_file = true;
		SKIP_SPACES(s);
		if (!take_to_comma(&s, q->dat, sizeof q->dat))
			return false;
		SKIP_SPACES(s);
	}
	if (s[0] == '{') {
		s++;
		char* t = s;
		if (!skip_to(&t, '}'))
			return false;
		*t        = 0;
		int   raw = 0;
		float rgb[3];
		if (sscanf(s, "%d, [%f, %f, %f]", &raw, &rgb[0], &rgb[1], &rgb[2]) >= 4) {
			q->alpha_mode = raw == 1 || raw == 2 ? raw : 0;
			memcpy(q->tint, rgb, sizeof rgb);
		}
		s = t + 1;
		t = s;
		if (!skip_to(&t, ','))
			return false;
		s = t + 1;
		SKIP_SPACES(s);
	}
	int   prev_group = -1, prev_image = -1;
	float prev_seconds = 0.0f, prev_intensity = 1.0f;
	/* As in XWAU, a frame without a readable intensity uses the previous one. */
	float intensity = 1.0f;
	while (*s) {
		char name[80];
		if (!take_to_comma(&s, token, sizeof token))
			return false;
		first_token(token, name, sizeof name);
		SKIP_SPACES(s);
		bool ellipsis = false;
		if (strstr(name, "..")) {
			ellipsis = true;
			if (!take_to_comma(&s, token, sizeof token))
				return false;
			first_token(token, name, sizeof name);
			SKIP_SPACES(s);
		}
		char* t = s;
		if (!skip_to(&t, ','))
			return false;
		float seconds = 0.0f;
		sscanf(s, "%f", &seconds);
		s = t + 1;
		SKIP_SPACES(s);
		t = s;
		while (*t && *t != ',')
			t++;
		sscanf(s, "%f", &intensity);
		int group = -1, image = -1;
		if (!dat_file || sscanf(name, "%d-%d", &group, &image) < 2)
			return false;
		if (!ellipsis) {
			if (!push_frame(q, group, image, seconds, intensity))
				return false;
		} else if (image != prev_image && prev_image >= 0) {
			/* InterpolateTexSequence: adds the images after the previous image up
			 * to this one, in the group of the previous image. */
			const int   step = image > prev_image ? 1 : -1;
			const float size = (float)(image - prev_image);
			for (int cur = prev_image + step;; cur += step) {
				const float sec =
					seconds * (float)(cur - prev_image) / size + prev_seconds * (float)(image - cur) / size;
				const float in = intensity * (float)(cur - prev_image) / size +
								 prev_intensity * (float)(image - cur) / size;
				if (!push_frame(q, prev_group, cur, sec, in))
					return false;
				if (cur == image)
					break;
			}
		}
		prev_group     = group;
		prev_image     = image;
		prev_seconds   = seconds;
		prev_intensity = intensity;
		if (!*t)
			break;
		s = t + 1;
	}
	return true;
}

/* LoadFrameSequence, for lines of the form
 * event, [LYR_x,] [modifiers,] <file>.dat-G[-I], fps, is_lightmap, intensity, alpha
 * [; [r,g,b], aspect, offset_x, offset_y, clamp]. */
static bool parse_frames(const char* line, XwauSequence* q) {
	char buf[XWAU_MAT_LINE_MAX];
	remove_parentheses(line, buf, sizeof buf);
	char* s = strchr(buf, '=');
	if (!s)
		return false;
	s++;
	char token[XWAU_MAT_PATH_MAX];
	if (!take_to_comma(&s, token, sizeof token))
		return false;
	parse_event(token, q);
	SKIP_SPACES(s);
	if (stristr_m(s, "LYR_")) {
		if (!take_to_comma(&s, token, sizeof token))
			return false;
		if (ascii_casecmp(token, "LYR_MULT") == 0)
			q->overlay = XWAU_SEQ_MULT;
		else if (ascii_casecmp(token, "LYR_SCREEN") == 0)
			q->overlay = XWAU_SEQ_SCREEN;
		SKIP_SPACES(s);
	}
	s = parse_modifiers(s, q);
	if (!s)
		return false;
	char path[256];
	if (!take_to_comma(&s, token, sizeof token))
		return false;
	first_token(token, path, sizeof path);
	SKIP_SPACES(s);
	float     fps = 30.0f, intensity = 0.0f, rgb[3] = { 1, 1, 1 }, aspect = 1.0f, ofs[2] = { 0, 0 };
	int       lightmap = 0, raw_alpha = 0, clamp = 0;
	const int n = sscanf(s, "%f, %d, %f, %d; [%f, %f, %f], %f, %f, %f, %d", &fps, &lightmap, &intensity,
						 &raw_alpha, &rgb[0], &rgb[1], &rgb[2], &aspect, &ofs[0], &ofs[1], &clamp);
	/* All frames of the group use this intensity. */
	q->intensity  = intensity;
	q->fps        = fps;
	q->layer      = lightmap ? XWAU_SEQ_ILLUMINATION : XWAU_SEQ_COLOUR;
	q->alpha_mode = n >= 4 && (raw_alpha == 1 || raw_alpha == 2) ? raw_alpha : 0;
	if (n >= 7)
		memcpy(q->tint, rgb, sizeof rgb);
	if (n >= 8)
		q->aspect = aspect;
	if (n >= 10) {
		q->offset[0] = ofs[0];
		q->offset[1] = ofs[1];
	}
	if (n >= 11)
		q->clamp = clamp != 0;
	/* <path>.dat-G or <path>.dat-G-I */
	char* split = stristr_m(path, ".dat-");
	if (!split)
		split = stristr_m(path, ".zip-");
	if (!split)
		return false;
	split[4] = 0;
	if (!PathFormat(q->dat, sizeof q->dat, "%s", path))
		return false;
	const char* gi   = split + 5;
	const char* dash = strrchr(gi, '-');
	q->group_frames  = true;
	q->group         = atoi(gi);
	q->image         = dash && dash > gi ? atoi(dash + 1) : -1;
	return true;
}

static void sequence_free(XwauSequence* q) {
	free(q->frames);
	q->frames      = NULL;
	q->frame_count = 0;
}

/* Parses a sequence key line into a new sequence of `section`. Other keys are
 * ignored. */
static bool read_sequence_line(const char* param, const char* line, XwauMatSection* section) {
	const bool listed = ascii_casecmp(param, "lightmap_seq") == 0 ||
						ascii_casecmp(param, "lightmap_rand") == 0 ||
						ascii_casecmp(param, "lightmap_once") == 0 || ascii_casecmp(param, "anim_seq") == 0 ||
						ascii_casecmp(param, "anim_rand") == 0 || ascii_casecmp(param, "anim_once") == 0;
	const bool frames = ascii_casecmp(param, "frame_seq") == 0 || ascii_casecmp(param, "frame_rand") == 0 ||
						ascii_casecmp(param, "frame_once") == 0;
	if (!listed && !frames)
		return true;
	XwauSequence q;
	memset(&q, 0, sizeof q);
	for (size_t i = 0; param[i] && i + 1 < sizeof q.key; ++i)
		q.key[i] = (char)tolower((unsigned char)param[i]);
	q.mode    = stristr_c(param, "_rand")   ? XWAU_SEQ_RANDOM
				: stristr_c(param, "_once") ? XWAU_SEQ_ONCE
											: XWAU_SEQ_LOOP;
	q.layer   = ascii_casecmp(param, "lightmap_seq") == 0 || ascii_casecmp(param, "lightmap_rand") == 0
					? XWAU_SEQ_ILLUMINATION
					: XWAU_SEQ_COLOUR;
	q.tint[0] = q.tint[1] = q.tint[2] = 1.0f;
	q.aspect                          = 1.0f;
	q.scale[0] = q.scale[1] = 1.0f;
	q.loc                   = XWAU_SEQ_LOC_RANGE;
	q.loc_range[0] = q.loc_range[1] = -0.2f;
	q.loc_range[2] = q.loc_range[3] = 0.2f;
	q.image                         = -1;
	q.section                       = section->order;
	if (listed)
		parse_listed(line, &q);
	else
		parse_frames(line, &q);
	if (!q.group_frames && q.frame_count == 0) {
		/* The renderer drops a line without animation data. */
		sequence_free(&q);
		return true;
	}
	XwauSequence* added = ARRAY_PUSH(section->items, section->count);
	if (!added) {
		sequence_free(&q);
		return false;
	}
	*added = q;
	return true;
}

static bool push_entry(XwauMatFile* file, const char* name, const XwauMatParams* params) {
	if (file->count == file->cap) {
		size_t        cap   = file->cap ? file->cap * 2 : 32;
		XwauMatEntry* grown = (XwauMatEntry*)realloc(file->entries, cap * sizeof *grown);
		if (!grown)
			return false;
		file->entries = grown;
		file->cap     = cap;
	}
	XwauMatEntry* e = &file->entries[file->count++];
	snprintf(e->name, sizeof e->name, "%s", name);
	e->params = *params;
	return true;
}

/* Closes a section. Each listed name gets the material of the section, except
 * "Default", which replaces slot 0 instead. */
static bool save_section(XwauMatFile* file, char (*names)[XWAU_MAT_NAME_MAX], size_t name_count,
						 const XwauMatParams* cur, int section) {
	for (size_t i = 0; i < name_count; ++i) {
		if (ascii_casecmp(names[i], "default") == 0) {
			file->slot0         = *cur;
			file->slot0_section = section;
		} else if (!push_entry(file, names[i], cur)) {
			return false;
		} else {
			file->entries[file->count - 1].section = section;
		}
	}
	return true;
}

/* Opens a new section. Its order is 0 when one of its names is Default. */
static int open_section(XwauMatFile* file, char (*names)[XWAU_MAT_NAME_MAX], size_t name_count,
						int* next_order) {
	XwauMatSection* section = ARRAY_PUSH(file->sections, file->section_count);
	if (!section)
		return -1;
	bool is_default = false;
	for (size_t i = 0; i < name_count; ++i)
		is_default |= ascii_casecmp(names[i], "default") == 0;
	section->order = is_default ? 0 : (*next_order)++;
	return (int)file->section_count - 1;
}

/* Splits "[A,B,...]" into names. A name counts only when ',' or ']' ends it.
 * Like in the renderer, names are not trimmed. */
static size_t parse_section_names(const char* line, char (*names)[XWAU_MAT_NAME_MAX], size_t cap) {
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
			if (len >= XWAU_MAT_NAME_MAX)
				len = XWAU_MAT_NAME_MAX - 1;
			memcpy(names[count], start, len);
			names[count][len] = 0;
			count++;
		}
		start = end + 1;
	}
	return count;
}

XwauMatFile* XwauMat_Load(const char* path, const XwauMatParams* global) {
	FILE* f = fopen(path, "rt");
	if (!f)
		return NULL;
	XwauMatFile* file = (XwauMatFile*)calloc(1, sizeof *file);
	if (!file) {
		fclose(f);
		return NULL;
	}
	file->slot0         = *global;
	file->slot0_section = -1;

	enum { MAX_SECTION_NAMES = 256 };

	int next_order = 1;
	int section    = -1;

	char          names[MAX_SECTION_NAMES][XWAU_MAT_NAME_MAX];
	size_t        name_count = 0;
	bool          saved      = true; /* lines before the first section are dropped */
	XwauMatParams cur        = file->slot0;
	char          line[XWAU_MAT_LINE_MAX];
	bool          ok = true;
	while (ok && fgets(line, sizeof line, f)) {
		if (line[0] == ';' || line[0] == '#')
			continue;
		char param[256], value[XWAU_MAT_LINE_MAX];
		if (scan_key_line(line, param, sizeof param, value, sizeof value) <= 0)
			continue;
		if (line[0] == '[') {
			if (!saved)
				ok = save_section(file, names, name_count, &cur, section);
			name_count = parse_section_names(line, names, MAX_SECTION_NAMES);
			cur        = file->slot0;
			saved      = false;
			section    = ok ? open_section(file, names, name_count, &next_order) : -1;
			ok         = ok && section >= 0;
		} else {
			read_material_line(line, &cur);
			if (!saved && section >= 0)
				ok = read_sequence_line(param, line, &file->sections[section]);
		}
	}
	if (ok && !saved)
		ok = save_section(file, names, name_count, &cur, section);
	fclose(f);
	if (!ok) {
		XwauMat_Free(file);
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

static const XwauMatEntry* find_entry(const XwauMatFile* file, const char* texture_name) {
	char        name[XWAU_MAT_NAME_MAX];
	const char* skin = find_skin_suffix(texture_name);
	size_t      len  = skin ? (size_t)(skin - texture_name) : strlen(texture_name);
	if (len >= sizeof name)
		len = sizeof name - 1;
	memcpy(name, texture_name, len);
	name[len] = 0;
	if (!name[0])
		return NULL;
	for (size_t i = 0; i < file->count; ++i) {
		if (ascii_casecmp(name, file->entries[i].name) == 0)
			return &file->entries[i];
	}
	return NULL;
}

const XwauMatParams* XwauMat_Find(const XwauMatFile* file, const char* texture_name) {
	const XwauMatEntry* entry = find_entry(file, texture_name);
	return entry ? &entry->params : &file->slot0;
}

static void section_sequences(const XwauMatFile* file, int section, XwauMatSequences* out) {
	memset(out, 0, sizeof *out);
	if (section < 0)
		return;
	out->items   = file->sections[section].items;
	out->count   = file->sections[section].count;
	out->section = file->sections[section].order;
}

bool XwauMat_FindSequences(const XwauMatFile* file, const char* texture_name, XwauMatSequences* own,
						   XwauMatSequences* inherited) {
	const XwauMatEntry* entry = find_entry(file, texture_name);
	section_sequences(file, file->slot0_section, inherited);
	if (!entry) {
		*own = *inherited;
		memset(inherited, 0, sizeof *inherited);
		return false;
	}
	section_sequences(file, entry->section, own);
	return true;
}

void XwauMat_Free(XwauMatFile* file) {
	if (!file)
		return;
	for (size_t s = 0; s < file->section_count; ++s) {
		for (size_t i = 0; i < file->sections[s].count; ++i)
			sequence_free(&file->sections[s].items[i]);
		free(file->sections[s].items);
	}
	free(file->sections);
	free(file->entries);
	free(file);
}
