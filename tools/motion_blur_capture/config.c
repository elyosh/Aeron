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
									  "models" };
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
		ok = load_models(yaml, config);
	AeronConfigFile_Destroy(yaml);
	AeronVfs_Destroy(vfs);
	return ok;
}
