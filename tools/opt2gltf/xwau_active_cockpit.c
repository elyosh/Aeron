/*
 * Reads XWAU Active Cockpit files into the buttons of opt2gltf materials
 * (xwau_active_cockpit.h).
 */

#include "xwau_active_cockpit.h"

#include "opt_json.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Writes the key's name as XwauNames.key_actions spells it: in capitals,
 * with the modifiers first. XWAU looks for the modifiers anywhere in the
 * name, and reads the key from what follows the last one. */
static void normalise_key(const char* name, char out[64]) {
	char   upper[64];
	size_t length = 0;
	for (const char* p = name; *p && length + 1 < sizeof upper; ++p)
		if (!isspace((unsigned char)*p))
			upper[length++] = (char)toupper((unsigned char)*p);
	upper[length]   = 0;
	const char* key = strrchr(upper, '+');
	/* "SHIFT+'" and such: the key is after the last '+'. A lone '+' is not
	 * a key of these files (XWAU names it PLUS). */
	key                      = key ? key + 1 : upper;
	const bool has_modifiers = key != upper;
	snprintf(out, 64, "%s%s%s%s", has_modifiers && strstr(upper, "CTRL") ? "CTRL+" : "",
			 has_modifiers && strstr(upper, "SHIFT") ? "SHIFT+" : "",
			 has_modifiers && strstr(upper, "ALT") ? "ALT+" : "", key);
}

int XwauActiveCockpit_ParseAction(const char* value, float width, float height, bool flip_v,
								  const XwauNames* names, XwauActiveCockpitButton* out, char key[64]) {
	while (*value == ' ' || *value == '\t')
		value++;
	/* The name ends at the first comma. */
	const char* comma = strchr(value, ',');
	char        name[64];
	float       x0, y0, x1, y1;
	if (!comma || comma == value || (size_t)(comma - value) >= sizeof name ||
		sscanf(comma + 1, "%f , %f , %f , %f", &x0, &y0, &x1, &y1) != 4 || !(width > 0.0f) ||
		!(height > 0.0f))
		return -1;
	memcpy(name, value, (size_t)(comma - value));
	name[comma - value] = 0;
	normalise_key(name, key);
	const char* action = strcmp(key, "HOLOGRAM") == 0        ? "toggle_holograms"
						 : strcmp(key, "VRKEYB_TOGGLE") == 0 ? "toggle_vr_keyboard"
															 : XwauNames_KeyAction(names, key);
	if (!action)
		return 0;
	x0 /= width;
	x1 /= width;
	y0 /= height;
	y1 /= height;
	if (flip_v) {
		y0 = 1.0f - y0;
		y1 = 1.0f - y1;
	}
	out->rect[0] = x0 < x1 ? x0 : x1;
	out->rect[1] = y0 < y1 ? y0 : y1;
	out->rect[2] = x0 < x1 ? x1 : x0;
	out->rect[3] = y0 < y1 ? y1 : y0;
	out->action  = action;
	return 1;
}

char* XwauActiveCockpit_ButtonsJson(const XwauActiveCockpitButton* buttons, int count) {
	cJSON* list = cJSON_CreateArray();
	bool   ok   = list != NULL;
	for (int i = 0; ok && i < count; ++i) {
		cJSON* button = OptJson_AddObject(list, NULL);
		ok            = OptJson_Add(button, "rect", OptJson_Floats(buttons[i].rect, 4)) &&
						OptJson_Add(button, "action", cJSON_CreateString(buttons[i].action));
	}
	if (!ok) {
		cJSON_Delete(list);
		return NULL;
	}
	return OptJson_Print(list);
}

bool XwauActiveCockpit_Load(const char* ac_path, const opt_file_t* opt, const char* basename,
							const XwauNames* names, XwauActiveCockpit* out, const char* label) {
	memset(out, 0, sizeof *out);
	FILE* f = fopen(ac_path, "rt");
	if (!f) {
		Report(label, "cannot read %s", ac_path);
		return false;
	}
	const size_t             textures = opt->texture_count ? (size_t)opt->texture_count : 1;
	XwauActiveCockpitButton* buttons =
		(XwauActiveCockpitButton*)calloc(textures * XWAU_ACTIVE_COCKPIT_MAX_BUTTONS, sizeof *buttons);
	int* counts        = (int*)calloc(textures, sizeof *counts);
	out->buttons       = (char**)calloc(textures, sizeof *out->buttons);
	out->texture_count = opt->texture_count;
	bool ok            = buttons && counts && out->buttons;

	int32_t texture = -1; /* the current section's OPT texture, or -1 */
	float   width = 1.0f, height = 1.0f;
	char    buf[256], param[128];
	while (ok && fgets(buf, sizeof buf, f)) {
		if (buf[0] == ';' || buf[0] == '#' || sscanf(buf, "%127s", param) < 1)
			continue;
		if (buf[0] == '[') {
			/* [<Cockpit>.opt,<Texture>], as in a .dc file. */
			char name[128];
			snprintf(name, sizeof name, "%s", buf + 1);
			char* end = strchr(name, ']');
			if (end)
				*end = 0;
			texture     = -1;
			char* comma = strchr(name, ',');
			if (!comma)
				continue;
			*comma                  = 0;
			const char*  opt_ext    = strrchr(name, '.');
			const size_t stem       = opt_ext ? (size_t)(opt_ext - name) : strlen(name);
			const bool   same_model = strlen(basename) == stem && strncasecmp(name, basename, stem) == 0 &&
									  (!opt_ext || strcasecmp(opt_ext, ".opt") == 0);
			for (int32_t i = 0; same_model && i < opt->texture_count && texture < 0; ++i)
				if (strcasecmp(opt->textures[i].name, comma + 1) == 0)
					texture = i;
			if (texture < 0)
				Report(label, "%s: the model has no texture for the section [%s,%s], which is left out",
					   ac_path, name, comma + 1);
			continue;
		}
		const char* value = strchr(buf, '=');
		if (!value)
			continue;
		value++;
		if (strcasecmp(param, "texture_size") == 0) {
			float w, h;
			if (sscanf(value, "%f , %f", &w, &h) == 2 && w > 0.0f && h > 0.0f) {
				width  = w;
				height = h;
			}
		} else if (strcasecmp(param, "action") == 0 && texture >= 0) {
			XwauActiveCockpitButton button;
			char                    key[64];
			const int               result =
				XwauActiveCockpit_ParseAction(value, width, height, false, names, &button, key);
			if (result == 0) {
				Report(label,
					   "%s: the key %s has no action in the game's default key bindings, so its button is "
					   "left out",
					   ac_path, key);
				out->skipped++;
			} else if (result > 0 && counts[texture] < XWAU_ACTIVE_COCKPIT_MAX_BUTTONS) {
				buttons[(size_t)texture * XWAU_ACTIVE_COCKPIT_MAX_BUTTONS + (size_t)counts[texture]++] =
					button;
				out->converted++;
			}
		}
	}
	fclose(f);
	for (size_t t = 0; ok && t < textures; ++t) {
		if (!counts[t])
			continue;
		out->buttons[t] =
			XwauActiveCockpit_ButtonsJson(&buttons[t * XWAU_ACTIVE_COCKPIT_MAX_BUTTONS], counts[t]);
		ok = out->buttons[t] != NULL;
	}
	free(buttons);
	free(counts);
	if (!ok)
		XwauActiveCockpit_Free(out);
	return ok;
}

void XwauActiveCockpit_Free(XwauActiveCockpit* ac) {
	for (int32_t t = 0; ac->buttons && t < ac->texture_count; ++t)
		free(ac->buttons[t]);
	free(ac->buttons);
	memset(ac, 0, sizeof *ac);
}
