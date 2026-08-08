/*
 * AeronUi theme — compiled-in default dark theme (zero assets) and the
 * optional YAML overlay loader. Colors are stored as straight-alpha
 * LINEAR RGBA; YAML authors sRGB values which convert at load.
 */

#include "internal.h"

#include "aeron/config_file.h"

#include <stddef.h>
#include <stdio.h>

/* Linear-space values (authored as sRGB, converted offline). */
static const AeronUiTheme k_default_theme = {
	.window_shadow   = { 0.0f, 0.0f, 0.0f, 0.55f },
	.surface         = { 0.008f, 0.009f, 0.013f, 0.97f },
	.surface_border  = { 0.046f, 0.054f, 0.075f, 1.0f },
	.title_bar       = { 0.022f, 0.028f, 0.046f, 1.0f },
	.title_bar_low   = { 0.011f, 0.014f, 0.023f, 1.0f },
	.title_text      = { 0.88f, 0.90f, 0.94f, 1.0f },
	.text            = { 0.72f, 0.75f, 0.80f, 1.0f },
	.text_dim        = { 0.24f, 0.26f, 0.30f, 1.0f },
	.accent          = { 0.075f, 0.34f, 0.95f, 1.0f },
	.focus_outline   = { 0.11f, 0.44f, 1.0f, 1.0f },
	.widget_bg       = { 0.020f, 0.025f, 0.039f, 1.0f },
	.widget_bg_hot   = { 0.036f, 0.046f, 0.070f, 1.0f },
	.widget_bg_low   = { 0.012f, 0.015f, 0.023f, 1.0f },
	.widget_border   = { 0.062f, 0.072f, 0.095f, 1.0f },
	.slider_track    = { 0.030f, 0.036f, 0.052f, 1.0f },
	.scrollbar       = { 0.018f, 0.022f, 0.032f, 0.85f },
	.scrollbar_thumb = { 0.10f, 0.12f, 0.16f, 1.0f },
	.separator       = { 0.050f, 0.057f, 0.074f, 1.0f },
	.row_highlight   = { 0.11f, 0.44f, 1.0f, 0.10f },
	.scrim           = { 0.0f, 0.0f, 0.0f, 0.55f },
	.bevel_hi        = { 1.0f, 1.0f, 1.0f, 0.055f },
	.bevel_lo        = { 0.0f, 0.0f, 0.0f, 0.22f },

	.window_pad       = 18.0f,
	.window_border_px = 1.0f,
	.title_height     = 46.0f,
	.item_spacing     = 10.0f,
	.column_spacing   = 26.0f,
	.row_height       = 40.0f,
	.widget_border_px = 1.0f,
	.corner_radius    = 4.0f,
	.focus_px         = 2.0f,
	.slider_track_px  = 4.0f,
	.scrollbar_px     = 8.0f,
	.label_fraction   = 0.45f,
	.text_px          = 20.0f,
	.title_px         = 22.0f,
	.help_px          = 20.0f,
	.bevel_px         = 2.0f,

	.widget_gradient = 1,
	.title_gradient  = 1,
	.shadow_soft_px  = 26.0f,

	.repeat_delay_s    = 0.40f,
	.repeat_interval_s = 0.09f,
	.open_fade_s       = 0.12f,
};

const AeronUiTheme* AeronUi_DefaultTheme(void) { return &k_default_theme; }

/* ---------------------------- YAML overlay ---------------------------- */

typedef struct UiThemeKey {
	const char* name;
	size_t      offset;
} UiThemeKey;

#define UI_THEME_FIELD(field) { #field, offsetof(AeronUiTheme, field) }

static const UiThemeKey k_color_keys[] = {
	UI_THEME_FIELD(window_shadow), UI_THEME_FIELD(surface),         UI_THEME_FIELD(surface_border),
	UI_THEME_FIELD(title_bar),     UI_THEME_FIELD(title_bar_low),   UI_THEME_FIELD(title_text),
	UI_THEME_FIELD(text),          UI_THEME_FIELD(text_dim),        UI_THEME_FIELD(accent),
	UI_THEME_FIELD(focus_outline), UI_THEME_FIELD(widget_bg),       UI_THEME_FIELD(widget_bg_hot),
	UI_THEME_FIELD(widget_bg_low), UI_THEME_FIELD(widget_border),   UI_THEME_FIELD(slider_track),
	UI_THEME_FIELD(scrollbar),     UI_THEME_FIELD(scrollbar_thumb), UI_THEME_FIELD(separator),
	UI_THEME_FIELD(row_highlight), UI_THEME_FIELD(scrim),           UI_THEME_FIELD(bevel_hi),
	UI_THEME_FIELD(bevel_lo),
};

static const UiThemeKey k_metric_keys[] = {
	UI_THEME_FIELD(window_pad),       UI_THEME_FIELD(window_border_px), UI_THEME_FIELD(title_height),
	UI_THEME_FIELD(item_spacing),     UI_THEME_FIELD(column_spacing),   UI_THEME_FIELD(row_height),
	UI_THEME_FIELD(widget_border_px), UI_THEME_FIELD(corner_radius),    UI_THEME_FIELD(focus_px),
	UI_THEME_FIELD(slider_track_px),  UI_THEME_FIELD(scrollbar_px),     UI_THEME_FIELD(label_fraction),
	UI_THEME_FIELD(text_px),          UI_THEME_FIELD(title_px),         UI_THEME_FIELD(help_px),
	UI_THEME_FIELD(bevel_px),         UI_THEME_FIELD(shadow_soft_px),
};

static const UiThemeKey k_int_keys[] = {
	UI_THEME_FIELD(widget_gradient),
	UI_THEME_FIELD(title_gradient),
};

static const UiThemeKey k_timing_keys[] = {
	UI_THEME_FIELD(repeat_delay_s),
	UI_THEME_FIELD(repeat_interval_s),
	UI_THEME_FIELD(open_fade_s),
};

static float ui_srgb_to_linear(float srgb) {
	if (srgb <= 0.04045f) {
		return srgb / 12.92f;
	}
	return powf((srgb + 0.055f) / 1.055f, 2.4f);
}

static void ui_theme_load_color(AeronUiTheme* out, const AeronConfigNode* node, const UiThemeKey* key,
								const char* path) {
	float* color = (float*)((char*)out + key->offset);
	if (AeronConfigNode_Type(node) != AERON_CONFIG_SEQUENCE || AeronConfigNode_SequenceCount(node) < 3) {
		Aeron_LogWarn("aeron.scene", "ui theme %s: expected [r, g, b(, a)]", path);
		return;
	}
	for (size_t i = 0; i < 4; i++) {
		const AeronConfigNode* item = AeronConfigNode_SequenceGet(node, i);
		if (!item) {
			break; /* alpha defaults to the existing value (3-item form keeps it) */
		}
		float value = (float)AeronConfigNode_Float(item, i == 3 ? 1.0 : 0.0);
		value       = fminf(fmaxf(value, 0.0f), 1.0f);
		color[i]    = i < 3 ? ui_srgb_to_linear(value) : value;
	}
	if (AeronConfigNode_SequenceCount(node) == 3) {
		color[3] = 1.0f;
	}
}

/* Overlays one section; warns for unknown keys so theme typos surface. */
static void ui_theme_load_section(AeronUiTheme* out, const AeronConfigFile* cfg, const char* section,
								  const UiThemeKey* keys, size_t key_count, int is_color, int is_int) {
	char path[128];
	snprintf(path, sizeof path, "%s", section);
	const AeronConfigNode* map = AeronConfigFile_GetNode(cfg, section);
	if (!map) {
		return;
	}
	const size_t entries = AeronConfigNode_MapCount(map);
	for (size_t i = 0; i < entries; i++) {
		const char*            name = AeronConfigNode_MapKeyAt(map, i);
		const AeronConfigNode* node = AeronConfigNode_MapValueAt(map, i);
		const UiThemeKey*      key  = NULL;
		for (size_t k = 0; k < key_count; k++) {
			if (name && strcmp(name, keys[k].name) == 0) {
				key = &keys[k];
				break;
			}
		}
		snprintf(path, sizeof path, "%s.%s", section, name ? name : "?");
		if (!key) {
			Aeron_LogWarn("aeron.scene", "ui theme: unknown key %s (line %d)", path,
						  AeronConfigNode_Line(node));
			continue;
		}
		if (is_color) {
			ui_theme_load_color(out, node, key, path);
		} else if (is_int) {
			*(int*)((char*)out + key->offset) =
				AeronConfigNode_Bool(node, (int)AeronConfigNode_Int(node, *(int*)((char*)out + key->offset)));
		} else {
			*(float*)((char*)out + key->offset) =
				(float)AeronConfigNode_Float(node, (double)*(float*)((char*)out + key->offset));
		}
	}
}

int AeronUi_ThemeLoadYaml(AeronUiTheme* out, AeronVfs* vfs, AeronVfsRoot root, const char* path) {
	if (!out || !path) {
		return 0;
	}
	*out = k_default_theme;

	AeronConfigFile* cfg = NULL;
	if (!AeronConfigFile_LoadYaml(vfs, root, path, &cfg)) {
		Aeron_LogError("aeron.scene", "ui theme load failed: %s", path);
		return 0;
	}
	ui_theme_load_section(out, cfg, "colors", k_color_keys, sizeof k_color_keys / sizeof k_color_keys[0], 1,
						  0);
	ui_theme_load_section(out, cfg, "metrics", k_metric_keys, sizeof k_metric_keys / sizeof k_metric_keys[0],
						  0, 0);
	ui_theme_load_section(out, cfg, "effects", k_int_keys, sizeof k_int_keys / sizeof k_int_keys[0], 0, 1);
	ui_theme_load_section(out, cfg, "timing", k_timing_keys, sizeof k_timing_keys / sizeof k_timing_keys[0],
						  0, 0);
	AeronConfigFile_Destroy(cfg);
	return 1;
}
