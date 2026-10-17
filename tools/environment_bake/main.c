/*
 * environment_bake — offline HDR environment baker.
 *
 * Bake mode converts an equirectangular Radiance .hdr panorama and an
 * optional light list into BC6H KTX2 cubes and an `environments:` entry. With
 * --radiance-out, the panorama is a sky without suns and is also written as
 * a radiance cube with box mips. Without it, the panorama is only used for
 * lighting (an enclosed space, such as a hangar). In both cases the baker
 * writes the Lambert irradiance / pi cube and the GGX cube with roughness
 * mips.
 * Diagnostic mode writes a labelled cube for checking orientation.
 * Sprite mode converts an HDR image to a KTX2 sun sprite. Capture mode
 * bakes a capture directory written by the game (six cube faces, sun
 * images, capture.yaml) into an environment under the game's mod folder,
 * with the `environments:` and `xwa:` snippets to merge into the manifest. Probes
 * mode bakes the panoramas that the Blender add-on renders for a model's
 * reflection probes and irradiance grid. It writes two KTX2 images and the
 * probe layout used for the bake (bake.json). aeron_gltf_cook embeds them
 * in the model's GLB.
 *
 * See envmap.h for the cube face and panorama conventions. Output is
 * deterministic for identical inputs and does not depend on --jobs.
 */

#include "aeron/asset/flight_model.h"
#include "aeron/config_file.h"
#include "aeron/scene/environment.h"
#include "aeron/scene/probe_set.h"
#include "aeron/vfs.h"
#include "cJSON.h"
#include "bc6h_codec.h"
#include "envmap.h"
#include "hdr_read.h"
#include "ktx2_writer.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAKE_PI 3.14159265358979323846

typedef struct BakeOptions {
	const char* input;
	const char* lights_input;
	const char* name;
	const char* radiance_out;
	const char* diffuse_out;
	const char* specular_out;
	const char* environment_out;
	const char* manifest_root;
	const char* diagnostic_out;
	const char* sprite_input;
	const char* sprite_out;
	const char* sprite_format;
	const char* capture_dir;
	const char* bake_root;
	const char* probes_input;
	const char* cubes_out;
	const char* grid_out;
	const char* bake_out;
	int         radiance_size;
	int         diffuse_size;
	int         specular_size;
	int         specular_samples;
	int         sprite_size;
	int         jobs;
	bool        specular_size_given; /* probes mode defaults to 256 */
} BakeOptions;

static void usage(void) {
	fprintf(
		stderr,
		"usage:\n"
		"  environment_bake --input sky.hdr --name NAME --radiance-out P --diffuse-out P --specular-out P\n"
		"                   [--lights-input lights.yaml] [--environment-out FILE] [--manifest-root DIR]\n"
		"                   [--radiance-size 2048] [--diffuse-size 32] [--specular-size 512]\n"
		"                   [--specular-samples 1024] [--jobs 8]\n"
		"  environment_bake --input space.hdr --name NAME --diffuse-out P --specular-out P\n"
		"                   [--lights-input lights.yaml] [--environment-out FILE] [--manifest-root DIR]\n"
		"                   [--diffuse-size 32] [--specular-size 512] [--specular-samples 1024] [--jobs 8]\n"
		"  environment_bake --diagnostic-faces out.ktx2 [--radiance-size 2048] [--jobs 8]\n"
		"  environment_bake --sprite in.hdr --sprite-out out.ktx2 [--sprite-size 256]\n"
		"                   [--sprite-format bc6h|rgba16f] [--jobs 8]\n"
		"  environment_bake --capture DIR --bake-root DIR [--radiance-size 2048] [--diffuse-size 32]\n"
		"                   [--specular-size 512] [--specular-samples 1024] [--sprite-size 256] [--jobs 8]\n"
		"  environment_bake --probes job.json --cubes-out P --grid-out P --bake-out bake.json\n"
		"                   [--specular-size 256] [--specular-samples 1024] [--jobs 8]\n"
		"\n"
		"The sky is an equirectangular panorama (2:1) without suns, in the authored\n"
		"frame: +X right, +Y up, -Z forward at the image centre. The light list is a\n"
		"YAML map with a `directional_lights` sequence in the environment light schema.\n"
		"Texture paths in the environment entry are relative to --manifest-root when given.\n"
		"A capture is baked into <bake root>/flight/environments/captured/<mission>/region_<n>/;\n"
		"its environment.yaml and mapping.yaml are merged into the manifest by hand.\n"
		"A probes job is a JSON object with the probe layout fields (the AERON_flight_model\n"
		"probeData fields, except the images), plus `probePanoramas`, `gridPanoramas`\n"
		"(x fastest, then y, then z) and `gridInvalid` (indices of points inside geometry).\n"
		"Panorama paths are relative to the job file. --bake-out writes the probe layout\n"
		"used for the bake. aeron_gltf_cook --probes embeds it with the images.\n");
}

static bool is_power_of_two(int v) { return v > 0 && (v & (v - 1)) == 0; }

static bool parse_int(const char* text, int* out) {
	char* end   = NULL;
	long  value = strtol(text, &end, 10);
	if (!text[0] || *end || value < 0 || value > 1 << 20)
		return false;
	*out = (int)value;
	return true;
}

static bool parse_args(int argc, char** argv, BakeOptions* o) {
	memset(o, 0, sizeof *o);
	o->radiance_size    = 2048;
	o->diffuse_size     = 32;
	o->specular_size    = 512;
	o->specular_samples = 1024;
	o->sprite_size      = 256;
	o->sprite_format    = "bc6h";
	o->jobs             = 8;
	for (int i = 1; i < argc; i += 2) {
		const char*  arg    = argv[i];
		const char*  value  = i + 1 < argc ? argv[i + 1] : NULL;
		const char** text   = NULL;
		int*         number = NULL;
		if (strcmp(arg, "--input") == 0)
			text = &o->input;
		else if (strcmp(arg, "--lights-input") == 0)
			text = &o->lights_input;
		else if (strcmp(arg, "--name") == 0)
			text = &o->name;
		else if (strcmp(arg, "--radiance-out") == 0)
			text = &o->radiance_out;
		else if (strcmp(arg, "--diffuse-out") == 0)
			text = &o->diffuse_out;
		else if (strcmp(arg, "--specular-out") == 0)
			text = &o->specular_out;
		else if (strcmp(arg, "--environment-out") == 0)
			text = &o->environment_out;
		else if (strcmp(arg, "--manifest-root") == 0)
			text = &o->manifest_root;
		else if (strcmp(arg, "--diagnostic-faces") == 0)
			text = &o->diagnostic_out;
		else if (strcmp(arg, "--sprite") == 0)
			text = &o->sprite_input;
		else if (strcmp(arg, "--sprite-out") == 0)
			text = &o->sprite_out;
		else if (strcmp(arg, "--sprite-format") == 0)
			text = &o->sprite_format;
		else if (strcmp(arg, "--capture") == 0)
			text = &o->capture_dir;
		else if (strcmp(arg, "--bake-root") == 0)
			text = &o->bake_root;
		else if (strcmp(arg, "--probes") == 0)
			text = &o->probes_input;
		else if (strcmp(arg, "--cubes-out") == 0)
			text = &o->cubes_out;
		else if (strcmp(arg, "--grid-out") == 0)
			text = &o->grid_out;
		else if (strcmp(arg, "--bake-out") == 0)
			text = &o->bake_out;
		else if (strcmp(arg, "--radiance-size") == 0)
			number = &o->radiance_size;
		else if (strcmp(arg, "--diffuse-size") == 0)
			number = &o->diffuse_size;
		else if (strcmp(arg, "--specular-size") == 0)
			number = &o->specular_size;
		else if (strcmp(arg, "--specular-samples") == 0)
			number = &o->specular_samples;
		else if (strcmp(arg, "--sprite-size") == 0)
			number = &o->sprite_size;
		else if (strcmp(arg, "--jobs") == 0)
			number = &o->jobs;
		else {
			fprintf(stderr, "environment_bake: unknown argument '%s'\n", arg);
			return false;
		}
		if (!value) {
			fprintf(stderr, "environment_bake: '%s' needs a value\n", arg);
			return false;
		}
		if (number == &o->specular_size)
			o->specular_size_given = true;
		if (text) {
			*text = value;
		} else if (!parse_int(value, number)) {
			fprintf(stderr, "environment_bake: '%s' needs a non-negative integer\n", arg);
			return false;
		}
	}
	if (o->jobs < 1 || o->jobs > ENVMAP_MAX_JOBS) {
		fprintf(stderr, "environment_bake: --jobs must be 1 to %d\n", ENVMAP_MAX_JOBS);
		return false;
	}
	return true;
}

/* ---- encoding ---------------------------------------------------------- */

static float luminance(const float* rgb) { return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]; }

static uint16_t float_to_half(float value) {
	uint32_t bits;
	if (!(value > 0.0f))
		value = 0.0f; /* environment data is non-negative */
	if (value > 65504.0f)
		value = 65504.0f;
	memcpy(&bits, &value, sizeof bits);
	const uint32_t exponent = (bits >> 23) & 0xFFu;
	uint32_t       mantissa = bits & 0x7FFFFFu;
	if (exponent < 103)
		return 0;
	if (exponent < 113) {
		/* Subnormal half: shift the implicit-one mantissa, round to nearest even. */
		mantissa |= 0x800000u;
		const uint32_t shift   = 126u - exponent;
		uint32_t       half    = mantissa >> shift;
		const uint32_t rest    = mantissa & ((1u << shift) - 1u);
		const uint32_t halfway = 1u << (shift - 1u);
		if (rest > halfway || (rest == halfway && (half & 1u)))
			++half;
		return (uint16_t)half;
	}
	uint32_t       half = ((exponent - 112u) << 10) | (mantissa >> 13);
	const uint32_t rest = mantissa & 0x1FFFu;
	if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
		++half;
	return (uint16_t)half;
}

typedef struct EncodeTask {
	const float* const* images; /* [level * face_count + face], RGB; faces of every layer */
	const int*          sizes;  /* per level, square */
	int                 face_count;
	int                 level_count;
	uint8_t**           blobs; /* per level, faces back to back */
	size_t*             face_bytes;
	int*                task_level; /* task -> level */
	int*                task_face;
	int*                task_row; /* block row */
} EncodeTask;

static void encode_bc6h_task(void* context, int index) {
	const EncodeTask* t      = (const EncodeTask*)context;
	const int         level  = t->task_level[index];
	const int         face   = t->task_face[index];
	const int         by     = t->task_row[index];
	const int         n      = t->sizes[level];
	const float*      src    = t->images[level * t->face_count + face];
	const int         blocks = (n + 3) / 4;
	const int         rows   = n - by * 4 < 4 ? n - by * 4 : 4;
	uint8_t*          out = t->blobs[level] + (size_t)face * t->face_bytes[level] + (size_t)by * blocks * 16u;
	/* One block row per task: the encoder batches neighbouring blocks. */
	bc6h_codec_encode_image_serial(out, (size_t)blocks * 16u, src + (size_t)by * 4u * n * 3u, n, rows,
								   BC6H_QUALITY_UBER);
}

/* Writes square RGB float images as BC6H (format "bc6h") or RGBA16F KTX2.
 * `face_count` counts every slice of a level: 6 per cube of a cube array
 * of `layer_count` cubes (0 for one image). */
static bool write_levels(const char* path, const char* format, const float* const* images, const int* sizes,
						 int face_count, int layer_count, int level_count, int jobs) {
	const bool     bc6h                          = strcmp(format, "bc6h") == 0;
	uint8_t*       blobs[ENVMAP_MAX_LEVELS]      = { 0 };
	size_t         face_bytes[ENVMAP_MAX_LEVELS] = { 0 };
	Ktx2WriteLevel levels[ENVMAP_MAX_LEVELS];
	bool           ok         = true;
	int            task_count = 0;
	for (int level = 0; level < level_count; ++level) {
		const size_t n    = (size_t)sizes[level];
		face_bytes[level] = bc6h ? bc6h_codec_image_size((int)n, (int)n) : n * n * 8u;
		blobs[level]      = (uint8_t*)malloc(face_bytes[level] * (size_t)face_count);
		if (!blobs[level])
			ok = false;
		task_count += face_count * (int)((n + 3) / 4);
	}
	if (ok && bc6h) {
		bc6h_codec_init();
		EncodeTask t = { images, sizes, face_count, level_count, blobs, face_bytes, NULL, NULL, NULL };
		t.task_level = (int*)malloc((size_t)task_count * sizeof(int));
		t.task_face  = (int*)malloc((size_t)task_count * sizeof(int));
		t.task_row   = (int*)malloc((size_t)task_count * sizeof(int));
		if (t.task_level && t.task_face && t.task_row) {
			int k = 0;
			for (int level = 0; level < level_count; ++level)
				for (int face = 0; face < face_count; ++face)
					for (int by = 0; by < (sizes[level] + 3) / 4; ++by, ++k) {
						t.task_level[k] = level;
						t.task_face[k]  = face;
						t.task_row[k]   = by;
					}
			envmap_parallel_for(task_count, jobs, encode_bc6h_task, &t);
		} else {
			ok = false;
		}
		free(t.task_level);
		free(t.task_face);
		free(t.task_row);
	} else if (ok) {
		for (int level = 0; level < level_count; ++level) {
			const size_t n = (size_t)sizes[level];
			for (int face = 0; face < face_count; ++face) {
				const float* src = images[level * face_count + face];
				uint16_t*    dst = (uint16_t*)(blobs[level] + (size_t)face * face_bytes[level]);
				for (size_t i = 0; i < n * n; ++i) {
					dst[i * 4 + 0] = float_to_half(src[i * 3 + 0]);
					dst[i * 4 + 1] = float_to_half(src[i * 3 + 1]);
					dst[i * 4 + 2] = float_to_half(src[i * 3 + 2]);
					dst[i * 4 + 3] = float_to_half(1.0f);
				}
			}
		}
	}
	for (int level = 0; ok && level < level_count; ++level) {
		levels[level].width  = sizes[level];
		levels[level].height = sizes[level];
		levels[level].data   = blobs[level];
		levels[level].size   = face_bytes[level] * (size_t)face_count;
	}
	if (ok) {
		const int faces = layer_count > 0 ? face_count / layer_count : face_count;
		ok = bc6h ? write_ktx2_bc6h_levels(path, faces, layer_count, levels, level_count, true)
				  : write_ktx2_rgba16f_levels(path, faces, layer_count, levels, level_count, true);
		if (!ok)
			fprintf(stderr, "environment_bake: cannot write %s\n", path);
	}
	for (int level = 0; level < level_count; ++level)
		free(blobs[level]);
	return ok;
}

static bool write_cube(const char* path, const EnvCube* cube, int jobs) {
	const float* images[ENVMAP_MAX_LEVELS * 6];
	int          sizes[ENVMAP_MAX_LEVELS];
	for (int level = 0; level < cube->level_count; ++level) {
		sizes[level] = envmap_level_size(cube, level);
		for (int face = 0; face < 6; ++face)
			images[level * 6 + face] = cube->faces[level][face];
	}
	return write_levels(path, "bc6h", images, sizes, 6, 0, cube->level_count, jobs);
}

static double file_size_mib(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return 0.0;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fclose(f);
	return size > 0 ? (double)size / (1024.0 * 1024.0) : 0.0;
}

/* ---- YAML -------------------------------------------------------------- */

/* Loads a YAML file from the host filesystem through a private VFS. */
static AeronConfigFile* load_yaml(const char* path, AeronVfs** out_vfs) {
	char        dir[1024];
	const char* slash = strrchr(path, '/');
#ifdef _WIN32
	const char* bslash = strrchr(path, '\\');
	if (bslash && (!slash || bslash > slash))
		slash = bslash;
#endif
	if (slash) {
		snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);
		if (!dir[0])
			snprintf(dir, sizeof dir, "/");
	} else {
		snprintf(dir, sizeof dir, ".");
	}
	AeronVfs* vfs = AeronVfs_Create(NULL);
	if (!vfs || !AeronVfs_SetRoot(vfs, AERON_VFS_ROOT_ASSET, dir)) {
		AeronVfs_Destroy(vfs);
		fprintf(stderr, "environment_bake: cannot open directory of %s\n", path);
		return NULL;
	}
	AeronConfigFile* config = NULL;
	AeronConfigError error;
	memset(&error, 0, sizeof error);
	if (!AeronConfigFile_LoadYamlEx(vfs, AERON_VFS_ROOT_ASSET, slash ? slash + 1 : path, &config, &error)) {
		fprintf(stderr, "environment_bake: %s:%d: %s\n", path, error.line,
				error.message[0] ? error.message : "cannot load");
		AeronVfs_Destroy(vfs);
		return NULL;
	}
	*out_vfs = vfs;
	return config;
}

static bool load_lights(const char* path, AeronEnvironmentLightDefinition lights[AERON_ENV_MAX_LIGHTS],
						uint32_t* count, int32_t* primary) {
	AeronVfs*        vfs    = NULL;
	AeronConfigFile* config = load_yaml(path, &vfs);
	if (!config)
		return false;
	const AeronConfigNode* root = AeronConfigFile_Root(config);
	bool                   ok   = true;
	for (size_t i = 0; i < AeronConfigNode_MapCount(root); ++i) {
		if (strcmp(AeronConfigNode_MapKeyAt(root, i), "directional_lights") != 0) {
			fprintf(stderr, "environment_bake: %s: unknown field '%s'\n", path,
					AeronConfigNode_MapKeyAt(root, i));
			ok = false;
		}
	}
	AeronConfigError error;
	memset(&error, 0, sizeof error);
	if (ok && !AeronEnvironment_ParseLights(AeronConfigNode_MapGet(root, "directional_lights"), "light list",
											lights, count, primary, &error)) {
		fprintf(stderr, "environment_bake: %s:%d: %s\n", path, error.line, error.message);
		ok = false;
	}
	AeronConfigFile_Destroy(config);
	AeronVfs_Destroy(vfs);
	return ok;
}

/* Output path as written in the manifest: relative to the manifest root,
 * with forward slashes. */
static bool manifest_path(const char* path, const char* root, char* out, size_t size) {
	/* Leading "./" components are not part of either path. */
	while (path[0] == '.' && (path[1] == '/' || path[1] == '\\'))
		path += 2;
	const char* relative = path;
	if (root) {
		while (root[0] == '.' && (root[1] == '/' || root[1] == '\\'))
			root += 2;
		size_t length = strlen(root);
		while (length > 0 && (root[length - 1] == '/' || root[length - 1] == '\\'))
			--length;
		const bool current_dir = length == 0 || (length == 1 && root[0] == '.');
		if (!current_dir) {
			if (strncmp(path, root, length) != 0 || (path[length] != '/' && path[length] != '\\')) {
				fprintf(stderr, "environment_bake: %s is not under --manifest-root %s\n", path, root);
				return false;
			}
			relative = path + length + 1;
		}
	}
	if (strlen(relative) >= size || strlen(relative) >= AERON_ENV_PATH_MAX) {
		fprintf(stderr, "environment_bake: texture path %s is too long\n", relative);
		return false;
	}
	snprintf(out, size, "%s", relative);
	for (char* p = out; *p; ++p)
		if (*p == '\\')
			*p = '/';
	return true;
}

/* Writes the baked environment as an `environments:` document. Strengths,
 * fill and rotation are starting values to calibrate per environment. The
 * fill starts at 0. An environment with a sky (`radiance`) always lists its
 * lights, as [] when there is no light list. An environment without a sky
 * lists lights only when it has a light list (`has_lights`). Otherwise the
 * game keeps its own lights. */
static bool write_environment_entry(FILE* f, const char* name, const char* radiance, const char* diffuse,
									const char* specular, bool has_lights,
									const AeronEnvironmentLightDefinition* lights, uint32_t light_count,
									int32_t primary) {
	AeronEnvironmentDefinition* d = (AeronEnvironmentDefinition*)calloc(1, sizeof *d);
	if (!d)
		return false;
	snprintf(d->name, sizeof d->name, "%s", name);
	if (radiance)
		snprintf(d->radiance_path, sizeof d->radiance_path, "%s", radiance);
	snprintf(d->diffuse_path, sizeof d->diffuse_path, "%s", diffuse);
	snprintf(d->specular_path, sizeof d->specular_path, "%s", specular);
	d->intensity         = 1.0f;
	d->diffuse_strength  = 1.0f;
	d->specular_strength = 1.0f;
	d->has_directional_lights = radiance != NULL || has_lights;
	memcpy(d->lights, lights, sizeof d->lights);
	d->light_count         = light_count;
	d->primary_light_index = primary;
	const size_t length    = AeronEnvironment_FormatDefinitionYaml(d, NULL, 0);
	char*        text      = (char*)malloc(length + 1);
	bool         ok        = text != NULL;
	if (ok) {
		AeronEnvironment_FormatDefinitionYaml(d, text, length + 1);
		ok = fwrite(text, 1, length, f) == length;
	}
	free(text);
	free(d);
	return ok;
}

/* Re-parses a written environment file with the runtime parser. */
static bool validate_environment_file(const char* path) {
	AeronVfs*        vfs    = NULL;
	AeronConfigFile* config = load_yaml(path, &vfs);
	if (!config)
		return false;
	AeronEnvironmentDefinitionSet* set = NULL;
	AeronConfigError               error;
	memset(&error, 0, sizeof error);
	const bool ok = AeronEnvironment_ParseDefinitions(
		AeronConfigNode_MapGet(AeronConfigFile_Root(config), "environments"), &set, &error);
	if (!ok)
		fprintf(stderr, "environment_bake: written environment %s:%d: %s\n", path, error.line, error.message);
	AeronEnvironmentDefinitionSet_Destroy(set);
	AeronConfigFile_Destroy(config);
	AeronVfs_Destroy(vfs);
	return ok;
}

/* ---- statistics -------------------------------------------------------- */

typedef struct CubeStats {
	double peak;
	double mean; /* solid-angle weighted */
} CubeStats;

static int stats_level(const EnvCube* cube, int max_size) {
	int level = 0;
	while (level + 1 < cube->level_count && envmap_level_size(cube, level) > max_size)
		++level;
	return level;
}

static CubeStats cube_stats(const EnvCube* cube) {
	CubeStats stats = { 0.0, 0.0 };
	const int n     = cube->size;
	double    total = 0.0;
	for (int face = 0; face < 6; ++face) {
		for (int y = 0; y < n; ++y) {
			double row = 0.0;
			for (int x = 0; x < n; ++x) {
				const double l = luminance(cube->faces[0][face] + ((size_t)y * n + x) * 3u);
				if (l > stats.peak)
					stats.peak = l;
				row += l * envmap_texel_solid_angle(x, y, n);
			}
			total += row;
		}
	}
	stats.mean = total / (4.0 * BAKE_PI);
	return stats;
}

/* Radiant flux of the sky within `radius_degrees` of a direction, as a
 * fraction of the light's own irradiance: a sun left in the image shows up
 * as a large fraction. */
static double light_energy_fraction(const EnvCube* cube, const AeronEnvironmentLightDefinition* light,
									double radius_degrees) {
	const int    level = stats_level(cube, 1024);
	const int    n     = envmap_level_size(cube, level);
	const double cos_r = cos(radius_degrees * BAKE_PI / 180.0);
	double       flux  = 0.0;
	for (int face = 0; face < 6; ++face) {
		for (int y = 0; y < n; ++y) {
			for (int x = 0; x < n; ++x) {
				float d[3];
				envmap_texel_direction(face, x, y, n, d);
				const double c = d[0] * light->direction_cube[0] + d[1] * light->direction_cube[1] +
								 d[2] * light->direction_cube[2];
				if (c >= cos_r)
					flux += luminance(cube->faces[level][face] + ((size_t)y * n + x) * 3u) *
							envmap_texel_solid_angle(x, y, n);
			}
		}
	}
	const double irradiance = light->intensity * luminance(light->color_linear);
	return irradiance > 0.0 ? flux / irradiance : 0.0;
}

/* ---- bake ---------------------------------------------------------------- */

static bool check_size(const char* what, int value, int min, int max) {
	if (!is_power_of_two(value) || value < min || value > max) {
		fprintf(stderr, "environment_bake: %s must be a power of two from %d to %d\n", what, min, max);
		return false;
	}
	return true;
}

/* Filters a radiance cube whose level 0 is filled into the diffuse and
 * specular cubes, reports the sky and lights, and writes the cubes; the
 * radiance cube only with a `radiance_out`. Frees `radiance`. */
static bool bake_cubes(const BakeOptions* o, EnvCube* radiance, int specular_size,
					   const AeronEnvironmentLightDefinition* lights, uint32_t light_count,
					   const char* radiance_out, const char* diffuse_out, const char* specular_out) {
	EnvCube diffuse, specular;
	memset(&diffuse, 0, sizeof diffuse);
	memset(&specular, 0, sizeof specular);
	if (!envmap_cube_alloc(&diffuse, o->diffuse_size, envmap_mip_count(o->diffuse_size)) ||
		!envmap_cube_alloc(&specular, specular_size, envmap_mip_count(specular_size))) {
		fprintf(stderr, "environment_bake: out of memory\n");
		envmap_cube_free(radiance);
		envmap_cube_free(&diffuse);
		envmap_cube_free(&specular);
		return false;
	}
	envmap_build_box_mips(radiance);
	envmap_irradiance(&diffuse, radiance, o->jobs);
	envmap_build_box_mips(&diffuse);
	envmap_ggx_prefilter(&specular, radiance, o->specular_samples, o->jobs);

	const CubeStats stats = cube_stats(radiance);
	printf("sky luminance: peak %.6g, mean %.6g\n", stats.peak, stats.mean);
	for (uint32_t i = 0; i < light_count; ++i) {
		const double radius   = fmax(2.0 * lights[i].angular_radius_degrees, 1.0);
		const double fraction = light_energy_fraction(radiance, &lights[i], radius);
		printf("light %s: sky within %.2g deg holds %.3g%% of its irradiance\n", lights[i].name, radius,
			   fraction * 100.0);
		if (fraction > 0.1)
			fprintf(stderr,
					"environment_bake: warning: light '%s': the sky is bright around its direction; "
					"a sun may have been left in the image\n",
					lights[i].name);
	}

	const bool ok = (!radiance_out || write_cube(radiance_out, radiance, o->jobs)) &&
					write_cube(diffuse_out, &diffuse, o->jobs) && write_cube(specular_out, &specular, o->jobs);
	if (ok) {
		if (radiance_out)
			printf("radiance %s: %d px, %d levels, %.2f MiB\n", radiance_out, radiance->size,
				   radiance->level_count, file_size_mib(radiance_out));
		printf("diffuse  %s: %d px, %d levels, exact irradiance over %d px faces, %.2f MiB\n", diffuse_out,
			   diffuse.size, diffuse.level_count, envmap_level_size(radiance, stats_level(radiance, 64)),
			   file_size_mib(diffuse_out));
		printf("specular %s: %d px, %d roughness levels, %d samples, %.2f MiB\n", specular_out, specular.size,
			   specular.level_count, o->specular_samples, file_size_mib(specular_out));
	}
	envmap_cube_free(radiance);
	envmap_cube_free(&diffuse);
	envmap_cube_free(&specular);
	return ok;
}

static bool check_pixels(const char* path, const float* rgb, size_t pixel_count) {
	for (size_t i = 0; i < pixel_count * 3u; ++i) {
		if (!isfinite(rgb[i]) || rgb[i] < 0.0f) {
			fprintf(stderr, "environment_bake: %s has a non-finite or negative pixel at %zu\n", path, i / 3u);
			return false;
		}
	}
	return true;
}

static int run_bake(const BakeOptions* o) {
	const bool sky = o->radiance_out != NULL;
	if (!o->name || !o->diffuse_out || !o->specular_out) {
		usage();
		return 2;
	}
	/* Without a sky, the radiance cube is only the input of the filters, so
	 * it uses the specular size. */
	const int radiance_size = sky ? o->radiance_size : o->specular_size;
	if (!AeronEnvironment_IsValidName(o->name)) {
		fprintf(stderr, "environment_bake: --name must be 1 to 63 lower-case letters, digits, '.' and '_'\n");
		return 2;
	}
	if (!check_size("--radiance-size", o->radiance_size, 16, 4096) ||
		!check_size("--diffuse-size", o->diffuse_size, 4, 256) ||
		!check_size("--specular-size", o->specular_size, 16, 2048))
		return 2;
	if (o->specular_size > radiance_size) {
		fprintf(stderr, "environment_bake: --specular-size must not exceed --radiance-size\n");
		return 2;
	}
	if (o->specular_samples < 16 || o->specular_samples > 65536) {
		fprintf(stderr, "environment_bake: --specular-samples must be 16 to 65536\n");
		return 2;
	}
	char radiance_path[256], diffuse_path[256], specular_path[256];
	if ((sky && !manifest_path(o->radiance_out, o->manifest_root, radiance_path, sizeof radiance_path)) ||
		!manifest_path(o->diffuse_out, o->manifest_root, diffuse_path, sizeof diffuse_path) ||
		!manifest_path(o->specular_out, o->manifest_root, specular_path, sizeof specular_path))
		return 2;

	AeronEnvironmentLightDefinition lights[AERON_ENV_MAX_LIGHTS];
	uint32_t                        light_count = 0;
	int32_t                         primary     = -1;
	memset(lights, 0, sizeof lights);
	if (o->lights_input && !load_lights(o->lights_input, lights, &light_count, &primary))
		return 1;

	float* pano = NULL;
	int    w = 0, h = 0;
	char   err[512];
	if (!read_hdr_rgb(o->input, &pano, &w, &h, err, sizeof err)) {
		fprintf(stderr, "environment_bake: %s\n", err);
		return 1;
	}
	if (w != 2 * h) {
		fprintf(stderr, "environment_bake: %s is %dx%d; an equirectangular panorama is 2:1\n", o->input, w,
				h);
		free(pano);
		return 1;
	}
	if (!check_pixels(o->input, pano, (size_t)w * h)) {
		free(pano);
		return 1;
	}
	if ((double)w < BAKE_PI * (double)radiance_size)
		fprintf(stderr,
				"environment_bake: warning: %dx%d panorama is below pi x %d wide; the %s will be soft\n", w, h,
				radiance_size, sky ? "sky" : "reflections");

	EnvCube radiance;
	if (!envmap_cube_alloc(&radiance, radiance_size, envmap_mip_count(radiance_size))) {
		fprintf(stderr, "environment_bake: out of memory\n");
		free(pano);
		return 1;
	}
	printf("input %s: %dx%d\n", o->input, w, h);
	envmap_from_equirect(&radiance, pano, w, h, o->jobs);
	free(pano);
	if (!bake_cubes(o, &radiance, o->specular_size, lights, light_count, sky ? o->radiance_out : NULL,
					o->diffuse_out, o->specular_out))
		return 1;

	FILE* f = o->environment_out ? fopen(o->environment_out, "wb") : stdout;
	if (!f) {
		fprintf(stderr, "environment_bake: cannot write %s\n", o->environment_out);
		return 1;
	}
	const bool written =
		write_environment_entry(f, o->name, sky ? radiance_path : NULL, diffuse_path, specular_path,
								o->lights_input != NULL, lights, light_count, primary);
	if (o->environment_out) {
		fclose(f);
		if (!written) {
			fprintf(stderr, "environment_bake: cannot write %s\n", o->environment_out);
			return 1;
		}
		if (!validate_environment_file(o->environment_out))
			return 1;
		printf("environment %s: %s\n", o->name, o->environment_out);
	}
	return 0;
}

/* ---- probes ------------------------------------------------------------- */

#define PROBE_GRID_CUBE_SIZE 32
#define PROBE_PATH_MAX 1024

typedef struct ProbeJob {
	AeronFlightProbeLayout layout; /* probes and grid */
	char (*probe_panoramas)[PROBE_PATH_MAX];
	char (*grid_panoramas)[PROBE_PATH_MAX];
	uint8_t* invalid; /* per grid point */
	uint32_t point_count;
} ProbeJob;

static void probe_job_free(ProbeJob* job) {
	free(job->probe_panoramas);
	free(job->grid_panoramas);
	free(job->invalid);
	memset(job, 0, sizeof *job);
}

/* A panorama path from the job: relative to the job file's folder. */
static bool job_path(const char* job, const cJSON* node, char out[PROBE_PATH_MAX]) {
	const char* name = cJSON_IsString(node) && node->valuestring ? node->valuestring : "";
	if (!name[0])
		return false;
	const char* slash = strrchr(job, '/');
	const int   n     = name[0] == '/' || !slash
							? snprintf(out, PROBE_PATH_MAX, "%s", name)
							: snprintf(out, PROBE_PATH_MAX, "%.*s/%s", (int)(slash - job), job, name);
	return n > 0 && n < PROBE_PATH_MAX;
}

/* Reads a whole file. Returns NULL on failure. */
static char* read_text(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	long length = -1;
	if (fseek(f, 0, SEEK_END) == 0)
		length = ftell(f);
	rewind(f);
	char* text = length >= 0 ? (char*)malloc((size_t)length + 1) : NULL;
	if (text && fread(text, 1, (size_t)length, f) != (size_t)length) {
		free(text);
		text = NULL;
	}
	fclose(f);
	if (text) {
		text[length] = '\0';
		*size        = (size_t)length;
	}
	return text;
}

/* Reads a probes job. The job is a JSON object with the probe layout
 * fields (AeronFlightProbeLayout) and `probePanoramas`, `gridPanoramas` (x
 * fastest, then y, then z) and `gridInvalid` (indices of points inside
 * geometry). On error, prints the reason and returns false. */
static bool load_probe_job(const char* path, ProbeJob* job) {
	memset(job, 0, sizeof *job);
	size_t size = 0;
	char*  text = read_text(path, &size);
	if (!text) {
		fprintf(stderr, "environment_bake: cannot read %s\n", path);
		return false;
	}
	char error[256] = "";
	bool ok         = Aeron_FlightProbeLayoutParse(text, size, &job->layout, error, sizeof error);
	if (!ok)
		fprintf(stderr, "environment_bake: %s: %s\n", path, error);
	cJSON* root = ok ? cJSON_ParseWithLength(text, size) : NULL;
	free(text);
	ok = ok && root;

	const cJSON* probes = cJSON_GetObjectItemCaseSensitive(root, "probePanoramas");
	if (ok && (!cJSON_IsArray(probes) || (uint32_t)cJSON_GetArraySize(probes) != job->layout.probe_count)) {
		fprintf(stderr, "environment_bake: %s: 'probePanoramas' must list one panorama per probe\n", path);
		ok = false;
	}
	if (ok) {
		job->probe_panoramas = calloc(job->layout.probe_count, sizeof *job->probe_panoramas);
		ok                   = job->probe_panoramas != NULL;
	}
	for (uint32_t i = 0; ok && i < job->layout.probe_count; ++i) {
		if (!job_path(path, cJSON_GetArrayItem(probes, (int)i), job->probe_panoramas[i])) {
			fprintf(stderr, "environment_bake: %s: probe panorama %u must be a file name\n", path, i);
			ok = false;
		}
	}

	const uint32_t* r = job->layout.grid_resolution;
	job->point_count  = r[0] * r[1] * r[2];
	const cJSON* points = cJSON_GetObjectItemCaseSensitive(root, "gridPanoramas");
	if (ok && (!cJSON_IsArray(points) || (uint32_t)cJSON_GetArraySize(points) != job->point_count)) {
		fprintf(stderr, "environment_bake: %s: 'gridPanoramas' must list one panorama per grid point (%u)\n",
				path, job->point_count);
		ok = false;
	}
	if (ok) {
		job->grid_panoramas = calloc(job->point_count, sizeof *job->grid_panoramas);
		job->invalid        = calloc(job->point_count, 1);
		ok                  = job->grid_panoramas && job->invalid;
	}
	for (uint32_t i = 0; ok && i < job->point_count; ++i) {
		if (!job_path(path, cJSON_GetArrayItem(points, (int)i), job->grid_panoramas[i])) {
			fprintf(stderr, "environment_bake: %s: grid panorama %u must be a file name\n", path, i);
			ok = false;
		}
	}
	const cJSON* invalid = cJSON_GetObjectItemCaseSensitive(root, "gridInvalid");
	if (ok && invalid && !cJSON_IsArray(invalid)) {
		fprintf(stderr, "environment_bake: %s: 'gridInvalid' must be an array of point indices\n", path);
		ok = false;
	}
	const cJSON* indices = ok ? invalid : NULL;
	const cJSON* item    = NULL;
	cJSON_ArrayForEach(item, indices) {
		if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble >= job->point_count ||
			(double)(uint32_t)item->valuedouble != item->valuedouble) {
			fprintf(stderr, "environment_bake: %s: 'gridInvalid' must only hold point indices below %u\n",
					path, job->point_count);
			ok = false;
			break;
		}
		job->invalid[(uint32_t)item->valuedouble] = 1;
	}
	cJSON_Delete(root);
	if (!ok)
		probe_job_free(job);
	return ok;
}

/* Reads a 2:1 panorama of finite, non-negative pixels. */
static bool read_panorama(const char* path, float** rgb, int* w, int* h) {
	char err[512];
	if (!read_hdr_rgb(path, rgb, w, h, err, sizeof err)) {
		fprintf(stderr, "environment_bake: %s\n", err);
		return false;
	}
	if (*w != 2 * *h || !check_pixels(path, *rgb, (size_t)*w * *h)) {
		if (*w != 2 * *h)
			fprintf(stderr, "environment_bake: %s is %dx%d; a panorama is 2:1\n", path, *w, *h);
		free(*rgb);
		*rgb = NULL;
		return false;
	}
	return true;
}

/* Bakes each probe's panorama into GGX-prefiltered cubes and writes them
 * as one cube array, probe k in layer k. */
static bool bake_probe_cubes(const BakeOptions* o, const ProbeJob* job, int size) {
	const uint32_t count  = job->layout.probe_count;
	const int      levels = envmap_mip_count(size);
	EnvCube*       cubes  = (EnvCube*)calloc(count, sizeof *cubes);
	const float**  images = (const float**)malloc((size_t)levels * 6u * count * sizeof *images);
	int            sizes[ENVMAP_MAX_LEVELS];
	bool           ok = cubes && images;
	for (uint32_t p = 0; ok && p < count; ++p) {
		float*  pano = NULL;
		int     w = 0, h = 0;
		EnvCube radiance;
		memset(&radiance, 0, sizeof radiance);
		ok = read_panorama(job->probe_panoramas[p], &pano, &w, &h) &&
			 envmap_cube_alloc(&radiance, size, levels) && envmap_cube_alloc(&cubes[p], size, levels);
		if (ok) {
			if ((double)w < BAKE_PI * (double)size)
				fprintf(stderr,
						"environment_bake: warning: %s is below pi x %d wide; its reflections will be soft\n",
						job->probe_panoramas[p], size);
			envmap_from_equirect(&radiance, pano, w, h, o->jobs);
			envmap_build_box_mips(&radiance);
			envmap_ggx_prefilter(&cubes[p], &radiance, o->specular_samples, o->jobs);
			printf("probe %u %s: %dx%d\n", p, job->probe_panoramas[p], w, h);
		}
		free(pano);
		envmap_cube_free(&radiance);
	}
	if (ok) {
		for (int level = 0; level < levels; ++level) {
			sizes[level] = envmap_level_size(&cubes[0], level);
			for (uint32_t p = 0; p < count; ++p)
				for (int face = 0; face < 6; ++face)
					images[((size_t)level * count + p) * 6u + face] = cubes[p].faces[level][face];
		}
		ok = write_levels(o->cubes_out, "bc6h", images, sizes, 6 * (int)count, (int)count, levels, o->jobs);
		if (ok)
			printf("cubes %s: %u probes, %d px, %d roughness levels, %d samples, %.2f MiB\n", o->cubes_out,
				   count, size, levels, o->specular_samples, file_size_mib(o->cubes_out));
	}
	for (uint32_t p = 0; cubes && p < count; ++p)
		envmap_cube_free(&cubes[p]);
	free(cubes);
	free(images);
	return ok;
}

/* A grid point's light in the model frame: irradiance / pi toward +x, -x,
 * +y, -y, +z, -z, then the mean radiance, per RGB channel. */
enum { GRID_FACES = 6, GRID_TERMS = 7 };

typedef struct GridTask {
	const ProbeJob* job;
	float (*light)[GRID_TERMS][3]; /* per point */
	uint8_t* failed;
} GridTask;

static void grid_point_task(void* context, int index) {
	GridTask* t = (GridTask*)context;
	if (t->job->invalid[index])
		return;
	float*  pano = NULL;
	int     w = 0, h = 0;
	EnvCube cube;
	memset(&cube, 0, sizeof cube);
	if (!read_panorama(t->job->grid_panoramas[index], &pano, &w, &h) ||
		!envmap_cube_alloc(&cube, PROBE_GRID_CUBE_SIZE, 1)) {
		t->failed[index] = 1;
	} else {
		envmap_from_equirect(&cube, pano, w, h, 1);
		/* The cube's y is the model's z (up), its z the model's y. */
		static const int model_term[GRID_TERMS] = { 0, 1, 4, 5, 2, 3, 6 };
		float            cube_light[GRID_TERMS][3];
		envmap_axis_irradiance(&cube, cube_light);
		for (int f = 0; f < GRID_TERMS; ++f)
			memcpy(t->light[index][model_term[f]], cube_light[f], sizeof cube_light[f]);
	}
	free(pano);
	envmap_cube_free(&cube);
}

/* Gives every valid grid point its light on the faces of an axis-aligned
 * cube, each invalid point the average of the valid ones within 1, then 2
 * points, else zero, and writes the grid: a 2D RGBA16F image 6 rx wide and
 * ry rz high, point (x, y, z) at column 6 x, row y + ry z; texel f holds
 * the irradiance / pi toward +x, -x, +y, -y, +z, -z (model frame) in RGB,
 * and the luminance of the mean radiance in A. */
static bool bake_probe_grid(const BakeOptions* o, const ProbeJob* job) {
	const uint32_t* r      = job->layout.grid_resolution;
	const uint32_t  points = job->point_count;
	GridTask        t      = { job, calloc(points, sizeof *t.light), calloc(points, 1) };
	const int       width = GRID_FACES * (int)r[0], height = (int)(r[1] * r[2]);
	uint16_t*       texels = (uint16_t*)calloc((size_t)width * height * 4u, sizeof *texels);
	bool            ok     = t.light && t.failed && texels;
	if (ok)
		envmap_parallel_for((int)points, o->jobs, grid_point_task, &t);
	for (uint32_t i = 0; ok && i < points; ++i)
		if (t.failed[i]) {
			fprintf(stderr, "environment_bake: grid point %u: cannot use %s\n", i, job->grid_panoramas[i]);
			ok = false;
		}
	uint32_t invalid = 0, unfilled = 0;
	for (uint32_t i = 0; ok && i < points; ++i) {
		if (!job->invalid[i])
			continue;
		++invalid;
		const int x = (int)(i % r[0]), y = (int)(i / r[0] % r[1]), z = (int)(i / (r[0] * r[1]));
		int       used = 0;
		for (int radius = 1; radius <= 2 && used == 0; ++radius) {
			for (int dz = -radius; dz <= radius; ++dz)
				for (int dy = -radius; dy <= radius; ++dy)
					for (int dx = -radius; dx <= radius; ++dx) {
						const int nx = x + dx, ny = y + dy, nz = z + dz;
						if (nx < 0 || ny < 0 || nz < 0 || nx >= (int)r[0] || ny >= (int)r[1] ||
							nz >= (int)r[2])
							continue;
						const uint32_t n = (uint32_t)nx + r[0] * ((uint32_t)ny + r[1] * (uint32_t)nz);
						if (job->invalid[n])
							continue;
						for (int f = 0; f < GRID_TERMS; ++f)
							for (int c = 0; c < 3; ++c)
								t.light[i][f][c] += t.light[n][f][c];
						++used;
					}
		}
		for (int f = 0; f < GRID_TERMS; ++f)
			for (int c = 0; c < 3; ++c)
				t.light[i][f][c] = used ? t.light[i][f][c] / (float)used : 0.0f;
		unfilled += used == 0;
	}
	for (uint32_t i = 0; ok && i < points; ++i) {
		const uint32_t x = i % r[0], row = i / r[0];
		const uint16_t mean = float_to_half(luminance(t.light[i][GRID_FACES]));
		for (int f = 0; f < GRID_FACES; ++f) {
			uint16_t* texel = texels + ((size_t)row * width + (size_t)GRID_FACES * x + (uint32_t)f) * 4u;
			for (int c = 0; c < 3; ++c)
				texel[c] = float_to_half(t.light[i][f][c]);
			texel[3] = mean;
		}
	}
	if (ok) {
		const Ktx2WriteLevel level = { width, height, (const uint8_t*)texels,
									   (size_t)width * height * 4u * sizeof *texels };
		ok                         = write_ktx2_rgba16f_levels(o->grid_out, 1, 0, &level, 1, true);
		if (!ok)
			fprintf(stderr, "environment_bake: cannot write %s\n", o->grid_out);
	}
	if (ok) {
		printf("grid %s: %ux%ux%u points, %u inside geometry", o->grid_out, r[0], r[1], r[2], invalid);
		if (unfilled)
			printf(" (%u with no valid neighbour, left dark)", unfilled);
		printf(", %.2f MiB\n", file_size_mib(o->grid_out));
	}
	free(t.light);
	free(t.failed);
	free(texels);
	return ok;
}

static int run_probes(const BakeOptions* o) {
	if (!o->cubes_out || !o->grid_out || !o->bake_out) {
		usage();
		return 2;
	}
	const int size = o->specular_size_given ? o->specular_size : 256;
	if (!check_size("--specular-size", size, 16, 2048))
		return 2;
	if (o->specular_samples < 16 || o->specular_samples > 65536) {
		fprintf(stderr, "environment_bake: --specular-samples must be 16 to 65536\n");
		return 2;
	}
	ProbeJob job;
	if (!load_probe_job(o->probes_input, &job))
		return 2;

	AeronProbeCells cells;
	if (!AeronProbeCells_Build(&job.layout, &cells)) {
		fprintf(stderr, "environment_bake: out of memory\n");
		probe_job_free(&job);
		return 1;
	}
	printf("cells: %ux%ux%u of %.3g m, at most %u probes in one\n", cells.dims[0], cells.dims[1],
		   cells.dims[2], (double)cells.cell_size, cells.most_probes);
	if (cells.overflow_cells)
		fprintf(stderr,
				"environment_bake: warning: %u cells overlap more than %d probes. Those cells use only the %d "
				"smallest\n",
				cells.overflow_cells, AERON_PROBE_CELL_PROBES, AERON_PROBE_CELL_PROBES);
	AeronProbeCells_Free(&cells);

	if (!bake_probe_cubes(o, &job, size) || !bake_probe_grid(o, &job)) {
		probe_job_free(&job);
		return 1;
	}
	/* Write the probe layout used for the bake. The cooker embeds it with
	 * the images. The written text is parsed again with the parser that the
	 * cooker and the game use. */
	const size_t length = Aeron_FlightProbeLayoutFormat(&job.layout, NULL, 0);
	char*        text   = (char*)malloc(length + 1);
	FILE*        f      = fopen(o->bake_out, "wb");
	bool         ok     = text && f;
	if (ok) {
		Aeron_FlightProbeLayoutFormat(&job.layout, text, length + 1);
		ok = fwrite(text, 1, length, f) == length;
	}
	if (f && fclose(f) != 0)
		ok = false;
	AeronFlightProbeLayout check;
	char                   error[256] = "";
	if (ok && !Aeron_FlightProbeLayoutParse(text, length, &check, error, sizeof error)) {
		fprintf(stderr, "environment_bake: written %s is invalid: %s\n", o->bake_out, error);
		ok = false;
	} else if (!ok) {
		fprintf(stderr, "environment_bake: cannot write %s\n", o->bake_out);
	}
	free(text);
	probe_job_free(&job);
	if (!ok)
		return 1;
	printf("layout %s\n", o->bake_out);
	return 0;
}

/* ---- diagnostic cube ------------------------------------------------------- */

/* 5x7 glyphs, rows top to bottom, bit 4 = leftmost column. */
static const uint8_t* glyph(char c) {
	static const uint8_t plus[7]  = { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 };
	static const uint8_t minus[7] = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 };
	static const uint8_t x[7]     = { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 };
	static const uint8_t y[7]     = { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 };
	static const uint8_t z[7]     = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F };
	switch (c) {
		case '+':
			return plus;
		case '-':
			return minus;
		case 'X':
			return x;
		case 'Y':
			return y;
		default:
			return z;
	}
}

static int run_diagnostic(const BakeOptions* o) {
	if (!check_size("--radiance-size", o->radiance_size, 16, 4096))
		return 2;
	/* Viewer frame per face, so labels read correctly from inside the cube:
	 * outward normal, up; right = normal x up. */
	static const float       normals[6][3] = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },
											   { 0, -1, 0 }, { 0, 0, 1 },  { 0, 0, -1 } };
	static const float       ups[6][3]     = { { 0, 1, 0 },  { 0, 1, 0 }, { 0, 0, 1 },
											   { 0, 0, -1 }, { 0, 1, 0 }, { 0, 1, 0 } };
	static const float       colors[6][3]  = { { 1.0f, 0.15f, 0.15f }, { 0.15f, 0.8f, 0.8f },
											   { 0.15f, 1.0f, 0.15f }, { 0.8f, 0.15f, 0.8f },
											   { 0.15f, 0.15f, 1.0f }, { 0.9f, 0.9f, 0.15f } };
	static const char* const labels[6]     = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };

	EnvCube cube;
	if (!envmap_cube_alloc(&cube, o->radiance_size, envmap_mip_count(o->radiance_size))) {
		fprintf(stderr, "environment_bake: out of memory\n");
		return 1;
	}
	const int   n    = cube.size;
	const float cell = 0.08f; /* glyph pixel, in face tangent units */
	for (int face = 0; face < 6; ++face) {
		const float* nrm      = normals[face];
		const float* up       = ups[face];
		const float  right[3] = { nrm[1] * up[2] - nrm[2] * up[1], nrm[2] * up[0] - nrm[0] * up[2],
								  nrm[0] * up[1] - nrm[1] * up[0] };
		for (int y = 0; y < n; ++y) {
			for (int x = 0; x < n; ++x) {
				float d[3];
				envmap_texel_direction(face, x, y, n, d);
				const float dn       = d[0] * nrm[0] + d[1] * nrm[1] + d[2] * nrm[2];
				const float a        = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / dn;
				const float b        = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / dn;
				float       value[3] = { colors[face][0], colors[face][1], colors[face][2] };
				/* Grid every eighth of the face, for checking seams. */
				const float ga = (a + 1.0f) * 4.0f, gb = (b + 1.0f) * 4.0f;
				if (fabsf(ga - roundf(ga)) < 0.02f || fabsf(gb - roundf(gb)) < 0.02f)
					value[0] = value[1] = value[2] = 0.05f;
				/* Label: two glyphs of 5x7 cells with one cell of spacing. */
				const float left = -5.5f * cell, top = 3.5f * cell;
				const int   col = (int)floorf((a - left) / cell);
				const int   row = (int)floorf((top - b) / cell);
				if (a >= left && b <= top && col >= 0 && col < 11 && row >= 0 && row < 7 && col != 5) {
					const int      index = col / 6;
					const uint8_t* g     = glyph(labels[face][index]);
					if (g[row] & (0x10 >> (col % 6)))
						value[0] = value[1] = value[2] = 4.0f;
				}
				float* px = cube.faces[0][face] + ((size_t)y * n + x) * 3u;
				px[0]     = value[0];
				px[1]     = value[1];
				px[2]     = value[2];
			}
		}
	}
	envmap_build_box_mips(&cube);
	const bool ok = write_cube(o->diagnostic_out, &cube, o->jobs);
	envmap_cube_free(&cube);
	if (!ok)
		return 1;
	printf("diagnostic cube %s: %d px, faces labelled +X -X +Y -Y +Z -Z (-Z is forward, +Y up)\n",
		   o->diagnostic_out, n);
	return 0;
}

/* ---- sun sprite -------------------------------------------------------------- */

static void zero_border(float* rgb, int n, int margin) {
	for (int y = 0; y < n; ++y)
		for (int x = 0; x < n; ++x)
			if (x < margin || y < margin || x >= n - margin || y >= n - margin)
				memset(rgb + ((size_t)y * n + x) * 3u, 0, 3u * sizeof(float));
}

/* Converts a square HDR image to a KTX2 sprite of `size` pixels. */
static bool bake_sprite(const char* input, const char* output, int size, const char* format, int jobs) {
	float* src = NULL;
	int    w = 0, h = 0;
	char   err[512];
	if (!read_hdr_rgb(input, &src, &w, &h, err, sizeof err)) {
		fprintf(stderr, "environment_bake: %s\n", err);
		return false;
	}
	if (w != h) {
		fprintf(stderr, "environment_bake: sprite %s is %dx%d; sprites are square\n", input, w, h);
		free(src);
		return false;
	}
	if (!check_pixels(input, src, (size_t)w * h)) {
		free(src);
		return false;
	}

	const int levels                    = envmap_mip_count(size);
	float*    images[ENVMAP_MAX_LEVELS] = { 0 };
	int       sizes[ENVMAP_MAX_LEVELS];
	bool      ok = true;
	for (int level = 0; level < levels; ++level) {
		sizes[level]  = size >> level > 0 ? size >> level : 1;
		images[level] = (float*)calloc((size_t)sizes[level] * sizes[level] * 3u, sizeof(float));
		if (!images[level])
			ok = false;
	}
	if (ok) {
		/* Area resample: average k x k bilinear taps per output texel. */
		const int n   = sizes[0];
		const int sub = (w + n - 1) / n < 1 ? 1 : (w + n - 1) / n;
		for (int y = 0; y < n; ++y) {
			for (int x = 0; x < n; ++x) {
				double sum[3] = { 0.0, 0.0, 0.0 };
				for (int sy = 0; sy < sub; ++sy) {
					for (int sx = 0; sx < sub; ++sx) {
						float fx = ((float)x + ((float)sx + 0.5f) / (float)sub) * (float)w / (float)n - 0.5f;
						float fy = ((float)y + ((float)sy + 0.5f) / (float)sub) * (float)h / (float)n - 0.5f;
						fx       = fx < 0.0f ? 0.0f : (fx > (float)(w - 1) ? (float)(w - 1) : fx);
						fy       = fy < 0.0f ? 0.0f : (fy > (float)(h - 1) ? (float)(h - 1) : fy);
						const int   x0 = (int)fx, y0 = (int)fy;
						const int   x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
						const float ax = fx - (float)x0, ay = fy - (float)y0;
						for (int c = 0; c < 3; ++c) {
							const float top    = src[((size_t)y0 * w + x0) * 3u + c] * (1.0f - ax) +
												 src[((size_t)y0 * w + x1) * 3u + c] * ax;
							const float bottom = src[((size_t)y1 * w + x0) * 3u + c] * (1.0f - ax) +
												 src[((size_t)y1 * w + x1) * 3u + c] * ax;
							sum[c] += top * (1.0f - ay) + bottom * ay;
						}
					}
				}
				for (int c = 0; c < 3; ++c)
					images[0][((size_t)y * n + x) * 3u + c] = (float)(sum[c] / (double)(sub * sub));
			}
		}
		/* No faint square around the sun: zero the outer texels before
		 * compression, and the outermost ring of every mip. */
		const int margin = n / 64 > 2 ? n / 64 : 2;
		/* Sprites add to the sky, so any background level shows as a
		 * square: warn when the band inside the margin is not black. */
		const int band = n / 16 > 2 ? n / 16 : 2;
		double    peak = 0.0, edge = 0.0;
		int       edges = 0;
		for (int y = 0; y < n; ++y) {
			for (int x = 0; x < n; ++x) {
				const double l = luminance(images[0] + ((size_t)y * n + x) * 3u);
				peak           = l > peak ? l : peak;
				const int d    = x < y ? x : y;
				const int e    = (n - 1 - x) < (n - 1 - y) ? (n - 1 - x) : (n - 1 - y);
				const int dist = d < e ? d : e;
				if (dist >= margin && dist < margin + band) {
					edge += l;
					++edges;
				}
			}
		}
		edge = edges ? edge / edges : 0.0;
		if (peak > 0.0 && edge > 1e-4 * peak)
			fprintf(stderr,
					"environment_bake: warning: sprite background is not black (edge mean %.4g, peak %.4g); "
					"it adds a visible square to the sky\n",
					edge, peak);
		zero_border(images[0], n, margin);
		for (int level = 1; level < levels; ++level) {
			const int sn = sizes[level - 1], dn = sizes[level];
			for (int y = 0; y < dn; ++y)
				for (int x = 0; x < dn; ++x)
					for (int c = 0; c < 3; ++c) {
						const float* s = images[level - 1];
						images[level][((size_t)y * dn + x) * 3u + c] =
							0.25f * (s[((size_t)(2 * y) * sn + 2 * x) * 3u + c] +
									 s[((size_t)(2 * y) * sn + 2 * x + 1) * 3u + c] +
									 s[((size_t)(2 * y + 1) * sn + 2 * x) * 3u + c] +
									 s[((size_t)(2 * y + 1) * sn + 2 * x + 1) * 3u + c]);
					}
			if (dn >= 4)
				zero_border(images[level], dn, 1);
		}
		ok = write_levels(output, format, (const float* const*)images, sizes, 1, 0, levels, jobs);
	}
	for (int level = 0; level < levels; ++level)
		free(images[level]);
	free(src);
	if (ok)
		printf("sprite %s: %d px, %d levels, %s, %.2f MiB\n", output, size, levels, format,
			   file_size_mib(output));
	return ok;
}

static int run_sprite(const BakeOptions* o) {
	if (!o->sprite_out) {
		usage();
		return 2;
	}
	if (!check_size("--sprite-size", o->sprite_size, 16, 1024))
		return 2;
	if (strcmp(o->sprite_format, "bc6h") != 0 && strcmp(o->sprite_format, "rgba16f") != 0) {
		fprintf(stderr, "environment_bake: --sprite-format must be bc6h or rgba16f\n");
		return 2;
	}
	return bake_sprite(o->sprite_input, o->sprite_out, o->sprite_size, o->sprite_format, o->jobs) ? 0 : 1;
}

/* ---- capture ------------------------------------------------------------------ */

/* Face file suffixes in KTX2 face order. */
static const char* const k_capture_faces[6] = { "px", "nx", "py", "ny", "pz", "nz" };

/* A file name inside the capture directory: no separators, no leading dot. */
static bool is_plain_file_name(const char* name) {
	if (!name[0] || name[0] == '.')
		return false;
	for (const char* p = name; *p; ++p)
		if (*p == '/' || *p == '\\' || *p == ':' || (unsigned char)*p < 0x20)
			return false;
	return true;
}

/* A normalized mission key: lower-case, '/'-separated, no empty, '.' or
 * '..' components. */
static bool is_mission_key(const char* key) {
	if (!key[0] || strlen(key) >= 128)
		return false;
	const char* component = key;
	for (const char* p = key;; ++p) {
		if (*p == '/' || *p == '\0') {
			const size_t length = (size_t)(p - component);
			if (length == 0 || (length == 1 && component[0] == '.') ||
				(length == 2 && component[0] == '.' && component[1] == '.'))
				return false;
			if (*p == '\0')
				return true;
			component = p + 1;
		} else if (*p == '\\' || *p == '"' || (*p >= 'A' && *p <= 'Z') || (unsigned char)*p < 0x20) {
			return false;
		}
	}
}

static bool join_path(char* out, size_t size, const char* dir, const char* name) {
	const size_t length = strlen(dir);
	const bool   slash  = length > 0 && (dir[length - 1] == '/' || dir[length - 1] == '\\');
	const int    n      = snprintf(out, size, "%s%s%s", dir, slash ? "" : "/", name);
	if (n < 0 || (size_t)n >= size) {
		fprintf(stderr, "environment_bake: path %s/%s is too long\n", dir, name);
		return false;
	}
	return true;
}

typedef struct CaptureDescription {
	char                            environment[AERON_ENV_NAME_MAX];
	char                            mission[128];
	int                             region;
	int                             face_size;
	char                            sky_faces[64];
	AeronEnvironmentLightDefinition lights[AERON_ENV_MAX_LIGHTS];
	uint32_t                        light_count;
	int32_t                         primary;
} CaptureDescription;

static bool capture_error(const char* path, const AeronConfigNode* node, const char* message) {
	fprintf(stderr, "environment_bake: %s:%d: %s\n", path, node ? AeronConfigNode_Line(node) : 0, message);
	return false;
}

static bool capture_string(const char* path, const AeronConfigNode* root, const char* key, char* out,
						   size_t size) {
	const AeronConfigNode* node = AeronConfigNode_MapGet(root, key);
	char                   message[160];
	if (!node || AeronConfigNode_Type(node) != AERON_CONFIG_STRING) {
		snprintf(message, sizeof message, "'%s' must be a string", key);
		return capture_error(path, node ? node : root, message);
	}
	const char* value = AeronConfigNode_String(node, "");
	if (strlen(value) >= size) {
		snprintf(message, sizeof message, "'%s' must be shorter than %zu bytes", key, size);
		return capture_error(path, node, message);
	}
	snprintf(out, size, "%s", value);
	return true;
}

static bool capture_int(const char* path, const AeronConfigNode* root, const char* key, int64_t min,
						int64_t max, int* out) {
	const AeronConfigNode* node = AeronConfigNode_MapGet(root, key);
	const int64_t          value =
		node && AeronConfigNode_Type(node) == AERON_CONFIG_INT ? AeronConfigNode_Int(node, min - 1) : min - 1;
	if (value < min || value > max) {
		char message[160];
		snprintf(message, sizeof message, "'%s' must be an integer from %lld to %lld", key, (long long)min,
				 (long long)max);
		return capture_error(path, node ? node : root, message);
	}
	*out = (int)value;
	return true;
}

static bool load_capture(const char* path, CaptureDescription* c) {
	AeronVfs*        vfs    = NULL;
	AeronConfigFile* config = load_yaml(path, &vfs);
	if (!config)
		return false;
	static const char* const fields[] = {
		"version",   "environment",        "mission",         "region", "face_size",
		"sky_faces", "directional_lights", "capture_settings"
	};
	const AeronConfigNode* root = AeronConfigFile_Root(config);
	bool                   ok   = AeronConfigNode_Type(root) == AERON_CONFIG_MAP;
	if (!ok)
		capture_error(path, root, "the capture description must be a map");
	for (size_t i = 0; ok && i < AeronConfigNode_MapCount(root); ++i) {
		const char* key   = AeronConfigNode_MapKeyAt(root, i);
		bool        known = false;
		for (size_t k = 0; k < sizeof fields / sizeof fields[0]; ++k)
			known = known || strcmp(key, fields[k]) == 0;
		if (!known) {
			char message[160];
			snprintf(message, sizeof message, "unknown field '%s'", key);
			ok = capture_error(path, AeronConfigNode_MapValueAt(root, i), message);
		}
	}
	int version = 0;
	ok          = ok && capture_int(path, root, "version", 1, 1, &version) &&
				  capture_string(path, root, "environment", c->environment, sizeof c->environment) &&
				  capture_string(path, root, "mission", c->mission, sizeof c->mission) &&
				  capture_int(path, root, "region", 0, 3, &c->region) &&
				  capture_int(path, root, "face_size", 16, 8192, &c->face_size) &&
				  capture_string(path, root, "sky_faces", c->sky_faces, sizeof c->sky_faces);
	if (ok && !AeronEnvironment_IsValidName(c->environment))
		ok = capture_error(path, AeronConfigNode_MapGet(root, "environment"),
						   "'environment' must be 1 to 63 lower-case letters, digits, '.' and '_'");
	if (ok && !is_mission_key(c->mission))
		ok = capture_error(path, AeronConfigNode_MapGet(root, "mission"),
						   "'mission' must be a normalized mission key");
	if (ok && !is_power_of_two(c->face_size))
		ok = capture_error(path, AeronConfigNode_MapGet(root, "face_size"),
						   "'face_size' must be a power of two");
	if (ok && !is_plain_file_name(c->sky_faces))
		ok =
			capture_error(path, AeronConfigNode_MapGet(root, "sky_faces"), "'sky_faces' must be a file name");
	if (ok) {
		AeronConfigError error;
		memset(&error, 0, sizeof error);
		if (!AeronEnvironment_ParseLights(AeronConfigNode_MapGet(root, "directional_lights"), "capture",
										  c->lights, &c->light_count, &c->primary, &error)) {
			fprintf(stderr, "environment_bake: %s:%d: %s\n", path, error.line, error.message);
			ok = false;
		}
	}
	for (uint32_t i = 0; ok && i < c->light_count; ++i) {
		if (c->lights[i].visual == AERON_ENV_SUN_SPRITE && !is_plain_file_name(c->lights[i].sprite_path)) {
			fprintf(stderr,
					"environment_bake: %s: light '%s': the sprite must be a file in the capture directory\n",
					path, c->lights[i].name);
			ok = false;
		}
	}
	AeronConfigFile_Destroy(config);
	AeronVfs_Destroy(vfs);
	return ok;
}

/* Reads one capture face and box-reduces it into a radiance face of
 * `size` pixels. */
static bool read_capture_face(const char* path, int face_size, float* out, int size) {
	float* rgb = NULL;
	int    w = 0, h = 0;
	char   err[512];
	if (!read_hdr_rgb(path, &rgb, &w, &h, err, sizeof err)) {
		fprintf(stderr, "environment_bake: %s\n", err);
		return false;
	}
	if (w != face_size || h != face_size) {
		fprintf(stderr, "environment_bake: %s is %dx%d; the capture's face_size is %d\n", path, w, h,
				face_size);
		free(rgb);
		return false;
	}
	if (!check_pixels(path, rgb, (size_t)w * h)) {
		free(rgb);
		return false;
	}
	const int    k     = face_size / size;
	const double scale = 1.0 / ((double)k * k);
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			double sum[3] = { 0.0, 0.0, 0.0 };
			for (int sy = 0; sy < k; ++sy)
				for (int sx = 0; sx < k; ++sx)
					for (int c = 0; c < 3; ++c)
						sum[c] += rgb[(((size_t)y * k + sy) * face_size + (size_t)x * k + sx) * 3u + c];
			for (int c = 0; c < 3; ++c)
				out[((size_t)y * size + x) * 3u + c] = (float)(sum[c] * scale);
		}
	}
	free(rgb);
	return true;
}

/* Writes the xwa: mapping entry. Mission keys contain no quote or
 * backslash, so they are written quoted as they are. */
static bool write_mapping(const char* path, const CaptureDescription* c) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "environment_bake: cannot write %s\n", path);
		return false;
	}
	const bool ok = fprintf(f,
							"# Mapping for the captured region: merge under the manifest's xwa.missions.\n"
							"xwa:\n"
							"  missions:\n"
							"    \"%s\":\n"
							"      regions:\n"
							"        \"%d\": %s\n",
							c->mission, c->region, c->environment) > 0;
	if (fclose(f) != 0 || !ok) {
		fprintf(stderr, "environment_bake: cannot write %s\n", path);
		return false;
	}
	/* Syntax check only: the xwa: schema belongs to the game. */
	AeronVfs*        vfs    = NULL;
	AeronConfigFile* config = load_yaml(path, &vfs);
	if (!config)
		return false;
	AeronConfigFile_Destroy(config);
	AeronVfs_Destroy(vfs);
	return true;
}

static int run_capture(const BakeOptions* o) {
	if (!o->bake_root) {
		usage();
		return 2;
	}
	if (!check_size("--radiance-size", o->radiance_size, 16, 4096) ||
		!check_size("--diffuse-size", o->diffuse_size, 4, 256) ||
		!check_size("--specular-size", o->specular_size, 16, 2048) ||
		!check_size("--sprite-size", o->sprite_size, 16, 1024))
		return 2;
	if (o->specular_samples < 16 || o->specular_samples > 65536) {
		fprintf(stderr, "environment_bake: --specular-samples must be 16 to 65536\n");
		return 2;
	}
	char description_path[1024];
	if (!join_path(description_path, sizeof description_path, o->capture_dir, "capture.yaml"))
		return 2;
	CaptureDescription* c = (CaptureDescription*)calloc(1, sizeof *c);
	if (!c) {
		fprintf(stderr, "environment_bake: out of memory\n");
		return 1;
	}
	int status = 0;
	if (!load_capture(description_path, c)) {
		free(c);
		return 1;
	}

	/* Every input must exist before anything is written. */
	for (uint32_t i = 0; i < 6u + c->light_count; ++i) {
		char name[AERON_ENV_PATH_MAX], input[1024];
		if (i < 6u)
			snprintf(name, sizeof name, "%s_%s.hdr", c->sky_faces, k_capture_faces[i]);
		else if (c->lights[i - 6u].visual == AERON_ENV_SUN_SPRITE)
			snprintf(name, sizeof name, "%s", c->lights[i - 6u].sprite_path);
		else
			continue;
		FILE* f = join_path(input, sizeof input, o->capture_dir, name) ? fopen(input, "rb") : NULL;
		if (!f) {
			fprintf(stderr, "environment_bake: cannot open %s/%s\n", o->capture_dir, name);
			free(c);
			return 1;
		}
		fclose(f);
	}

	/* The capture is never upsampled: product sizes are capped by the face. */
	const int radiance_size = o->radiance_size < c->face_size ? o->radiance_size : c->face_size;
	const int specular_size = o->specular_size < radiance_size ? o->specular_size : radiance_size;
	if (radiance_size < o->radiance_size)
		fprintf(stderr, "environment_bake: warning: %d px capture faces limit the sky to %d px\n",
				c->face_size, radiance_size);

	/* Output directory, relative to the bake root as manifest paths are. */
	char mission_dir[128];
	snprintf(mission_dir, sizeof mission_dir, "%s", c->mission);
	for (char* p = mission_dir; *p; ++p)
		if (*p == '/')
			*p = '_';
	char relative_dir[AERON_ENV_PATH_MAX];
	if (snprintf(relative_dir, sizeof relative_dir, "flight/environments/captured/%s/region_%d", mission_dir,
				 c->region) >= (int)sizeof relative_dir - 32) {
		fprintf(stderr, "environment_bake: mission key %s is too long\n", c->mission);
		free(c);
		return 1;
	}
	/* The bake root is opened as a writable user root. */
	AeronVfsConfig vfs_config;
	memset(&vfs_config, 0, sizeof vfs_config);
	vfs_config.user_root = o->bake_root;
	AeronVfs* vfs        = AeronVfs_Create(&vfs_config);
	if (!vfs || !AeronVfs_CreateDirectory(vfs, AERON_VFS_ROOT_USER, relative_dir)) {
		fprintf(stderr, "environment_bake: cannot create %s under %s\n", relative_dir, o->bake_root);
		AeronVfs_Destroy(vfs);
		free(c);
		return 1;
	}
	AeronVfs_Destroy(vfs);
	char output_dir[1024];
	if (!join_path(output_dir, sizeof output_dir, o->bake_root, relative_dir)) {
		free(c);
		return 1;
	}
	printf("capture %s: %s region %d, %d px faces, %u lights -> %s\n", o->capture_dir, c->mission, c->region,
		   c->face_size, c->light_count, output_dir);

	/* Sun sprites first: a missing image fails before the long filtering. */
	for (uint32_t i = 0; status == 0 && i < c->light_count; ++i) {
		AeronEnvironmentLightDefinition* light = &c->lights[i];
		if (light->visual != AERON_ENV_SUN_SPRITE)
			continue;
		char input[1024], name[AERON_ENV_NAME_MAX + 8], output[1024];
		snprintf(name, sizeof name, "%s.ktx2", light->name);
		if (!join_path(input, sizeof input, o->capture_dir, light->sprite_path) ||
			!join_path(output, sizeof output, output_dir, name) ||
			!bake_sprite(input, output, o->sprite_size, "bc6h", o->jobs)) {
			status = 1;
			break;
		}
		snprintf(light->sprite_path, sizeof light->sprite_path, "%s/%s", relative_dir, name);
	}

	EnvCube radiance;
	memset(&radiance, 0, sizeof radiance);
	if (status == 0 && !envmap_cube_alloc(&radiance, radiance_size, envmap_mip_count(radiance_size))) {
		fprintf(stderr, "environment_bake: out of memory\n");
		status = 1;
	}
	for (int face = 0; status == 0 && face < 6; ++face) {
		char name[96], input[1024];
		snprintf(name, sizeof name, "%s_%s.hdr", c->sky_faces, k_capture_faces[face]);
		if (!join_path(input, sizeof input, o->capture_dir, name) ||
			!read_capture_face(input, c->face_size, radiance.faces[0][face], radiance_size))
			status = 1;
	}
	char sky_path[AERON_ENV_PATH_MAX], diffuse_path[AERON_ENV_PATH_MAX], specular_path[AERON_ENV_PATH_MAX];
	snprintf(sky_path, sizeof sky_path, "%s/sky.ktx2", relative_dir);
	snprintf(diffuse_path, sizeof diffuse_path, "%s/diffuse.ktx2", relative_dir);
	snprintf(specular_path, sizeof specular_path, "%s/specular.ktx2", relative_dir);
	char sky_out[1024], diffuse_out[1024], specular_out[1024], environment_out[1024], mapping_out[1024];
	if (status == 0 && (!join_path(sky_out, sizeof sky_out, output_dir, "sky.ktx2") ||
						!join_path(diffuse_out, sizeof diffuse_out, output_dir, "diffuse.ktx2") ||
						!join_path(specular_out, sizeof specular_out, output_dir, "specular.ktx2") ||
						!join_path(environment_out, sizeof environment_out, output_dir, "environment.yaml") ||
						!join_path(mapping_out, sizeof mapping_out, output_dir, "mapping.yaml")))
		status = 1;
	if (status == 0) {
		/* bake_cubes frees the radiance cube. */
		if (!bake_cubes(o, &radiance, specular_size, c->lights, c->light_count, sky_out, diffuse_out,
						specular_out))
			status = 1;
	} else {
		envmap_cube_free(&radiance);
	}

	if (status == 0) {
		FILE* f = fopen(environment_out, "wb");
		if (!f || !write_environment_entry(f, c->environment, sky_path, diffuse_path, specular_path, true,
										   c->lights, c->light_count, c->primary)) {
			fprintf(stderr, "environment_bake: cannot write %s\n", environment_out);
			status = 1;
		}
		if (f && fclose(f) != 0)
			status = 1;
	}
	if (status == 0 && (!validate_environment_file(environment_out) || !write_mapping(mapping_out, c)))
		status = 1;
	if (status == 0)
		printf("environment %s: %s\nmapping: %s\n", c->environment, environment_out, mapping_out);
	free(c);
	return status;
}

int main(int argc, char** argv) {
	BakeOptions o;
	if (argc < 2 || !parse_args(argc, argv, &o)) {
		usage();
		return 2;
	}
	const int modes = (o.input != NULL) + (o.diagnostic_out != NULL) + (o.sprite_input != NULL) +
					  (o.capture_dir != NULL) + (o.probes_input != NULL);
	if (modes != 1) {
		fprintf(stderr,
				"environment_bake: give exactly one of --input, --diagnostic-faces, --sprite, --capture "
				"or --probes\n");
		usage();
		return 2;
	}
	if (o.diagnostic_out)
		return run_diagnostic(&o);
	if (o.sprite_input)
		return run_sprite(&o);
	if (o.capture_dir)
		return run_capture(&o);
	if (o.probes_input)
		return run_probes(&o);
	return run_bake(&o);
}
