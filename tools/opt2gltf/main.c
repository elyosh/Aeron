/*
 * opt2gltf — convert one OPT file (or every OPT in a directory) to
 * glTF 2.0 + .bin + .png assets.
 *
 * Usage:
 *   opt2gltf [options] <input.opt|input_dir> <output_dir>
 *   opt2gltf [options] --project DIR [--replace] <input.opt|input_dir>
 *   opt2gltf --xwau-effects --xwau-root DIR... --project DIR <project|model.gltf>...
 *   opt2gltf --state-count FILE.opt...
 *
 * Options:
 *   [--stock] [--smooth-angle DEG] [--no-normal-repair] [--emissive]
 *   [--fullbright STRENGTH] [--tag N=TAG]...
 *   [--articulation N=GROUP:ANGLE:SPEED[:SPEED_BACK][:BEHAVIOR]]...
 *   [--articulation-setting NAME=VALUE]... [--component-set NAME=N[,N...]]...
 *   [--subdivide N] [--cockpit-eye X,Y,Z] [--cockpit-light X,Y,Z]
 *   [--cockpit-light-intensity CD] [--xwau-root DIR]...
 *   [--xwau-ignore-shadeless TEXTURE]... [--dynamic-cockpit FILE.dc]
 *
 * If the input is a directory, every *.OPT inside (case-insensitive)
 * is converted and emitted under <output_dir>/<basename>/. If a single
 * OPT file, the output goes directly into <output_dir>/. A file that fails
 * does not stop the others, but the exit status is then non-zero.
 *
 * --project DIR converts into a project instead, a folder holding models
 * (models/<key>/) and their effect libraries (effects/<library>/): each OPT
 * becomes models/<key>/<key>.gltf, its .bin and textures/, the key being
 * the OPT's name in lower case; with --xwau-root, the effect library images
 * its materials name that the project lacks are added (xwau_library.h). An
 * existing model folder is an error, unless --replace replaces it.
 *
 * --xwau-effects writes the effect libraries every model given names (a
 * model .gltf, or every model of a project) into the --project's effects/,
 * replacing them, with the game's effects.yaml (xwau_library.h).
 *
 * Replacement workflow (both tools are built with AERON_BUILD_TOOLS):
 *
 *   opt2gltf --stock FLIGHTMODELS out
 *   aeron_gltf_cook out/XWING/XWING.gltf --out <asset root>/remaster/models/xwing.glb
 *
 * The cooked GLB carries the AERON_flight_model extension with cooked
 * simulation and render data, and replaces the OPT whose lower-cased
 * basename it has. Conversion keeps the component order and every mesh's
 * type, explosion flags, descriptor, target, rotation frame, hardpoints and
 * engine glows, the LOD 0 geometry and the textures. Components stay
 * one-to-one with the OPT meshes: a mesh with vertices but no LOD 0 faces
 * keeps its vertices as one degenerate triangle (no faces once cooked), and
 * a mesh without LOD 0 triangles otherwise is an error naming the file and
 * mesh. Expected differences from the stock OPT:
 *   - quads become two triangles sharing corner 0 and the face normal;
 *   - only LOD 0 is kept;
 *   - positions round-trip through metres and differ at the ulp level;
 *   - the trailing MESHVERTS box follows the game's replacement policy;
 *   - engine-glow axes round-trip through a quaternion and come back
 *     orthonormalized;
 *   - components carry no tags unless --tag or --stock names them, so
 *     tag-driven behaviour such as the crew animation is off.
 *
 * --stock converts an OPT of the original game as the game draws it, with
 * nothing read beside it: --smooth-angle 90 and --emissive, unless given;
 * projectiles (model_tables.h) fully self-lit with --fullbright 5 instead;
 * the component tags and articulations the game ties to fixed component
 * numbers, and the pilot eye the game's craft table gives the player's
 * craft (model_tables.h). It excludes --xwau-root.
 *
 * --tag N=TAG (repeatable) adds TAG ([a-z0-9._]{1,63}) to the component
 * built from OPT mesh N, e.g. --tag 0=hangarcrane.arm.
 *
 * --articulation N=GROUP:ANGLE:SPEED[:SPEED_BACK][:BEHAVIOR] (repeatable) adds
 * an AERON_flight_model articulation for component N (0..254, an OPT mesh
 * index, which may be past this OPT's meshes when the game draws another
 * model with the same per-component state) to the model root: GROUP and
 * BEHAVIOR match the tag syntax, ANGLE is -128..255, SPEED and SPEED_BACK
 * 0..255 (SPEED_BACK defaults to SPEED), e.g.
 * --articulation 3=sfoils:12:1 or --articulation 23=crew:32:2:random_pause.
 * --articulation-setting NAME=VALUE (repeatable) adds an integer to the model
 * root's articulationSettings. The game defines groups, behaviors and settings.
 *
 * --component-set NAME=N[,N...] (repeatable; a repeated NAME adds members)
 * adds component ordinals N (0..254, which may be past this OPT's meshes) to
 * the model root's componentSets entry NAME (tag syntax), e.g.
 * --component-set hidden_by_profile.quad=7,8,19. The game defines the names.
 *
 * --cockpit-eye X,Y,Z (single OPT only) sets the pilot eye in craft-table
 * units, as XWAU's CockpitPovX/Y/Z: the OPT's Cockpit (type 31) hardpoints
 * are replaced by one at (X, -Y, Z) on the first MainHull mesh (mesh 0
 * without one). The game reads the eye from the craft model's Cockpit
 * hardpoint; OpenXWA applies all three axes for GLB replacements.
 *
 * --cockpit-light X,Y,Z (single OPT only) adds a dim point light at the
 * pilot eye given in craft-table units, as --cockpit-eye, for a cockpit
 * model (which shares its craft's frame): white, COCKPIT_LIGHT_RANGE metres,
 * on the first MainHull mesh (mesh 0 without one). It lights the interior
 * softly from the viewpoint, so its shadows would be hidden anyway.
 * --cockpit-light-intensity CD (> 0) sets its intensity in candela, and
 * that of the cockpit lights XWAU conversion adds; the default is
 * DEFAULT_COCKPIT_LIGHT_INTENSITY.
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
 * everything XWAU gives them. The roots are layered game directories (an
 * installation, or unpacked package groups), searched in order with
 * case-insensitive paths: list an update before the release it updates.
 * For each OPT:
 *   - Materials/<basename>.mat (on top of Materials/DefaultGlobalMaterial.mat)
 *     gives each texture's material and normal map, read from the DAT image
 *     it names. The converter then reproduces that renderer's shading in
 *     standard glTF inputs: authored colors, illumination, shadeless and
 *     glass texels, metallic-roughness, KHR_materials_specular, normal maps
 *     with NMIntensity baked in and MikkTSpace tangents. Stored normals are
 *     used per corner;
 *   - its image sequences become material animations (xwau_anim.h);
 *   - its files beside the OPT give the pilot eye, object profiles,
 *     articulations, Dynamic Cockpit and a cockpit model's interior light
 *     (xwau_model.h), and its skins material variants (xwau_skins.h), with
 *     what the missions give it (xwau_missions.h);
 *   - the crew and glass textures model_tables.h lists are lit surfaces;
 *   - more than 120 weapon hardpoints are reported: the game keeps the
 *     first 120 weapon slots, as XWAU does.
 * The options --cockpit-eye, --cockpit-light and --dynamic-cockpit replace
 * what the model's files give; the other options add to it. Excludes
 * --stock, --smooth-angle, --emissive, --fullbright and --no-normal-repair.
 *
 * --xwau-ignore-shadeless TEXTURE (repeatable, with --xwau-root) shades the
 * named OPT texture as a plain lit surface although its material sets
 * Shadeless: some XWAU materials make crew textures and cockpit glass
 * shadeless, which reads as glowing under physical lighting. A transparent
 * texture keeps its alpha and does not become XWAU glass. Names match as material lookups do (case-
 * insensitive, ignoring a "_fg_" skin suffix); a name that matches no
 * texture is reported.
 *
 * --dynamic-cockpit FILE.dc (single OPT, with --xwau-root) applies an XWAU
 * Dynamic Cockpit file: the cockpit textures it
 * names become screen surfaces showing the captured HUD, with their cover
 * images from the Effects DATs, and hologram faces move by the craft INI's
 * [CockpitHoloOffsets]. See dynamic_cockpit.h for the parsing rules.
 *
 * --state-count prints, for each OPT given, the largest texture count of a
 * LOD 0 face group (its states), one per line, and exits.
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

#include "dynamic_cockpit.h"
#include "model_tables.h"
#include "opt.h"
#include "opt2gltf.h"
#include "xwau_anim.h"
#include "xwau_config.h"
#include "xwau_dat.h"
#include "xwau_library.h"
#include "xwau_mat.h"
#include "xwau_missions.h"
#include "xwau_model.h"
#include "xwau_skins.h"

#define OPT2GLTF_MAX_ARTICULATIONS 4096
#define OPT2GLTF_MAX_SETTINGS 64
#define OPT2GLTF_MAX_COMPONENT_SETS 256

/* Copies the name ending at `end` into `out` (64 bytes); false when empty or
 * too long. The name's syntax is checked by the converter. */
static bool copy_name(const char* begin, const char* end, char out[64]) {
	if (end <= begin || end - begin >= 64)
		return false;
	memcpy(out, begin, (size_t)(end - begin));
	out[end - begin] = 0;
	return true;
}

/* Parses NAME=N[,N...] into the set named NAME among `sets`, adding it when
 * new. Fails on a bad name, member or a full table. */
static bool parse_component_set(const char* text, OptGltfComponentSet* sets, size_t* count) {
	const char* equals = strchr(text, '=');
	char        name[64];
	if (!equals || !copy_name(text, equals, name))
		return false;
	size_t index = 0;
	while (index < *count && strcmp(sets[index].name, name) != 0)
		++index;
	if (index == *count) {
		if (*count == OPT2GLTF_MAX_COMPONENT_SETS)
			return false;
		memset(&sets[index], 0, sizeof sets[index]);
		strcpy(sets[index].name, name);
	}
	const char* cur = equals + 1;
	do {
		char*         end    = NULL;
		const long    member = strtol(cur, &end, 10);
		if (end == cur || (*end && *end != ',') || member < 0 || member > 254)
			return false;
		sets[index].members[member / 32] |= 1u << (member % 32);
		cur = *end ? end + 1 : end;
	} while (*cur);
	if (index == *count)
		++*count;
	return true;
}

/* Parses N=GROUP:ANGLE:SPEED[:SPEED_BACK][:BEHAVIOR]. */
static bool parse_articulation(const char* text, OptGltfComponentArticulation* out) {
	char*               end  = NULL;
	const unsigned long mesh = strtoul(text, &end, 10);
	if (end == text || *end != '=' || mesh > UINT32_MAX)
		return false;
	memset(out, 0, sizeof *out);
	out->component    = (uint32_t)mesh;
	const char* group = end + 1;
	const char* colon = strchr(group, ':');
	if (!colon || !copy_name(group, colon, out->group))
		return false;
	long value = strtol(colon + 1, &end, 10);
	if (end == colon + 1 || *end != ':' || value < INT32_MIN || value > INT32_MAX)
		return false;
	out->angle      = (int32_t)value;
	const char* cur = end + 1;
	value           = strtol(cur, &end, 10);
	if (end == cur || (*end && *end != ':') || value < 0 || value > INT32_MAX)
		return false;
	out->speed = out->speed_back = (int32_t)value;
	if (!*end)
		return true;
	cur   = end + 1;
	value = strtol(cur, &end, 10);
	if (end != cur) {
		if ((*end && *end != ':') || value < 0 || value > INT32_MAX)
			return false;
		out->speed_back = (int32_t)value;
		if (!*end)
			return true;
		cur = end + 1;
	}
	return copy_name(cur, cur + strlen(cur), out->behavior);
}

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

#define XWAU_MAX_IGNORE_SHADELESS 64

typedef struct XwauContext {
	XwauRoots     roots;
	XwauMatParams global; /* Materials/DefaultGlobalMaterial.mat, else code defaults */
	const char*   ignore_shadeless[XWAU_MAX_IGNORE_SHADELESS]; /* --xwau-ignore-shadeless */
	int           ignore_shadeless_count;
	XwauMissions* missions;
} XwauContext;

/* Texture `name` without a "_fg_" skin suffix equals `wanted`, ignoring ASCII
 * case (XwauMat_Find's comparison). */
static bool xwau_texture_named(const char* name, const char* wanted) {
	for (;; ++name, ++wanted) {
		const bool name_end = !*name || (name[0] == '_' && tolower((unsigned char)name[1]) == 'f' &&
										 tolower((unsigned char)name[2]) == 'g' && name[3] == '_');
		if (name_end || !*wanted)
			return name_end && !*wanted;
		if (tolower((unsigned char)*name) != tolower((unsigned char)*wanted))
			return false;
	}
}

static int xwau_find(const XwauContext* x, const char* rel, char* out, size_t cap) {
	return XwauRoots_FindFile(&x->roots, rel, out, cap) ? 1 : 0;
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

static int xwau_find_file(void* context, const char* rel, char* out, size_t cap) {
	return xwau_find((const XwauContext*)context, rel, out, cap);
}

#define MAX_LIT_TEXTURES 128

/* `lit` names the textures shaded as lit surfaces although their material
 * makes them Shadeless. */
static int convert_xwau(opt_file_t* opt, const char* out_dir, const char* basename,
						const OptGltfBuildOptions* base, const XwauContext* x, const OptGltfScreen* screens,
						const char* const* lit, int lit_count, const char* label) {
	char rel[512], path[1024];
	snprintf(rel, sizeof rel, "Materials/%s.mat", basename);
	XwauMatFile* mat = xwau_find(x, rel, path, sizeof path) ? XwauMat_Load(path, &x->global) : NULL;

	const size_t         count     = opt->texture_count ? (size_t)opt->texture_count : 1;
	OptGltfXwauMaterial* materials = (OptGltfXwauMaterial*)calloc(count, sizeof *materials);
	OptGltfXwauAnim*     anims     = (OptGltfXwauAnim*)calloc(count, sizeof *anims);
	XwauAnim*            anim      = XwauAnim_Begin(basename, xwau_find_file, (void*)x, label);
	XwauNormalMap**      maps      = NULL;
	size_t               map_count = 0;
	if (!materials || !anims || !anim) {
		free(materials);
		free(anims);
		XwauAnim_End(anim);
		XwauMat_Free(mat);
		return 0;
	}
	size_t normal_textures = 0;
	bool   ignored[MAX_LIT_TEXTURES] = { false };
	for (int32_t i = 0; i < opt->texture_count; ++i) {
		const XwauMatParams* p = mat ? XwauMat_Find(mat, opt->textures[i].name) : &x->global;
		xwau_fill(&materials[i], p);
		for (int k = 0; k < lit_count; ++k) {
			if (xwau_texture_named(opt->textures[i].name, lit[k])) {
				materials[i].shadeless = false;
				materials[i].lit       = true;
				ignored[k]             = true;
			}
		}
		XwauAnimScreen screen = XWAU_ANIM_SCREEN_NONE;
		if (screens && screens[i].present)
			screen = screens[i].hologram           ? XWAU_ANIM_SCREEN_HOLOGRAM
					 : screens[i].cover_base.rgba ? XWAU_ANIM_SCREEN_COVER
												  : XWAU_ANIM_SCREEN_NO_COVER;
		if (!XwauAnim_Texture(anim, mat, &opt->textures[i], OptGltf_XwauUnlit(&materials[i]), screen,
							  &anims[i])) {
			fprintf(stderr, "opt2gltf: %s: out of memory (animations)\n", label);
			free(materials);
			free(anims);
			XwauAnim_End(anim);
			XwauMat_Free(mat);
			return 0;
		}
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
	for (int k = 0; k < lit_count; ++k) {
		if (!ignored[k])
			fprintf(stderr, "opt2gltf: %s: lit texture %s: no such texture\n", label, lit[k]);
	}
	/* Untextured faces: the material lookup of an empty name is slot 0. */
	OptGltfXwauMaterial untextured;
	xwau_fill(&untextured, mat ? XwauMat_Find(mat, "") : &x->global);

	OptGltfBuildOptions options = *base;
	options.xwau_materials      = materials;
	options.xwau_untextured     = &untextured;
	options.screens             = screens;
	options.xwau_anim           = XwauAnim_Any(anim) ? anims : NULL;
	fprintf(stderr, "opt2gltf: %s: xwau materials %s, %zu of %d textures normal-mapped (%zu maps)\n", label,
			mat ? path : "(default)", normal_textures, (int)opt->texture_count, map_count);
	XwauAnim_Report(anim);
	int ok = opt2gltf_convert(opt, out_dir, basename, &options, label) ? 1 : 0;

	for (size_t i = 0; i < map_count; ++i) {
		XwauDat_FreeImage(&maps[i]->image);
		free(maps[i]);
	}
	free(maps);
	free(materials);
	free(anims);
	XwauAnim_End(anim);
	XwauMat_Free(mat);
	return ok;
}

/* Replaces the OPT's Cockpit (type 31) hardpoints with one at the pilot eye
 * given in craft-table units (CockpitPovX/Y/Z): ModelMesh_GetHardpoint
 * returns (x, -y, z), so the eye's Y is the hardpoint's -y. It goes on the
 * first MainHull mesh, or mesh 0. */
/* The first MainHull mesh, or mesh 0; -1 without meshes. */
static int32_t main_hull_mesh(const opt_file_t* opt) {
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m = &opt->meshes[mi];
		if (m->has_descriptor && m->descriptor.mesh_type == OPT_MT_MAIN_HULL)
			return mi;
	}
	return opt->mesh_count > 0 ? 0 : -1;
}

/* Interior light of --cockpit-light. Its default intensity is a 100 W
 * Blender point light in candela, as Blender's glTF exporter converts it
 * (683 lm/W over 4 pi sr). */
#define DEFAULT_COCKPIT_LIGHT_INTENSITY (100.0f * 683.0f / (4.0f * 3.14159265f))
#define COCKPIT_LIGHT_RANGE 3.0f

static int apply_cockpit_eye(opt_file_t* opt, const int eye[3]) {
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		opt_mesh_t* m    = &opt->meshes[mi];
		int32_t     kept = 0;
		for (int32_t hi = 0; hi < m->hardpoint_count; ++hi) {
			if ((int)m->hardpoints[hi].type != OPT_GLTF_COCKPIT_HARDPOINT)
				m->hardpoints[kept++] = m->hardpoints[hi];
		}
		m->hardpoint_count = kept;
	}
	const int32_t owner_index = main_hull_mesh(opt);
	if (owner_index < 0)
		return 0;
	opt_mesh_t* owner = &opt->meshes[owner_index];
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

/* The weapon hardpoints of the laser and warhead kinds, types 1 to 18
 * (g_optHardpointWeaponGroupKindByType): the game keeps 120 weapon slots. */
#define WEAPON_SLOTS 120

static int32_t weapon_hardpoints(const opt_file_t* opt) {
	int32_t count = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
		for (int32_t hi = 0; hi < opt->meshes[mi].hardpoint_count; ++hi)
			count += opt->meshes[mi].hardpoints[hi].type >= 1 && opt->meshes[mi].hardpoints[hi].type <= 18;
	return count;
}

/* What a conversion applies besides the build options. */
typedef struct Settings {
	const OptGltfBuildOptions* base;
	const XwauContext*         xwau;  /* XWAU mode, or NULL */
	bool                       stock; /* --stock */
	bool                       fullbright_given;
	const int*                 cockpit_eye; /* --cockpit-eye, or NULL */
	const int*                 cockpit_light;
	float                      cockpit_light_intensity;
	const char*                dynamic_cockpit;
} Settings;

/* The model's own options: the given ones, then the model's. */
typedef struct ModelOptions {
	OptGltfComponentTag*          tags;
	size_t                        tag_count;
	OptGltfComponentArticulation* articulations;
	size_t                        articulation_count;
	OptGltfComponentSet*          sets;
	size_t                        set_count;
	OptGltfArticulationSetting*   settings;
	size_t                        setting_count;
	char*                         tag_text; /* the stock tags' text the tags point into */
} ModelOptions;

static void model_options_free(ModelOptions* m) {
	free(m->tags);
	free(m->articulations);
	free(m->sets);
	free(m->settings);
	free(m->tag_text);
	memset(m, 0, sizeof *m);
}

/* Copies the given options, with room for `more` articulations, sets and
 * settings. */
static bool model_options_init(ModelOptions* m, const OptGltfBuildOptions* base, size_t more) {
	memset(m, 0, sizeof *m);
	m->tags          = (OptGltfComponentTag*)calloc(base->component_tag_count + more + 1, sizeof *m->tags);
	m->articulations = (OptGltfComponentArticulation*)calloc(base->component_articulation_count + more + 1,
															 sizeof *m->articulations);
	m->sets          = (OptGltfComponentSet*)calloc(base->component_set_count + more + 1, sizeof *m->sets);
	m->settings =
		(OptGltfArticulationSetting*)calloc(base->articulation_setting_count + more + 1, sizeof *m->settings);
	if (!m->tags || !m->articulations || !m->sets || !m->settings) {
		model_options_free(m);
		return false;
	}
	memcpy(m->tags, base->component_tags, base->component_tag_count * sizeof *m->tags);
	m->tag_count = base->component_tag_count;
	memcpy(m->articulations, base->component_articulations,
		   base->component_articulation_count * sizeof *m->articulations);
	m->articulation_count = base->component_articulation_count;
	memcpy(m->sets, base->component_sets, base->component_set_count * sizeof *m->sets);
	m->set_count = base->component_set_count;
	memcpy(m->settings, base->articulation_settings, base->articulation_setting_count * sizeof *m->settings);
	m->setting_count = base->articulation_setting_count;
	return true;
}

/* Adds the stock tags and articulations of `model` (model_tables.h). */
static bool add_stock_tags(ModelOptions* m, const char* model, const char* label) {
	const char* table = ModelTables_StockTags(model);
	if (!table)
		return true;
	size_t count = 1;
	for (const char* c = table; *c; ++c)
		count += *c == ' ';
	OptGltfComponentTag* tags = (OptGltfComponentTag*)realloc(m->tags, (m->tag_count + count) * sizeof *tags);
	OptGltfComponentArticulation* arts =
		tags ? (OptGltfComponentArticulation*)realloc(m->articulations,
													  (m->articulation_count + count) * sizeof *arts)
			 : NULL;
	if (tags)
		m->tags = tags;
	if (arts)
		m->articulations = arts;
	if (!tags || !arts || !(m->tag_text = Xwau_Strdup(table)))
		return false;
	for (char *token = m->tag_text, *next; token; token = next) {
		next = strchr(token, ' ');
		if (next)
			*next++ = 0;
		if (!*token)
			continue;
		char*         equals = strchr(token, '=');
		char*         end    = NULL;
		unsigned long mesh   = strtoul(token, &end, 10);
		if (strchr(token, ':')) {
			if (!parse_articulation(token, &m->articulations[m->articulation_count])) {
				fprintf(stderr, "opt2gltf: %s: bad stock articulation %s\n", label, token);
				return false;
			}
			m->articulation_count++;
		} else if (equals && end == equals) {
			m->tags[m->tag_count].mesh_index = (uint32_t)mesh;
			m->tags[m->tag_count++].tag      = equals + 1;
		}
	}
	return true;
}

/* Adds what the model's XWAU files give (xwau_model.h). */
static bool add_xwau_model(ModelOptions* m, const XwauModel* model) {
	memcpy(m->articulations + m->articulation_count, model->articulations,
		   model->articulation_count * sizeof *m->articulations);
	m->articulation_count += model->articulation_count;
	for (size_t i = 0; i < model->set_count; ++i) {
		/* A set the options name gets the model's members too. */
		size_t s = 0;
		while (s < m->set_count && strcmp(m->sets[s].name, model->sets[i].name) != 0)
			++s;
		if (s == m->set_count)
			m->sets[m->set_count++] = model->sets[i];
		else
			for (int w = 0; w < 8; ++w)
				m->sets[s].members[w] |= model->sets[i].members[w];
	}
	memcpy(m->settings + m->setting_count, model->settings, model->setting_count * sizeof *m->settings);
	m->setting_count += model->setting_count;
	return true;
}

static int convert_one(const char* opt_path, const char* out_dir, const char* basename, const Settings* s) {
	opt_error_t err;
	opt_file_t* opt = opt_load_file(opt_path, &err);
	if (!opt) {
		fprintf(stderr, "opt2gltf: %s: %s\n", opt_path, err.msg);
		return 0;
	}
	OptGltfBuildOptions options = *s->base;
	ModelOptions        model_options;
	XwauModel*          model = s->xwau ? (XwauModel*)malloc(sizeof *model) : NULL;
	XwauSkins           skins = { 0 };
	DynamicCockpit      dc    = { 0 };
	const char*         lit[MAX_LIT_TEXTURES];
	int                 lit_count      = 0;
	char                lit_text[1024] = "";
	int                 eye[3], light_eye[3];
	const int*          cockpit_eye   = s->cockpit_eye;
	const int*          cockpit_light = s->cockpit_light;
	const char*         dynamic       = s->dynamic_cockpit;
	int ok = model_options_init(&model_options, s->base, model ? XWAU_MODEL_MAX_ARTICULATIONS : 0) &&
			 (!s->xwau || model);
	if (ok && s->stock) {
		/* The original game's settings. */
		if (ModelTables_IsProjectile(basename) && !s->fullbright_given)
			options.fullbright_strength = 5.0f;
		if (!cockpit_eye && ModelTables_StockEye(basename, eye))
			cockpit_eye = eye;
		ok = add_stock_tags(&model_options, basename, opt_path);
	}
	if (ok && s->xwau) {
		ok = XwauModel_Read(&s->xwau->roots, s->xwau->missions, basename, model, opt_path) &&
			 add_xwau_model(&model_options, model);
		if (ok && !cockpit_eye && model->has_eye)
			cockpit_eye = memcpy(eye, model->eye, sizeof eye);
		if (ok && !cockpit_light && model->has_craft_eye)
			cockpit_light = memcpy(light_eye, model->craft_eye, sizeof light_eye);
		if (ok && !dynamic && model->dynamic_cockpit[0])
			dynamic = model->dynamic_cockpit;
		for (int k = 0; k < s->xwau->ignore_shadeless_count; ++k)
			lit[lit_count++] = s->xwau->ignore_shadeless[k];
		const char* listed = ModelTables_LitTextures(basename);
		if (listed)
			snprintf(lit_text, sizeof lit_text, "%s", listed);
		char* token = lit_text[0] ? lit_text : NULL;
		while (token && lit_count < MAX_LIT_TEXTURES) {
			char* next = strchr(token, ' ');
			if (next)
				*next++ = 0;
			lit[lit_count++] = token;
			token            = next;
		}
		const int32_t weapons = weapon_hardpoints(opt);
		if (weapons > WEAPON_SLOTS)
			fprintf(stderr, "opt2gltf: %s: %d weapon hardpoints; the game keeps %d weapon slots\n", opt_path,
					(int)weapons, WEAPON_SLOTS);
		ok = ok && XwauSkins_Resolve(&s->xwau->roots, s->xwau->missions, basename, opt, opt_path, &skins);
		if (ok && skins.skins.variant_count)
			options.skins = &skins.skins;
	}
	options.component_tags               = model_options.tags;
	options.component_tag_count          = model_options.tag_count;
	options.component_articulations      = model_options.articulations;
	options.component_articulation_count = model_options.articulation_count;
	options.component_sets               = model_options.sets;
	options.component_set_count          = model_options.set_count;
	options.articulation_settings        = model_options.settings;
	options.articulation_setting_count   = model_options.setting_count;

	if (ok && cockpit_eye && !apply_cockpit_eye(opt, cockpit_eye)) {
		fprintf(stderr, "opt2gltf: %s: cannot add the cockpit eye hardpoint\n", opt_path);
		ok = 0;
	}
	OptGltfLight light;
	if (ok && cockpit_light) {
		light = (OptGltfLight) {
			.mesh      = main_hull_mesh(opt),
			.position  = { (float)cockpit_light[0], (float)-cockpit_light[1], (float)cockpit_light[2] },
			.color     = { 1.0f, 1.0f, 1.0f },
			.intensity = s->cockpit_light_intensity,
			.range     = COCKPIT_LIGHT_RANGE,
		};
		if (light.mesh < 0) {
			fprintf(stderr, "opt2gltf: %s: no mesh for the cockpit light\n", opt_path);
			ok = 0;
		}
		options.lights      = &light;
		options.light_count = 1;
	}
	if (ok && dynamic &&
		(!DynamicCockpit_Load(dynamic, opt, basename, xwau_find_file, (void*)s->xwau, &dc, opt_path) ||
		 !DynamicCockpit_ApplyHologramOffset(opt, &dc))) {
		fprintf(stderr, "opt2gltf: %s: cannot apply %s\n", opt_path, dynamic);
		ok = 0;
	}
	if (ok)
		ok = s->xwau ? convert_xwau(opt, out_dir, basename, &options, s->xwau, dc.screens, lit, lit_count,
									opt_path)
					 : (opt2gltf_convert(opt, out_dir, basename, &options, opt_path) ? 1 : 0);
	DynamicCockpit_Free(&dc);
	XwauSkins_Free(&skins);
	model_options_free(&model_options);
	free(model);
	opt_free(opt);
	return ok;
}

/* Converts into a project: models/<key>/<key>.gltf, its .bin and textures,
 * then, in XWAU mode, the library images it names. */
static int convert_into_project(const char* opt_path, const char* basename, const Settings* s,
								const char* project, bool replace) {
	char key[256], models[1024], final_dir[1300], scratch[1300];
	snprintf(key, sizeof key, "%s", basename);
	for (char* c = key; *c; ++c)
		*c = (char)tolower((unsigned char)*c);
	snprintf(models, sizeof models, "%s/models", project);
	snprintf(final_dir, sizeof final_dir, "%s/%s", models, key);
	snprintf(scratch, sizeof scratch, "%s/.%s.opt2gltf", models, key);
	struct stat st;
	if (stat(final_dir, &st) == 0 && !replace) {
		fprintf(stderr, "opt2gltf: %s: %s exists (--replace replaces it)\n", opt_path, final_dir);
		return 0;
	}
	if (!Xwau_MakeDir(project) || !Xwau_MakeDir(models) || !Xwau_RemoveTree(scratch)) {
		fprintf(stderr, "opt2gltf: %s: cannot write into %s\n", opt_path, models);
		return 0;
	}
	if (!convert_one(opt_path, scratch, basename, s)) {
		Xwau_RemoveTree(scratch);
		return 0;
	}
	char from[1600], to[1600];
	snprintf(from, sizeof from, "%s/%s.gltf", scratch, basename);
	snprintf(to, sizeof to, "%s/%s.gltf", scratch, key);
	if ((strcmp(from, to) != 0 && rename(from, to) != 0) || !Xwau_RemoveTree(final_dir) ||
		rename(scratch, final_dir) != 0) {
		fprintf(stderr, "opt2gltf: %s: cannot place %s: %s\n", opt_path, final_dir, strerror(errno));
		Xwau_RemoveTree(scratch);
		return 0;
	}
	snprintf(to, sizeof to, "%s/%s.gltf", final_dir, key);
	fprintf(stderr, "opt2gltf: %s: wrote %s\n", opt_path, to);
	return !s->xwau || XwauLibrary_AddModel(&s->xwau->roots, project, to);
}

/* --state-count: the largest face-group texture count of LOD 0, the one
 * converted. */
static int print_state_counts(int argc, char** argv) {
	int status = 0;
	for (int ai = 2; ai < argc; ++ai) {
		opt_error_t err;
		opt_file_t* opt = opt_load_file(argv[ai], &err);
		if (!opt) {
			fprintf(stderr, "opt2gltf: %s: %s\n", argv[ai], err.msg);
			status = 1;
			continue;
		}
		int32_t states = 1;
		for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
			for (int32_t g = 0; opt->meshes[mi].lod_count > 0 && g < opt->meshes[mi].lods[0].group_count; ++g)
				if (opt->meshes[mi].lods[0].groups[g].state_count > states)
					states = opt->meshes[mi].lods[0].groups[g].state_count;
		printf("%d\n", (int)states);
		opt_free(opt);
	}
	return status;
}

static int usage(const char* program) {
	fprintf(
		stderr,
		"usage: %s [options] <input.OPT|input_dir> <output_dir>\n"
		"       %s [options] --project DIR [--replace] <input.OPT|input_dir>\n"
		"       %s --xwau-effects --xwau-root DIR... --project DIR <project|model.gltf>...\n"
		"       %s --state-count FILE.opt...\n"
		"options: [--stock] [--smooth-angle DEG] [--no-normal-repair] [--emissive] [--fullbright STRENGTH] "
		"[--tag N=TAG]... [--articulation N=GROUP:ANGLE:SPEED[:SPEED_BACK][:BEHAVIOR]]... "
		"[--articulation-setting NAME=VALUE]... [--component-set NAME=N[,N...]]... [--subdivide N] "
		"[--cockpit-eye X,Y,Z] [--cockpit-light X,Y,Z] [--cockpit-light-intensity CD] "
		"[--xwau-root DIR]... [--xwau-ignore-shadeless TEXTURE]... [--dynamic-cockpit FILE.dc]\n",
		program, program, program, program);
	return 2;
}

int main(int argc, char** argv) {
	if (argc > 1 && strcmp(argv[1], "--state-count") == 0)
		return argc > 2 ? print_state_counts(argc, argv) : 2;
	/* Default: keep the original 1998 OPT normals (full-Gouraud rounded
	 * look). Pass --smooth-angle DEG to regenerate with a threshold. */
	float smooth_angle_deg = -1.0f;
	bool  smooth_given     = false;
	/* Classic position-normal remap + canonical-normal repair are on by
	 * default (stored-normals mode); --no-normal-repair emits the source
	 * per-corner data verbatim for diagnostics. */
	bool repair_normals = true;
	/* Self-illumination export is opt-in. */
	bool emissive = false;
	/* Fullbright export is opt-in; 0 leaves materials lit. */
	float fullbright_strength = 0.0f;
	bool  stock               = false;
	/* Tags point into argv. */
	OptGltfComponentTag*                tags      = NULL;
	size_t                              tag_count = 0;
	static OptGltfComponentArticulation articulations[OPT2GLTF_MAX_ARTICULATIONS];
	size_t                              articulation_count = 0;
	static OptGltfArticulationSetting   settings[OPT2GLTF_MAX_SETTINGS];
	size_t                              setting_count = 0;
	static OptGltfComponentSet          component_sets[OPT2GLTF_MAX_COMPONENT_SETS];
	size_t                              component_set_count = 0;
	unsigned long                       subdivide_levels    = 0;
	int                                 cockpit_eye[3];
	bool                                has_cockpit_eye = false;
	int                                 cockpit_light[3];
	bool                                has_cockpit_light       = false;
	float                               cockpit_light_intensity = DEFAULT_COCKPIT_LIGHT_INTENSITY;
	const char*                         dynamic_cockpit         = NULL;
	const char*                         project                 = NULL;
	bool                                replace                 = false;
	bool                                xwau_effects            = false;
	static XwauContext                  xwau;
	int                                 ai = 1;
	/* Optional conversion flags, in any order before the positional args. */
	while (ai < argc && argv[ai][0] == '-') {
		const char* flag  = argv[ai];
		const char* value = ai + 1 < argc ? argv[ai + 1] : NULL;
		if (strcmp(flag, "--stock") == 0 || strcmp(flag, "--no-normal-repair") == 0 ||
			strcmp(flag, "--emissive") == 0 || strcmp(flag, "--replace") == 0 ||
			strcmp(flag, "--xwau-effects") == 0) {
			stock |= strcmp(flag, "--stock") == 0;
			repair_normals &= strcmp(flag, "--no-normal-repair") != 0;
			emissive |= strcmp(flag, "--emissive") == 0;
			replace |= strcmp(flag, "--replace") == 0;
			xwau_effects |= strcmp(flag, "--xwau-effects") == 0;
			ai += 1;
			continue;
		}
		if (!value) {
			fprintf(stderr, "opt2gltf: %s needs a value\n", flag);
			free(tags);
			return 2;
		}
		if (strcmp(flag, "--smooth-angle") == 0) {
			smooth_angle_deg = strtof(value, NULL);
			smooth_given     = true;
		} else if (strcmp(flag, "--fullbright") == 0) {
			char* end           = NULL;
			fullbright_strength = strtof(value, &end);
			if (end == value || *end || !isfinite(fullbright_strength) || fullbright_strength <= 0.0f) {
				fprintf(stderr, "opt2gltf: --fullbright needs a positive strength\n");
				free(tags);
				return 2;
			}
		} else if (strcmp(flag, "--project") == 0) {
			project = value;
		} else if (strcmp(flag, "--tag") == 0) {
			const char*   equals = strchr(value, '=');
			char*         end    = NULL;
			unsigned long mesh   = equals ? strtoul(value, &end, 10) : 0;
			if (!equals || end != equals || equals == value || mesh > UINT32_MAX) {
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
		} else if (strcmp(flag, "--articulation") == 0) {
			if (articulation_count == OPT2GLTF_MAX_ARTICULATIONS ||
				!parse_articulation(value, &articulations[articulation_count])) {
				fprintf(stderr,
						"opt2gltf: --articulation needs N=GROUP:ANGLE:SPEED[:SPEED_BACK][:BEHAVIOR] (at most %d)\n",
						OPT2GLTF_MAX_ARTICULATIONS);
				free(tags);
				return 2;
			}
			++articulation_count;
		} else if (strcmp(flag, "--component-set") == 0) {
			if (!parse_component_set(value, component_sets, &component_set_count)) {
				fprintf(stderr, "opt2gltf: --component-set needs NAME=N[,N...] with N in 0..254 (at most %d sets)\n",
						OPT2GLTF_MAX_COMPONENT_SETS);
				free(tags);
				return 2;
			}
		} else if (strcmp(flag, "--articulation-setting") == 0) {
			const char* equals = strchr(value, '=');
			char*       end    = NULL;
			const long  number = equals ? strtol(equals + 1, &end, 10) : 0;
			if (!equals || equals == value || (size_t)(equals - value) >= 64 || end == equals + 1 || *end ||
				number < INT32_MIN || number > INT32_MAX || setting_count == OPT2GLTF_MAX_SETTINGS) {
				fprintf(stderr, "opt2gltf: --articulation-setting needs NAME=INTEGER (at most %d)\n",
						OPT2GLTF_MAX_SETTINGS);
				free(tags);
				return 2;
			}
			memcpy(settings[setting_count].name, value, (size_t)(equals - value));
			settings[setting_count].name[equals - value] = 0;
			settings[setting_count++].value              = (int32_t)number;
		} else if (strcmp(flag, "--xwau-root") == 0) {
			if (xwau.roots.count == XWAU_MAX_ROOTS) {
				fprintf(stderr, "opt2gltf: --xwau-root: at most %d\n", XWAU_MAX_ROOTS);
				free(tags);
				return 2;
			}
			xwau.roots.dirs[xwau.roots.count++] = value;
		} else if (strcmp(flag, "--xwau-ignore-shadeless") == 0) {
			if (xwau.ignore_shadeless_count == XWAU_MAX_IGNORE_SHADELESS) {
				fprintf(stderr, "opt2gltf: --xwau-ignore-shadeless: at most %d\n", XWAU_MAX_IGNORE_SHADELESS);
				free(tags);
				return 2;
			}
			xwau.ignore_shadeless[xwau.ignore_shadeless_count++] = value;
		} else if (strcmp(flag, "--cockpit-eye") == 0 || strcmp(flag, "--cockpit-light") == 0) {
			const bool light = strcmp(flag, "--cockpit-light") == 0;
			int*       eye   = light ? cockpit_light : cockpit_eye;
			char       end   = 0;
			if (sscanf(value, "%d,%d,%d%c", &eye[0], &eye[1], &eye[2], &end) != 3) {
				fprintf(stderr, "opt2gltf: %s needs X,Y,Z (craft-table units)\n", flag);
				free(tags);
				return 2;
			}
			for (int k = 0; k < 3; ++k) {
				if (eye[k] < -32768 || eye[k] > 32767) {
					fprintf(stderr, "opt2gltf: %s values must fit in 16 bits\n", flag);
					free(tags);
					return 2;
				}
			}
			if (light)
				has_cockpit_light = true;
			else
				has_cockpit_eye = true;
		} else if (strcmp(flag, "--cockpit-light-intensity") == 0) {
			char* end               = NULL;
			cockpit_light_intensity = strtof(value, &end);
			if (end == value || *end || !isfinite(cockpit_light_intensity) ||
				cockpit_light_intensity <= 0.0f) {
				fprintf(stderr, "opt2gltf: --cockpit-light-intensity needs a positive intensity in candela\n");
				free(tags);
				return 2;
			}
		} else if (strcmp(flag, "--dynamic-cockpit") == 0) {
			dynamic_cockpit = value;
		} else if (strcmp(flag, "--subdivide") == 0) {
			char* end        = NULL;
			subdivide_levels = strtoul(value, &end, 10);
			if (end == value || *end || subdivide_levels > OPT_GLTF_MAX_SUBDIVIDE) {
				fprintf(stderr, "opt2gltf: --subdivide needs a level from 0 to %u\n", OPT_GLTF_MAX_SUBDIVIDE);
				free(tags);
				return 2;
			}
		} else {
			fprintf(stderr, "opt2gltf: unknown option '%s'\n", flag);
			free(tags);
			return 2;
		}
		ai += 2;
	}
	if (xwau_effects) {
		free(tags);
		if (!xwau.roots.count || !project || ai == argc) {
			fprintf(stderr, "opt2gltf: --xwau-effects needs --xwau-root, --project and inputs\n");
			return 2;
		}
		return XwauLibrary_Rebuild(&xwau.roots, project, (const char* const*)argv + ai, (size_t)(argc - ai))
				   ? 0
				   : 1;
	}
	if (argc - ai != (project ? 1 : 2)) {
		free(tags);
		return usage(argv[0]);
	}
	if (replace && !project) {
		fprintf(stderr, "opt2gltf: --replace needs --project\n");
		free(tags);
		return 2;
	}
	if (xwau.ignore_shadeless_count && !xwau.roots.count) {
		fprintf(stderr, "opt2gltf: --xwau-ignore-shadeless needs --xwau-root\n");
		free(tags);
		return 2;
	}
	if (stock && xwau.roots.count) {
		fprintf(stderr, "opt2gltf: --stock excludes --xwau-root\n");
		free(tags);
		return 2;
	}
	if (stock) {
		/* The original game's settings, unless given. */
		if (!smooth_given)
			smooth_angle_deg = 90.0f;
		emissive = true;
	}
	if (xwau.roots.count) {
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
		if (!(xwau.missions = XwauMissions_Load(&xwau.roots))) {
			fprintf(stderr, "opt2gltf: out of memory (missions)\n");
			free(tags);
			return 1;
		}
	}
	const XwauContext*        xwau_ctx = xwau.roots.count ? &xwau : NULL;
	const OptGltfBuildOptions options  = {
		.smooth_angle_degrees         = smooth_angle_deg,
		.repair_normals               = repair_normals,
		.emissive                     = emissive,
		.fullbright_strength          = fullbright_strength,
		.component_tags               = tags,
		.component_tag_count          = tag_count,
		.component_articulations      = articulations,
		.component_articulation_count = articulation_count,
		.articulation_settings        = settings,
		.articulation_setting_count   = setting_count,
		.component_sets               = component_sets,
		.component_set_count          = component_set_count,
		.subdivide_levels             = (uint32_t)subdivide_levels,
	};
	int         status;
	const char* in   = argv[ai];
	const char* outd = project ? NULL : argv[ai + 1];

	struct stat st;
	if (stat(in, &st) != 0) {
		fprintf(stderr, "opt2gltf: cannot stat '%s': %s\n", in, strerror(errno));
		free(tags);
		XwauMissions_Free(xwau.missions);
		return 1;
	}

	if (S_ISDIR(st.st_mode) && (has_cockpit_eye || has_cockpit_light || dynamic_cockpit)) {
		fprintf(stderr,
				"opt2gltf: --cockpit-eye, --cockpit-light and --dynamic-cockpit apply to a single OPT, not a "
				"directory\n");
		free(tags);
		XwauMissions_Free(xwau.missions);
		return 2;
	}
	if (dynamic_cockpit && !xwau_ctx) {
		fprintf(stderr, "opt2gltf: --dynamic-cockpit needs --xwau-root (covers and hologram offsets)\n");
		free(tags);
		return 2;
	}
	const Settings conversion = {
		.base                    = &options,
		.xwau                    = xwau_ctx,
		.stock                   = stock,
		.fullbright_given        = fullbright_strength > 0.0f,
		.cockpit_eye             = has_cockpit_eye ? cockpit_eye : NULL,
		.cockpit_light           = has_cockpit_light ? cockpit_light : NULL,
		.cockpit_light_intensity = cockpit_light_intensity,
		.dynamic_cockpit         = dynamic_cockpit,
	};
	if (S_ISDIR(st.st_mode)) {
		DIR* d = opendir(in);
		if (!d) {
			fprintf(stderr, "opt2gltf: cannot open dir '%s'\n", in);
			free(tags);
			XwauMissions_Free(xwau.missions);
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
			snprintf(out_sub, sizeof out_sub, "%s/%s", outd ? outd : "", base);
			if (project ? convert_into_project(src, base, &conversion, project, replace)
						: convert_one(src, out_sub, base, &conversion))
				++ok_count;
			else
				++fail_count;
		}
		closedir(d);
		fprintf(stderr, "opt2gltf: %d ok, %d failed\n", ok_count, fail_count);
		status = fail_count == 0 ? 0 : 1;
	} else {
		/* Single file: its name after the last '/' (or '\\' on Windows). */
		const char* fname = in;
		for (const char* c = in; *c; ++c)
			if (*c == '/' || *c == '\\')
				fname = c + 1;
		char        base[256];
		strip_ext(base, sizeof base, fname);
		status = (project ? convert_into_project(in, base, &conversion, project, replace)
						  : convert_one(in, outd, base, &conversion))
					 ? 0
					 : 1;
	}
	free(tags);
	XwauMissions_Free(xwau.missions);
	return status;
}
