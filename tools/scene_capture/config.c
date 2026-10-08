#include "capture.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int number(const AeronConfigFile* yaml, const char* key, double min, double max, double* out) {
	const AeronConfigNode* node  = AeronConfigFile_GetNode(yaml, key);
	AeronConfigNodeType    type  = AeronConfigNode_Type(node);
	double                 value = AeronConfigNode_Float(node, NAN);
	if ((type != AERON_CONFIG_INT && type != AERON_CONFIG_FLOAT) || !isfinite(value) || value < min ||
		value > max) {
		fprintf(stderr, "Invalid or missing '%s' (expected %g..%g)\n", key, min, max);
		return 0;
	}
	*out = value;
	return 1;
}

static int integer(const AeronConfigFile* yaml, const char* key, int min, int max, int* out) {
	double value;
	if (!number(yaml, key, min, max, &value))
		return 0;
	if (value != floor(value)) {
		fprintf(stderr, "'%s' must be an integer\n", key);
		return 0;
	}
	*out = (int)value;
	return 1;
}

static int copy_path(char* out, size_t capacity, const char* value) {
	if (!value || !value[0] || strlen(value) >= capacity)
		return 0;
	for (const unsigned char* p = (const unsigned char*)value; *p; ++p)
		if (*p < 32 || *p == 127)
			return 0;
	memcpy(out, value, strlen(value) + 1);
	return 1;
}

static int load_models(const AeronConfigFile* yaml, CaptureConfig* config) {
	const AeronConfigNode* root       = AeronConfigFile_GetNode(yaml, "asset_root");
	const char*            asset_root = root ? AeronConfigNode_String(root, NULL) : ".";
	if (!copy_path(config->asset_root, sizeof config->asset_root, asset_root)) {
		fprintf(stderr, "Invalid asset_root\n");
		return 0;
	}
	config->model_count           = 0;
	const AeronConfigNode* models = AeronConfigFile_GetNode(yaml, "models");
	if (!models)
		return 1;
	size_t count = AeronConfigNode_SequenceCount(models);
	if (AeronConfigNode_Type(models) != AERON_CONFIG_SEQUENCE || count == 0 || count > CAPTURE_MAX_MODELS) {
		fprintf(stderr, "models must list 1-%d OPT or cooked GLB paths relative to asset_root\n",
				CAPTURE_MAX_MODELS);
		return 0;
	}
	for (size_t i = 0; i < count; ++i) {
		const char* value = AeronConfigNode_String(AeronConfigNode_SequenceGet(models, i), NULL);
		if (!copy_path(config->models[i], sizeof config->models[i], value)) {
			fprintf(stderr, "Invalid model path at index %zu\n", i);
			return 0;
		}
		for (size_t j = 0; j < i; ++j) {
			if (strcmp(config->models[i], config->models[j]) == 0) {
				fprintf(stderr, "Duplicate model path: %s\n", value);
				return 0;
			}
		}
	}
	config->model_count = (int)count;
	return 1;
}

static int load_effects(const AeronConfigFile* yaml, CaptureConfig* config) {
	double emission = 1;
	if (AeronConfigFile_GetNode(yaml, "emissive_scale") && !number(yaml, "emissive_scale", 0, 64, &emission))
		return 0;
	config->emissive_scale = (float)emission;

	const AeronConfigNode* fixture_node = AeronConfigFile_GetNode(yaml, "fixture");
	const char*            fixture = fixture_node ? AeronConfigNode_String(fixture_node, NULL) : "shapes";
	if (!fixture || (strcmp(fixture, "shapes") && strcmp(fixture, "bloom_chart"))) {
		fprintf(stderr, "fixture must be shapes or bloom_chart\n");
		return 0;
	}
	config->bloom_chart  = !strcmp(fixture, "bloom_chart");
	config->msaa_samples = 2;
	if (AeronConfigFile_GetNode(yaml, "msaa_samples") &&
		!integer(yaml, "msaa_samples", 1, 8, &config->msaa_samples))
		return 0;
	if (config->msaa_samples != 1 && config->msaa_samples != 2 && config->msaa_samples != 4 &&
		config->msaa_samples != 8) {
		fprintf(stderr, "msaa_samples must be 1, 2, 4 or 8\n");
		return 0;
	}
	const AeronConfigNode* cull_node = AeronConfigFile_GetNode(yaml, "cull");
	const char*            cull      = cull_node ? AeronConfigNode_String(cull_node, NULL) : "none";
	if (!cull || (strcmp(cull, "none") && strcmp(cull, "front") && strcmp(cull, "back"))) {
		fprintf(stderr, "cull must be none, front or back\n");
		return 0;
	}
	config->cull_mode = !strcmp(cull, "back") ? AERON_CULL_BACK : !strcmp(cull, "front") ? AERON_CULL_FRONT : AERON_CULL_NONE;
	const AeronConfigNode* fill_node = AeronConfigFile_GetNode(yaml, "backface_fill");
	if (fill_node && AeronConfigNode_Type(fill_node) != AERON_CONFIG_BOOL) {
		fprintf(stderr, "backface_fill must be true or false\n");
		return 0;
	}
	config->backface_fill = fill_node ? AeronConfigNode_Bool(fill_node, 1) : 1;

	const AeronConfigNode* screen = AeronConfigFile_GetNode(yaml, "screen_test");
	if (screen) {
		const AeronConfigNode* eye = AeronConfigNode_MapGet(screen, "eye");
		if (AeronConfigNode_Type(screen) != AERON_CONFIG_MAP || AeronConfigNode_SequenceCount(eye) != 3 ||
			!config->model_count) {
			fprintf(stderr, "screen_test needs models and eye: [x, y, z]\n");
			return 0;
		}
		for (int k = 0; k < 3; ++k)
			config->screen_eye[k] =
				(float)AeronConfigNode_Float(AeronConfigNode_SequenceGet(eye, (size_t)k), 0);
		config->screen_yaw = (float)AeronConfigNode_Float(AeronConfigNode_MapGet(screen, "yaw_degrees"), 0);
		config->screen_pitch =
			(float)AeronConfigNode_Float(AeronConfigNode_MapGet(screen, "pitch_degrees"), 0);
		config->screen_test = 1;
	}

	const AeronConfigNode* tonemap_node = AeronConfigFile_GetNode(yaml, "tonemap");
	const char*            tonemap      = tonemap_node ? AeronConfigNode_String(tonemap_node, NULL) : "agx";
	if (!tonemap || (strcmp(tonemap, "agx") && strcmp(tonemap, "aces"))) {
		fprintf(stderr, "tonemap must be agx or aces\n");
		return 0;
	}
	config->tonemap =
		!strcmp(tonemap, "aces") ? AERON_SCENE_TONEMAP_ACES : AERON_SCENE_TONEMAP_AGX_PARAMETRIC;
	config->bloom_kernel         = AERON_SCENE_BLOOM_KERNEL_4_TAP;
	config->bloom_intensity      = .5f;
	const AeronConfigNode* bloom = AeronConfigFile_GetNode(yaml, "bloom");
	if (bloom) {
		if (AeronConfigNode_Type(bloom) != AERON_CONFIG_MAP) {
			fprintf(stderr, "bloom must be a mapping\n");
			return 0;
		}
		for (size_t i = 0; i < AeronConfigNode_MapCount(bloom); ++i) {
			const char* key = AeronConfigNode_MapKeyAt(bloom, i);
			if (strcmp(key, "intensity") && strcmp(key, "kernel")) {
				fprintf(stderr, "Unknown bloom setting: %s\n", key);
				return 0;
			}
		}
		double intensity;
		int    kernel;
		if (!number(yaml, "bloom.intensity", 0, 16, &intensity) ||
			!integer(yaml, "bloom.kernel", 1, 4, &kernel))
			return 0;
		if (kernel != 1 && kernel != 4) {
			fprintf(stderr, "bloom.kernel must be 1 or 4\n");
			return 0;
		}
		config->bloom_enabled   = 1;
		config->bloom_intensity = (float)intensity;
		config->bloom_kernel = kernel == 1 ? AERON_SCENE_BLOOM_KERNEL_1_TAP : AERON_SCENE_BLOOM_KERNEL_4_TAP;
	}
	if (config->bloom_chart &&
		(config->model_count || config->quality || config->pan_speed || config->reference_samples ||
		 config->msaa_samples != 1 || config->emissive_scale != 1)) {
		fprintf(stderr, "bloom_chart requires no models, quality/pan/reference_samples 0, msaa_samples 1 and "
						"emissive_scale 1\n");
		return 0;
	}
	return 1;
}

static int vec3(const AeronConfigNode* node, float out[3]) {
	if (AeronConfigNode_Type(node) != AERON_CONFIG_SEQUENCE || AeronConfigNode_SequenceCount(node) != 3)
		return 0;
	for (size_t k = 0; k < 3; ++k) {
		const AeronConfigNode* item = AeronConfigNode_SequenceGet(node, k);
		const double           v    = AeronConfigNode_Float(item, NAN);
		if ((AeronConfigNode_Type(item) != AERON_CONFIG_INT && AeronConfigNode_Type(item) != AERON_CONFIG_FLOAT) ||
			!isfinite(v))
			return 0;
		out[k] = (float)v;
	}
	return 1;
}

static int only_keys(const AeronConfigNode* map, const char* what, const char* const* keys, size_t count) {
	if (AeronConfigNode_Type(map) != AERON_CONFIG_MAP) {
		fprintf(stderr, "%s must be a mapping\n", what);
		return 0;
	}
	for (size_t i = 0; i < AeronConfigNode_MapCount(map); ++i) {
		const char* key = AeronConfigNode_MapKeyAt(map, i);
		size_t      k;
		for (k = 0; k < count && strcmp(key, keys[k]); ++k) {
		}
		if (k == count) {
			fprintf(stderr, "Unknown %s setting: %s\n", what, key);
			return 0;
		}
	}
	return 1;
}

static int game_bool(const AeronConfigNode* map, const char* key, int fallback, int* out) {
	const AeronConfigNode* node = AeronConfigNode_MapGet(map, key);
	if (node && AeronConfigNode_Type(node) != AERON_CONFIG_BOOL) {
		fprintf(stderr, "game_scene.%s must be true or false\n", key);
		return 0;
	}
	*out = node ? AeronConfigNode_Bool(node, fallback) : fallback;
	return 1;
}

static int load_game_instances(const AeronConfigNode* list, CaptureConfig* config) {
	CaptureGameScene* game  = &config->game;
	const size_t      count = AeronConfigNode_SequenceCount(list);
	if (AeronConfigNode_Type(list) != AERON_CONFIG_SEQUENCE || count == 0 || count > CAPTURE_MAX_INSTANCES) {
		fprintf(stderr, "game_scene.instances must list 1-%d instances\n", CAPTURE_MAX_INSTANCES);
		return 0;
	}
	static const char* const keys[] = { "model", "role", "position", "rotation_degrees" };
	for (size_t i = 0; i < count; ++i) {
		const AeronConfigNode* item = AeronConfigNode_SequenceGet(list, i);
		CaptureInstance*       out  = &game->instances[i];
		if (!only_keys(item, "game_scene.instances item", keys, sizeof keys / sizeof keys[0]))
			return 0;
		const char* model = AeronConfigNode_String(AeronConfigNode_MapGet(item, "model"), NULL);
		out->model        = -1;
		for (int m = 0; model && m < config->model_count; ++m)
			if (!strcmp(model, config->models[m]))
				out->model = m;
		if (out->model < 0) {
			fprintf(stderr, "game_scene.instances[%zu].model must be one of models\n", i);
			return 0;
		}
		const AeronConfigNode* role_node = AeronConfigNode_MapGet(item, "role");
		const char*            role      = role_node ? AeronConfigNode_String(role_node, NULL) : "ship";
		if (!role || (strcmp(role, "ship") && strcmp(role, "cockpit") && strcmp(role, "exterior"))) {
			fprintf(stderr, "game_scene.instances[%zu].role must be ship, cockpit or exterior\n", i);
			return 0;
		}
		out->role = !strcmp(role, "cockpit")	 ? CAPTURE_ROLE_COCKPIT
					: !strcmp(role, "exterior") ? CAPTURE_ROLE_EXTERIOR
												: CAPTURE_ROLE_SHIP;
		if (out->role != CAPTURE_ROLE_SHIP && !config->screen_test) {
			fprintf(stderr, "game_scene cockpit and exterior instances need screen_test's eye\n");
			return 0;
		}
		const AeronConfigNode* position = AeronConfigNode_MapGet(item, "position");
		const AeronConfigNode* rotation = AeronConfigNode_MapGet(item, "rotation_degrees");
		if ((out->role == CAPTURE_ROLE_SHIP && !vec3(position, out->position)) ||
			(rotation && !vec3(rotation, out->rotation))) {
			fprintf(stderr, "game_scene.instances[%zu]: a ship needs position: [x, y, z]; "
							"rotation_degrees is [yaw, pitch, roll]\n", i);
			return 0;
		}
	}
	game->instance_count = (int)count;
	return 1;
}

static int load_game_lights(const AeronConfigNode* list, CaptureGameScene* game) {
	const size_t count = AeronConfigNode_SequenceCount(list);
	if (AeronConfigNode_Type(list) != AERON_CONFIG_SEQUENCE || count > CAPTURE_MAX_LIGHTS) {
		fprintf(stderr, "game_scene.point_lights must list at most %d lights\n", CAPTURE_MAX_LIGHTS);
		return 0;
	}
	static const char* const keys[] = { "position", "radius", "color" };
	for (size_t i = 0; i < count; ++i) {
		const AeronConfigNode* item   = AeronConfigNode_SequenceGet(list, i);
		CaptureLight*          out    = &game->lights[i];
		const double           radius = AeronConfigNode_Float(AeronConfigNode_MapGet(item, "radius"), NAN);
		if (!only_keys(item, "game_scene.point_lights item", keys, sizeof keys / sizeof keys[0]) ||
			!vec3(AeronConfigNode_MapGet(item, "position"), out->position) ||
			!vec3(AeronConfigNode_MapGet(item, "color"), out->color) || !isfinite(radius) || radius <= 0) {
			fprintf(stderr, "game_scene.point_lights[%zu] needs position, radius > 0 and color\n", i);
			return 0;
		}
		out->radius = (float)radius;
	}
	game->light_count = (int)count;
	return 1;
}

static int load_game_scene(AeronVfs* vfs, const AeronConfigFile* yaml, CaptureConfig* config) {
	const AeronConfigNode* node = AeronConfigFile_GetNode(yaml, "game_scene");
	if (!node)
		return 1;
	static const char* const keys[] = { "defaults", "ssao",    "shadows",      "sun_direction",
										"sun_color", "ambient", "point_lights", "instances",
										"cockpit_follows_camera", "model_light_intensity",
										"temporal_upscaling" };
	CaptureGameScene*        game   = &config->game;
	if (!only_keys(node, "game_scene", keys, sizeof keys / sizeof keys[0]))
		return 0;
	if (!config->model_count || config->bloom_chart) {
		fprintf(stderr, "game_scene needs models and the shapes fixture\n");
		return 0;
	}

	/* The game's Scene3D defaults: SSAO, shadows (tonemap stays the capture's). */
	const char*               path  = AeronConfigNode_String(AeronConfigNode_MapGet(node, "defaults"), NULL);
	AeronConfigFile*          file  = NULL;
	AeronConfigError          error = { 0 };
	AeronSceneTonemapSettings tonemap;
	if (!path || !AeronConfigFile_LoadYamlEx(vfs, AERON_VFS_ROOT_ASSET, path, &file, &error) ||
		!AeronSceneSettings_Load(AeronConfigFile_Root(file), &game->ssao, &game->shadows, &tonemap, &error)) {
		fprintf(stderr, "game_scene.defaults: cannot load Scene3D defaults '%s': %s\n", path ? path : "",
				error.message);
		AeronConfigFile_Destroy(file);
		return 0;
	}
	AeronConfigFile_Destroy(file);
	int ssao, shadows;
	if (!game_bool(node, "cockpit_follows_camera", 0, &game->cockpit_follows_camera))
		return 0;
	const AeronConfigNode* temporal = AeronConfigNode_MapGet(node, "temporal_upscaling");
	if (temporal) {
		static const char* const temporal_keys[] = { "mode", "sharpness" };
		const char*            mode = AeronConfigNode_String(AeronConfigNode_MapGet(temporal, "mode"), NULL);
		const AeronConfigNode* sharpness       = AeronConfigNode_MapGet(temporal, "sharpness");
		const double           sharpness_value = sharpness ? AeronConfigNode_Float(sharpness, NAN) : 0.0;
		if (AeronConfigNode_Type(temporal) != AERON_CONFIG_MAP ||
			!only_keys(temporal, "game_scene.temporal_upscaling", temporal_keys,
					   sizeof temporal_keys / sizeof temporal_keys[0]) ||
			!AeronTemporal_ParseMode(mode, &game->temporal_mode) || !isfinite(sharpness_value) ||
			sharpness_value < 0 || sharpness_value > 1) {
			fprintf(stderr, "game_scene.temporal_upscaling needs mode (off, native_aa, quality, balanced or "
							"performance) and optional sharpness 0..1\n");
			return 0;
		}
		game->temporal_sharpness = (float)sharpness_value;
		if (game->temporal_mode != AERON_TEMPORAL_OFF && config->msaa_samples != 1) {
			fprintf(stderr, "game_scene.temporal_upscaling needs msaa_samples: 1, as in the game\n");
			return 0;
		}
	}
	game->model_light_intensity                  = 1;
	const AeronConfigNode* model_light_intensity = AeronConfigNode_MapGet(node, "model_light_intensity");
	if (model_light_intensity) {
		const double value = AeronConfigNode_Float(model_light_intensity, NAN);
		if (!isfinite(value) || value < 0 || value > 64) {
			fprintf(stderr, "game_scene.model_light_intensity must be 0..64\n");
			return 0;
		}
		game->model_light_intensity = (float)value;
	}
	if (!game_bool(node, "ssao", 1, &ssao) || !game_bool(node, "shadows", 1, &shadows))
		return 0;
	if (!ssao)
		game->ssao.ssao_quality = 0;
	game->shadows.enabled = game->shadows.enabled && shadows;

	/* Lighting as resources/remaster/config.yaml's `lighting` defaults. */
	const float default_direction[3] = { -.5f, -.6f, .62f };
	memcpy(game->sun_direction, default_direction, sizeof default_direction);
	game->sun_color[0] = game->sun_color[1] = game->sun_color[2] = 1;
	game->ambient                                            = .08f;
	const AeronConfigNode* direction = AeronConfigNode_MapGet(node, "sun_direction");
	const AeronConfigNode* color     = AeronConfigNode_MapGet(node, "sun_color");
	const AeronConfigNode* ambient   = AeronConfigNode_MapGet(node, "ambient");
	if ((direction && !vec3(direction, game->sun_direction)) || (color && !vec3(color, game->sun_color))) {
		fprintf(stderr, "game_scene.sun_direction and sun_color must be [x, y, z]\n");
		return 0;
	}
	const float norm = sqrtf(game->sun_direction[0] * game->sun_direction[0] +
							 game->sun_direction[1] * game->sun_direction[1] +
							 game->sun_direction[2] * game->sun_direction[2]);
	if (!(norm > 1e-6f)) {
		fprintf(stderr, "game_scene.sun_direction must be non-zero\n");
		return 0;
	}
	for (int k = 0; k < 3; ++k)
		game->sun_direction[k] /= norm;
	if (ambient) {
		const double value = AeronConfigNode_Float(ambient, NAN);
		if (!isfinite(value) || value < 0 || value > 4) {
			fprintf(stderr, "game_scene.ambient must be 0..4\n");
			return 0;
		}
		game->ambient = (float)value;
	}
	const AeronConfigNode* lights = AeronConfigNode_MapGet(node, "point_lights");
	if ((lights && !load_game_lights(lights, game)) ||
		!load_game_instances(AeronConfigNode_MapGet(node, "instances"), config))
		return 0;
	game->enabled = 1;
	return 1;
}

static int load_benchmark(const AeronConfigFile* yaml, CaptureBenchmarkConfig* config) {
	*config = (CaptureBenchmarkConfig) {
		.warmup_seconds = 1, .sample_seconds = 1, .samples = 7, .min_frames = 120, .frames_in_flight = 2
	};
	const AeronConfigNode* node = AeronConfigFile_GetNode(yaml, "benchmark");
	if (!node)
		return 1;
	if (AeronConfigNode_Type(node) != AERON_CONFIG_MAP) {
		fprintf(stderr, "benchmark must be a mapping\n");
		return 0;
	}
	for (size_t i = 0; i < AeronConfigNode_MapCount(node); ++i) {
		const char* key = AeronConfigNode_MapKeyAt(node, i);
		if (strcmp(key, "warmup_seconds") && strcmp(key, "sample_seconds") && strcmp(key, "samples") &&
			strcmp(key, "min_frames") && strcmp(key, "frames_in_flight")) {
			fprintf(stderr, "Unknown benchmark setting: %s\n", key);
			return 0;
		}
	}
	return (!AeronConfigFile_GetNode(yaml, "benchmark.warmup_seconds") ||
			number(yaml, "benchmark.warmup_seconds", .1, 60, &config->warmup_seconds)) &&
		   (!AeronConfigFile_GetNode(yaml, "benchmark.sample_seconds") ||
			number(yaml, "benchmark.sample_seconds", .1, 60, &config->sample_seconds)) &&
		   (!AeronConfigFile_GetNode(yaml, "benchmark.samples") ||
			integer(yaml, "benchmark.samples", 3, 31, &config->samples)) &&
		   (!AeronConfigFile_GetNode(yaml, "benchmark.min_frames") ||
			integer(yaml, "benchmark.min_frames", 1, 100000, &config->min_frames)) &&
		   (!AeronConfigFile_GetNode(yaml, "benchmark.frames_in_flight") ||
			integer(yaml, "benchmark.frames_in_flight", 1, 4, &config->frames_in_flight));
}

int CaptureConfig_Load(const char* path, CaptureConfig* config) {
	AeronVfs* vfs = AeronVfs_Create(
		&(AeronVfsConfig) { .asset_root = ".", .resource_root = ".", .user_root = ".", .temp_root = "." });
	AeronConfigFile* yaml  = NULL;
	AeronConfigError error = { 0 };
	if (!vfs || !AeronConfigFile_LoadYamlEx(vfs, AERON_VFS_ROOT_ASSET, path, &yaml, &error)) {
		fprintf(stderr, "Cannot load capture YAML: %s\n", error.message);
		AeronVfs_Destroy(vfs);
		return 0;
	}
	const char*            keys[] = { "width",
									  "height",
									  "frames",
									  "reference_samples",
									  "quality",
									  "fps",
									  "pan_degrees_per_second",
									  "pan_direction_degrees",
									  "shutter",
									  "output_dir",
									  "asset_root",
									  "models",
									  "fixture",
									  "msaa_samples",
									  "tonemap",
									  "cull",
									  "backface_fill",
									  "screen_test",
									  "bloom",
									  "emissive_scale",
									  "benchmark",
									  "game_scene" };
	const AeronConfigNode* root   = AeronConfigFile_Root(yaml);
	int                    ok     = AeronConfigNode_Type(root) == AERON_CONFIG_MAP;
	for (size_t i = 0; ok && i < AeronConfigNode_MapCount(root); ++i) {
		const char* key = AeronConfigNode_MapKeyAt(root, i);
		size_t      k;
		for (k = 0; k < sizeof keys / sizeof keys[0]; ++k)
			if (!strcmp(key, keys[k]))
				break;
		if (k == sizeof keys / sizeof keys[0]) {
			fprintf(stderr, "Unknown capture setting: %s\n", key);
			ok = 0;
		}
	}
	ok = ok && integer(yaml, "width", 64, 8192, &config->width) &&
		 integer(yaml, "height", 64, 8192, &config->height) &&
		 integer(yaml, "frames", 1, 120, &config->frames) &&
		 integer(yaml, "reference_samples", 0, 256, &config->reference_samples) &&
		 integer(yaml, "quality", 0, 2, &config->quality) && number(yaml, "fps", 1, 1000, &config->fps) &&
		 number(yaml, "pan_degrees_per_second", -720, 720, &config->pan_speed) &&
		 number(yaml, "pan_direction_degrees", -360, 360, &config->pan_direction) &&
		 number(yaml, "shutter", 0, 1, &config->shutter);
	const char* output = AeronConfigFile_GetString(yaml, "output_dir", "");
	if (!copy_path(config->output_dir, sizeof config->output_dir, output)) {
		fprintf(stderr, "Invalid output_dir\n");
		ok = 0;
	}
	if (ok)
		ok = load_models(yaml, config) && load_effects(yaml, config) &&
			 load_benchmark(yaml, &config->benchmark) && load_game_scene(vfs, yaml, config);
	AeronConfigFile_Destroy(yaml);
	AeronVfs_Destroy(vfs);
	return ok;
}
