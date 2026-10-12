/*
 * aeron_gltf_cook — CLI entry point.
 *
 * Usage: aeron_gltf_cook <in.gltf> --out <out.glb> [options]
 *        aeron_gltf_cook --library <library>.yaml --out <effects dir>
 *        aeron_gltf_cook --animations-json <model>.anim.yaml
 *        aeron_gltf_cook --library-json <library>.yaml
 *
 * Cooks an artist-authored .gltf into a runtime-ready .glb with
 * KHR_texture_basisu / KHR_texture_transform per-channel atlases. An
 * AERON_flight_model asset also gets cooked simulation and render data, and
 * the result is loaded back the way a game loads it before it is written.
 * --library builds an effect library (tools/effect_pack/README.md).
 */

#include "animations.h"
#include "effect_pack.h"
#include "flight_data.h"
#include "gltf_cook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static void usage(void) {
	fprintf(stderr, "Usage: aeron_gltf_cook <in.gltf> --out <out.glb> [options]\n"
					"       aeron_gltf_cook --library <library>.yaml --out <effects dir>\n"
					"       aeron_gltf_cook --animations-json <model>.anim.yaml\n"
					"       aeron_gltf_cook --library-json <library>.yaml\n"
					"\n"
					"Cooks an artist-authored .gltf (PNG textures, optional\n"
					"KHR_materials_variants) into one .glb file with four compressed\n"
					"KTX2 atlas arrays (base_color / normal / metallic_roughness / emissive)\n"
					"referenced via KHR_texture_basisu. Each material's texture\n"
					"bindings carry a KHR_texture_transform mapping its UVs into the\n"
					"appropriate sub-rect of each atlas.\n"
					"\n"
					"For an AERON_flight_model asset it also adds cooked simulation data\n"
					"(per-component collision BVH) and render data (render BVH), then\n"
					"loads the result as a game would and fails if it does not load.\n"
					"\n"
					"Options:\n"
					"  --out <path>          Output .glb path (required).\n"
					"  --atlas-page <N>      Atlas page size per axis, a power of 2 >= 128\n"
					"                        (default 2048). Raised to fit the largest\n"
					"                        source; sources over half a page get their\n"
					"                        own page.\n"
					"  --max-texture <N>     Downscale larger sources to N (default 4096).\n"
					"  --mip-min-subrect <N> Gutter texels around each sub-rect, a power\n"
					"                        of 2 in [4, 32]; also caps mip depth\n"
					"                        (default 16).\n"
					"  --quality {fast|med|uber}\n"
					"                        BC7 encoder preset (default med).\n"
					"  --no-zstd             Skip zstd supercompression on KTX2 payloads.\n"
					"  --zstd-level <N>      zstd level, 1 to 22 (default 9; 19 is about\n"
					"                        14x slower for ~6%% smaller payloads).\n"
					"  --animations <file>   Merge a model animation file (<model>.anim.yaml:\n"
					"                        materials' flipbooks, damage and energy)\n"
					"                        into the materials first.\n"
					"  --effects-dir <dir>   With --animations, check every image key\n"
					"                        against the libraries in <dir>.\n"
					"  --library-sources <dir>\n"
					"                        With --effects-dir, first build each library\n"
					"                        the animations name that <dir> holds, as\n"
					"                        <dir>/<library>/<library>.yaml, into the\n"
					"                        effects directory (as --library does).\n"
					"  --verbose             Print per-channel cook progress.\n"
					"  -h, --help            Show this help.\n"
					"\n"
					"--library builds an effect library file: its images, baked by kind,\n"
					"into page arrays and an index in <effects dir>. A library whose\n"
					"index there records the same inputs (the file, its images and the\n"
					"baking version) is left as it is.\n"
					"\n"
					"--animations-json checks a model animation file as --animations\n"
					"does and prints it as JSON (for tools without a YAML reader).\n"
					"--library-json does the same for an effect library file, with the\n"
					"checks --library applies before reading its images.\n");
	exit(2);
}

#define LIBRARY_TAG "[aeron_gltf_cook] library"

/* Prints effect library file `file` as JSON on stdout, once checked. */
static bool print_library(const char* file) {
	cJSON* library = EffectPack_ReadLibrary(LIBRARY_TAG, file);
	char*  printed = library ? cJSON_PrintUnformatted(library) : NULL;
	const bool ok  = printed && fputs(printed, stdout) >= 0 && fputc('\n', stdout) != EOF && fflush(stdout) == 0;
	free(printed);
	cJSON_Delete(library);
	return ok;
}

/* Rewrites the cooked GLB with cooked AERON_flight_model data. */
static bool add_flight_data(const char* path) {
	FILE* file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "[aeron_gltf_cook] cannot reopen %s\n", path);
		return false;
	}
	fseek(file, 0, SEEK_END);
	const long length = ftell(file);
	rewind(file);
	uint8_t* bytes = length > 0 ? malloc((size_t)length) : NULL;
	const bool read = bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length;
	fclose(file);
	if (!read) {
		free(bytes);
		fprintf(stderr, "[aeron_gltf_cook] cannot read %s\n", path);
		return false;
	}
	uint8_t* cooked      = NULL;
	size_t   cooked_size = 0;
	bool     applied     = false;
	char     error[256]  = { 0 };
	const bool ok = aeron_gltf_cook_flight_data(bytes, (size_t)length, &cooked, &cooked_size, &applied, error,
												sizeof error);
	free(bytes);
	if (!ok) {
		fprintf(stderr, "[aeron_gltf_cook] flight data: %s\n", error);
		remove(path);
		return false;
	}
	if (!applied)
		return true;
	file = fopen(path, "wb");
	const bool written = file && fwrite(cooked, 1, cooked_size, file) == cooked_size;
	if (file && fclose(file) != 0)
		return false;
	free(cooked);
	if (!written)
		fprintf(stderr, "[aeron_gltf_cook] cannot write %s\n", path);
	return written;
}

int main(int argc, char** argv) {
	if (argc < 2)
		usage();
	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
		usage();
	if (strcmp(argv[1], "--animations-json") == 0) {
		if (argc != 3)
			usage();
		return aeron_gltf_cook_print_animations(argv[2]) ? 0 : 1;
	}
	if (strcmp(argv[1], "--library") == 0) {
		if (argc != 5 || strcmp(argv[3], "--out") != 0)
			usage();
		return EffectPack_BuildLibrary(LIBRARY_TAG, argv[2], NULL, argv[4]) != EFFECT_PACK_FAILED ? 0 : 1;
	}
	if (strcmp(argv[1], "--library-json") == 0) {
		if (argc != 3)
			usage();
		return print_library(argv[2]) ? 0 : 1;
	}

	const char* in_path     = argv[1];
	const char* out_path    = NULL;
	const char* animations  = NULL;
	const char* effects_dir = NULL;
	const char* library_sources = NULL;

	AeronGltfCookOptions opts;
	aeron_gltf_cook_default_options(&opts);

	for (int i = 2; i < argc; i++) {
		const char* a = argv[i];
		if (strcmp(a, "--out") == 0 && i + 1 < argc) {
			out_path = argv[++i];
		} else if (strcmp(a, "--atlas-page") == 0 && i + 1 < argc) {
			opts.atlas_page_size = atoi(argv[++i]);
		} else if (strcmp(a, "--max-texture") == 0 && i + 1 < argc) {
			opts.max_texture_size = atoi(argv[++i]);
		} else if (strcmp(a, "--mip-min-subrect") == 0 && i + 1 < argc) {
			opts.mip_min_subrect = atoi(argv[++i]);
		} else if (strcmp(a, "--quality") == 0 && i + 1 < argc) {
			const char* q = argv[++i];
			if (strcmp(q, "fast") == 0)
				opts.bc7_quality = 0;
			else if (strcmp(q, "med") == 0)
				opts.bc7_quality = 1;
			else if (strcmp(q, "uber") == 0)
				opts.bc7_quality = 2;
			else {
				fprintf(stderr, "[aeron_gltf_cook] unknown quality '%s'\n", q);
				return 2;
			}
		} else if (strcmp(a, "--no-zstd") == 0) {
			opts.zstd_supercompress = false;
		} else if (strcmp(a, "--zstd-level") == 0 && i + 1 < argc) {
			opts.zstd_level = atoi(argv[++i]);
		} else if (strcmp(a, "--animations") == 0 && i + 1 < argc) {
			animations = argv[++i];
		} else if (strcmp(a, "--effects-dir") == 0 && i + 1 < argc) {
			effects_dir = argv[++i];
		} else if (strcmp(a, "--library-sources") == 0 && i + 1 < argc) {
			library_sources = argv[++i];
		} else if (strcmp(a, "--verbose") == 0) {
			opts.verbose = true;
		} else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			usage();
		} else {
			fprintf(stderr, "[aeron_gltf_cook] unknown arg '%s'\n", a);
			usage();
		}
	}

	if (!out_path) {
		fprintf(stderr, "[aeron_gltf_cook] --out is required\n");
		usage();
	}
	if (opts.atlas_page_size < 128 || (opts.atlas_page_size & (opts.atlas_page_size - 1))) {
		fprintf(stderr, "[aeron_gltf_cook] --atlas-page must be a power of 2 ≥ 128 (got %d)\n",
				opts.atlas_page_size);
		return 2;
	}
	if (opts.max_texture_size < 1) {
		fprintf(stderr, "[aeron_gltf_cook] --max-texture must be ≥ 1\n");
		return 2;
	}
	if (opts.mip_min_subrect < 4 || opts.mip_min_subrect > 32 ||
		(opts.mip_min_subrect & (opts.mip_min_subrect - 1))) {
		fprintf(stderr, "[aeron_gltf_cook] --mip-min-subrect must be a power of 2 in [4, 32]\n");
		return 2;
	}
	if (opts.atlas_page_size < 8 * opts.mip_min_subrect) {
		fprintf(stderr, "[aeron_gltf_cook] --atlas-page must be at least 8 × --mip-min-subrect\n");
		return 2;
	}

	if (effects_dir && !animations) {
		fprintf(stderr, "[aeron_gltf_cook] --effects-dir needs --animations\n");
		return 2;
	}
	if (library_sources && !effects_dir) {
		fprintf(stderr, "[aeron_gltf_cook] --library-sources needs --effects-dir\n");
		return 2;
	}

	/* The animation file merged into a copy of the input, next to it so its
	 * buffer and image URIs still resolve. */
	char merged[1024];
	if (animations) {
		snprintf(merged, sizeof merged, "%s.animations.gltf", in_path);
		if (!aeron_gltf_cook_merge_animations(in_path, animations, effects_dir, library_sources, merged)) {
			remove(merged);
			return 1;
		}
		in_path = merged;
	}
	const bool cooked = aeron_gltf_cook(in_path, out_path, &opts);
	if (animations)
		remove(merged);
	if (!cooked)
		return 1;
	return add_flight_data(out_path) ? 0 : 1;
}
