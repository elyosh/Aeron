#include "model_conversion.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "model_tables.h"
#include "opt.h"
#include "opt_gltf.h"
#include "opt_gltf_write.h"
#include "util.h"
#include "xwau_anim.h"
#include "xwau_dat.h"
#include "xwau_dynamic_cockpit.h"
#include "xwau_gltf_material.h"
#include "xwau_model.h"
#include "xwau_skins.h"

/* ===== Stock tables ===================================================== */

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
	t->text          = StrDup(table);
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
				Report(label, "bad stock articulation %s", token);
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

static int xwau_find(const ModelConversion* mc, const char* rel, char* out, size_t cap) {
	return Xwau_FindFile(mc->game, rel, out, cap) ? 1 : 0;
}

typedef struct XwauNormalMap {
	char         key[1100]; /* resolved path + group + image */
	char         name[160]; /* file-name label: <datbase>_<group>_<image> */
	XwauDatImage image;
	int          ok;
} XwauNormalMap;

/* Load (once per OPT) the image a NormalMap value names: "<path>.dat-G-I".
 * Entries are allocated individually: materials keep pointers into them. */
static const XwauNormalMap* xwau_normal_map(const ModelConversion* mc, const char* value,
											XwauNormalMap*** cache, size_t* count, const char* label) {
	char spec[XWAU_MAT_PATH_MAX];
	if (!PathFormat(spec, sizeof spec, "%s", value))
		return NULL;
	char* image_sep = strrchr(spec, '-');
	char* group_sep = NULL;
	if (image_sep) {
		*image_sep = 0;
		group_sep  = strrchr(spec, '-');
	}
	if (!group_sep) {
		Report(label, "unreadable NormalMap '%s'", value);
		return NULL;
	}
	*group_sep      = 0;
	const int group = atoi(group_sep + 1), image = atoi(image_sep + 1);
	if (!EndsWithNoCase(spec, ".dat")) {
		Report(label, "NormalMap '%s' is not a DAT image; skipped", value);
		return NULL;
	}
	char path[1024];
	if (!xwau_find(mc, spec, path, sizeof path)) {
		Report(label, "NormalMap file '%s' not found; skipped", spec);
		return NULL;
	}
	char key[1100];
	snprintf(key, sizeof key, "%s|%d|%d", path, group, image);
	for (size_t i = 0; i < *count; ++i)
		if (strcmp((*cache)[i]->key, key) == 0)
			return (*cache)[i]->ok ? (*cache)[i] : NULL;
	XwauNormalMap*  nm   = (XwauNormalMap*)calloc(1, sizeof *nm);
	XwauNormalMap** slot = nm ? ARRAY_PUSH(*cache, *count) : NULL;
	if (!slot) {
		free(nm);
		return NULL;
	}
	*slot = nm;
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
		Report(label, "NormalMap %s; skipped", err);
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

static void xwau_fill(XwauGltfMaterial* out, const XwauMatParams* p) {
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
	return xwau_find((const ModelConversion*)context, rel, out, cap);
}

#define MAX_LIT_TEXTURES 128

/* ===== The cockpit and the weapons ====================================== */

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
	opt_mesh_t*      owner     = &opt->meshes[owner_index];
	opt_hardpoint_t* eye_point = ARRAY_PUSH(owner->hardpoints, owner->hardpoint_count);
	if (!eye_point)
		return 0;
	*eye_point = (opt_hardpoint_t) {
		.type = (opt_hardpoint_type_t)OPT_GLTF_COCKPIT_HARDPOINT,
		.pos  = { (float)eye[0], (float)-eye[1], (float)eye[2] },
	};
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

/* ===== One conversion =================================================== */

/* An OPT's build options and the data they point to. */
typedef struct Conversion {
	OptGltfBuildOptions options;
	const int*          cockpit_eye;   /* craft-table units, or NULL */
	const int*          cockpit_light; /* craft-table units, or NULL */
	OptGltfLight        light;

	/* Stock mode. */
	StockTags stock;
	int       stock_eye[3];

	/* XWAU mode. */
	XwauModel*         model;
	XwauSkins          skins;
	XwauDynamicCockpit dc;
	/* The textures shaded as lit surfaces although their material makes
	 * them Shadeless, pointing into lit_text. */
	char        lit_text[1024];
	const char* lit[MAX_LIT_TEXTURES];
	int         lit_count;
	/* The materials' inputs and their bake. */
	XwauMatFile*      mat;
	char              mat_path[1024];
	XwauGltfMaterial* materials;
	XwauGltfAnim*     anims;
	XwauAnim*         anim;
	XwauNormalMap**   maps;
	size_t            map_count;
	XwauGltfMaterials baked;
	bool              has_baked;
} Conversion;

/* The original game's settings. */
static bool prepare_stock(Conversion* c, const ModelConversion* mc, const char* basename, const char* label) {
	c->options.geometry.normals              = OPT_GLTF_NORMALS_SMOOTHED;
	c->options.geometry.smooth_angle_degrees = mc->smooth_angle >= 0.0f ? mc->smooth_angle : 90.0f;
	c->options.material.emissive             = true;
	if (ModelTables_IsProjectile(basename))
		c->options.material =
			(OptGltfMaterialOptions) { .source = OPT_GLTF_MATERIALS_FULLBRIGHT, .fullbright_strength = 5.0f };
	if (ModelTables_StockEye(basename, c->stock_eye))
		c->cockpit_eye = c->stock_eye;
	const bool ok                       = stock_tags_read(&c->stock, basename, label);
	c->options.model.tags               = c->stock.tags;
	c->options.model.tag_count          = c->stock.tag_count;
	c->options.model.articulations      = c->stock.articulations;
	c->options.model.articulation_count = c->stock.articulation_count;
	return ok;
}

/* What XWAU's files give the model, but its materials. */
static bool prepare_xwau(Conversion* c, const ModelConversion* mc, const opt_file_t* opt,
						 const char* basename, const char* label) {
	c->model        = (XwauModel*)malloc(sizeof *c->model);
	const bool read = c->model && XwauModel_Read(mc->game, mc->missions, basename, c->model, label);
	if (read) {
		c->options.model.articulations      = c->model->articulations;
		c->options.model.articulation_count = c->model->articulation_count;
		c->options.model.sets               = c->model->sets;
		c->options.model.set_count          = c->model->set_count;
		c->options.model.settings           = c->model->settings;
		c->options.model.setting_count      = c->model->setting_count;
		c->cockpit_eye                      = c->model->has_eye ? c->model->eye : NULL;
		c->cockpit_light                    = c->model->has_craft_eye ? c->model->craft_eye : NULL;
	}
	const char* listed = ModelTables_LitTextures(basename);
	if (listed)
		snprintf(c->lit_text, sizeof c->lit_text, "%s", listed);
	char* token = c->lit_text[0] ? c->lit_text : NULL;
	while (token && c->lit_count < MAX_LIT_TEXTURES) {
		char* next = strchr(token, ' ');
		if (next)
			*next++ = 0;
		c->lit[c->lit_count++] = token;
		token                  = next;
	}
	const int32_t weapons = weapon_hardpoints(opt);
	if (weapons > WEAPON_SLOTS)
		Report(label, "%d weapon hardpoints; the game keeps %d weapon slots", (int)weapons, WEAPON_SLOTS);
	return read && XwauSkins_Resolve(mc->game, mc->missions, basename, opt, label, &c->skins);
}

/* The pilot eye hardpoint and a cockpit model's interior light. */
static bool place_cockpit(Conversion* c, opt_file_t* opt, const char* label) {
	if (c->cockpit_eye && !apply_cockpit_eye(opt, c->cockpit_eye)) {
		Report(label, "cannot add the cockpit eye hardpoint");
		return false;
	}
	if (!c->cockpit_light)
		return true;
	c->light = (OptGltfLight) {
		.mesh      = main_hull_mesh(opt),
		.position  = { (float)c->cockpit_light[0], (float)-c->cockpit_light[1], (float)c->cockpit_light[2] },
		.color     = { 1.0f, 1.0f, 1.0f },
		.intensity = COCKPIT_LIGHT_INTENSITY,
		.range     = COCKPIT_LIGHT_RANGE,
	};
	if (c->light.mesh < 0) {
		Report(label, "no mesh for the cockpit light");
		return false;
	}
	c->options.model.lights      = &c->light;
	c->options.model.light_count = 1;
	return true;
}

/* The model's Dynamic Cockpit screens, when it has a file for them. */
static bool load_dynamic_cockpit(Conversion* c, const ModelConversion* mc, opt_file_t* opt,
								 const char* basename, const char* label) {
	const char* dynamic = c->model && c->model->dynamic_cockpit[0] ? c->model->dynamic_cockpit : NULL;
	if (dynamic &&
		(!XwauDynamicCockpit_Load(dynamic, opt, basename, xwau_find_file, (void*)mc, &c->dc, label) ||
		 !XwauDynamicCockpit_ApplyHologramOffset(opt, &c->dc))) {
		Report(label, "cannot apply %s", dynamic);
		return false;
	}
	return true;
}

/* Reads each texture's XWAU material, normal map and animations, and bakes
 * them into the given materials. */
static bool bake_xwau(Conversion* c, const ModelConversion* mc, const opt_file_t* opt, const char* basename,
					  const char* label) {
	char rel[512];
	if (!PathFormat(rel, sizeof rel, "Materials/%s.mat", basename))
		return false;
	c->mat =
		xwau_find(mc, rel, c->mat_path, sizeof c->mat_path) ? XwauMat_Load(c->mat_path, &mc->global) : NULL;

	const OptGltfScreen* screens = c->dc.screens;
	const size_t         count   = opt->texture_count ? (size_t)opt->texture_count : 1;
	c->materials                 = (XwauGltfMaterial*)calloc(count, sizeof *c->materials);
	c->anims                     = (XwauGltfAnim*)calloc(count, sizeof *c->anims);
	c->anim                      = XwauAnim_Begin(basename, xwau_find_file, (void*)mc, label);
	if (!c->materials || !c->anims || !c->anim)
		return false;
	size_t normal_textures           = 0;
	bool   ignored[MAX_LIT_TEXTURES] = { false };
	for (int32_t i = 0; i < opt->texture_count; ++i) {
		XwauGltfMaterial*    material = &c->materials[i];
		const XwauMatParams* p        = c->mat ? XwauMat_Find(c->mat, opt->textures[i].name) : &mc->global;
		xwau_fill(material, p);
		for (int k = 0; k < c->lit_count; ++k) {
			if (xwau_texture_named(opt->textures[i].name, c->lit[k])) {
				material->shadeless = false;
				material->lit       = true;
				ignored[k]          = true;
			}
		}
		XwauAnimScreen screen = XWAU_ANIM_SCREEN_NONE;
		if (screens && screens[i].present)
			screen = screens[i].hologram          ? XWAU_ANIM_SCREEN_HOLOGRAM
					 : screens[i].cover_base.rgba ? XWAU_ANIM_SCREEN_COVER
												  : XWAU_ANIM_SCREEN_NO_COVER;
		if (!XwauAnim_Texture(c->anim, c->mat, &opt->textures[i], XwauGltf_Unlit(material), screen,
							  &c->anims[i])) {
			Report(label, "out of memory (animations)");
			return false;
		}
		if (p->normal_map[0]) {
			const XwauNormalMap* nm = xwau_normal_map(mc, p->normal_map, &c->maps, &c->map_count, label);
			if (nm) {
				material->normal_map.rgba   = nm->image.rgba;
				material->normal_map.width  = nm->image.width;
				material->normal_map.height = nm->image.height;
				material->normal_map_name   = nm->name;
				normal_textures++;
			}
		}
	}
	for (int k = 0; k < c->lit_count; ++k) {
		if (!ignored[k])
			Report(label, "lit texture %s: no such texture", c->lit[k]);
	}
	/* Untextured faces: the material lookup of an empty name is slot 0. */
	XwauGltfMaterial untextured;
	xwau_fill(&untextured, c->mat ? XwauMat_Find(c->mat, "") : &mc->global);

	Report(label, "xwau materials %s, %zu of %d textures normal-mapped (%zu maps)",
		   c->mat ? c->mat_path : "(default)", normal_textures, (int)opt->texture_count, c->map_count);
	XwauAnim_Report(c->anim);
	c->has_baked =
		XwauGltf_Bake(opt, basename, c->materials, &untextured, XwauAnim_Any(c->anim) ? c->anims : NULL,
					  screens, c->skins.skins.variant_count ? &c->skins : NULL, &c->baked);
	if (!c->has_baked)
		return false;
	OptGltfMaterialOptions* m = &c->options.material;
	m->source                 = OPT_GLTF_MATERIALS_GIVEN;
	m->given                  = c->baked.textures;
	m->untextured             = &c->baked.untextured;
	m->variants               = c->baked.variants.count ? &c->baked.variants : NULL;
	m->screens                = screens;
	/* XWAU's renderer uses each corner's stored normal: no remap, no
	 * repair. */
	c->options.geometry.normals = OPT_GLTF_NORMALS_STORED;
	return true;
}

static void conversion_free(Conversion* c, const opt_file_t* opt) {
	if (c->has_baked)
		XwauGltf_Free(&c->baked, opt->texture_count);
	for (size_t i = 0; i < c->map_count; ++i) {
		XwauDat_FreeImage(&c->maps[i]->image);
		free(c->maps[i]);
	}
	free(c->maps);
	free(c->materials);
	free(c->anims);
	XwauAnim_End(c->anim);
	XwauMat_Free(c->mat);
	XwauDynamicCockpit_Free(&c->dc);
	XwauSkins_Free(&c->skins);
	stock_tags_free(&c->stock);
	free(c->model);
}

bool ModelConversion_Convert(const ModelConversion* mc, const char* opt_path, const char* basename,
							 const char* out_dir) {
	opt_error_t err;
	opt_file_t* opt = opt_load_file(opt_path, &err);
	if (!opt) {
		Report(opt_path, "%s", err.msg);
		return false;
	}
	Conversion* c  = (Conversion*)calloc(1, sizeof *c);
	bool        ok = c != NULL;
	if (ok && mc->xwau)
		ok = prepare_xwau(c, mc, opt, basename, opt_path) && place_cockpit(c, opt, opt_path) &&
			 load_dynamic_cockpit(c, mc, opt, basename, opt_path) &&
			 bake_xwau(c, mc, opt, basename, opt_path) &&
			 OptGltf_Convert(opt, out_dir, basename, &c->options, opt_path);
	else if (ok)
		ok = prepare_stock(c, mc, basename, opt_path) && place_cockpit(c, opt, opt_path) &&
			 OptGltf_Convert(opt, out_dir, basename, &c->options, opt_path);
	if (ok && mc->xwau)
		XwauGltf_ReportSkins(&c->baked, basename);
	if (c)
		conversion_free(c, opt);
	free(c);
	opt_free(opt);
	return ok;
}

/* ===== The mode ========================================================= */

bool ModelConversion_InitXwau(ModelConversion* mc, const char* game) {
	memset(mc, 0, sizeof *mc);
	mc->xwau = true;
	mc->game = game;
	XwauMat_CodeDefaults(&mc->global);
	char global_path[1024];
	if (xwau_find(mc, "Materials/DefaultGlobalMaterial.mat", global_path, sizeof global_path))
		XwauMat_ReadGlobalDefault(global_path, &mc->global);
	else
		Report(NULL, "no Materials/DefaultGlobalMaterial.mat; using built-in defaults");
	if (!(mc->missions = XwauMissions_Load(mc->game, false))) {
		Report(NULL, "out of memory (missions)");
		return false;
	}
	return true;
}

void ModelConversion_Free(ModelConversion* mc) {
	XwauMissions_Free(mc->missions);
	mc->missions = NULL;
}
