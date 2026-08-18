#ifndef AERON_SCENE_SETTINGS_H
#define AERON_SCENE_SETTINGS_H

#include "aeron/config_file.h"
#include "aeron/scene/present.h"
#include "aeron/scene/scene3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronSceneSsaoSettings {
	int ssao_quality;
	float ssao_intensity;
	float ssao_power;
	float ssao_radius_view;
	float ssao_bias_view;
	float ssao_direct;
	int ssao_debug_viz;
	float ssao_min_screen_frac;
	float ssao_max_screen_frac;
	float ssao_sample_jitter;
} AeronSceneSsaoSettings;

typedef struct AeronSceneShadowSettings {
	int enabled;
	uint32_t atlas_size;
	uint32_t cascade_count;
	uint32_t fit_mode;
	float max_distance;
	float split_lambda;
	int explicit_splits;
	float split_positions[AERON_SCENE_SHADOW_MAX_CASCADES - 1];
	uint32_t filter_quality;
	float filter_radius;
	int contact_hardening;
	float light_angular_radius_degrees;
	float max_filter_radius;
	float pcss_min_filter_radius;
	float normal_bias_texels;
	float depth_bias_texels;
	float transition_fraction;
	float distance_fade_fraction;
	int debug_cascades;
} AeronSceneShadowSettings;

/* `root` contains `ssao`, `shadows`, and `tonemap` maps. Load requires the
 * complete schema; Overlay accepts partial maps and validates the result. */
int AeronSceneSettings_Load(const AeronConfigNode* root, AeronSceneSsaoSettings* ssao,
								AeronSceneShadowSettings* shadows, AeronSceneTonemapSettings* tonemap,
								AeronConfigError* error);
int AeronSceneSettings_Overlay(const AeronConfigNode* root, AeronSceneSsaoSettings* ssao,
								   AeronSceneShadowSettings* shadows, AeronSceneTonemapSettings* tonemap,
								   AeronConfigError* error);

#ifdef __cplusplus
}
#endif

#endif
