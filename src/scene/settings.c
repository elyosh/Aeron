#include "aeron/scene/settings.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int settings_error(AeronConfigError* error, const AeronConfigNode* node, const char* format, ...) {
	if (error) {
		va_list args;
		memset(error, 0, sizeof *error);
		error->code = AERON_CONFIG_ERROR_INVALID_ARGUMENT;
		if (node) {
			const char* source_path = AeronConfigNode_SourcePath(node);
			error->root = AeronConfigNode_SourceRoot(node);
			error->line = AeronConfigNode_Line(node);
			error->column = AeronConfigNode_Column(node);
			snprintf(error->path, sizeof error->path, "%s", source_path ? source_path : "<configuration>");
		}
		va_start(args, format);
		vsnprintf(error->message, sizeof error->message, format, args);
		va_end(args);
	}
	return 0;
}

static int key_allowed(const char* key, const char* const* allowed, size_t count) {
	for (size_t index = 0; index < count; ++index)
		if (strcmp(key, allowed[index]) == 0)
			return 1;
	return 0;
}

static int check_map(const AeronConfigNode* map, const char* name, const char* const* allowed,
					 size_t allowed_count, int required, AeronConfigError* error) {
	if (!map)
		return required ? settings_error(error, NULL, "missing required '%s' map", name) : 1;
	if (AeronConfigNode_Type(map) != AERON_CONFIG_MAP)
		return settings_error(error, map, "'%s' must be a map", name);
	for (size_t index = 0; index < AeronConfigNode_MapCount(map); ++index) {
		const char* key = AeronConfigNode_MapKeyAt(map, index);
		if (!key_allowed(key, allowed, allowed_count))
			return settings_error(error, AeronConfigNode_MapValueAt(map, index), "unknown setting '%s.%s'", name,
							  key);
	}
	return 1;
}

static int read_number(const AeronConfigNode* map, const char* section, const char* key, int required,
					   float* out, AeronConfigError* error) {
	const AeronConfigNode* node = AeronConfigNode_MapGet(map, key);
	if (!node)
		return required ? settings_error(error, map, "missing required setting '%s.%s'", section, key) : 1;
	if (AeronConfigNode_Type(node) != AERON_CONFIG_INT && AeronConfigNode_Type(node) != AERON_CONFIG_FLOAT)
		return settings_error(error, node, "setting '%s.%s' must be numeric", section, key);
	double value = AeronConfigNode_Float(node, NAN);
	if (!isfinite(value))
		return settings_error(error, node, "setting '%s.%s' must be finite", section, key);
	*out = (float)value;
	return 1;
}

static int read_integer(const AeronConfigNode* map, const char* section, const char* key, int required,
						int* out, AeronConfigError* error) {
	const AeronConfigNode* node = AeronConfigNode_MapGet(map, key);
	if (!node)
		return required ? settings_error(error, map, "missing required setting '%s.%s'", section, key) : 1;
	if (AeronConfigNode_Type(node) != AERON_CONFIG_INT)
		return settings_error(error, node, "setting '%s.%s' must be an integer", section, key);
	const int64_t value = AeronConfigNode_Int(node, 0);
	if (value < INT32_MIN || value > INT32_MAX)
		return settings_error(error, node, "setting '%s.%s' is outside the integer range", section, key);
	*out = (int)value;
	return 1;
}

static int read_bool(const AeronConfigNode* map, const char* section, const char* key, int required, int* out,
					 AeronConfigError* error) {
	const AeronConfigNode* node = AeronConfigNode_MapGet(map, key);
	if (!node)
		return required ? settings_error(error, map, "missing required setting '%s.%s'", section, key) : 1;
	if (AeronConfigNode_Type(node) != AERON_CONFIG_BOOL)
		return settings_error(error, node, "setting '%s.%s' must be boolean", section, key);
	*out = AeronConfigNode_Bool(node, 0);
	return 1;
}

static int read_string(const AeronConfigNode* map, const char* section, const char* key, int required,
					   const char** out, AeronConfigError* error) {
	const AeronConfigNode* node = AeronConfigNode_MapGet(map, key);
	if (!node)
		return required ? settings_error(error, map, "missing required setting '%s.%s'", section, key) : 1;
	if (AeronConfigNode_Type(node) != AERON_CONFIG_STRING)
		return settings_error(error, node, "setting '%s.%s' must be a string", section, key);
	*out = AeronConfigNode_String(node, "");
	return 1;
}

static int ssao_valid(const AeronSceneSsaoSettings* value) {
	return value->ssao_quality >= 0 && value->ssao_quality <= 2 && isfinite(value->ssao_intensity) &&
		   value->ssao_intensity >= 0.0f && isfinite(value->ssao_power) && value->ssao_power > 0.0f &&
		   isfinite(value->ssao_radius_view) && value->ssao_radius_view > 0.0f &&
		   isfinite(value->ssao_bias_view) && value->ssao_bias_view >= 0.0f && isfinite(value->ssao_direct) &&
		   value->ssao_direct >= 0.0f && value->ssao_direct <= 1.0f &&
		   isfinite(value->ssao_min_screen_frac) && value->ssao_min_screen_frac >= 0.0f &&
		   isfinite(value->ssao_max_screen_frac) && value->ssao_max_screen_frac >= 0.0f &&
		   value->ssao_min_screen_frac <= value->ssao_max_screen_frac &&
		   isfinite(value->ssao_sample_jitter) && value->ssao_sample_jitter >= 0.0f &&
		   value->ssao_sample_jitter <= 1.0f;
}

static int shadows_valid(const AeronSceneShadowSettings* value) {
	return value->atlas_size >= 1024 && value->atlas_size <= 8192 && value->fit_mode <= 2 &&
		   (value->atlas_size & (value->atlas_size - 1)) == 0 && value->cascade_count >= 1 &&
		   value->cascade_count <= AERON_SCENE_SHADOW_MAX_CASCADES && value->filter_quality <= 3 &&
		   isfinite(value->max_distance) && value->max_distance > 1.0f && isfinite(value->split_lambda) &&
		   value->split_lambda >= 0.0f && value->split_lambda <= 1.0f &&
		   value->split_positions[0] > 0.0f && value->split_positions[0] < value->split_positions[1] &&
		   value->split_positions[1] < value->split_positions[2] && value->split_positions[2] < 1.0f &&
		   value->filter_radius >= 0.5f && value->filter_radius <= 3.0f &&
		   value->light_angular_radius_degrees >= 0.0f && value->light_angular_radius_degrees <= 5.0f &&
		   value->max_filter_radius >= value->filter_radius && value->max_filter_radius <= 16.0f &&
		   value->pcss_min_filter_radius >= 0.5f && value->pcss_min_filter_radius <= value->filter_radius &&
		   value->normal_bias_texels >= 0.0f && value->normal_bias_texels <= 4.0f &&
		   value->depth_bias_texels >= 0.0f && value->depth_bias_texels <= 4.0f &&
		   value->transition_fraction >= 0.0f && value->transition_fraction <= 0.5f &&
		   value->distance_fade_fraction >= 0.0f && value->distance_fade_fraction <= 0.5f;
}

static int tonemap_valid(const AeronSceneTonemapSettings* value) {
	return value->tonemap_operator >= 0 && value->tonemap_operator < AERON_SCENE_TONEMAP_COUNT &&
		   value->agx_look >= 0 && value->agx_look < AERON_SCENE_AGX_LOOK_COUNT &&
		   isfinite(value->agx_eotf_exponent) && value->agx_eotf_exponent >= 1.8f &&
		   value->agx_eotf_exponent <= 2.6f && isfinite(value->agx_punchy_power) &&
		   value->agx_punchy_power >= 0.5f && value->agx_punchy_power <= 2.0f &&
		   isfinite(value->agx_punchy_saturation) && value->agx_punchy_saturation >= 0.0f &&
		   value->agx_punchy_saturation <= 2.0f && isfinite(value->aces_pre_exposure) &&
		   value->aces_pre_exposure >= 1.0f && value->aces_pre_exposure <= 3.0f;
}

static int parse_ssao(const AeronConfigNode* map, int required, AeronSceneSsaoSettings* value,
					  AeronConfigError* error) {
	static const char* const keys[] = { "quality", "intensity", "power", "radius_view", "bias_view",
									   "direct", "debug_viz", "min_screen_frac", "max_screen_frac",
									   "sample_jitter" };
	if (!check_map(map, "ssao", keys, sizeof keys / sizeof keys[0], required, error))
		return 0;
	if (!map)
		return 1;
	int quality = value->ssao_quality;
	if (!read_integer(map, "ssao", "quality", required, &quality, error) ||
		!read_number(map, "ssao", "intensity", required, &value->ssao_intensity, error) ||
		!read_number(map, "ssao", "power", required, &value->ssao_power, error) ||
		!read_number(map, "ssao", "radius_view", required, &value->ssao_radius_view, error) ||
		!read_number(map, "ssao", "bias_view", required, &value->ssao_bias_view, error) ||
		!read_number(map, "ssao", "direct", required, &value->ssao_direct, error) ||
		!read_bool(map, "ssao", "debug_viz", required, &value->ssao_debug_viz, error) ||
		!read_number(map, "ssao", "min_screen_frac", required, &value->ssao_min_screen_frac, error) ||
		!read_number(map, "ssao", "max_screen_frac", required, &value->ssao_max_screen_frac, error) ||
		!read_number(map, "ssao", "sample_jitter", required, &value->ssao_sample_jitter, error))
		return 0;
	value->ssao_quality = quality;
	return ssao_valid(value) ? 1 : settings_error(error, map, "invalid SSAO settings");
}

static int parse_shadow_enums(const AeronConfigNode* map, int required, AeronSceneShadowSettings* value,
							  AeronConfigError* error) {
	const char* mode = NULL;
	if (!read_string(map, "shadows", "mode", required, &mode, error))
		return 0;
	if (mode) {
		if (strcmp(mode, "off") == 0)
			value->enabled = 0;
		else if (strcmp(mode, "pcf") == 0)
			value->enabled = 1;
		else
			return settings_error(error, AeronConfigNode_MapGet(map, "mode"),
							  "shadows.mode must be 'off' or 'pcf'");
	}
	const char* fit = NULL;
	if (!read_string(map, "shadows", "fit_mode", required, &fit, error))
		return 0;
	if (fit) {
		if (strcmp(fit, "stable") == 0)
			value->fit_mode = AERON_SCENE_SHADOW_FIT_STABLE;
		else if (strcmp(fit, "frustum") == 0)
			value->fit_mode = AERON_SCENE_SHADOW_FIT_FRUSTUM;
		else if (strcmp(fit, "scene_dependent") == 0)
			value->fit_mode = AERON_SCENE_SHADOW_FIT_SCENE_DEPENDENT;
		else
			return settings_error(error, AeronConfigNode_MapGet(map, "fit_mode"),
							  "invalid shadows.fit_mode");
	}
	return 1;
}

static int parse_shadows(const AeronConfigNode* map, int required, AeronSceneShadowSettings* value,
						 AeronConfigError* error) {
	static const char* const keys[] = {
		"mode", "atlas_size", "cascade_count", "fit_mode", "max_distance", "split_lambda",
		"explicit_splits", "split_1", "split_2", "split_3", "filter_quality", "filter_radius",
		"contact_hardening", "light_angular_radius_degrees", "max_filter_radius", "pcss_min_filter_radius",
		"normal_bias_texels", "depth_bias_texels", "transition_fraction", "distance_fade_fraction",
		"debug_cascades"
	};
	if (!check_map(map, "shadows", keys, sizeof keys / sizeof keys[0], required, error))
		return 0;
	if (!map)
		return 1;
	int atlas = (int)value->atlas_size, cascades = (int)value->cascade_count;
	int quality = (int)value->filter_quality;
	if (!parse_shadow_enums(map, required, value, error) ||
		!read_integer(map, "shadows", "atlas_size", required, &atlas, error) ||
		!read_integer(map, "shadows", "cascade_count", required, &cascades, error) ||
		!read_number(map, "shadows", "max_distance", required, &value->max_distance, error) ||
		!read_number(map, "shadows", "split_lambda", required, &value->split_lambda, error) ||
		!read_bool(map, "shadows", "explicit_splits", required, &value->explicit_splits, error) ||
		!read_number(map, "shadows", "split_1", required, &value->split_positions[0], error) ||
		!read_number(map, "shadows", "split_2", required, &value->split_positions[1], error) ||
		!read_number(map, "shadows", "split_3", required, &value->split_positions[2], error) ||
		!read_integer(map, "shadows", "filter_quality", required, &quality, error) ||
		!read_number(map, "shadows", "filter_radius", required, &value->filter_radius, error) ||
		!read_bool(map, "shadows", "contact_hardening", required, &value->contact_hardening, error) ||
		!read_number(map, "shadows", "light_angular_radius_degrees", required,
					 &value->light_angular_radius_degrees, error) ||
		!read_number(map, "shadows", "max_filter_radius", required, &value->max_filter_radius, error) ||
		!read_number(map, "shadows", "pcss_min_filter_radius", required, &value->pcss_min_filter_radius,
					 error) ||
		!read_number(map, "shadows", "normal_bias_texels", required, &value->normal_bias_texels, error) ||
		!read_number(map, "shadows", "depth_bias_texels", required, &value->depth_bias_texels, error) ||
		!read_number(map, "shadows", "transition_fraction", required, &value->transition_fraction, error) ||
		!read_number(map, "shadows", "distance_fade_fraction", required, &value->distance_fade_fraction,
					 error) ||
		!read_bool(map, "shadows", "debug_cascades", required, &value->debug_cascades, error))
		return 0;
	if (atlas < 0 || cascades < 0 || quality < 0)
		return settings_error(error, map, "invalid negative shadow integer setting");
	value->atlas_size = (uint32_t)atlas;
	value->cascade_count = (uint32_t)cascades;
	value->filter_quality = (uint32_t)quality;
	return shadows_valid(value) ? 1 : settings_error(error, map, "invalid directional-shadow settings");
}

static int parse_tonemap(const AeronConfigNode* map, int required, AeronSceneTonemapSettings* value,
						 AeronConfigError* error) {
	static const char* const keys[] = { "operator", "agx_look", "agx_eotf_exponent", "agx_punchy_power",
									  "agx_punchy_saturation", "aces_pre_exposure" };
	if (!check_map(map, "tonemap", keys, sizeof keys / sizeof keys[0], required, error))
		return 0;
	if (!map)
		return 1;

	const char* operator_name = NULL;
	const char* look_name = NULL;
	if (!read_string(map, "tonemap", "operator", required, &operator_name, error) ||
		!read_string(map, "tonemap", "agx_look", required, &look_name, error) ||
		!read_number(map, "tonemap", "agx_eotf_exponent", required, &value->agx_eotf_exponent, error) ||
		!read_number(map, "tonemap", "agx_punchy_power", required, &value->agx_punchy_power, error) ||
		!read_number(map, "tonemap", "agx_punchy_saturation", required, &value->agx_punchy_saturation,
					 error) ||
		!read_number(map, "tonemap", "aces_pre_exposure", required, &value->aces_pre_exposure, error))
		return 0;

	if (operator_name) {
		if (strcmp(operator_name, "agx") == 0)
			value->tonemap_operator = AERON_SCENE_TONEMAP_AGX_PARAMETRIC;
		else if (strcmp(operator_name, "aces") == 0)
			value->tonemap_operator = AERON_SCENE_TONEMAP_ACES;
		else
			return settings_error(error, AeronConfigNode_MapGet(map, "operator"),
							  "tonemap.operator must be 'agx' or 'aces'");
	}
	if (look_name) {
		if (strcmp(look_name, "base") == 0)
			value->agx_look = AERON_SCENE_AGX_LOOK_BASE;
		else if (strcmp(look_name, "punchy") == 0)
			value->agx_look = AERON_SCENE_AGX_LOOK_PUNCHY;
		else
			return settings_error(error, AeronConfigNode_MapGet(map, "agx_look"),
							  "tonemap.agx_look must be 'base' or 'punchy'");
	}
	return tonemap_valid(value) ? 1 : settings_error(error, map, "invalid tone-map settings");
}

static int parse_settings(const AeronConfigNode* root, int required, AeronSceneSsaoSettings* ssao,
						  AeronSceneShadowSettings* shadows, AeronSceneTonemapSettings* tonemap,
						  AeronConfigError* error) {
	if (!root || AeronConfigNode_Type(root) != AERON_CONFIG_MAP || !ssao || !shadows || !tonemap)
		return settings_error(error, root, "invalid scene settings root or output");
	AeronSceneSsaoSettings next_ssao = *ssao;
	AeronSceneShadowSettings next_shadows = *shadows;
	AeronSceneTonemapSettings next_tonemap = *tonemap;
	if (!parse_ssao(AeronConfigNode_MapGet(root, "ssao"), required, &next_ssao, error) ||
		!parse_shadows(AeronConfigNode_MapGet(root, "shadows"), required, &next_shadows, error) ||
		!parse_tonemap(AeronConfigNode_MapGet(root, "tonemap"), required, &next_tonemap, error))
		return 0;
	*ssao = next_ssao;
	*shadows = next_shadows;
	*tonemap = next_tonemap;
	return 1;
}

int AeronSceneSettings_Load(const AeronConfigNode* root, AeronSceneSsaoSettings* ssao,
								AeronSceneShadowSettings* shadows, AeronSceneTonemapSettings* tonemap,
								AeronConfigError* error) {
	if (!ssao || !shadows || !tonemap)
		return settings_error(error, root, "missing scene settings output");
	memset(ssao, 0, sizeof *ssao);
	memset(shadows, 0, sizeof *shadows);
	memset(tonemap, 0, sizeof *tonemap);
	return parse_settings(root, 1, ssao, shadows, tonemap, error);
}

int AeronSceneSettings_Overlay(const AeronConfigNode* root, AeronSceneSsaoSettings* ssao,
								   AeronSceneShadowSettings* shadows, AeronSceneTonemapSettings* tonemap,
								   AeronConfigError* error) {
	return parse_settings(root, 0, ssao, shadows, tonemap, error);
}
