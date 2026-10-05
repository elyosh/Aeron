/*
 * environment_bake — offline HDR environment package baker.
 *
 * Bake mode: an equirectangular Radiance .hdr sky without suns and an
 * optional light list become three BC6H KTX2 cubes (radiance with box
 * mips, Lambert irradiance / pi, GGX roughness mips) and a `packages:`
 * entry. Diagnostic mode writes a labelled cube for checking orientation.
 * Sprite mode converts an HDR image to a KTX2 sun sprite.
 *
 * See envmap.h for the cube face and panorama conventions. Output is
 * deterministic for identical inputs and does not depend on --jobs.
 */

#include "aeron/config_file.h"
#include "aeron/scene/environment.h"
#include "aeron/vfs.h"
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
	const char* package_out;
	const char* manifest_root;
	const char* diagnostic_out;
	const char* sprite_input;
	const char* sprite_out;
	const char* sprite_format;
	int         radiance_size;
	int         diffuse_size;
	int         specular_size;
	int         specular_samples;
	int         sprite_size;
	int         jobs;
} BakeOptions;

static void usage(void) {
	fprintf(
		stderr,
		"usage:\n"
		"  environment_bake --input sky.hdr --name NAME --radiance-out P --diffuse-out P --specular-out P\n"
		"                   [--lights-input lights.yaml] [--package-out FILE] [--manifest-root DIR]\n"
		"                   [--radiance-size 2048] [--diffuse-size 32] [--specular-size 512]\n"
		"                   [--specular-samples 1024] [--jobs 8]\n"
		"  environment_bake --diagnostic-faces out.ktx2 [--radiance-size 2048] [--jobs 8]\n"
		"  environment_bake --sprite in.hdr --sprite-out out.ktx2 [--sprite-size 256]\n"
		"                   [--sprite-format bc6h|rgba16f] [--jobs 8]\n"
		"\n"
		"The sky is an equirectangular panorama (2:1) without suns, in the authored\n"
		"frame: +X right, +Y up, -Z forward at the image centre. The light list is a\n"
		"YAML map with a `directional_lights` sequence in the package light schema.\n"
		"Package entry paths are written relative to --manifest-root when given.\n");
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
		else if (strcmp(arg, "--package-out") == 0)
			text = &o->package_out;
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
	const float* const* images; /* [level * face_count + face], RGB */
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
	uint8_t*          out = t->blobs[level] + (size_t)face * t->face_bytes[level] + (size_t)by * blocks * 16u;
	/* A run of identical constant blocks (black space, flat backgrounds)
	 * reuses the previous encoding: same input, same output. */
	float last_constant[3];
	bool  have_constant = false;
	for (int bx = 0; bx < blocks; ++bx) {
		float block[48];
		for (int py = 0; py < 4; ++py) {
			const int y = by * 4 + py < n ? by * 4 + py : n - 1;
			for (int px = 0; px < 4; ++px) {
				const int x = bx * 4 + px < n ? bx * 4 + px : n - 1;
				for (int c = 0; c < 3; ++c) {
					float v                      = src[((size_t)y * n + x) * 3u + c];
					block[(py * 4 + px) * 3 + c] = v > 0.0f ? (v < 65504.0f ? v : 65504.0f) : 0.0f;
				}
			}
		}
		bool constant = true;
		for (int i = 3; i < 48 && constant; ++i)
			constant = block[i] == block[i % 3];
		if (constant && have_constant && memcmp(block, last_constant, sizeof last_constant) == 0) {
			memcpy(out + (size_t)bx * 16u, out + (size_t)(bx - 1) * 16u, 16u);
			continue;
		}
		bc6h_codec_encode_block(out + (size_t)bx * 16u, block, BC6H_QUALITY_UBER);
		have_constant = constant;
		if (constant)
			memcpy(last_constant, block, sizeof last_constant);
	}
}

/* Writes square RGB float images as BC6H (format "bc6h") or RGBA16F KTX2. */
static bool write_levels(const char* path, const char* format, const float* const* images, const int* sizes,
						 int face_count, int level_count, int jobs) {
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
		ok = bc6h ? write_ktx2_bc6h_levels(path, face_count, levels, level_count, true)
				  : write_ktx2_rgba16f_levels(path, face_count, levels, level_count, true);
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
	return write_levels(path, "bc6h", images, sizes, 6, cube->level_count, jobs);
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

/* Output path as written in the package: relative to the manifest root,
 * with forward slashes. */
static bool package_path(const char* path, const char* root, char* out, size_t size) {
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
		fprintf(stderr, "environment_bake: package path %s is too long\n", relative);
		return false;
	}
	snprintf(out, size, "%s", relative);
	for (char* p = out; *p; ++p)
		if (*p == '\\')
			*p = '/';
	return true;
}

/* Writes the baked package as a `packages:` document. Strengths, floor and
 * rotation are starting values to calibrate per package. */
static bool write_package_entry(FILE* f, const BakeOptions* o, const char* radiance, const char* diffuse,
								const char* specular, const AeronEnvironmentLightDefinition* lights,
								uint32_t light_count, int32_t primary) {
	AeronEnvironmentDefinition* d = (AeronEnvironmentDefinition*)calloc(1, sizeof *d);
	if (!d)
		return false;
	snprintf(d->name, sizeof d->name, "%s", o->name);
	d->kind = AERON_ENV_KIND_FULL;
	snprintf(d->radiance_path, sizeof d->radiance_path, "%s", radiance);
	snprintf(d->diffuse_path, sizeof d->diffuse_path, "%s", diffuse);
	snprintf(d->specular_path, sizeof d->specular_path, "%s", specular);
	d->sky_exposure        = 1.0f;
	d->diffuse_strength    = 1.0f;
	d->specular_strength   = 1.0f;
	d->ambient_fill[0] = d->ambient_fill[1] = d->ambient_fill[2] = 0.02f;
	memcpy(d->lights, lights, sizeof d->lights);
	d->light_count         = light_count;
	d->primary_light_index = primary;
	const size_t length    = AeronEnvironment_FormatPackageYaml(d, NULL, 0);
	char*        text      = (char*)malloc(length + 1);
	bool         ok        = text != NULL;
	if (ok) {
		AeronEnvironment_FormatPackageYaml(d, text, length + 1);
		ok = fwrite(text, 1, length, f) == length;
	}
	free(text);
	free(d);
	return ok;
}

/* Re-parses a written package file with the runtime parser. */
static bool validate_package_file(const char* path) {
	AeronVfs*        vfs    = NULL;
	AeronConfigFile* config = load_yaml(path, &vfs);
	if (!config)
		return false;
	AeronEnvironmentDefinitionSet* set = NULL;
	AeronConfigError               error;
	memset(&error, 0, sizeof error);
	const bool ok = AeronEnvironment_ParsePackages(
		AeronConfigNode_MapGet(AeronConfigFile_Root(config), "packages"), &set, &error);
	if (!ok)
		fprintf(stderr, "environment_bake: written package %s:%d: %s\n", path, error.line, error.message);
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

static int run_bake(const BakeOptions* o) {
	if (!o->name || !o->radiance_out || !o->diffuse_out || !o->specular_out) {
		usage();
		return 2;
	}
	if (!AeronEnvironment_IsValidName(o->name)) {
		fprintf(stderr, "environment_bake: --name must be 1 to 63 lower-case letters, digits, '.' and '_'\n");
		return 2;
	}
	if (!check_size("--radiance-size", o->radiance_size, 16, 4096) ||
		!check_size("--diffuse-size", o->diffuse_size, 4, 256) ||
		!check_size("--specular-size", o->specular_size, 16, 2048))
		return 2;
	if (o->specular_size > o->radiance_size) {
		fprintf(stderr, "environment_bake: --specular-size must not exceed --radiance-size\n");
		return 2;
	}
	if (o->specular_samples < 16 || o->specular_samples > 65536) {
		fprintf(stderr, "environment_bake: --specular-samples must be 16 to 65536\n");
		return 2;
	}
	char radiance_path[256], diffuse_path[256], specular_path[256];
	if (!package_path(o->radiance_out, o->manifest_root, radiance_path, sizeof radiance_path) ||
		!package_path(o->diffuse_out, o->manifest_root, diffuse_path, sizeof diffuse_path) ||
		!package_path(o->specular_out, o->manifest_root, specular_path, sizeof specular_path))
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
	for (size_t i = 0; i < (size_t)w * h * 3u; ++i) {
		if (!isfinite(pano[i]) || pano[i] < 0.0f) {
			fprintf(stderr, "environment_bake: %s has a non-finite or negative pixel at %zu\n", o->input,
					i / 3u);
			free(pano);
			return 1;
		}
	}
	if ((double)w < BAKE_PI * (double)o->radiance_size)
		fprintf(stderr,
				"environment_bake: warning: %dx%d panorama is below pi x %d wide; the sky will be soft\n", w,
				h, o->radiance_size);

	EnvCube radiance, diffuse, specular;
	if (!envmap_cube_alloc(&radiance, o->radiance_size, envmap_mip_count(o->radiance_size)) ||
		!envmap_cube_alloc(&diffuse, o->diffuse_size, envmap_mip_count(o->diffuse_size)) ||
		!envmap_cube_alloc(&specular, o->specular_size, envmap_mip_count(o->specular_size))) {
		fprintf(stderr, "environment_bake: out of memory\n");
		free(pano);
		return 1;
	}
	printf("input %s: %dx%d\n", o->input, w, h);
	envmap_from_equirect(&radiance, pano, w, h, o->jobs);
	free(pano);
	envmap_build_box_mips(&radiance);
	envmap_irradiance(&diffuse, &radiance, o->jobs);
	envmap_build_box_mips(&diffuse);
	envmap_ggx_prefilter(&specular, &radiance, o->specular_samples, o->jobs);

	const CubeStats stats = cube_stats(&radiance);
	printf("sky luminance: peak %.6g, mean %.6g\n", stats.peak, stats.mean);
	for (uint32_t i = 0; i < light_count; ++i) {
		const double radius   = fmax(2.0 * lights[i].angular_radius_degrees, 1.0);
		const double fraction = light_energy_fraction(&radiance, &lights[i], radius);
		printf("light %s: sky within %.2g deg holds %.3g%% of its irradiance\n", lights[i].name, radius,
			   fraction * 100.0);
		if (fraction > 0.1)
			fprintf(stderr,
					"environment_bake: warning: light '%s': the sky is bright around its direction; "
					"a sun may have been left in the image\n",
					lights[i].name);
	}

	int status = 0;
	if (!write_cube(o->radiance_out, &radiance, o->jobs) || !write_cube(o->diffuse_out, &diffuse, o->jobs) ||
		!write_cube(o->specular_out, &specular, o->jobs))
		status = 1;
	if (status == 0) {
		printf("radiance %s: %d px, %d levels, %.2f MiB\n", o->radiance_out, radiance.size,
			   radiance.level_count, file_size_mib(o->radiance_out));
		printf("diffuse  %s: %d px, %d levels, exact irradiance over %d px faces, %.2f MiB\n", o->diffuse_out,
			   diffuse.size, diffuse.level_count, envmap_level_size(&radiance, stats_level(&radiance, 64)),
			   file_size_mib(o->diffuse_out));
		printf("specular %s: %d px, %d roughness levels, %d samples, %.2f MiB\n", o->specular_out,
			   specular.size, specular.level_count, o->specular_samples, file_size_mib(o->specular_out));
	}
	envmap_cube_free(&radiance);
	envmap_cube_free(&diffuse);
	envmap_cube_free(&specular);
	if (status != 0)
		return status;

	FILE* f = o->package_out ? fopen(o->package_out, "wb") : stdout;
	if (!f) {
		fprintf(stderr, "environment_bake: cannot write %s\n", o->package_out);
		return 1;
	}
	const bool written =
		write_package_entry(f, o, radiance_path, diffuse_path, specular_path, lights, light_count, primary);
	if (o->package_out) {
		fclose(f);
		if (!written) {
			fprintf(stderr, "environment_bake: cannot write %s\n", o->package_out);
			return 1;
		}
		if (!validate_package_file(o->package_out))
			return 1;
		printf("package %s: %s\n", o->name, o->package_out);
	}
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
	float* src = NULL;
	int    w = 0, h = 0;
	char   err[512];
	if (!read_hdr_rgb(o->sprite_input, &src, &w, &h, err, sizeof err)) {
		fprintf(stderr, "environment_bake: %s\n", err);
		return 1;
	}
	if (w != h) {
		fprintf(stderr, "environment_bake: sprite %s is %dx%d; sprites are square\n", o->sprite_input, w, h);
		free(src);
		return 1;
	}
	for (size_t i = 0; i < (size_t)w * h * 3u; ++i) {
		if (!isfinite(src[i]) || src[i] < 0.0f) {
			fprintf(stderr, "environment_bake: %s has a non-finite or negative pixel\n", o->sprite_input);
			free(src);
			return 1;
		}
	}

	const int levels                    = envmap_mip_count(o->sprite_size);
	float*    images[ENVMAP_MAX_LEVELS] = { 0 };
	int       sizes[ENVMAP_MAX_LEVELS];
	bool      ok = true;
	for (int level = 0; level < levels; ++level) {
		sizes[level]  = o->sprite_size >> level > 0 ? o->sprite_size >> level : 1;
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
		ok = write_levels(o->sprite_out, o->sprite_format, (const float* const*)images, sizes, 1, levels,
						  o->jobs);
	}
	for (int level = 0; level < levels; ++level)
		free(images[level]);
	free(src);
	if (!ok)
		return 1;
	printf("sprite %s: %d px, %d levels, %s, %.2f MiB\n", o->sprite_out, o->sprite_size, levels,
		   o->sprite_format, file_size_mib(o->sprite_out));
	return 0;
}

int main(int argc, char** argv) {
	BakeOptions o;
	if (argc < 2 || !parse_args(argc, argv, &o)) {
		usage();
		return 2;
	}
	const int modes = (o.input != NULL) + (o.diagnostic_out != NULL) + (o.sprite_input != NULL);
	if (modes != 1) {
		fprintf(stderr, "environment_bake: give exactly one of --input, --diagnostic-faces or --sprite\n");
		usage();
		return 2;
	}
	if (o.diagnostic_out)
		return run_diagnostic(&o);
	if (o.sprite_input)
		return run_sprite(&o);
	return run_bake(&o);
}
