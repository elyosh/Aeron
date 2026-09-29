#include "capture.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int vector(const AeronConfigFile* file, const char* key, double* values, size_t count) {
	const AeronConfigNode* node = AeronConfigFile_GetNode(file, key);
	if (AeronConfigNode_Type(node) != AERON_CONFIG_SEQUENCE || AeronConfigNode_SequenceCount(node) != count)
		return 0;
	for (size_t i = 0; i < count; ++i) {
		values[i] = AeronConfigNode_Float(AeronConfigNode_SequenceGet(node, i), NAN);
		if (!isfinite(values[i]) || values[i] < 0)
			return 0;
	}
	return 1;
}

static int region_values(const AeronConfigFile* file, const char* region, double sigma[2], double energy[3]) {
	char key[256];
	snprintf(key, sizeof key, "regions.%s.bloom_sigma_over_height", region);
	if (!vector(file, key, sigma, 2))
		return 0;
	snprintf(key, sizeof key, "regions.%s.bloom_rgb_integral", region);
	return vector(file, key, energy, 3);
}

static void ratio(double baseline, double candidate) {
	if (baseline > 0)
		printf("%.9g", candidate / baseline);
	else
		printf("null");
}

static int compare(const AeronConfigFile* a, const AeronConfigFile* b) {
	const char*            expected = "raw_bloom_before_present_kernel_and_intensity";
	double                 sizes[2][2], grids[2][2];
	const AeronConfigNode* regions = AeronConfigFile_GetNode(a, "regions");
	const AeronConfigNode* other   = AeronConfigFile_GetNode(b, "regions");
	if (strcmp(AeronConfigFile_GetString(a, "measurement", ""), expected) ||
		strcmp(AeronConfigFile_GetString(b, "measurement", ""), expected) ||
		!vector(a, "scene_size", sizes[0], 2) || !vector(b, "scene_size", sizes[1], 2) ||
		!vector(a, "chart_grid", grids[0], 2) || !vector(b, "chart_grid", grids[1], 2) ||
		memcmp(grids[0], grids[1], sizeof grids[0]) || AeronConfigNode_Type(regions) != AERON_CONFIG_MAP ||
		AeronConfigNode_Type(other) != AERON_CONFIG_MAP ||
		AeronConfigNode_MapCount(regions) != AeronConfigNode_MapCount(other))
		return 0;
	/* Validate the complete report before writing any output. */
	for (size_t i = 0; i < AeronConfigNode_MapCount(regions); ++i) {
		const char* key = AeronConfigNode_MapKeyAt(regions, i);
		if (!key[0] || strlen(key) > 128)
			return 0;
		for (const char* p = key; *p; ++p)
			if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
				return 0;
		double sigma[2], energy[3];
		if (!region_values(a, key, sigma, energy) || !region_values(b, key, sigma, energy))
			return 0;
	}
	printf("baseline_scene_size: [%.0f, %.0f]\ncandidate_scene_size: [%.0f, %.0f]\n"
		   "ratio_convention: candidate_divided_by_baseline\n"
		   "note: Compare matching fixtures, camera poses and settings; this is not a ground-truth score.\n"
		   "regions:\n",
		   sizes[0][0], sizes[0][1], sizes[1][0], sizes[1][1]);
	for (size_t i = 0; i < AeronConfigNode_MapCount(regions); ++i) {
		const char* key = AeronConfigNode_MapKeyAt(regions, i);
		double      sigma[2][2], energy[2][3];
		region_values(a, key, sigma[0], energy[0]);
		region_values(b, key, sigma[1], energy[1]);
		printf("  %s:\n    normalized_spread_ratio: [", key);
		ratio(sigma[0][0], sigma[1][0]);
		printf(", ");
		ratio(sigma[0][1], sigma[1][1]);
		printf("]\n    bloom_energy_ratio: ");
		ratio(energy[0][0] + energy[0][1] + energy[0][2], energy[1][0] + energy[1][1] + energy[1][2]);
		printf("\n    bloom_rgb_integral_delta: [%.9g, %.9g, %.9g]\n", energy[1][0] - energy[0][0],
			   energy[1][1] - energy[0][1], energy[1][2] - energy[0][2]);
	}
	return !ferror(stdout);
}

int Capture_Compare(const char* baseline, const char* candidate) {
	AeronVfs*        vfs = AeronVfs_Create(&(AeronVfsConfig) { .asset_root = "." });
	AeronConfigFile *a = NULL, *b = NULL;
	AeronConfigError error = { 0 };
	int ok = vfs && AeronConfigFile_LoadYamlEx(vfs, AERON_VFS_ROOT_ASSET, baseline, &a, &error) &&
			 AeronConfigFile_LoadYamlEx(vfs, AERON_VFS_ROOT_ASSET, candidate, &b, &error);
	if (ok)
		ok = compare(a, b);
	if (!ok)
		fprintf(stderr, "Cannot compare bloom metrics (missing, invalid or incompatible reports): %s\n",
				error.message);
	AeronConfigFile_Destroy(a);
	AeronConfigFile_Destroy(b);
	AeronVfs_Destroy(vfs);
	return ok;
}
