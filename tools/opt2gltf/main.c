/*
 * opt2gltf — convert an OPT model to glTF 2.0 (.gltf, .bin and PNG
 * textures), and write the batch outputs of an XWA Upgrade conversion.
 *
 * Usage:
 *   opt2gltf (--stock | --xwau-root DIR...) [--smooth-angle DEG]
 *            (--project DIR [--replace] | <output_dir>) FILE.opt
 *   opt2gltf effects --xwau-root DIR... --project DIR <project|model.gltf>...
 *   opt2gltf missions --xwau-root DIR... --stock-game DIR <output_dir>
 *
 * The model is written into <output_dir>, or into a project with --project:
 * a folder holding models (models/<key>/) and their effect libraries
 * (effects/<library>/), where the OPT becomes models/<key>/<key>.gltf, its
 * .bin and textures/, the key being the OPT's name in lower case; in XWAU
 * mode, the effect library images its materials name that the project
 * lacks are added (xwau_library.h). An existing model folder is an error,
 * unless --replace replaces it.
 *
 * `effects` writes the effect libraries every model given names (a model
 * .gltf, or every model of a project) into the --project's effects/,
 * replacing them, with the game's effects.yaml (xwau_library.h).
 *
 * `missions` writes what XWAU's missions select for the original game's
 * missions (installed at --stock-game) as <output_dir>/<dir>/<mission>.yaml
 * (xwau_mission_files.h).
 *
 * Replacement workflow (both tools are built with AERON_BUILD_TOOLS):
 *
 *   opt2gltf --stock FLIGHTMODELS/XWING.OPT out
 *   aeron_gltf_cook out/XWING.gltf --out <asset root>/remaster/models/xwing.glb
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
 *     orthonormalized.
 *
 * --stock converts an OPT of the original game as the game draws it, with
 * nothing read beside it:
 *   - normals regenerated with a 90 degree smoothing threshold, or the
 *     --smooth-angle one: connected smoothing fans are built across shared
 *     edges, then averaged with corner-angle weights so triangulation
 *     density does not bias them; wrapped/non-manifold fans split before a
 *     normal can point behind its rendered triangle (e.g. 45 for crisp flat
 *     panels);
 *   - self-illumination (lit windows, cockpit lights): textures with
 *     self-lit texels get a second *_emissive.png as the material's
 *     emissiveTexture. For XWA (version 5) OPTs this replicates the
 *     engine's lightmap derivation (ModelTexture_FilterHardwarePalette @
 *     0x44A600 + CreateD3DfromTexture): an entry is self-lit when its
 *     darkest palette row is bright (squared 5-bit magnitude >= 32, green
 *     LSB dropped) and rows 1..6 stay within squared distance 16 of row 0;
 *     glow color from palette row 10; textures named "_*" and palettes
 *     where none/all 256 entries classify are skipped;
 *   - projectiles (model_tables.h) fully self-lit instead, as the classic
 *     renderer draws them: black base color with the texture's alpha kept,
 *     the base texture as emissiveTexture at strength 5;
 *   - the component tags and articulations the game ties to fixed component
 *     numbers, and the pilot eye the game's craft table gives the player's
 *     craft (model_tables.h): the OPT's Cockpit (type 31) hardpoints are
 *     replaced by one at the eye.
 *
 * --xwau-root DIR (repeatable) converts X-Wing Alliance Upgrade models with
 * everything XWAU gives them. The roots are layered game directories (an
 * installation, or unpacked package groups), searched in order with
 * case-insensitive paths: list an update before the release it updates.
 * Stored normals are used per corner. For each OPT:
 *   - Materials/<basename>.mat (on top of Materials/DefaultGlobalMaterial.mat)
 *     gives each texture's material and normal map, read from the DAT image
 *     it names. The converter then reproduces that renderer's shading in
 *     standard glTF inputs: authored colors, illumination, shadeless and
 *     glass texels, metallic-roughness, KHR_materials_specular, normal maps
 *     with NMIntensity baked in and MikkTSpace tangents;
 *   - its image sequences become material animations (xwau_anim.h);
 *   - its files beside the OPT give the pilot eye, object profiles,
 *     articulations, Dynamic Cockpit and a cockpit model's interior light
 *     (xwau_model.h), and its skins material variants (xwau_skins.h), with
 *     what the missions give it (xwau_missions.h);
 *   - the crew and glass textures model_tables.h lists are lit surfaces
 *     although their material makes them Shadeless;
 *   - more than 120 weapon hardpoints are reported: the game keeps the
 *     first 120 weapon slots, as XWAU does.
 */

#include <ctype.h>
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
#include "xwau_mission_files.h"
#include "xwau_missions.h"
#include "xwau_model.h"
#include "xwau_skins.h"

/* Copies the name ending at `end` into `out` (64 bytes); false when empty or
 * too long. The name's syntax is checked by the converter. */
static bool copy_name(const char* begin, const char* end, char out[64]) {
	if (end <= begin || end - begin >= 64)
		return false;
	memcpy(out, begin, (size_t)(end - begin));
	out[end - begin] = 0;
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

typedef struct XwauContext {
	XwauRoots     roots;
	XwauMatParams global; /* Materials/DefaultGlobalMaterial.mat, else code defaults */
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

/* The first MainHull mesh, or mesh 0; -1 without meshes. */
static int32_t main_hull_mesh(const opt_file_t* opt) {
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m = &opt->meshes[mi];
		if (m->has_descriptor && m->descriptor.mesh_type == OPT_MT_MAIN_HULL)
			return mi;
	}
	return opt->mesh_count > 0 ? 0 : -1;
}

/* A cockpit model's interior light: white, at its craft's pilot eye, dim
 * enough to light the interior softly from the viewpoint (its shadows would
 * be hidden anyway). Its intensity is a 100 W Blender point light in
 * candela, as Blender's glTF exporter converts it (683 lm/W over 4 pi sr). */
#define COCKPIT_LIGHT_INTENSITY (100.0f * 683.0f / (4.0f * 3.14159265f))
#define COCKPIT_LIGHT_RANGE 3.0f

/* Replaces the OPT's Cockpit (type 31) hardpoints with one at the pilot eye
 * given in craft-table units (CockpitPovX/Y/Z): ModelMesh_GetHardpoint
 * returns (x, -y, z), so the eye's Y is the hardpoint's -y. It goes on the
 * first MainHull mesh, or mesh 0. */
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
	const XwauContext*         xwau; /* XWAU mode, else stock mode */
} Settings;

/* The original game's tags and articulations of a model (model_tables.h). */
typedef struct StockTags {
	OptGltfComponentTag*          tags;
	size_t                        tag_count;
	OptGltfComponentArticulation* articulations;
	size_t                        articulation_count;
	char*                         text; /* the table's text the tags point into */
} StockTags;

static void stock_tags_free(StockTags* t) {
	free(t->tags);
	free(t->articulations);
	free(t->text);
	memset(t, 0, sizeof *t);
}

static bool stock_tags_read(StockTags* t, const char* model, const char* label) {
	memset(t, 0, sizeof *t);
	const char* table = ModelTables_StockTags(model);
	if (!table)
		return true;
	size_t count = 1;
	for (const char* c = table; *c; ++c)
		count += *c == ' ';
	t->tags          = (OptGltfComponentTag*)calloc(count, sizeof *t->tags);
	t->articulations = (OptGltfComponentArticulation*)calloc(count, sizeof *t->articulations);
	t->text          = Xwau_Strdup(table);
	if (!t->tags || !t->articulations || !t->text)
		return false;
	for (char *token = t->text, *next; token; token = next) {
		next = strchr(token, ' ');
		if (next)
			*next++ = 0;
		if (!*token)
			continue;
		char*         equals = strchr(token, '=');
		char*         end    = NULL;
		unsigned long mesh   = strtoul(token, &end, 10);
		if (strchr(token, ':')) {
			if (!parse_articulation(token, &t->articulations[t->articulation_count])) {
				fprintf(stderr, "opt2gltf: %s: bad stock articulation %s\n", label, token);
				return false;
			}
			t->articulation_count++;
		} else if (equals && end == equals) {
			t->tags[t->tag_count].mesh_index = (uint32_t)mesh;
			t->tags[t->tag_count++].tag      = equals + 1;
		}
	}
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
	StockTags           stock   = { 0 };
	XwauModel*          model   = s->xwau ? (XwauModel*)malloc(sizeof *model) : NULL;
	XwauSkins           skins   = { 0 };
	DynamicCockpit      dc      = { 0 };
	const char*         lit[MAX_LIT_TEXTURES];
	int                 lit_count      = 0;
	char                lit_text[1024] = "";
	int                 eye[3];
	const int*          cockpit_eye   = NULL;
	const int*          cockpit_light = NULL;
	int                 ok            = 1;
	if (!s->xwau) {
		/* The original game's settings. */
		if (ModelTables_IsProjectile(basename))
			options.fullbright_strength = 5.0f;
		if (ModelTables_StockEye(basename, eye))
			cockpit_eye = eye;
		ok                                   = stock_tags_read(&stock, basename, opt_path);
		options.component_tags               = stock.tags;
		options.component_tag_count          = stock.tag_count;
		options.component_articulations      = stock.articulations;
		options.component_articulation_count = stock.articulation_count;
	} else {
		ok = model && XwauModel_Read(&s->xwau->roots, s->xwau->missions, basename, model, opt_path);
		if (ok) {
			options.component_articulations      = model->articulations;
			options.component_articulation_count = model->articulation_count;
			options.component_sets               = model->sets;
			options.component_set_count          = model->set_count;
			options.articulation_settings        = model->settings;
			options.articulation_setting_count   = model->setting_count;
			cockpit_eye                          = model->has_eye ? model->eye : NULL;
			cockpit_light                        = model->has_craft_eye ? model->craft_eye : NULL;
		}
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
			.intensity = COCKPIT_LIGHT_INTENSITY,
			.range     = COCKPIT_LIGHT_RANGE,
		};
		if (light.mesh < 0) {
			fprintf(stderr, "opt2gltf: %s: no mesh for the cockpit light\n", opt_path);
			ok = 0;
		}
		options.lights      = &light;
		options.light_count = 1;
	}
	const char* dynamic = ok && model && model->dynamic_cockpit[0] ? model->dynamic_cockpit : NULL;
	if (dynamic &&
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
	stock_tags_free(&stock);
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

static int usage(void) {
	fprintf(stderr, "usage: opt2gltf (--stock | --xwau-root DIR...) [--smooth-angle DEG]\n"
					"                (--project DIR [--replace] | <output_dir>) FILE.opt\n"
					"       opt2gltf effects --xwau-root DIR... --project DIR <project|model.gltf>...\n"
					"       opt2gltf missions --xwau-root DIR... --stock-game DIR <output_dir>\n");
	return 2;
}

/* ===== Arguments ======================================================== */

enum {
	FLAG_STOCK        = 1 << 0,
	FLAG_XWAU_ROOT    = 1 << 1,
	FLAG_SMOOTH_ANGLE = 1 << 2,
	FLAG_PROJECT      = 1 << 3,
	FLAG_REPLACE      = 1 << 4,
	FLAG_STOCK_GAME   = 1 << 5,
};

static const struct {
	const char* name;
	unsigned    flag;
	bool        value;
} kFlags[] = {
	{ "--stock", FLAG_STOCK, false },
	{ "--xwau-root", FLAG_XWAU_ROOT, true },
	{ "--smooth-angle", FLAG_SMOOTH_ANGLE, true },
	{ "--project", FLAG_PROJECT, true },
	{ "--replace", FLAG_REPLACE, false },
	{ "--stock-game", FLAG_STOCK_GAME, true },
};

typedef struct Args {
	XwauRoots   roots;
	bool        stock;
	float       smooth_angle; /* negative when not given */
	const char* project;
	bool        replace;
	const char* stock_game;
	char**      inputs; /* the arguments after the flags */
	int         input_count;
} Args;

/* Parses the flags `allowed` names, then the inputs; false on a flag that
 * is unknown, not allowed or without its value (reported). */
static bool parse_args(int argc, char** argv, unsigned allowed, Args* out) {
	memset(out, 0, sizeof *out);
	out->smooth_angle = -1.0f;
	int ai            = 0;
	for (; ai < argc && strncmp(argv[ai], "--", 2) == 0; ++ai) {
		size_t f = 0;
		while (f < sizeof kFlags / sizeof kFlags[0] && strcmp(argv[ai], kFlags[f].name) != 0)
			++f;
		if (f == sizeof kFlags / sizeof kFlags[0] || !(allowed & kFlags[f].flag)) {
			fprintf(stderr, "opt2gltf: unknown option '%s'\n", argv[ai]);
			return false;
		}
		const char* value = kFlags[f].value ? argv[ai + 1] : NULL;
		if (kFlags[f].value && (ai + 1 == argc || !value)) {
			fprintf(stderr, "opt2gltf: %s needs a value\n", argv[ai]);
			return false;
		}
		ai += kFlags[f].value;
		switch (kFlags[f].flag) {
			case FLAG_STOCK:
				out->stock = true;
				break;
			case FLAG_XWAU_ROOT:
				if (out->roots.count == XWAU_MAX_ROOTS) {
					fprintf(stderr, "opt2gltf: --xwau-root: at most %d\n", XWAU_MAX_ROOTS);
					return false;
				}
				out->roots.dirs[out->roots.count++] = value;
				break;
			case FLAG_SMOOTH_ANGLE: {
				char* end         = NULL;
				out->smooth_angle = strtof(value, &end);
				if (end == value || *end || !isfinite(out->smooth_angle) || out->smooth_angle < 0.0f ||
					out->smooth_angle > 180.0f) {
					fprintf(stderr, "opt2gltf: --smooth-angle needs an angle from 0 to 180 degrees\n");
					return false;
				}
				break;
			}
			case FLAG_PROJECT:
				out->project = value;
				break;
			case FLAG_REPLACE:
				out->replace = true;
				break;
			case FLAG_STOCK_GAME:
				out->stock_game = value;
				break;
		}
	}
	out->inputs      = argv + ai;
	out->input_count = argc - ai;
	return true;
}

/* ===== Commands ========================================================= */

/* opt2gltf (--stock | --xwau-root DIR...) [--smooth-angle DEG]
 *          (--project DIR [--replace] | <output_dir>) FILE.opt */
static int run_convert(int argc, char** argv) {
	Args a;
	if (!argc)
		return usage();
	if (!parse_args(argc, argv, FLAG_STOCK | FLAG_XWAU_ROOT | FLAG_SMOOTH_ANGLE | FLAG_PROJECT | FLAG_REPLACE,
					&a))
		return 2;
	if (a.stock == (a.roots.count > 0)) {
		fprintf(stderr, "opt2gltf: give --stock or --xwau-root\n");
		return usage();
	}
	if (a.smooth_angle >= 0.0f && !a.stock) {
		fprintf(stderr, "opt2gltf: --smooth-angle applies to --stock\n");
		return 2;
	}
	if (a.replace && !a.project) {
		fprintf(stderr, "opt2gltf: --replace needs --project\n");
		return 2;
	}
	if (a.input_count != (a.project ? 1 : 2))
		return usage();
	const char* in = a.inputs[0];
	struct stat st;
	if (stat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
		fprintf(stderr, "opt2gltf: %s: not an OPT file\n", in);
		return 1;
	}

	OptGltfBuildOptions options = { .smooth_angle_degrees = -1.0f, .repair_normals = true };
	static XwauContext  xwau;
	if (a.stock) {
		options.smooth_angle_degrees = a.smooth_angle >= 0.0f ? a.smooth_angle : 90.0f;
		options.emissive             = true;
	} else {
		xwau.roots = a.roots;
		XwauMat_CodeDefaults(&xwau.global);
		char global_path[1024];
		if (xwau_find(&xwau, "Materials/DefaultGlobalMaterial.mat", global_path, sizeof global_path))
			XwauMat_ReadGlobalDefault(global_path, &xwau.global);
		else
			fprintf(stderr, "opt2gltf: no Materials/DefaultGlobalMaterial.mat; using built-in defaults\n");
		if (!(xwau.missions = XwauMissions_Load(&xwau.roots, false))) {
			fprintf(stderr, "opt2gltf: out of memory (missions)\n");
			return 1;
		}
	}
	const Settings conversion = { .base = &options, .xwau = a.stock ? NULL : &xwau };

	/* The OPT's name: after the last '/' (or '\\' on Windows). */
	const char* fname = in;
	for (const char* c = in; *c; ++c)
		if (*c == '/' || *c == '\\')
			fname = c + 1;
	char base[256];
	strip_ext(base, sizeof base, fname);
	const int ok = a.project ? convert_into_project(in, base, &conversion, a.project, a.replace)
							 : convert_one(in, a.inputs[1], base, &conversion);
	XwauMissions_Free(xwau.missions);
	return ok ? 0 : 1;
}

/* opt2gltf effects --xwau-root DIR... --project DIR <project|model.gltf>... */
static int run_effects(int argc, char** argv) {
	Args a;
	if (!parse_args(argc, argv, FLAG_XWAU_ROOT | FLAG_PROJECT, &a))
		return 2;
	if (!a.roots.count || !a.project || !a.input_count) {
		fprintf(stderr, "opt2gltf: effects needs --xwau-root, --project and inputs\n");
		return 2;
	}
	return XwauLibrary_Rebuild(&a.roots, a.project, (const char* const*)a.inputs, (size_t)a.input_count) ? 0
																										 : 1;
}

/* opt2gltf missions --xwau-root DIR... --stock-game DIR <output_dir> */
static int run_missions(int argc, char** argv) {
	Args a;
	if (!parse_args(argc, argv, FLAG_XWAU_ROOT | FLAG_STOCK_GAME, &a))
		return 2;
	if (!a.roots.count || !a.stock_game || a.input_count != 1) {
		fprintf(stderr, "opt2gltf: missions needs --xwau-root, --stock-game and an output folder\n");
		return 2;
	}
	XwauMissions* missions = XwauMissions_Load(&a.roots, true);
	if (!missions) {
		fprintf(stderr, "opt2gltf: out of memory (missions)\n");
		return 1;
	}
	const bool ok = XwauMissionFiles_Write(&a.roots, missions, a.stock_game, a.inputs[0]);
	XwauMissions_Free(missions);
	return ok ? 0 : 1;
}

int main(int argc, char** argv) {
	if (argc > 1 && strcmp(argv[1], "effects") == 0)
		return run_effects(argc - 2, argv + 2);
	if (argc > 1 && strcmp(argv[1], "missions") == 0)
		return run_missions(argc - 2, argv + 2);
	return run_convert(argc - 1, argv + 1);
}
