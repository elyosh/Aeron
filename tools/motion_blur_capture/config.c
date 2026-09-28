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
									  "output_dir" };
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
	if (!output[0] || strlen(output) >= sizeof config->output_dir || strchr(output, '\n') ||
		strchr(output, '\r')) {
		fprintf(stderr, "Invalid output_dir\n");
		ok = 0;
	}
	if (ok)
		memcpy(config->output_dir, output, strlen(output) + 1);
	AeronConfigFile_Destroy(yaml);
	AeronVfs_Destroy(vfs);
	return ok;
}
