#include "model_conversion.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aeron/asset/flight_names.h"
#include "classic_radiance.h"
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

/* Copies the name from `begin` to `end` into `out` (64 bytes). Returns false
 * when it is empty or too long. The converter checks the name syntax. */
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

/* The tags and articulations of a model in the original game (model_tables.h). */
typedef struct StockTags {
	OptGltfComponentTag*          tags;
	size_t                        tag_count;
	OptGltfComponentArticulation* articulations;
	size_t                        articulation_count;
	char*                         text; /* copy of the table text, which the tags point into */
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

/* Whether texture `name`, without its "_fg_" skin suffix, equals `wanted` in
 * any ASCII case. XwauMat_Find compares names the same way. */
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
	char         name[160]; /* used in file names: <datbase>_<group>_<image> */
	XwauDatImage image;
	int          ok;
} XwauNormalMap;

/* Loads the image that a NormalMap value names ("<path>.dat-G-I"), once per
 * OPT. Each entry is allocated on its own because materials keep pointers
 * into it. */
static const XwauNormalMap* xwau_normal_map(const ModelConversion* mc, const char* value,
											XwauNormalMap*** cache, size_t* count, const char* label) {
	char spec[XWAU_MAT_PATH_MAX];
	if (!PathFormat(spec, sizeof spec, "%s", value))
		return NULL;
	/* A value that names no DAT file can be a ZIP file or another image,
	 * which XWAU can load. */
	bool names_dat = false;
	for (const char* at = spec; *at && !names_dat; ++at)
		names_dat = strncasecmp(at, ".dat", 4) == 0;
	if (!names_dat) {
		ReportUnsupported("NormalMap images that aren't in a DAT file");
		return NULL;
	}
	char* image_sep = strrchr(spec, '-');
	char* group_sep = NULL;
	if (image_sep) {
		*image_sep = 0;
		group_sep  = strrchr(spec, '-');
	}
	if (!group_sep)
		return NULL;
	*group_sep      = 0;
	const int group = atoi(group_sep + 1), image = atoi(image_sep + 1);
	if (!EndsWithNoCase(spec, ".dat")) {
		ReportUnsupported("NormalMap images that aren't in a DAT file");
		return NULL;
	}
	char path[1024];
	if (!xwau_find(mc, spec, path, sizeof path))
		return NULL;
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
		/* BC5 stores only X and Y, so Z is rebuilt here. XWAU's DATReader
		 * rebuilds Z from 0..1 values, which is a bug and is not reproduced. */
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
	/* NoColorAlpha only has an effect together with AlphaToBloom, and
	 * AlphaToBloom already maps to a texel that only glows. */
	out->alpha_to_bloom = p->alpha_to_bloom;
	out->surface_tag    = p->surface_tag[0] ? p->surface_tag : NULL;
}

static int xwau_find_file(void* context, const char* rel, char* out, size_t cap) {
	return xwau_find((const ModelConversion*)context, rel, out, cap);
}

#define MAX_LIT_TEXTURES 128

/* ===== The cockpit and the weapons ====================================== */

/* Returns the first MainHull mesh, or mesh 0, or -1 when there are no meshes. */
static int32_t main_hull_mesh(const opt_file_t* opt) {
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m = &opt->meshes[mi];
		if (m->has_descriptor && m->descriptor.mesh_type == OPT_MT_MAIN_HULL)
			return mi;
	}
	return opt->mesh_count > 0 ? 0 : -1;
}

/* Replaces the Cockpit hardpoints (type 31) of the OPT with one hardpoint at
 * the pilot eye. The eye is in craft-table units (CockpitPovX/Y/Z).
 * ModelMesh_GetHardpoint returns (x, -y, z), so the hardpoint's y is the
 * eye's -Y. The new hardpoint goes on the first MainHull mesh, or on mesh 0. */
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
		.type = OPT_GLTF_COCKPIT_HARDPOINT,
		.pos  = { (float)eye[0], (float)-eye[1], (float)eye[2] },
	};
	return 1;
}

/* Reports what the glTF model can't keep from the OPT:
 *   - The model has LOD 0 only, and the game draws it at every distance.
 *     The LODs after it are counted with ReportUnsupported.
 *   - A mesh type has a name only from 0 to 31. XWA reads the mesh type of a
 *     component as an index in tables of 32 entries, such as
 *     g_meshTypeComponentMaxHp, without a range check. So a larger type
 *     changes the component in XWAU, and it can't become `default`.
 *   - Only the explosion type flags 1 and 2 have names, because the game
 *     reads only those (ModelMesh_HasExplosionType1 and 2). The other flags
 *     have no effect, so they are left out without a report.
 *   - The NodeSwitch states past OPT_MAX_NODE_SWITCH_STATES are left out.
 *     XWA never shows them.
 * In XWAU mode, a mesh type above 31 makes the import fail. */
static void report_opt_drops(const ModelConversion* mc, const opt_file_t* opt, const char* label) {
	const bool xwau = mc->xwau;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m = &opt->meshes[mi];
		if (m->lod_count > 1)
			ReportUnsupported("OPT LODs after LOD 0");
		if ((uint32_t)m->descriptor.mesh_type >= AERON_FLIGHT_MESH_TYPE_COUNT && xwau)
			ReportFatal(label, "mesh %d has mesh type %u, which has no name", (int)mi,
						(unsigned)m->descriptor.mesh_type);
		else if ((uint32_t)m->descriptor.mesh_type >= AERON_FLIGHT_MESH_TYPE_COUNT)
			Report(label, "mesh %d has mesh type %u, which has no name, so it becomes default", (int)mi,
				   (unsigned)m->descriptor.mesh_type);
	}
	if (opt->dropped_switch_states)
		Report(label,
			   "%d NodeSwitch state(s) are past the %d states that the game can show, so they are left out",
			   (int)opt->dropped_switch_states, OPT_MAX_NODE_SWITCH_STATES);
}

/* ===== One conversion =================================================== */

/* The build options of one OPT and the data they point to. */
typedef struct Conversion {
	OptGltfBuildOptions options;
	const int*          cockpit_eye; /* craft-table units, or NULL */

	/* Stock mode. */
	StockTags stock;
	int       stock_eye[3];

	/* XWAU mode. */
	XwauModel*         model;
	XwauSkins          skins;
	XwauDynamicCockpit dc;
	/* Textures shaded as lit surfaces even though their material makes them
	 * Shadeless. The names point into lit_text. */
	char        lit_text[1024];
	const char* lit[MAX_LIT_TEXTURES];
	int         lit_count;
	/* The material inputs and the baked result. */
	XwauMatFile*      mat;
	char              mat_path[1024];
	XwauGltfMaterial* materials;
	XwauGltfAnim*     anims;
	XwauAnim*         anim;
	XwauNormalMap**   maps;
	size_t            map_count;
	XwauGltfMaterials baked;
	bool              has_baked;
	/* The lights of a bolt model. */
	OptGltfLight* lights;
	size_t        light_count;
	/* The cockpit levers, with the textures of each in turn. */
	OptGltfControl* controls;
	size_t          control_count;
	int32_t*        control_textures;
} Conversion;

/* Whether the game only shows the model from close. It knows these models
 * by the names it gives them: <name>Cockpit and <name>Exterior for the
 * player's craft, <ship>Gunner and <ship>Gunner2 for its gunner seats, and
 * the fallbacks TieCockpit, CombatSimCockpit and CorellianTransportGunner. A
 * gunner seat with another name in a [Turrets] line is missed, and gets
 * levels of detail it never uses. */
static bool seen_close(const char* basename) {
	static const char* const kEndings[] = { "cockpit", "exterior", "gunner", "gunner2" };
	const size_t             length     = strlen(basename);
	for (size_t i = 0; i < sizeof kEndings / sizeof kEndings[0]; ++i) {
		const size_t ending = strlen(kEndings[i]);
		if (length >= ending && strcasecmp(basename + length - ending, kEndings[i]) == 0)
			return true;
	}
	return false;
}

/* Sets the options of stock mode, as the original game draws the model. */
static bool prepare_stock(Conversion* c, const ModelConversion* mc, const char* basename, const char* label) {
	c->options.geometry.normals              = OPT_GLTF_NORMALS_SMOOTHED;
	c->options.geometry.smooth_angle_degrees = mc->smooth_angle >= 0.0f ? mc->smooth_angle : 90.0f;
	c->options.material.emissive             = true;
	if (ModelTables_IsProjectile(basename))
		c->options.material =
			(OptGltfMaterialOptions) { .source              = OPT_GLTF_MATERIALS_FULLBRIGHT,
									   .fullbright_strength = OPT_GLTF_PROJECTILE_STRENGTH };
	if (ModelTables_StockEye(basename, c->stock_eye))
		c->cockpit_eye = c->stock_eye;
	const bool ok                       = stock_tags_read(&c->stock, basename, label);
	c->options.model.tags               = c->stock.tags;
	c->options.model.tag_count          = c->stock.tag_count;
	c->options.model.articulations      = c->stock.articulations;
	c->options.model.articulation_count = c->stock.articulation_count;
	return ok;
}

/* Reads what XWAU's files give the model, except its materials. */
static bool prepare_xwau(Conversion* c, const ModelConversion* mc, const opt_file_t* opt,
						 const char* basename, const char* label) {
	c->model        = (XwauModel*)malloc(sizeof *c->model);
	bool read       = c->model && XwauModel_Read(mc->game, mc->missions, basename, c->model, label);
	/* hook_pilot gives the stock hangar droids their parts when the model of
	   the type's list row has no Pilot lines. */
	for (int type = 311; read && mc->tables && !c->model->has_pilot_lines && type <= 312; ++type)
		if (strcasecmp(XwauGameTables_RowModel(mc->tables, type), basename) == 0)
			read = XwauModel_AddStockDroidParts(c->model, type, label);
	if (read) {
		c->options.model.articulations      = c->model->articulations;
		c->options.model.articulation_count = c->model->articulation_count;
		c->options.model.sets               = c->model->sets;
		c->options.model.set_count          = c->model->set_count;
		c->options.model.settings           = c->model->settings;
		c->options.model.setting_count      = c->model->setting_count;
		c->cockpit_eye                      = c->model->has_eye ? c->model->eye : NULL;
		/* The eye camera gets the regular field of view of the cockpit. */
		if (c->model->fov.has_regular && c->cockpit_eye)
			c->options.model.eye_vertical_fov =
				XwauModel_VerticalFov(c->model->fov.regular) * (3.14159265f / 180.0f);
		else if (c->model->fov.has_regular)
			ReportFatal(label,
						"the cockpit has a field of view, but the craft has no pilot eye to put it on");
	}
	/* The game and XWAU find some components by their stock number, so the
	   model gets the stock tags of the components it has. Its articulations
	   come from XWAU's files. */
	const bool tags_read = stock_tags_read(&c->stock, basename, label);
	size_t     kept      = 0;
	for (size_t i = 0; tags_read && i < c->stock.tag_count; ++i)
		if (c->stock.tags[i].mesh_index < (uint32_t)opt->mesh_count)
			c->stock.tags[kept++] = c->stock.tags[i];
	c->stock.tag_count         = kept;
	c->options.model.tags      = c->stock.tags;
	c->options.model.tag_count = c->stock.tag_count;
	const char* listed         = ModelTables_LitTextures(basename);
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
	return read && tags_read && XwauSkins_Resolve(mc->game, mc->missions, basename, opt, label, &c->skins);
}

/* Adds the pilot eye hardpoint. */
static bool place_cockpit(Conversion* c, opt_file_t* opt, const char* label) {
	if (c->cockpit_eye && !apply_cockpit_eye(opt, c->cockpit_eye)) {
		Report(label, "cannot add the cockpit eye hardpoint");
		return false;
	}
	return true;
}

/* Loads the Dynamic Cockpit screens of the model, if it has a .dc file. */
static bool load_dynamic_cockpit(Conversion* c, const ModelConversion* mc, opt_file_t* opt,
								 const char* basename, const char* label) {
	const char* dynamic = c->model && c->model->dynamic_cockpit[0] ? c->model->dynamic_cockpit : NULL;
	if (dynamic && !mc->names) {
		Report(label,
			   "%s: converting cockpit screens needs the game's names, which only the mod importer has. "
			   "The screens are left out",
			   dynamic);
		return true;
	}
	if (dynamic && (!XwauDynamicCockpit_Load(dynamic, opt, basename, xwau_find_file, (void*)mc, mc->names,
											 &c->dc, label) ||
					!XwauDynamicCockpit_ApplyHologramOffset(opt, &c->dc))) {
		Report(label, "cannot apply %s", dynamic);
		return false;
	}
	return true;
}

/* The candela of a bolt light per unit of its .mat Light, so that XWAU's
 * laser value, 0.7, is about as bright as the game's stock laser light
 * (classic intensity 200, as bright as the classic law at the game's match
 * distance of 27.5 m). The game gives that light an intensity of
 * 0.5 × c × D × CLASSIC_ILLUMINANCE; with pi in place of the illuminance,
 * it was about 1.44e5 cd, or 2.06e5 cd per unit of Light. */
#define XWAU_BOLT_LIGHT_CANDELA (2.06e5f * CLASSIC_ILLUMINANCE / 3.14159265f)

/* Whether XWAU's renderer treats the model as a laser or a turbolaser
 * (is_Laser and is_TurboLaser in XwaTextureData.cpp): its name has "Laser",
 * but not "LaserBat", or "Turbo". The renderer compares the name that the
 * game's model list gives, in its case. The files can have another case
 * (LASERREBEL.OPT for LaserRebel.opt), so the name is compared in any case. */
static bool xwau_is_laser(const char* basename) {
	char name[256];
	LowerCopy(name, sizeof name, basename);
	return (strstr(name, "laser") && !strstr(name, "laserbat")) || strstr(name, "turbo");
}

/* XWAU's renderer lights a bolt with the Light of the material of the
 * texture it shows (EffectsRenderer::AddLaserLights), so a bolt model gets a
 * light for each state of its face groups: the light of the first face group
 * of its mesh whose texture has one. A face group without states gives a
 * light for every variant. Without the model's .mat, a bolt has no light, as
 * in XWAU. */
static bool xwau_bolt_lights(Conversion* c, const opt_file_t* opt) {
	for (int32_t mi = 0; c->mat && mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m = &opt->meshes[mi];
		for (int32_t g = 0; m->lod_count > 0 && g < m->lods[0].group_count; ++g) {
			const opt_face_group_t* group  = &m->lods[0].groups[g];
			const int32_t           states = group->state_count > 1 ? group->state_count : 1;
			for (int32_t s = 0; s < states; ++s) {
				const int32_t texture =
					group->state_count > 1 ? group->state_textures[s] : group->texture_index;
				const int32_t state = group->state_count > 1 ? s : -1;
				if (texture < 0 || texture >= opt->texture_count)
					continue;
				const XwauMatParams* p    = XwauMat_Find(c->mat, opt->textures[texture].name);
				const float          peak = fmaxf(p->light[0], fmaxf(p->light[1], p->light[2]));
				bool                 seen = false;
				for (size_t i = 0; i < c->light_count; ++i)
					seen |= c->lights[i].mesh_index == (uint32_t)mi && c->lights[i].state == state;
				if (!p->has_light || !(peak > 0.0f) || seen)
					continue;
				OptGltfLight* grown = (OptGltfLight*)realloc(c->lights, (c->light_count + 1) * sizeof *grown);
				if (!grown)
					return false;
				c->lights           = grown;
				OptGltfLight* light = &grown[c->light_count++];
				*light              = (OptGltfLight) {
					.mesh_index = (uint32_t)mi,
					.state      = state,
					.intensity  = peak * XWAU_BOLT_LIGHT_CANDELA,
				};
				/* XWAU shades with Light as a linear colour. */
				for (int k = 0; k < 3; ++k)
					light->color[k] = fmaxf(p->light[k], 0.0f) / peak;
			}
		}
	}
	c->options.model.lights      = c->lights;
	c->options.model.light_count = c->light_count;
	return true;
}

/* The candela of a light from a .mat Light key, per unit of Light ×
 * light_falloff. XWAU's renderer lights a surface with
 * laser_light_intensity × light_falloff / d² × Light, with d in metres, in
 * units where the white sun of brightness 1 gives 1. laser_light_intensity
 * is twice its setting, which XWAU ships at 3.0, so 6. With the white sun
 * at CLASSIC_ILLUMINANCE, the light gives 6 × CLASSIC_ILLUMINANCE W/m² at
 * 1 m: that many W/sr, times 683 lm/W in candela. In game, the Imperial Star
 * Destroyer's lights look right 10 times brighter than that, so the factor
 * of 10 is tuned by eye. */
#define XWAU_MAT_LIGHT_CANDELA (10.0f * 6.0f * CLASSIC_ILLUMINANCE * 683.0f)
/* Lights of a component closer than this, in OPT units (1 cm), and in the
 * same direction are one light. */
#define XWAU_MAT_LIGHT_MERGE (0.01f * 65536.0f / 1600.0f)

/* IsInsideTriangle of XWAU's renderer (Direct3DDevice.cpp): whether `p` is in
 * the triangle a, b, c, with p = a + u (c - a) + v (b - a). */
static bool xwau_inside_triangle(const opt_vec2_t* p, const opt_vec2_t* a, const opt_vec2_t* b,
								 const opt_vec2_t* c, float* u, float* v) {
	const float v0[2] = { c->u - a->u, c->v - a->v };
	const float v1[2] = { b->u - a->u, b->v - a->v };
	const float v2[2] = { p->u - a->u, p->v - a->v };
	const float dot00 = v0[0] * v0[0] + v0[1] * v0[1], dot01 = v0[0] * v1[0] + v0[1] * v1[1];
	const float dot02 = v0[0] * v2[0] + v0[1] * v2[1], dot11 = v1[0] * v1[0] + v1[1] * v1[1];
	const float dot12       = v1[0] * v2[0] + v1[1] * v2[1];
	const float denominator = dot00 * dot11 - dot01 * dot01;
	if (denominator == 0.0f)
		return false;
	const float inverse = 1.0f / denominator;
	*u                  = (dot11 * dot02 - dot01 * dot12) * inverse;
	*v                  = (dot00 * dot12 - dot01 * dot02) * inverse;
	return *u >= -0.000001f && *v >= -0.000001f && *u + *v < 1.000001f;
}

/* The face's outward normal: its stored normal, or the mean of its corners'
 * normals. False when both are zero. */
static bool xwau_face_normal(const opt_mesh_t* m, const opt_face_t* f, int corners, opt_vec3_t* out) {
	opt_vec3_t n = f->face_normal;
	if (n.x * n.x + n.y * n.y + n.z * n.z == 0.0f) {
		for (int k = 0; k < corners; ++k) {
			if (f->normals[k] >= 0 && f->normals[k] < m->normal_count) {
				n.x += m->normals[f->normals[k]].x;
				n.y += m->normals[f->normals[k]].y;
				n.z += m->normals[f->normals[k]].z;
			}
		}
	}
	const float length = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
	if (!(length > 0.0f) || !isfinite(length))
		return false;
	*out = (opt_vec3_t) { n.x / length, n.y / length, n.z / length };
	return true;
}

/* Whether `light` repeats one of the model's lights: the same component,
 * state, colour, intensity and cone, closer than XWAU_MAT_LIGHT_MERGE and in
 * the same direction. */
static bool xwau_mat_light_repeats(const Conversion* c, const OptGltfLight* light) {
	for (size_t i = 0; i < c->light_count; ++i) {
		const OptGltfLight* other = &c->lights[i];
		const float dx = other->position.x - light->position.x, dy = other->position.y - light->position.y,
					dz = other->position.z - light->position.z;
		const bool same_tag =
			(!other->tag && !light->tag) || (other->tag && light->tag && strcmp(other->tag, light->tag) == 0);
		if (other->mesh_index == light->mesh_index && other->state == light->state && same_tag &&
			memcmp(other->color, light->color, sizeof light->color) == 0 &&
			other->intensity == light->intensity && other->spot_angle == light->spot_angle &&
			dx * dx + dy * dy + dz * dz < XWAU_MAT_LIGHT_MERGE * XWAU_MAT_LIGHT_MERGE &&
			other->direction.x * light->direction.x + other->direction.y * light->direction.y +
					other->direction.z * light->direction.z >
				0.999f)
			return true;
	}
	return false;
}

/* Whether two textures' materials give the same light on a face. */
static bool xwau_same_mat_light(const XwauMatParams* a, const XwauMatParams* b) {
	return a->has_light == b->has_light && memcmp(a->light, b->light, sizeof a->light) == 0 &&
		   memcmp(a->light_uv, b->light_uv, sizeof a->light_uv) == 0 &&
		   a->light_falloff == b->light_falloff && a->has_light_angle == b->has_light_angle &&
		   a->light_angle == b->light_angle && strcmp(a->surface_tag, b->surface_tag) == 0;
}

/* XWAU's renderer puts a light on each face drawn with a texture whose
 * material has Light (EffectsRenderer::AddLaserLights): where the face's
 * texture coordinates hold light_uv_coord_pos, along the face's normal, as
 * bright as light_falloff, in the cone light_angle. These become the
 * model's own lights, found once here: for each face of LOD 0 and each
 * state of its face group (with that state's variant, as bolt lights, or
 * once for every variant when all its states give the same light), on the
 * triangles that XWAU splits a quad into. A light_angle of 90° or more
 * gives a point light, since glTF spot lights end at 90°, and so does none.
 * Without light_falloff, XWAU fades the light the same way as a bolt's
 * light. Warhead models do this, so the light gets the brightness of a bolt
 * light. A negative light_falloff, or a light_angle that gives no cone,
 * gives no light. A face without a normal gives no light, and it is counted
 * with ReportUnsupported. Returns the number of faces that gave a light
 * through *found. */
static bool xwau_mat_lights(Conversion* c, const opt_file_t* opt, size_t* found) {
	*found = 0;
	for (int32_t mi = 0; c->mat && mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m = &opt->meshes[mi];
		for (int32_t g = 0; m->lod_count > 0 && g < m->lods[0].group_count; ++g) {
			const opt_face_group_t* group     = &m->lods[0].groups[g];
			bool                    per_state = false;
			for (int32_t s = 1; s < group->state_count; ++s) {
				const int32_t t0 = group->state_textures[0], ts = group->state_textures[s];
				per_state |= t0 < 0 || t0 >= opt->texture_count || ts < 0 || ts >= opt->texture_count ||
							 !xwau_same_mat_light(XwauMat_Find(c->mat, opt->textures[t0].name),
												  XwauMat_Find(c->mat, opt->textures[ts].name));
			}
			const int32_t states = per_state ? group->state_count : 1;
			for (int32_t s = 0; s < states; ++s) {
				const int32_t texture = per_state ? group->state_textures[s] : group->texture_index;
				if (texture < 0 || texture >= opt->texture_count)
					continue;
				const XwauMatParams* p    = XwauMat_Find(c->mat, opt->textures[texture].name);
				const float          peak = fmaxf(p->light[0], fmaxf(p->light[1], p->light[2]));
				/* XWAU keeps the cosine of light_angle, and a cosine of 1 or
				 * more gives no cone. */
				const float cosine       = p->has_light_angle ? cosf(p->light_angle * 0.0174532925f) : 0.0f;
				const bool  bolt_falloff = p->light_falloff == 0.0f;
				if (!p->has_light || !(peak > 0.0f) || !(bolt_falloff || p->light_falloff > 0.0f) ||
					!(cosine < 1.0f))
					continue;
				OptGltfLight light = {
					.mesh_index = (uint32_t)mi,
					.state      = per_state ? s : -1,
					.tag        = p->surface_tag[0] ? p->surface_tag : NULL,
					.intensity  = bolt_falloff ? XWAU_BOLT_LIGHT_CANDELA * peak
											   : XWAU_MAT_LIGHT_CANDELA * p->light_falloff * peak,
					.placed     = true,
					.spot_angle = cosine > 0.0f ? acosf(cosine) : 0.0f,
				};
				/* XWAU shades with Light as a linear colour. */
				for (int k = 0; k < 3; ++k)
					light.color[k] = fmaxf(p->light[k], 0.0f) / peak;
				const opt_vec2_t at = { p->light_uv[0], p->light_uv[1] };
				for (int32_t fi = 0; fi < group->face_count; ++fi) {
					const opt_face_t* f       = &group->faces[fi];
					const int         corners = f->verts[3] == -1 ? 3 : 4;
					bool              valid   = true;
					for (int k = 0; k < corners; ++k)
						valid = valid && f->verts[k] >= 0 && f->verts[k] < m->vertex_count &&
								f->uvs[k] >= 0 && f->uvs[k] < m->uv_count;
					/* As XWAU, the corners 0, 1, 2, then 0, 2, 3. */
					for (int edge = 2; valid && edge < corners; ++edge) {
						const int corner[3] = { 0, edge - 1, edge };
						float     u, v;
						if (!xwau_inside_triangle(&at, &m->uvs[f->uvs[corner[0]]], &m->uvs[f->uvs[corner[1]]],
												  &m->uvs[f->uvs[corner[2]]], &u, &v))
							continue;
						const opt_vec3_t* a = &m->vertices[f->verts[corner[0]]];
						const opt_vec3_t* b = &m->vertices[f->verts[corner[1]]];
						const opt_vec3_t* d = &m->vertices[f->verts[corner[2]]];
						light.position      = (opt_vec3_t) { a->x + u * (d->x - a->x) + v * (b->x - a->x),
															 a->y + u * (d->y - a->y) + v * (b->y - a->y),
															 a->z + u * (d->z - a->z) + v * (b->z - a->z) };
						if (!xwau_face_normal(m, f, corners, &light.direction)) {
							ReportUnsupported(".mat lights on faces without a normal");
							continue;
						}
						++*found;
						if (xwau_mat_light_repeats(c, &light))
							continue;
						OptGltfLight* grown =
							(OptGltfLight*)realloc(c->lights, (c->light_count + 1) * sizeof *grown);
						if (!grown)
							return false;
						c->lights                   = grown;
						c->lights[c->light_count++] = light;
					}
				}
			}
		}
	}
	c->options.model.lights      = c->lights;
	c->options.model.light_count = c->light_count;
	return true;
}

/* Whether two textures' materials make the same lever. */
static bool xwau_same_lever(const XwauMatParams* a, const XwauMatParams* b) {
	if (a->lever != b->lever)
		return false;
	if (a->lever == XWAU_LEVER_JOYSTICK)
		return memcmp(a->joystick_root, b->joystick_root, sizeof a->joystick_root) == 0 &&
			   a->joystick_max_yaw == b->joystick_max_yaw && a->joystick_max_pitch == b->joystick_max_pitch;
	return memcmp(a->lever_root, b->lever_root, sizeof a->lever_root) == 0 &&
		   a->lever_axis == b->lever_axis && a->lever_max_angle == b->lever_max_angle;
}

/* The control of a lever (XwauMatParams.lever): a joystick turns about X by
 * its pitch, then about Y by its yaw; a throttle or hyperspace lever about
 * its axis. Its pivot is in metres, in OPT axes. Hinges of 0 degrees are left
 * out. */
static void xwau_lever_control(const XwauMatParams* p, OptGltfControl* out) {
	memset(out, 0, sizeof *out);
	const float* root  = p->lever == XWAU_LEVER_JOYSTICK ? p->joystick_root : p->lever_root;
	const float  scale = 1.0f / (1600.0f / 65536.0f);
	out->pivot         = (opt_vec3_t) { root[0] * scale, root[1] * scale, root[2] * scale };
	OptGltfHinge hinges[2];
	uint32_t     count = 0;
	if (p->lever == XWAU_LEVER_JOYSTICK) {
		hinges[count++] = (OptGltfHinge) { 0, p->joystick_max_pitch, "cockpit.stick.y" };
		hinges[count++] = (OptGltfHinge) { 1, p->joystick_max_yaw, "cockpit.stick.x" };
	} else {
		hinges[count++] =
			(OptGltfHinge) { p->lever_axis, p->lever_max_angle,
							 p->lever == XWAU_LEVER_THROTTLE ? "cockpit.throttle" : "cockpit.hyperspace" };
	}
	for (uint32_t h = 0; h < count; ++h)
		if (hinges[h].degrees != 0.0f)
			out->hinges[out->hinge_count++] = hinges[h];
}

/* XWAU moves the faces of the textures whose material makes them a cockpit
 * lever (EffectsRenderer::ApplyDiegeticCockpit). They become controls, one
 * for the textures of each lever. XWAU turns every joystick about the pivot
 * of the first one it draws in a frame; each control turns about its own. */
static bool xwau_levers(Conversion* c, const opt_file_t* opt, const char* label) {
	if (!c->mat || opt->texture_count <= 0)
		return true;
	const size_t textures = (size_t)opt->texture_count;
	/* Each texture's lever, and the first texture of each lever. */
	int32_t* owner      = (int32_t*)malloc(textures * sizeof *owner);
	size_t*  first      = (size_t*)malloc(textures * sizeof *first);
	c->controls         = (OptGltfControl*)calloc(textures, sizeof *c->controls);
	c->control_textures = (int32_t*)malloc(textures * sizeof *c->control_textures);
	if (!owner || !first || !c->controls || !c->control_textures) {
		free(owner);
		free(first);
		return false;
	}
	size_t moved = 0;
	for (size_t t = 0; t < textures; ++t) {
		const XwauMatParams* p = XwauMat_Find(c->mat, opt->textures[t].name);
		owner[t]               = -1;
		if (p->lever == XWAU_LEVER_OTHER) {
			ReportUnsupported("sliding or free-axis cockpit levers");
			continue;
		}
		OptGltfControl control;
		xwau_lever_control(p, &control);
		if (p->lever == XWAU_LEVER_NONE || control.hinge_count == 0)
			continue;
		for (size_t k = 0; k < c->control_count && owner[t] < 0; ++k)
			if (xwau_same_lever(p, XwauMat_Find(c->mat, opt->textures[first[k]].name)))
				owner[t] = (int32_t)k;
		if (owner[t] < 0) {
			first[c->control_count]       = t;
			c->controls[c->control_count] = control;
			owner[t]                      = (int32_t)c->control_count++;
		}
		moved += owner[t] >= 0;
	}
	size_t next = 0;
	for (size_t k = 0; k < c->control_count; ++k) {
		c->controls[k].textures = &c->control_textures[next];
		for (size_t t = 0; t < textures; ++t)
			if (owner[t] == (int32_t)k)
				c->control_textures[next++] = (int32_t)t;
		c->controls[k].texture_count = (size_t)(&c->control_textures[next] - c->controls[k].textures);
	}
	free(owner);
	free(first);
	if (c->control_count)
		ReportDetail(label, "%zu cockpit levers, from %zu textures", c->control_count, moved);
	c->options.model.controls      = c->controls;
	c->options.model.control_count = c->control_count;
	return true;
}

/* Keeps a single light for each variant of a projectile. XWAU adds a light
 * for each face that shows a texture with Light, and a model can have several
 * of them. A battle can have hundreds of projectiles, so that doesn't scale,
 * and on a small object that moves fast, one light looks the same. For each
 * variant, the light is the brightest of the lights that shine with it. It
 * becomes a tube along its mesh, like a laser's light and the game's stock
 * light. A position from the faces would not work: some warheads are a
 * long mesh with lights at both ends. When all the lights shine with every
 * variant, one light is kept for all of them. Returns false when out of
 * memory. */
static bool xwau_projectile_light(Conversion* c, const opt_file_t* opt) {
	bool per_state = false;
	for (size_t i = 0; i < c->light_count; ++i)
		per_state |= c->lights[i].state >= 0;
	int32_t states = 1;
	for (int32_t mi = 0; per_state && mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m = &opt->meshes[mi];
		for (int32_t g = 0; m->lod_count > 0 && g < m->lods[0].group_count; ++g)
			if (m->lods[0].groups[g].state_count > states)
				states = m->lods[0].groups[g].state_count;
	}
	OptGltfLight* kept  = (OptGltfLight*)malloc((size_t)states * sizeof *kept);
	size_t        count = 0;
	if (!kept)
		return false;
	for (int32_t s = 0; c->light_count && s < states; ++s) {
		const int32_t       state = per_state ? s : -1;
		const OptGltfLight* best  = NULL;
		for (size_t i = 0; i < c->light_count; ++i) {
			const OptGltfLight* light = &c->lights[i];
			if ((light->state == state || light->state == -1) &&
				(!best || light->intensity > best->intensity))
				best = light;
		}
		if (!best)
			continue;
		kept[count] = (OptGltfLight) {
			.mesh_index = best->mesh_index,
			.state      = state,
			.tag        = best->tag,
			.color      = { best->color[0], best->color[1], best->color[2] },
			.intensity  = best->intensity,
		};
		++count;
	}
	free(c->lights);
	c->lights                    = kept;
	c->light_count               = count;
	c->options.model.lights      = c->lights;
	c->options.model.light_count = c->light_count;
	return true;
}

/* How XWAU's renderer draws the model when it is a projectile (see the
 * conversion's description). */
static XwauGltfProjectile xwau_projectile(const ModelConversion* mc, const char* basename) {
	bool projectile = false;
	for (int type = 0; mc->tables && !projectile && type < XWAU_GAME_OBJECT_TYPES; ++type) {
		const XwauGameType t = XwauGameTables_Type(mc->tables, type);
		projectile           = (t.genus == 6 || t.genus == 7) &&
							   strcasecmp(XwauGameTables_Model(mc->tables, type), basename) == 0;
	}
	if (!mc->tables)
		projectile = ModelTables_IsProjectile(basename);
	char name[256];
	LowerCopy(name, sizeof name, basename);
	for (size_t i = 0; !projectile && i < mc->weapon_models.count; ++i)
		projectile = strcmp(mc->weapon_models.items[i], name) == 0;
	if (!projectile)
		return XWAU_GLTF_NOT_PROJECTILE;
	static const char* const kWarheads[] = { "missile", "torpedo", "spacebomb", "pulse", "rocket", "flare" };
	for (size_t i = 0; i < sizeof kWarheads / sizeof kWarheads[0]; ++i) {
		if (strstr(name, kWarheads[i]) && !strstr(name, "boat"))
			return XWAU_GLTF_WARHEAD;
	}
	return XWAU_GLTF_LASER;
}

/* Reads the XWAU material, normal map and animations of each texture, and
 * bakes them into the material options. */
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
	size_t                   normal_textures = 0;
	const XwauGltfProjectile projectile      = xwau_projectile(mc, basename);
	if (projectile)
		ReportDetail(label, "%s: unlit at strength %g", projectile == XWAU_GLTF_LASER ? "laser" : "warhead",
					 (double)XwauGltf_EmissiveStrength(&(XwauGltfMaterial) { .projectile = projectile }));
	for (int32_t i = 0; i < opt->texture_count; ++i) {
		XwauGltfMaterial*    material = &c->materials[i];
		const XwauMatParams* p        = c->mat ? XwauMat_Find(c->mat, opt->textures[i].name) : &mc->global;
		xwau_fill(material, p);
		material->projectile = projectile;
		for (int k = 0; k < c->lit_count; ++k) {
			if (xwau_texture_named(opt->textures[i].name, c->lit[k])) {
				material->shadeless = false;
				material->lit       = true;
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
	/* Untextured faces use the material found for an empty name, which is slot 0. */
	XwauGltfMaterial untextured;
	xwau_fill(&untextured, c->mat ? XwauMat_Find(c->mat, "") : &mc->global);
	untextured.projectile = projectile;

	ReportDetail(label, "xwau materials %s, %zu of %d textures normal-mapped (%zu maps)",
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
	m->variants = c->baked.variants.count || c->baked.variants.skin_variant_count ? &c->baked.variants : NULL;
	m->screens                = screens;
	/* XWAU's renderer uses the stored normal of each corner, without
	 * remapping or repairing it. */
	c->options.geometry.normals = OPT_GLTF_NORMALS_STORED;
	if (!xwau_levers(c, opt, label)) {
		Report(label, "out of memory (levers)");
		return false;
	}
	const bool bolt  = xwau_is_laser(basename);
	size_t     faces = 0;
	if (!(bolt ? xwau_bolt_lights(c, opt) : xwau_mat_lights(c, opt, &faces))) {
		Report(label, "out of memory (lights)");
		return false;
	}
	const size_t found = c->light_count;
	if (projectile && found && !xwau_projectile_light(c, opt)) {
		Report(label, "out of memory (lights)");
		return false;
	}
	if (projectile && found)
		ReportDetail(label, "%zu projectile lights, one for each variant, from %zu", c->light_count, found);
	else if (bolt && found)
		ReportDetail(label, "%zu bolt lights", found);
	else if (found)
		ReportDetail(label, "%zu .mat lights, from %zu faces", found, faces);
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
	free(c->lights);
	free(c->controls);
	free(c->control_textures);
	XwauAnim_End(c->anim);
	XwauMat_Free(c->mat);
	XwauDynamicCockpit_Free(&c->dc);
	XwauSkins_Free(&c->skins);
	stock_tags_free(&c->stock);
	free(c->model);
}

struct ModelConversionModel {
	opt_file_t*      opt;
	Conversion*      c; /* what the document's images point to */
	OptGltfDocument* document;
};

void ModelConversion_FreeModel(ModelConversionModel* model) {
	if (!model)
		return;
	OptGltf_Free(model->document);
	if (model->c)
		conversion_free(model->c, model->opt);
	free(model->c);
	opt_free(model->opt);
	free(model);
}

bool ModelConversion_Build(const ModelConversion* mc, const char* opt_path, const char* basename,
						   ModelConversionModel** out) {
	*out                        = NULL;
	ModelConversionModel* model = (ModelConversionModel*)calloc(1, sizeof *model);
	opt_error_t           err;
	if (!model) {
		Report(opt_path, "out of memory");
		return false;
	}
	if (!(model->opt = opt_load_file(opt_path, &err))) {
		Report(opt_path, "%s", err.msg);
		free(model);
		return false;
	}
	opt_file_t* opt = model->opt;
	report_opt_drops(mc, opt, opt_path);
	Conversion* c = model->c = (Conversion*)calloc(1, sizeof *c);
	bool        ok           = c != NULL;
	if (ok && mc->xwau)
		ok = prepare_xwau(c, mc, opt, basename, opt_path) && place_cockpit(c, opt, opt_path) &&
			 load_dynamic_cockpit(c, mc, opt, basename, opt_path) &&
			 bake_xwau(c, mc, opt, basename, opt_path);
	else if (ok)
		ok = prepare_stock(c, mc, basename, opt_path) && place_cockpit(c, opt, opt_path);
	c->options.model.seen_close = seen_close(basename);
	/* The classic emission of the model, as physical radiance. */
	if (ok)
		c->options.material.emission_radiance = CLASSIC_RADIANCE;
	ok = ok && OptGltf_Build(opt, basename, &c->options, opt_path, &model->document);
	if (ok && mc->xwau)
		XwauGltf_ReportSkins(&c->baked, basename);
	if (!ok) {
		ModelConversion_FreeModel(model);
		return false;
	}
	*out = model;
	return true;
}

const OptGltfDocument* ModelConversion_Document(const ModelConversionModel* model) { return model->document; }

bool ModelConversion_Convert(const ModelConversion* mc, const char* opt_path, const char* basename,
							 const char* out_dir) {
	ModelConversionModel* model = NULL;
	if (!ModelConversion_Build(mc, opt_path, basename, &model))
		return false;
	const bool ok = OptGltf_WriteFiles(model->document, out_dir, basename);
	if (!ok)
		Report(opt_path, "could not write %s", out_dir);
	ModelConversion_FreeModel(model);
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
	if (!(mc->missions = XwauMissions_Load(mc->game, false))) {
		Report(NULL, "out of memory (missions)");
		return false;
	}
	if (!(mc->tables = XwauGameTables_Load(mc->game)))
		Report(NULL, "the stock projectile models are converted as projectiles");
	if (!XwauWeaponModels_FindAll(mc->game, &mc->weapon_models)) {
		Report(NULL, "out of memory (weapon models)");
		return false;
	}
	return true;
}

void ModelConversion_Free(ModelConversion* mc) {
	XwauMissions_Free(mc->missions);
	mc->missions = NULL;
	XwauGameTables_Free(mc->tables);
	mc->tables = NULL;
	XwauLines_Free(&mc->weapon_models);
}
