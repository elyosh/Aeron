/*
 * aeron_gltf_cook — CLI entry point.
 *
 * Usage: aeron_gltf_cook <in.gltf> --out <out.glb> [options]
 *
 * Cooks an artist-authored .gltf into a runtime-ready .glb with
 * KHR_texture_basisu / KHR_texture_transform per-channel atlases.
 */

#include "gltf_cook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
	fprintf(stderr, "Usage: aeron_gltf_cook <in.gltf> --out <out.glb> [options]\n"
					"\n"
					"Cooks an artist-authored .gltf (PNG textures, optional\n"
					"KHR_materials_variants, arbitrary extras) into one .glb file with\n"
					"four compressed KTX2 atlases (base_color / normal / metallic_roughness /\n"
					"emissive) referenced via KHR_texture_basisu. Each material's\n"
					"texture bindings carry a KHR_texture_transform mapping its UVs\n"
					"into the appropriate sub-rect of each atlas.\n"
					"\n"
					"Options:\n"
					"  --out <path>          Output .glb path (required).\n"
					"  --max-atlas <N>       Max atlas dimension per axis "
					"(default 2048).\n"
					"  --mip-min-subrect <N> Gutter texels around each sub-rect; also\n"
					"                        caps mip depth (default 4).\n"
					"  --quality {fast|med|uber}\n"
					"                        BC7 encoder preset (default med).\n"
					"  --no-zstd             Skip zstd supercompression on KTX2 payloads.\n"
					"  --verbose             Print per-channel cook progress.\n"
					"  -h, --help            Show this help.\n");
	exit(2);
}

int main(int argc, char** argv) {
	if (argc < 2)
		usage();
	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
		usage();

	const char* in_path  = argv[1];
	const char* out_path = NULL;

	AeronGltfCookOptions opts;
	aeron_gltf_cook_default_options(&opts);

	for (int i = 2; i < argc; i++) {
		const char* a = argv[i];
		if (strcmp(a, "--out") == 0 && i + 1 < argc) {
			out_path = argv[++i];
		} else if (strcmp(a, "--max-atlas") == 0 && i + 1 < argc) {
			opts.max_atlas_size = atoi(argv[++i]);
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
	if (opts.max_atlas_size < 4 || (opts.max_atlas_size & (opts.max_atlas_size - 1))) {
		fprintf(stderr, "[aeron_gltf_cook] --max-atlas must be a power of 2 ≥ 4 (got %d)\n",
				opts.max_atlas_size);
		return 2;
	}
	if (opts.mip_min_subrect < 1) {
		fprintf(stderr, "[aeron_gltf_cook] --mip-min-subrect must be ≥ 1\n");
		return 2;
	}

	return aeron_gltf_cook(in_path, out_path, &opts) ? 0 : 1;
}
