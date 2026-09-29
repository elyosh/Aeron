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
		fprintf(stderr, "models must list 1-%d OPT paths relative to asset_root\n", CAPTURE_MAX_MODELS);
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
									  "bloom",
									  "emissive_scale",
									  "benchmark" };
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
			 load_benchmark(yaml, &config->benchmark);
	AeronConfigFile_Destroy(yaml);
	AeronVfs_Destroy(vfs);
	return ok;
}
