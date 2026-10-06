/*
 * opt2gltf — convert one OPT file (or every OPT in a directory) to
 * glTF 2.0 + .bin + .png assets.
 *
 * Usage:
 *   opt2gltf [--smooth-angle DEG] [--emissive] [--fullbright STRENGTH]
 *            [--tag N=TAG]... [--subdivide N] [--cockpit-eye X,Y,Z]
 *            [--xwau-root DIR]...
 *            <input.opt|input_dir> <output_dir>
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
 * --cockpit-eye X,Y,Z (single OPT only) sets the pilot eye in craft-table
 * units, as XWAU's CockpitPovX/Y/Z: the OPT's Cockpit (type 31) hardpoints
 * are replaced by one at (X, -Y, Z) on the first MainHull mesh (mesh 0
 * without one). The game reads the eye from the craft model's Cockpit
 * hardpoint; OpenXWA applies all three axes for GLB replacements.
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
 *
 * --xwau-root DIR (repeatable) converts X-Wing Alliance Upgrade models with
 * their materials (docs: xwau_model_import.md). The roots are layered game
 * directories, searched in order with case-insensitive paths: list the
 * update before the full release, e.g. UPD .../Group_2_Materials/OverwriteOn,
 * UPD .../Group_2_Effects/OverwriteOn, then the same for Full. For each OPT,
 * Materials/<basename>.mat (on top of Materials/DefaultGlobalMaterial.mat)
 * gives each texture's material and normal map, read from the DAT image it
 * names. The converter then reproduces that renderer's shading in standard
 * glTF inputs: authored colors, illumination, shadeless and glass texels,
 * metallic-roughness, KHR_materials_specular, normal maps with NMIntensity
 * baked in and MikkTSpace tangents. Stored normals are used per corner.
 * Excludes --smooth-angle, --emissive, --fullbright and --no-normal-repair.
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
#include "xwau_dat.h"
#include "xwau_mat.h"

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

/* ===== XWAU materials (--xwau-root) =================================== */

#define XWAU_MAX_ROOTS 16

typedef struct XwauContext {
	const char*  roots[XWAU_MAX_ROOTS]; /* first root holding a file wins */
	int          root_count;
	XwauMatParams global; /* Materials/DefaultGlobalMaterial.mat, else code defaults */
} XwauContext;

/* Resolve a game-relative path (backslashes or slashes) under the first root
 * that has it, matching every component case-insensitively. */
static int xwau_find(const XwauContext* x, const char* rel, char* out, size_t cap) {
	for (int r = 0; r < x->root_count; ++r) {
		char path[1024], parts[512];
		snprintf(path, sizeof path, "%s", x->roots[r]);
		snprintf(parts, sizeof parts, "%s", rel);
		for (char* c = parts; *c; ++c)
			if (*c == '\\')
				*c = '/';
		int   found = 1;
		char* save  = NULL;
		for (char* part = strtok_r(parts, "/", &save); part && found; part = strtok_r(NULL, "/", &save)) {
			DIR* d = opendir(path);
			found  = 0;
			if (!d)
				break;
			struct dirent* de;
			while ((de = readdir(d))) {
				if (strcasecmp(de->d_name, part) == 0) {
					size_t len = strlen(path);
					snprintf(path + len, sizeof path - len, "/%s", de->d_name);
					found = 1;
					break;
				}
			}
			closedir(d);
		}
		struct stat st;
		if (found && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
			snprintf(out, cap, "%s", path);
			return 1;
		}
	}
	return 0;
}

typedef struct XwauNormalMap {
	char        key[1100]; /* resolved path + group + image */
	char        name[160]; /* file-name label: <datbase>_<group>_<image> */
	XwauDatImage image;
	int         ok;
} XwauNormalMap;

/* Load (once per OPT) the image a NormalMap value names: "<path>.dat-G-I".
 * Entries are allocated individually: materials keep pointers into them. */
static const XwauNormalMap* xwau_normal_map(const XwauContext* x, const char* value, XwauNormalMap*** cache,
											size_t* count, const char* label) {
	char spec[XWAU_MAT_PATH_MAX];
	snprintf(spec, sizeof spec, "%s", value);
	char* image_sep = strrchr(spec, '-');
	char* group_sep = NULL;
	if (image_sep) {
		*image_sep = 0;
		group_sep  = strrchr(spec, '-');
	}
	if (!group_sep) {
		fprintf(stderr, "opt2gltf: %s: unreadable NormalMap '%s'\n", label, value);
		return NULL;
	}
	*group_sep      = 0;
	const int group = atoi(group_sep + 1), image = atoi(image_sep + 1);
	if (!ends_with_icase(spec, ".dat")) {
		fprintf(stderr, "opt2gltf: %s: NormalMap '%s' is not a DAT image; skipped\n", label, value);
		return NULL;
	}
	char path[1024];
	if (!xwau_find(x, spec, path, sizeof path)) {
		fprintf(stderr, "opt2gltf: %s: NormalMap file '%s' not found; skipped\n", label, spec);
		return NULL;
	}
	char key[1100];
	snprintf(key, sizeof key, "%s|%d|%d", path, group, image);
	for (size_t i = 0; i < *count; ++i)
		if (strcmp((*cache)[i]->key, key) == 0)
			return (*cache)[i]->ok ? (*cache)[i] : NULL;
	XwauNormalMap** grown = (XwauNormalMap**)realloc(*cache, (*count + 1) * sizeof *grown);
	XwauNormalMap*  nm    = (XwauNormalMap*)calloc(1, sizeof *nm);
	if (grown)
		*cache = grown;
	if (!grown || !nm) {
		free(nm);
		return NULL;
	}
	grown[(*count)++] = nm;
	snprintf(nm->key, sizeof nm->key, "%s", key);
	const char* base = strrchr(path, '/');
	char        stem[128];
	snprintf(stem, sizeof stem, "%s", base ? base + 1 : path);
	char* dot = strrchr(stem, '.');
	if (dot)
		*dot = 0;
	snprintf(nm->name, sizeof nm->name, "%s_%d_%d", stem, group, image);
	char err[512];
	nm->ok = XwauDat_LoadImage(path, group, image, &nm->image, err, sizeof err);
	if (!nm->ok) {
		fprintf(stderr, "opt2gltf: %s: NormalMap %s; skipped\n", label, err);
		return NULL;
	}
	if (nm->image.encoding == XWAU_DAT_ENCODING_BC5) {
		/* BC5 stores X and Y only: rebuild Z (XWAU's DATReader rebuilds it
		 * from 0..1 values, a bug not reproduced here). */
		const size_t n = (size_t)nm->image.width * nm->image.height;
		for (size_t i = 0; i < n; ++i) {
			uint8_t*     px = &nm->image.rgba[i * 4];
			const double nx = px[0] / 127.5 - 1.0, ny = px[1] / 127.5 - 1.0;
			const double z2 = 1.0 - nx * nx - ny * ny;
			px[2]           = (uint8_t)lround((sqrt(z2 > 0.0 ? z2 : 0.0) * 0.5 + 0.5) * 255.0);
		}
	}
	return nm;
}

static void xwau_fill(OptGltfXwauMaterial* out, const XwauMatParams* p) {
	memset(out, 0, sizeof *out);
	out->metallic         = p->metallic;
	out->intensity        = p->intensity;
	out->glossiness       = p->glossiness;
	out->nm_intensity     = p->nm_intensity;
	out->specular_val     = p->specular_val;
	out->ambient          = p->ambient;
	out->shadeless        = p->shadeless;
	out->alpha_isnt_glass = p->alpha_isnt_glass;
	/* NoColorAlpha only acts with AlphaToBloom, which already maps to a
	 * glow-only texel. */
	out->alpha_to_bloom = p->alpha_to_bloom;
}

static int convert_xwau(opt_file_t* opt, const char* out_dir, const char* basename,
						const OptGltfBuildOptions* base, const XwauContext* x, const char* label) {
	char rel[512], path[1024];
	snprintf(rel, sizeof rel, "Materials/%s.mat", basename);
	XwauMatFile* mat = xwau_find(x, rel, path, sizeof path) ? XwauMat_Load(path, &x->global) : NULL;

	const size_t         count     = opt->texture_count ? (size_t)opt->texture_count : 1;
	OptGltfXwauMaterial* materials = (OptGltfXwauMaterial*)calloc(count, sizeof *materials);
	XwauNormalMap**      maps      = NULL;
	size_t               map_count = 0;
	if (!materials) {
		XwauMat_Free(mat);
		return 0;
	}
	size_t normal_textures = 0;
	for (int32_t i = 0; i < opt->texture_count; ++i) {
		const XwauMatParams* p = mat ? XwauMat_Find(mat, opt->textures[i].name) : &x->global;
		xwau_fill(&materials[i], p);
		if (p->normal_map[0]) {
			const XwauNormalMap* nm = xwau_normal_map(x, p->normal_map, &maps, &map_count, label);
			if (nm) {
				materials[i].normal_map.rgba   = nm->image.rgba;
				materials[i].normal_map.width  = nm->image.width;
				materials[i].normal_map.height = nm->image.height;
				materials[i].normal_map_name   = nm->name;
				normal_textures++;
			}
		}
	}
	/* Untextured faces: the material lookup of an empty name is slot 0. */
	OptGltfXwauMaterial untextured;
	xwau_fill(&untextured, mat ? XwauMat_Find(mat, "") : &x->global);

	OptGltfBuildOptions options = *base;
	options.xwau_materials      = materials;
	options.xwau_untextured     = &untextured;
	fprintf(stderr, "opt2gltf: %s: xwau materials %s, %zu of %d textures normal-mapped (%zu maps)\n", label,
			mat ? path : "(default)", normal_textures, (int)opt->texture_count, map_count);
	int ok = opt2gltf_convert(opt, out_dir, basename, &options, label) ? 1 : 0;

	for (size_t i = 0; i < map_count; ++i) {
		XwauDat_FreeImage(&maps[i]->image);
		free(maps[i]);
	}
	free(maps);
	free(materials);
	XwauMat_Free(mat);
	return ok;
}

/* Replaces the OPT's Cockpit (type 31) hardpoints with one at the pilot eye
 * given in craft-table units (CockpitPovX/Y/Z): ModelMesh_GetHardpoint
 * returns (x, -y, z), so the eye's Y is the hardpoint's -y. It goes on the
 * first MainHull mesh, or mesh 0. */
static int apply_cockpit_eye(opt_file_t* opt, const int eye[3]) {
	opt_mesh_t* owner = NULL;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		opt_mesh_t* m = &opt->meshes[mi];
		int32_t     kept = 0;
		for (int32_t hi = 0; hi < m->hardpoint_count; ++hi) {
			if ((int)m->hardpoints[hi].type != OPT_GLTF_COCKPIT_HARDPOINT)
				m->hardpoints[kept++] = m->hardpoints[hi];
		}
		m->hardpoint_count = kept;
		if (!owner && m->has_descriptor && m->descriptor.mesh_type == OPT_MT_MAIN_HULL)
			owner = m;
	}
	if (!owner && opt->mesh_count > 0)
		owner = &opt->meshes[0];
	if (!owner)
		return 0;
	opt_hardpoint_t* grown =
		(opt_hardpoint_t*)realloc(owner->hardpoints, (size_t)(owner->hardpoint_count + 1) * sizeof *grown);
	if (!grown)
		return 0;
	owner->hardpoints                         = grown;
	owner->hardpoints[owner->hardpoint_count] = (opt_hardpoint_t) {
		.type = (opt_hardpoint_type_t)OPT_GLTF_COCKPIT_HARDPOINT,
		.pos  = { (float)eye[0], (float)-eye[1], (float)eye[2] },
	};
	owner->hardpoint_count++;
	return 1;
}

static int convert_one(const char* opt_path, const char* out_dir, const char* basename,
					   const OptGltfBuildOptions* options, const XwauContext* xwau, const int* cockpit_eye) {
	opt_error_t err;
	opt_file_t* opt = opt_load_file(opt_path, &err);
	if (!opt) {
		fprintf(stderr, "opt2gltf: %s: %s\n", opt_path, err.msg);
		return 0;
	}
	if (cockpit_eye && !apply_cockpit_eye(opt, cockpit_eye)) {
		fprintf(stderr, "opt2gltf: %s: cannot add the cockpit eye hardpoint\n", opt_path);
		opt_free(opt);
		return 0;
	}
	int ok = xwau ? convert_xwau(opt, out_dir, basename, options, xwau, opt_path)
				  : (opt2gltf_convert(opt, out_dir, basename, options, opt_path) ? 1 : 0);
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
	int                  cockpit_eye[3];
	bool                 has_cockpit_eye = false;
	XwauContext          xwau             = { 0 };
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
		} else if (strcmp(argv[ai], "--xwau-root") == 0) {
			if (ai + 1 >= argc || xwau.root_count == XWAU_MAX_ROOTS) {
				fprintf(stderr, "opt2gltf: --xwau-root needs a directory (at most %d)\n", XWAU_MAX_ROOTS);
				free(tags);
				return 2;
			}
			xwau.roots[xwau.root_count++] = argv[ai + 1];
			ai += 2;
		} else if (strcmp(argv[ai], "--cockpit-eye") == 0) {
			char end = 0;
			if (ai + 1 >= argc || sscanf(argv[ai + 1], "%d,%d,%d%c", &cockpit_eye[0], &cockpit_eye[1],
										 &cockpit_eye[2], &end) != 3) {
				fprintf(stderr, "opt2gltf: --cockpit-eye needs X,Y,Z (craft-table units)\n");
				free(tags);
				return 2;
			}
			for (int k = 0; k < 3; ++k) {
				if (cockpit_eye[k] < -32768 || cockpit_eye[k] > 32767) {
					fprintf(stderr, "opt2gltf: --cockpit-eye values must fit in 16 bits\n");
					free(tags);
					return 2;
				}
			}
			has_cockpit_eye = true;
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
				"[--cockpit-eye X,Y,Z] [--xwau-root DIR]... <input.OPT|input_dir> <output_dir>\n",
				argv[0]);
		free(tags);
		return 2;
	}
	if (xwau.root_count) {
		if (smooth_angle_deg >= 0.0f || emissive || fullbright_strength > 0.0f || !repair_normals) {
			fprintf(stderr, "opt2gltf: --xwau-root excludes --smooth-angle, --emissive, --fullbright and "
							"--no-normal-repair\n");
			free(tags);
			return 2;
		}
		XwauMat_CodeDefaults(&xwau.global);
		char global_path[1024];
		if (xwau_find(&xwau, "Materials/DefaultGlobalMaterial.mat", global_path, sizeof global_path))
			XwauMat_ReadGlobalDefault(global_path, &xwau.global);
		else
			fprintf(stderr, "opt2gltf: no Materials/DefaultGlobalMaterial.mat; using built-in defaults\n");
	}
	const XwauContext*        xwau_ctx = xwau.root_count ? &xwau : NULL;
	const OptGltfBuildOptions options  = {
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

	if (S_ISDIR(st.st_mode) && has_cockpit_eye) {
		fprintf(stderr, "opt2gltf: --cockpit-eye applies to a single OPT, not a directory\n");
		free(tags);
		return 2;
	}
	const int* cockpit_eye_arg = has_cockpit_eye ? cockpit_eye : NULL;
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
			if (convert_one(src, out_sub, base, &options, xwau_ctx, cockpit_eye_arg))
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
		status = convert_one(in, outd, base, &options, xwau_ctx, cockpit_eye_arg) ? 0 : 1;
	}
	free(tags);
	return status;
}
