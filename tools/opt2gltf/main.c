/*
 * opt2gltf — convert one OPT file (or every OPT in a directory) to
 * glTF 2.0 + .bin + .png assets.
 *
 * Usage:
 *   opt2gltf [--smooth-angle DEG] [--emissive] [--fullbright STRENGTH]
 *            [--tag N=TAG]... [--subdivide N] <input.opt|input_dir> <output_dir>
 *
 * If the input is a directory, every *.OPT inside (case-insensitive)
 * is converted and emitted under <output_dir>/<basename>/. If a single
 * OPT file, the output goes directly into <output_dir>/. A file that fails
 * does not stop the others, but the exit status is then non-zero.
 *
 * Replacement workflow (both tools are built with AERON_BUILD_TOOLS):
 *
 *   opt2gltf FLIGHTMODELS out
 *   aeron_gltf_cook out/XWING/XWING.gltf --out <asset root>/remaster/models/xwing.glb
 *
 * The cooked GLB carries the AERON_flight_model extension with cooked
 * simulation and render data, and replaces the OPT whose lower-cased
 * basename it has. Conversion keeps the component order and every mesh's
 * type, explosion flags, descriptor, target, rotation frame, hardpoints and
 * engine glows, the LOD 0 geometry and the textures. A mesh without LOD 0
 * triangles is an error naming the file and mesh, so components stay
 * one-to-one with the OPT meshes. Expected differences from the stock OPT:
 *   - quads become two triangles sharing corner 0 and the face normal;
 *   - only LOD 0 is kept;
 *   - positions round-trip through metres and differ at the ulp level;
 *   - the trailing MESHVERTS box follows the game's replacement policy;
 *   - engine-glow axes round-trip through a quaternion and come back
 *     orthonormalized;
 *   - components carry no tags unless --tag names them, so tag-driven
 *     behaviour such as the crew animation is off.
 *
 * --tag N=TAG (repeatable) adds TAG ([a-z0-9._]{1,63}) to the component
 * built from OPT mesh N, e.g. --tag 6=crew.side --tag 9=crew.forward.
 *
 * --subdivide N (development only, N <= 8) splits every triangle into four
 * by its edge midpoints N times, multiplying the face count by 4^N, to
 * stress-test BVH, load and memory costs with stock silhouettes. Midpoints
 * are shared across edges so the mesh stays watertight; UVs are
 * interpolated, normals interpolated and normalized; descriptors,
 * hardpoints, glows and rotations are unchanged. Such models are test
 * content and are not meant to ship.
 *
 * --smooth-angle F regenerates vertex normals from geometry with an
 * F-degree smoothing threshold. Stored vertex and face normals are ignored.
 * Connected smoothing fans are built across shared edges, then averaged
 * with corner-angle weights so triangulation density does not bias them;
 * wrapped/non-manifold fans split before a normal can point behind its
 * rendered triangle.
 * The default is OFF (negative) — the original 1998 normals (full-Gouraud,
 * no threshold) are kept, preserving the rounded look.
 * Stored normals reproduce the classic renderer's per-FaceData position
 * remap: the first face-order normal for a position is reused by later
 * corners, preventing dormant conflicting normals from becoming glTF
 * seams. Pass e.g. --smooth-angle 45 for crisp flat panels, or 90 for a
 * smoother low-poly look without accepting back-facing neighbours.
 *
 * --emissive exports self-illumination (lit windows, cockpit lights, etc.):
 * textures with self-lit texels get a second *_emissive.png wired as the
 * material's emissiveTexture. Off by default. For XWA (version 5) OPTs this
 * replicates the engine's lightmap derivation EXACTLY
 * (ModelTexture_FilterHardwarePalette @ 0x44A600 + CreateD3DfromTexture):
 * an entry is self-lit when its darkest palette row is bright (squared
 * 5-bit magnitude >= 32, green LSB dropped) and rows 1..6 stay within
 * squared distance 16 of row 0; glow color from palette row 10; textures
 * named "_*" and palettes where none/all 256 entries classify are skipped.
 *
 * --fullbright STRENGTH (> 0) makes every material fully self-lit, as the
 * classic renderer draws projectiles: black base color with the texture's
 * alpha kept, the base texture as emissiveTexture, and STRENGTH as
 * KHR_materials_emissive_strength. Scene lighting then adds only a faint
 * specular. It supersedes --emissive.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "opt.h"
#include "opt2gltf.h"

static int ends_with_icase(const char* s, const char* suffix) {
	size_t ls = strlen(s), lf = strlen(suffix);
	if (lf > ls)
		return 0;
	return strcasecmp(s + ls - lf, suffix) == 0;
}

static void strip_ext(char* dst, size_t cap, const char* fname) {
	snprintf(dst, cap, "%s", fname);
	char* dot = strrchr(dst, '.');
	if (dot)
		*dot = '\0';
}

static int convert_one(const char* opt_path, const char* out_dir, const char* basename,
					   const OptGltfBuildOptions* options) {
	opt_error_t err;
	opt_file_t* opt = opt_load_file(opt_path, &err);
	if (!opt) {
		fprintf(stderr, "opt2gltf: %s: %s\n", opt_path, err.msg);
		return 0;
	}
	int ok = opt2gltf_convert(opt, out_dir, basename, options, opt_path) ? 1 : 0;
	opt_free(opt);
	return ok;
}

int main(int argc, char** argv) {
	/* Default: keep the original 1998 OPT normals (full-Gouraud rounded
	 * look). Pass --smooth-angle DEG to regenerate with a threshold. */
	float smooth_angle_deg = -1.0f;
	/* Classic position-normal remap + canonical-normal repair are on by
	 * default (stored-normals mode); --no-normal-repair emits the source
	 * per-corner data verbatim for diagnostics. */
	bool repair_normals = true;
	/* Self-illumination export is opt-in. */
	bool emissive = false;
	/* Fullbright export is opt-in; 0 leaves materials lit. */
	float fullbright_strength = 0.0f;
	/* Tags point into argv. */
	OptGltfComponentTag* tags             = NULL;
	size_t               tag_count        = 0;
	unsigned long        subdivide_levels = 0;
	int                  ai               = 1;
	/* Optional conversion flags, in any order before the positional args. */
	while (ai < argc && argv[ai][0] == '-') {
		if (strcmp(argv[ai], "--smooth-angle") == 0) {
			if (ai + 1 >= argc) {
				fprintf(stderr, "opt2gltf: --smooth-angle needs a value\n");
				return 2;
			}
			smooth_angle_deg = strtof(argv[ai + 1], NULL);
			ai += 2;
		} else if (strcmp(argv[ai], "--no-normal-repair") == 0) {
			repair_normals = false;
			ai += 1;
		} else if (strcmp(argv[ai], "--emissive") == 0) {
			emissive = true;
			ai += 1;
		} else if (strcmp(argv[ai], "--fullbright") == 0) {
			char* end           = NULL;
			fullbright_strength = ai + 1 < argc ? strtof(argv[ai + 1], &end) : 0.0f;
			if (ai + 1 >= argc || end == argv[ai + 1] || *end || !isfinite(fullbright_strength) ||
				fullbright_strength <= 0.0f) {
				fprintf(stderr, "opt2gltf: --fullbright needs a positive strength\n");
				free(tags);
				return 2;
			}
			ai += 2;
		} else if (strcmp(argv[ai], "--tag") == 0) {
			char*         equals = ai + 1 < argc ? strchr(argv[ai + 1], '=') : NULL;
			char*         end    = NULL;
			unsigned long mesh   = equals ? strtoul(argv[ai + 1], &end, 10) : 0;
			if (!equals || end != equals || equals == argv[ai + 1] || mesh > UINT32_MAX) {
				fprintf(stderr, "opt2gltf: --tag needs N=TAG\n");
				free(tags);
				return 2;
			}
			OptGltfComponentTag* grown = (OptGltfComponentTag*)realloc(tags, (tag_count + 1) * sizeof *tags);
			if (!grown) {
				free(tags);
				return 1;
			}
			tags                       = grown;
			tags[tag_count].mesh_index = (uint32_t)mesh;
			tags[tag_count++].tag      = equals + 1;
			ai += 2;
		} else if (strcmp(argv[ai], "--subdivide") == 0) {
			char* end        = NULL;
			subdivide_levels = ai + 1 < argc ? strtoul(argv[ai + 1], &end, 10) : 0;
			if (ai + 1 >= argc || end == argv[ai + 1] || *end || subdivide_levels > OPT_GLTF_MAX_SUBDIVIDE) {
				fprintf(stderr, "opt2gltf: --subdivide needs a level from 0 to %u\n", OPT_GLTF_MAX_SUBDIVIDE);
				free(tags);
				return 2;
			}
			ai += 2;
		} else {
			fprintf(stderr, "opt2gltf: unknown option '%s'\n", argv[ai]);
			free(tags);
			return 2;
		}
	}
	if (argc - ai != 2) {
		fprintf(stderr,
				"usage: %s [--smooth-angle DEG] [--no-normal-repair] "
				"[--emissive] [--fullbright STRENGTH] [--tag N=TAG]... [--subdivide N] "
				"<input.OPT|input_dir> <output_dir>\n",
				argv[0]);
		free(tags);
		return 2;
	}
	const OptGltfBuildOptions options = {
		.smooth_angle_degrees = smooth_angle_deg,
		.repair_normals       = repair_normals,
		.emissive             = emissive,
		.fullbright_strength  = fullbright_strength,
		.component_tags       = tags,
		.component_tag_count  = tag_count,
		.subdivide_levels     = (uint32_t)subdivide_levels,
	};
	int         status;
	const char* in   = argv[ai];
	const char* outd = argv[ai + 1];

	struct stat st;
	if (stat(in, &st) != 0) {
		fprintf(stderr, "opt2gltf: cannot stat '%s': %s\n", in, strerror(errno));
		free(tags);
		return 1;
	}

	if (S_ISDIR(st.st_mode)) {
		DIR* d = opendir(in);
		if (!d) {
			fprintf(stderr, "opt2gltf: cannot open dir '%s'\n", in);
			free(tags);
			return 1;
		}
		int            ok_count = 0, fail_count = 0;
		struct dirent* de;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			if (!ends_with_icase(de->d_name, ".opt"))
				continue;
			char src[1024];
			snprintf(src, sizeof src, "%s/%s", in, de->d_name);
			char base[256];
			strip_ext(base, sizeof base, de->d_name);
			char out_sub[1024];
			snprintf(out_sub, sizeof out_sub, "%s/%s", outd, base);
			if (convert_one(src, out_sub, base, &options))
				++ok_count;
			else
				++fail_count;
		}
		closedir(d);
		fprintf(stderr, "opt2gltf: %d ok, %d failed\n", ok_count, fail_count);
		status = fail_count == 0 ? 0 : 1;
	} else {
		/* Single file. */
		const char* slash = strrchr(in, '/');
		const char* fname = slash ? slash + 1 : in;
		char        base[256];
		strip_ext(base, sizeof base, fname);
		status = convert_one(in, outd, base, &options) ? 0 : 1;
	}
	free(tags);
	return status;
}
