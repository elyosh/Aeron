/*
 * aeron_gltf_cook — CLI entry point.
 *
 * Usage: aeron_gltf_cook <in.gltf> --out <out.glb> [options]
 *        aeron_gltf_cook --build-project <project> --out <mod folder> [options]
 *        aeron_gltf_cook --library <library>.yaml --out <effects dir> [--verbose]
 *        aeron_gltf_cook --library-json <library>.yaml
 *        aeron_gltf_cook --library-from-json <in.json> <library>.yaml
 *
 * Cooks an artist-authored .gltf into a runtime-ready .glb with per-channel
 * KTX2 atlases (tools/extensions/AERON_flight_model_cooked, "Textures and
 * images"), after checking its materials' animations and screens. An
 * AERON_flight_model asset also gets cooked simulation and render data, and
 * probe data with --probes. Before the result is written, it is loaded back
 * the way a game loads it.
 * --build-project builds a project folder of the Blender add-on into a mod.
 * --library builds an effect library (animated_materials.md).
 */

#include "bc7_codec.h"
#include "cook_model.h"
#include "effect_files.h"
#include "effect_library.h"
#include "gltf_cook.h"
#include "ktx2_writer.h"
#include "material_checks.h"
#include "project_build.h"
#include "tool_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"

/* Host logs for the Aeron flight-model libraries. They are defined here so
 * that the cooker runs without SDL. */
static void cook_log(const char* fmt, va_list args) {
	fputs("[aeron_gltf_cook] ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
}

void SDL_Log(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	cook_log(fmt, args);
	va_end(args);
}

void SDL_LogVerbose(int category, const char* fmt, ...) {
	va_list args;
	(void)category;
	va_start(args, fmt);
	cook_log(fmt, args);
	va_end(args);
}

static void usage(void) {
	fprintf(stderr, "Usage: aeron_gltf_cook <in.gltf> --out <out.glb> [options]\n"
					"       aeron_gltf_cook --build-project <project> --out <mod folder> [--jobs N]\n"
					"                       [--only KEY] [--zstd-level N] [--separate-pages-budget N]\n"
					"                       [--separate-pages-max-layers N] [--verbose]\n"
					"       aeron_gltf_cook --library <library>.yaml --out <effects dir> [--verbose]\n"
					"       aeron_gltf_cook --library-from-json <in.json> <library>.yaml\n"
					"       aeron_gltf_cook --library-json <library>.yaml\n"

					"\n"
					"Cooks an artist-authored .gltf (PNG textures, optional\n"
					"KHR_materials_variants) into one .glb file with compressed KTX2\n"
					"atlas arrays (base_color / normal / metallic_roughness / emissive).\n"
					"The textures reference the atlases through AERON_flight_model_cooked.\n"
					"Each material's texture bindings carry a KHR_texture_transform\n"
					"that maps its UVs into its sub-rect of the atlas. It first\n"
					"checks the materials' AERON_flight_model objects (animations,\n"
					"screens). Skin layers (`skins`, `skinVariants`) become material\n"
					"variants, composited while cooking.\n"
					"\n"
					"For an AERON_flight_model asset it also adds simulation data\n"
					"(per-component collision BVH) and render data (render BVH) in\n"
					"AERON_flight_model_cooked. It then loads the result as a game would\n"
					"and fails if it does not load.\n"
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
					"  --separate-pages-budget <N>\n"
					"                        Give each image of a channel a page of its own,\n"
					"                        with every mip level, when that takes at most N\n"
					"                        more texels than packing them (default 1048576).\n"
					"                        A negative N turns it off, except for a channel\n"
					"                        with one image per atlas.\n"
					"  --separate-pages-max-layers <N>\n"
					"                        At most N pages for a channel with one page per\n"
					"                        image, 1 to 4096 (default 256).\n"
					"  --quality {fast|med|uber}\n"
					"                        BC7 encoder preset (default med).\n"
					"  --threads <N>         Encode textures on at most N threads (default:\n"
					"                        one per hardware thread). Set it when several\n"
					"                        cooks run at once.\n"
					"  --no-zstd             Skip zstd supercompression on KTX2 payloads.\n"
					"  --zstd-level <N>      zstd level, 1 to 22 (default 9; 19 is about\n"
					"                        14x slower for ~6%% smaller payloads).\n"
					"  --effects-dir <dir>   Check every frame and damage stage the\n"
					"                        materials name against the libraries in <dir>.\n"
					"  --project <dir>       With --effects-dir, first build each library\n"
					"                        the materials name that the project <dir>\n"
					"                        holds, as <dir>/effects/<library>/<library>.yaml,\n"
					"                        into the effects directory (as --library does).\n"
					"  --probes <dir>        Embed the probes bake in <dir> (bake.json,\n"
					"                        probes.ktx2, grid.ktx2) as the model's probeData.\n"
					"  --verbose             Print per-channel cook progress and the detail\n"
					"                        lines, such as the use of each atlas.\n"
					"  -h, --help            Show this help.\n"
					"\n"
					"--build-project builds a project folder of the Blender add-on into a\n"
					"mod folder. It builds each effects/<library>/<library>.yaml into\n"
					"flight/effects, as --library does, and copies effects/effects.yaml\n"
					"there. Then it cooks each models/<key>/<key>.gltf into\n"
					"flight/models/<key>.glb, with its probes bake when the add-on has\n"
					"made one (models/<key>/bake/probes). --jobs cooks N models at once\n"
					"(default 1), and --only cooks only the model KEY. The libraries are\n"
					"always built, since the models are checked against them.\n"
					"\n"
					"--library builds an effect library file: its images, baked by kind,\n"
					"into page arrays and an index in <effects dir>. A library whose\n"
					"index there records the same inputs (the file, its images and the\n"
					"baking version) is left as it is.\n"
					"\n"
					"--library-json prints an effect library file as JSON (for tools\n"
					"without a YAML reader), with the checks --library applies before\n"
					"reading its images. --library-from-json is its inverse: it reads\n"
					"such JSON (<in.json>, or - for stdin), checks it the same way and\n"
					"writes the file in its canonical form, keeping the leading comment\n"
					"lines of the file it replaces.\n");
	exit(2);
}

#define LIBRARY_TAG "[aeron_gltf_cook] library"

/* Prints effect library file `file` as JSON on stdout, once checked. */
static bool print_library(const char* file) {
	cJSON* library = EffectLibrary_ReadLibrary(LIBRARY_TAG, file);
	char*  printed = library ? cJSON_PrintUnformatted(library) : NULL;
	const bool ok  = printed && fputs(printed, stdout) >= 0 && fputc('\n', stdout) != EOF && fflush(stdout) == 0;
	free(printed);
	cJSON_Delete(library);
	return ok;
}

/* The JSON in file `path` (- for stdin), or NULL with a message. */
static cJSON* read_json(const char* path) {
	FILE*  f    = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
	char*  text = NULL;
	size_t len = 0, cap = 0, n;
	char   chunk[65536];
	while (f && (n = fread(chunk, 1, sizeof chunk, f)) > 0) {
		if (len + n + 1 > cap) {
			cap        = (len + n + 1) * 2;
			char* grow = (char*)realloc(text, cap);
			if (!grow)
				break;
			text = grow;
		}
		memcpy(text + len, chunk, n);
		len += n;
		text[len] = '\0';
	}
	if (f && f != stdin)
		fclose(f);
	cJSON* json = text ? cJSON_Parse(text) : NULL;
	free(text);
	if (!json)
		fprintf(stderr, "[aeron_gltf_cook] %s: not JSON\n", path);
	return json;
}

/* Writes effect library file `out` from the JSON in `in`, once checked. */
static bool write_library(const char* in, const char* out) {
	cJSON*     library = read_json(in);
	const bool ok      = library && EffectLibrary_CheckLibrary(LIBRARY_TAG, in, library) &&
						 EffectFiles_Write(LIBRARY_TAG, library, NULL, out);
	cJSON_Delete(library);
	return ok;
}

/* Reads option `argv[*i]` when it is one of the one-page-per-image options,
 * with its value, into `cook`. Returns 1 when it read one, 0 when it isn't
 * one, and -1 when its value is invalid. */
static int read_separate_pages_option(int argc, char** argv, int* i, AeronGltfCookOptions* cook) {
	const char* a = argv[*i];
	char*       end;
	if (strcmp(a, "--separate-pages-budget") == 0 && *i + 1 < argc) {
		const char* value          = argv[++*i];
		cook->separate_pages_budget = strtoll(value, &end, 10);
		if (!value[0] || *end) {
			fprintf(stderr, "[aeron_gltf_cook] --separate-pages-budget must be a number of texels\n");
			return -1;
		}
		return 1;
	}
	if (strcmp(a, "--separate-pages-max-layers") == 0 && *i + 1 < argc) {
		const char* value              = argv[++*i];
		const long  layers             = strtol(value, &end, 10);
		cook->separate_pages_max_layers = (int)layers;
		if (!value[0] || *end || layers < 1 || layers > 4096) {
			fprintf(stderr, "[aeron_gltf_cook] --separate-pages-max-layers must be 1 to 4096\n");
			return -1;
		}
		return 1;
	}
	return 0;
}

/* Reads the options of --build-project and runs it. */
static int build_project(int argc, char** argv) {
	ProjectBuildOptions options = { .project = argc > 2 ? argv[2] : NULL, .jobs = 1 };
	aeron_gltf_cook_default_options(&options.cook);
	for (int i = 3; i < argc; i++) {
		const char* a    = argv[i];
		const int   read = read_separate_pages_option(argc, argv, &i, &options.cook);
		if (read < 0)
			return 2;
		if (read > 0) {
			continue;
		} else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
			options.mod = argv[++i];
		} else if (strcmp(a, "--jobs") == 0 && i + 1 < argc) {
			options.jobs = atoi(argv[++i]);
			if (options.jobs < 1) {
				fprintf(stderr, "[aeron_gltf_cook] --jobs must be 1 or more\n");
				return 2;
			}
		} else if (strcmp(a, "--only") == 0 && i + 1 < argc) {
			options.only = argv[++i];
		} else if (strcmp(a, "--zstd-level") == 0 && i + 1 < argc) {
			options.cook.zstd_level = atoi(argv[++i]);
			if (options.cook.zstd_level < 1 || options.cook.zstd_level > 22) {
				fprintf(stderr, "[aeron_gltf_cook] --zstd-level must be 1 to 22\n");
				return 2;
			}
		} else if (strcmp(a, "--verbose") == 0) {
			options.cook.verbose = true;
			ToolLog_SetDetails(1);
		} else {
			fprintf(stderr, "[aeron_gltf_cook] unknown arg '%s'\n", a);
			usage();
		}
	}
	if (!options.project || !options.mod)
		usage();
	/* Set once, before the cooks run at once. */
	if (!ktx2_set_zstd_level(options.cook.zstd_level)) {
		fprintf(stderr, "[aeron_gltf_cook] invalid zstd level %d\n", options.cook.zstd_level);
		return 2;
	}
	return ProjectBuild_Run(&options) ? 0 : 1;
}

int main(int argc, char** argv) {
	if (argc < 2)
		usage();
	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
		usage();
	if (strcmp(argv[1], "--library") == 0) {
		if ((argc != 5 && argc != 6) || strcmp(argv[3], "--out") != 0 ||
			(argc == 6 && strcmp(argv[5], "--verbose") != 0))
			usage();
		ToolLog_SetDetails(argc == 6);
		return EffectLibrary_BuildLibrary(LIBRARY_TAG, argv[2], NULL, argv[4]) != EFFECT_LIBRARY_FAILED ? 0 : 1;
	}
	if (strcmp(argv[1], "--build-project") == 0)
		return build_project(argc, argv);
	if (strcmp(argv[1], "--library-json") == 0) {
		if (argc != 3)
			usage();
		return print_library(argv[2]) ? 0 : 1;
	}
	if (strcmp(argv[1], "--library-from-json") == 0) {
		if (argc != 4)
			usage();
		return write_library(argv[2], argv[3]) ? 0 : 1;
	}

	const char* in_path     = argv[1];
	const char* out_path    = NULL;
	const char* effects_dir = NULL;
	const char* project     = NULL;
	const char* probes_dir  = NULL;

	AeronGltfCookOptions opts;
	aeron_gltf_cook_default_options(&opts);

	for (int i = 2; i < argc; i++) {
		const char* a    = argv[i];
		const int   read = read_separate_pages_option(argc, argv, &i, &opts);
		if (read < 0)
			return 2;
		if (read > 0) {
			continue;
		} else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
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
		} else if (strcmp(a, "--threads") == 0 && i + 1 < argc) {
			const int threads = atoi(argv[++i]);
			if (threads < 1) {
				fprintf(stderr, "[aeron_gltf_cook] --threads must be 1 or more\n");
				return 2;
			}
			bc7_codec_set_max_threads((unsigned)threads);
		} else if (strcmp(a, "--no-zstd") == 0) {
			opts.zstd_supercompress = false;
		} else if (strcmp(a, "--zstd-level") == 0 && i + 1 < argc) {
			opts.zstd_level = atoi(argv[++i]);
		} else if (strcmp(a, "--effects-dir") == 0 && i + 1 < argc) {
			effects_dir = argv[++i];
		} else if (strcmp(a, "--project") == 0 && i + 1 < argc) {
			project = argv[++i];
		} else if (strcmp(a, "--probes") == 0 && i + 1 < argc) {
			probes_dir = argv[++i];
		} else if (strcmp(a, "--verbose") == 0) {
			opts.verbose = true;
			ToolLog_SetDetails(1);
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

	if (project && !effects_dir) {
		fprintf(stderr, "[aeron_gltf_cook] --project needs --effects-dir\n");
		return 2;
	}
	struct stat st;
	if (effects_dir && (stat(effects_dir, &st) != 0 || !S_ISDIR(st.st_mode))) {
		fprintf(stderr, "[aeron_gltf_cook] effects folder not found: %s\n", effects_dir);
		return 2;
	}
	if (project && (stat(project, &st) != 0 || !S_ISDIR(st.st_mode))) {
		fprintf(stderr, "[aeron_gltf_cook] project folder not found: %s\n", project);
		return 2;
	}

	return aeron_gltf_cook_check_materials(in_path, effects_dir, project) &&
				   aeron_gltf_cook_model(in_path, out_path, &opts, probes_dir)
			   ? 0
			   : 1;
}
