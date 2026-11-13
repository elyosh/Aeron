/* aeron_lod_report: runs the level generator (lod_generate.h) on cooked
 * flight models and reports each level's triangles, error, dropped
 * triangles and time, to tune the generator.
 *
 *   aeron_lod_report [options] MODEL.glb...
 *
 * Options change the generator's defaults:
 *   --errors A,B,C     target errors, as fractions of the model's radius
 *   --min-gain G       share of triangles a level must remove (0 to 1)
 *   --no-permissive    don't collapse across UV and normal seams
 *   --free-borders     don't lock the components' open borders
 *   --no-prune         don't let the simplifier remove small disconnected parts
 *   --coarse-from N    the first step of the error ladder (1 to 3) that crosses
 *                      material borders and frees component borders; 4 for none
 *   --attribute-weight W  how much a change of appearance counts as error on
 *                      coarse levels, as a fraction of the radius; 0 for none
 */
#include "lod_generate.h"

#include "aeron/asset/flight_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void* read_file(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	const long length = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* data = length > 0 ? malloc((size_t)length) : NULL;
	if (data && fread(data, 1, (size_t)length, f) != (size_t)length) {
		free(data);
		data = NULL;
	}
	fclose(f);
	*size = data ? (size_t)length : 0;
	return data;
}

static const char* base_name(const char* path) {
	const char* slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static int usage(void) {
	fprintf(stderr,
			"usage: aeron_lod_report [--errors A,B,C] [--min-gain G] [--no-permissive] [--free-borders] "
			"[--no-prune] [--coarse-from N] [--attribute-weight W] MODEL.glb...\n");
	return 2;
}

int main(int argc, char** argv) {
	AeronLodGenerateOptions options = aeron_lod_default_options();
	int                     first   = 1;
	for (; first < argc && strncmp(argv[first], "--", 2) == 0; first++) {
		if (!strcmp(argv[first], "--errors") && first + 1 < argc) {
			options.level_count = sscanf(argv[++first], "%f,%f,%f", &options.relative_errors[0],
										 &options.relative_errors[1], &options.relative_errors[2]);
			if (options.level_count < 1)
				return usage();
		} else if (!strcmp(argv[first], "--min-gain") && first + 1 < argc) {
			options.min_gain = (float)atof(argv[++first]);
		} else if (!strcmp(argv[first], "--no-permissive")) {
			options.permissive = false;
		} else if (!strcmp(argv[first], "--free-borders")) {
			options.lock_component_borders = false;
		} else if (!strcmp(argv[first], "--no-prune")) {
			options.prune = false;
		} else if (!strcmp(argv[first], "--attribute-weight") && first + 1 < argc) {
			options.attribute_weight = (float)atof(argv[++first]);
		} else if (!strcmp(argv[first], "--coarse-from") && first + 1 < argc) {
			options.coarse_from_level = atoi(argv[++first]) - 1;
		} else {
			return usage();
		}
	}
	if (first >= argc)
		return usage();

	printf("%-34s %9s  %s\n", "model", "level 0",
		   "levels: triangles (share of level 0), error in m and % of radius");
	int failures = 0;
	for (int i = first; i < argc; i++) {
		size_t           size = 0;
		void*            data = read_file(argv[i], &size);
		AeronFlightModel model;
		memset(&model, 0, sizeof model);
		if (!data || !Aeron_FlightModelBuildMemory(data, size, argv[i], &model)) {
			fprintf(stderr, "%s: can't load\n", argv[i]);
			free(data);
			failures++;
			continue;
		}
		free(data);
		const AeronGltfModel* render = &model.render;
		AeronLodLevel         levels[AERON_LOD_MAX_GENERATED];
		const clock_t         start = clock();
		const int             count = aeron_lod_generate(render, &options, levels);
		const double          ms    = 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC;
		if (count < 0) {
			fprintf(stderr, "%s: out of memory\n", argv[i]);
			Aeron_FlightModelFree(&model);
			failures++;
			continue;
		}
		float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
		for (uint32_t v = 0; v < render->vertex_count; v++)
			for (int a = 0; a < 3; a++) {
				lo[a] = render->vertices[v].pos[a] < lo[a] ? render->vertices[v].pos[a] : lo[a];
				hi[a] = render->vertices[v].pos[a] > hi[a] ? render->vertices[v].pos[a] : hi[a];
			}
		float radius = 0.0f;
		for (int a = 0; a < 3; a++)
			radius += (hi[a] - lo[a]) * (hi[a] - lo[a]);
		radius = 0.5f * sqrtf(radius);

		const uint32_t full = render->index_count / 3;
		printf("%-34s %9u ", base_name(argv[i]), full);
		for (int k = 0; k < count; k++) {
			printf(" %6u (%4.1f%%) %.3f m %.2f%%", levels[k].index_count / 3,
				   100.0 * levels[k].index_count / 3 / (full ? full : 1), levels[k].error,
				   radius > 0 ? 100.0 * levels[k].error / radius : 0.0);
			if (levels[k].dropped_triangles)
				printf(" [%u dropped]", levels[k].dropped_triangles);
			if (levels[k].vertex_count)
				printf(" [+%u vertices]", levels[k].vertex_count);
			printf(k + 1 < count ? " |" : "");
			aeron_lod_level_free(&levels[k]);
		}
		printf("%s  (%.0f ms)\n", count ? "" : "  no levels", ms);
		Aeron_FlightModelFree(&model);
	}
	return failures ? 1 : 0;
}
