/* The conversion of an OPT into a glTF document (opt_gltf.h): its phases,
 * the geometry's buffers and primitives, the nodes and their
 * AERON_flight_model objects. */

#include <math.h>

#include "opt_gltf_internal.h"
#include "opt_json.h"

static void normalize3(float vector[3]) {
	const float length = sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
	if (length > 1.0e-9f) {
		vector[0] /= length;
		vector[1] /= length;
		vector[2] /= length;
	}
}

static void quaternion_from_axes(float right[3], float up[3], float look[3], float out[4]) {
	normalize3(right);
	normalize3(up);
	normalize3(look);
	const float trace = right[0] + up[1] + look[2];
	if (trace > 0.0f) {
		const float s = sqrtf(trace + 1.0f) * 2.0f;
		out[3]        = 0.25f * s;
		out[0]        = (up[2] - look[1]) / s;
		out[1]        = (look[0] - right[2]) / s;
		out[2]        = (right[1] - up[0]) / s;
	} else if (right[0] > up[1] && right[0] > look[2]) {
		const float s = sqrtf(1.0f + right[0] - up[1] - look[2]) * 2.0f;
		out[3]        = (up[2] - look[1]) / s;
		out[0]        = 0.25f * s;
		out[1]        = (up[0] + right[1]) / s;
		out[2]        = (look[0] + right[2]) / s;
	} else if (up[1] > look[2]) {
		const float s = sqrtf(1.0f + up[1] - right[0] - look[2]) * 2.0f;
		out[3]        = (look[0] - right[2]) / s;
		out[0]        = (up[0] + right[1]) / s;
		out[1]        = 0.25f * s;
		out[2]        = (look[1] + up[2]) / s;
	} else {
		const float s = sqrtf(1.0f + look[2] - right[0] - up[1]) * 2.0f;
		out[3]        = (right[1] - up[0]) / s;
		out[0]        = (look[0] + right[2]) / s;
		out[1]        = (look[1] + up[2]) / s;
		out[2]        = 0.25f * s;
	}
	const float length = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
	if (length > 1.0e-9f) {
		for (int index = 0; index < 4; ++index)
			out[index] /= length;
	}
}

static bool valid_component_tag(const char* tag) {
	const size_t length = tag ? strlen(tag) : 0;
	if (length == 0 || length > 63)
		return false;
	for (size_t i = 0; i < length; ++i) {
		const char c = tag[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_'))
			return false;
	}
	return true;
}

/* ===== The build ====================================================== */

/* The options' defaults, when none are given. */
static const OptGltfBuildOptions kDefaultOptions;

/* Sets the build's error message; returns false. */
bool opt_gltf_build_fail(Build* b, const char* format, ...) {
	if (b->error) {
		va_list args;
		va_start(args, format);
		vsnprintf(b->error->msg, sizeof b->error->msg, format, args);
		va_end(args);
	}
	return false;
}

static void build_free(Build* b) {
	free(b->promoted);
	free(b->tex_emissive);
	free(b->screen_cover_image);
	if (b->built) {
		for (int32_t mi = 0; mi < b->opt->mesh_count; ++mi)
			opt_gltf_built_mesh_free(&b->built[mi]);
		free(b->built);
	}
}

/* ---- Options ------------------------------------------------------------ */

/* The variants' names set and unique, their markings named, and their
 * materials' textures and indices in range. */
static bool validate_variants(Build* b) {
	const OptGltfVariants* v             = b->variants;
	const size_t           texture_count = (size_t)b->opt->texture_count;
	for (size_t i = 0; i < v->count; ++i) {
		bool ok = v->variants[i].name && v->variants[i].name[0];
		for (size_t j = 0; ok && j < i; ++j)
			ok = strcmp(v->variants[j].name, v->variants[i].name) != 0;
		for (size_t t = 0; ok && t < texture_count; ++t)
			ok = v->material_of[i * texture_count + t] >= -1 &&
				 v->material_of[i * texture_count + t] < (int32_t)v->material_count;
		if (!ok)
			return opt_gltf_build_fail(b, "invalid or duplicate material variant %zu '%s'", i,
									   v->variants[i].name ? v->variants[i].name : "");
	}
	for (size_t m = 0; m < v->material_count; ++m)
		if (v->materials[m].texture < 0 || v->materials[m].texture >= b->opt->texture_count ||
			!v->materials[m].material.base.view.rgba)
			return opt_gltf_build_fail(b, "invalid material %zu of the variants", m);
	for (size_t m = 0; m < v->markings_count; ++m) {
		const char* name  = v->default_markings[m];
		bool        found = name == NULL;
		for (size_t i = 0; !found && i < v->count; ++i)
			found = strcmp(v->variants[i].name, name) == 0;
		if (!found)
			return opt_gltf_build_fail(b, "markings %zu names unknown material variant '%s'", m, name);
	}
	return true;
}

static bool validate_options(Build* b) {
	const OptGltfBuildOptions* o   = b->options;
	const opt_file_t*          opt = b->opt;
	if (!opt_gltf_validate_alpha_overrides(o, b->error))
		return false;
	for (size_t i = 0; i < o->model.tag_count; ++i) {
		const OptGltfComponentTag* tag = &o->model.tags[i];
		if (!valid_component_tag(tag->tag) || tag->mesh_index >= (uint32_t)opt->mesh_count)
			return opt_gltf_build_fail(b, "invalid tag %u=%s (%d meshes)", tag->mesh_index,
									   tag->tag ? tag->tag : "", (int)opt->mesh_count);
	}
	for (size_t i = 0; i < o->model.articulation_count; ++i) {
		const OptGltfComponentArticulation* a = &o->model.articulations[i];
		if (!valid_component_tag(a->group) || (a->behavior[0] && !valid_component_tag(a->behavior)) ||
			a->angle < -128 || a->angle > 255 || a->speed < 0 || a->speed > 255 || a->speed_back < 0 ||
			a->speed_back > 255 || a->component > 254)
			return opt_gltf_build_fail(b, "invalid articulation %u=%s:%d:%d:%d:%s", a->component, a->group,
									   (int)a->angle, (int)a->speed, (int)a->speed_back, a->behavior);
	}
	for (size_t i = 0; i < o->model.set_count; ++i) {
		bool duplicate = false;
		for (size_t previous = 0; previous < i; ++previous)
			duplicate |= strcmp(o->model.sets[previous].name, o->model.sets[i].name) == 0;
		if (!valid_component_tag(o->model.sets[i].name) || duplicate ||
			(o->model.sets[i].members[7] & 0x80000000u))
			return opt_gltf_build_fail(b, "invalid or repeated component set %s", o->model.sets[i].name);
	}
	for (size_t i = 0; i < o->model.setting_count; ++i) {
		bool duplicate = false;
		for (size_t previous = 0; previous < i; ++previous)
			duplicate |= strcmp(o->model.settings[previous].name, o->model.settings[i].name) == 0;
		if (!valid_component_tag(o->model.settings[i].name) || duplicate)
			return opt_gltf_build_fail(b, "invalid or repeated articulation setting %s",
									   o->model.settings[i].name);
	}
	const OptGltfGeometryOptions* g = &o->geometry;
	if (g->normals == OPT_GLTF_NORMALS_SMOOTHED &&
		!(g->smooth_angle_degrees >= 0.0f && g->smooth_angle_degrees <= 180.0f))
		return opt_gltf_build_fail(b, "smoothing angle %g outside 0 to 180 degrees",
								   (double)g->smooth_angle_degrees);
	const OptGltfMaterialOptions* m = &o->material;
	if (m->source == OPT_GLTF_MATERIALS_FULLBRIGHT && !(m->fullbright_strength > 0.0f))
		return opt_gltf_build_fail(b, "fullbright materials need a positive strength");
	if (m->source == OPT_GLTF_MATERIALS_GIVEN && (!m->given || !m->untextured))
		return opt_gltf_build_fail(b, "given materials need one per texture and the untextured one");
	if (m->variants && m->source != OPT_GLTF_MATERIALS_GIVEN)
		return opt_gltf_build_fail(b, "material variants need given materials");
	return !b->variants || validate_variants(b);
}

/* Adds the model root's markings list to `root`: entry m is the variant
 * `default_markings` names for m, else state min(m, N - 1) (null for state
 * 0), up to the longer of the two lists; after a last variant, the state of
 * the values past it, which the list's last entry then repeats. Nothing
 * when every markings value shows the own materials. False when out of
 * memory. */
static bool markings_json(cJSON* root, const OptGltfVariants* variants, int state_count) {
	const size_t named = variants ? variants->markings_count : 0;
	size_t       count = named > (size_t)state_count ? named : (size_t)state_count;
	const char*  named_variant[257];
	int          state[257];
	for (size_t m = 0; m < count && m < 256; ++m) {
		named_variant[m] = m < named ? variants->default_markings[m] : NULL;
		state[m]         = m < (size_t)state_count ? (int)m : state_count - 1;
	}
	count = count < 256 ? count : 256;
	if (count && named_variant[count - 1] && count < 256) {
		named_variant[count] = NULL;
		state[count]         = count < (size_t)state_count ? (int)count : state_count - 1;
		count++;
	}
	while (count > 1 && !named_variant[count - 1] && !named_variant[count - 2] &&
		   state[count - 1] == state[count - 2])
		count--;
	bool shown = false;
	for (size_t m = 0; m < count; ++m)
		shown |= named_variant[m] != NULL || state[m] != 0;
	if (!shown)
		return true;
	cJSON* list = OptJson_AddArray(root, "markings");
	bool   ok   = list != NULL;
	for (size_t m = 0; ok && m < count; ++m) {
		char name[32];
		snprintf(name, sizeof name, "State %d", state[m]);
		ok = OptJson_Add(list, NULL,
						 named_variant[m] ? cJSON_CreateString(named_variant[m])
						 : state[m]       ? cJSON_CreateString(name)
										  : cJSON_CreateNull());
	}
	return ok;
}

/* ---- Geometry ----------------------------------------------------------- */

/* Pass 1: each OPT mesh's vertices and per-face-group indices, with
 * tangents for the meshes drawing a normal-mapped material in any switch
 * state. */
static bool build_geometry(Build* b) {
	const opt_file_t* opt = b->opt;
	b->built              = (BuiltMesh*)calloc(opt->mesh_count ? opt->mesh_count : 1, sizeof *b->built);
	if (!b->built) {
		fprintf(stderr, "opt2gltf: oom (meshes)\n");
		return false;
	}
	b->max_state_count  = 1;
	uint32_t fixed_zero = 0, fixed_flip = 0, skipped_repeats = 0, kept_repeats = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m  = &opt->meshes[mi];
		BuiltMesh*        bm = &b->built[mi];
		/* Components stay one-to-one with the OPT meshes. */
		if (!opt_gltf_built_mesh_from_opt(bm, m, OPT_METERS_PER_UNIT, b->smooth_angle >= 0.0f,
										  b->smooth_angle, b->repair_normals))
			return opt_gltf_build_fail(b, "mesh %d has no LOD 0 triangles", (int)mi);
		bool needs_tangents = false;
		for (int32_t g = 0; b->materials && !needs_tangents && g < m->lods[0].group_count; ++g) {
			const opt_face_group_t* grp = &m->lods[0].groups[g];
			if (bm->fg_index_count_arr[g] == 0)
				continue;
			const int states = (grp->state_count > 1 && grp->state_textures) ? grp->state_count : 1;
			for (int st = 0; st < states && !needs_tangents; ++st) {
				const int32_t ti = st == 0 ? grp->texture_index : grp->state_textures[st];
				needs_tangents   = ti >= 0 && ti < opt->texture_count && b->materials[ti].base.view.rgba &&
								   b->materials[ti].normal.view.rgba;
			}
		}
		if (needs_tangents && !opt_gltf_generate_tangents(bm)) {
			fprintf(stderr, "opt2gltf: oom (tangents)\n");
			return false;
		}
		fixed_zero += bm->fixed_zero_normals;
		fixed_flip += bm->fixed_flipped_normals;
		skipped_repeats += bm->skipped_repeats;
		kept_repeats += bm->kept_repeats;
		for (int32_t g = 0; g < m->lods[0].group_count; ++g)
			if (m->lods[0].groups[g].state_count > b->max_state_count)
				b->max_state_count = m->lods[0].groups[g].state_count;
	}
	if (fixed_zero || fixed_flip)
		fprintf(stderr, "opt2gltf: %s: repaired %u zero + %u face-opposed canonical normals\n", b->basename,
				fixed_zero, fixed_flip);
	if (skipped_repeats)
		fprintf(stderr, "opt2gltf: %s: left out %u repeated triangle(s)\n", b->basename, skipped_repeats);
	if (kept_repeats)
		fprintf(stderr, "opt2gltf: %s: %u triangle(s) repeat one with another texture (kept)\n", b->basename,
				kept_repeats);
	return true;
}

/* The asset's variants: state 0 is the primitives' own materials; states 1
 * to N - 1, then the given variants. */
static bool build_variants(Build* b) {
	GltfBuild* gb          = b->gb;
	b->given_variant_count = b->variants ? b->variants->count : 0;
	b->state_variant_count = (size_t)b->max_state_count - 1;
	const size_t count     = b->state_variant_count + b->given_variant_count;
	if (count == 0)
		return true;
	gb->variants = (cgltf_material_variant*)calloc(count, sizeof *gb->variants);
	if (!gb->variants) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	for (size_t v = 0; v < b->state_variant_count; ++v)
		gb->variants[v].name = opt_gltf_keep_string(gb, xprintf_dup("State %d", (int)v + 1));
	for (size_t v = 0; v < b->given_variant_count; ++v)
		gb->variants[b->state_variant_count + v].name =
			opt_gltf_keep_string(gb, xstrdup(b->variants->variants[v].name));
	gb->variants_count = count;
	return true;
}

/* The vertex attributes of a mesh: POSITION, NORMAL, TEXCOORD_0 [, TANGENT]. */
static size_t mesh_attribute_count(const BuiltMesh* bm) { return bm->has_tangents ? 4u : 3u; }

/* Whether a face group's primitive maps variants: it switches states, or a
 * given variant has a material for its texture. */
static bool group_has_variants(const Build* b, const opt_face_group_t* grp) {
	if (grp->state_count > 1 && grp->state_textures)
		return true;
	const int32_t t = grp->texture_index;
	for (size_t v = 0; t >= 0 && t < b->opt->texture_count && v < b->given_variant_count; ++v)
		if (b->variants->material_of[v * (size_t)b->opt->texture_count + (size_t)t] >= 0)
			return true;
	return false;
}

/* The meshes, buffer views, accessors, primitives, attributes and
 * mappings pass 2 writes. cgltf points into these arrays, so each is
 * allocated once, at its full size. */
static bool allocate_geometry(Build* b) {
	const opt_file_t* opt = b->opt;
	GltfBuild*        gb  = b->gb;
	gb->meshes            = (cgltf_mesh*)calloc((size_t)opt->mesh_count, sizeof *gb->meshes);
	if (!gb->meshes) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	gb->meshes_count = (size_t)opt->mesh_count;
	size_t prims = 0, attrs = 0, maps = 0, views = 0, accessors = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_lod_t* lod     = &opt->meshes[mi].lods[0];
		const size_t     streams = mesh_attribute_count(&b->built[mi]);
		/* One view and accessor per vertex attribute, then per face group
		 * with indices its index view and accessor. */
		views += streams;
		accessors += streams;
		for (int32_t g = 0; g < lod->group_count; ++g) {
			if (b->built[mi].fg_index_count_arr[g] == 0)
				continue;
			prims++;
			attrs += streams;
			views++;
			accessors++;
			if (group_has_variants(b, &lod->groups[g]))
				maps += gb->variants_count;
		}
	}
	gb->views          = (cgltf_buffer_view*)calloc(views ? views : 1, sizeof *gb->views);
	gb->accessors      = (cgltf_accessor*)calloc(accessors ? accessors : 1, sizeof *gb->accessors);
	gb->primitives     = (cgltf_primitive*)calloc(prims ? prims : 1, sizeof *gb->primitives);
	gb->attributes     = (cgltf_attribute*)calloc(attrs ? attrs : 1, sizeof *gb->attributes);
	gb->mappings       = (cgltf_material_mapping*)calloc(maps ? maps : 1, sizeof *gb->mappings);
	gb->views_cap      = views;
	gb->accessors_cap  = accessors;
	gb->primitives_cap = prims;
	gb->attributes_cap = attrs;
	gb->mappings_cap   = maps;
	if (!gb->views || !gb->accessors || !gb->primitives || !gb->attributes || !gb->mappings) {
		fprintf(stderr, "opt2gltf: oom (geometry arrays)\n");
		return false;
	}
	b->base_material_count = gb->materials_count;
	b->promoted            = (cgltf_material**)calloc(b->base_material_count * 2, sizeof *b->promoted);
	if (!b->promoted) {
		fprintf(stderr, "opt2gltf: oom (promoted materials)\n");
		return false;
	}
	return true;
}

/* One vertex attribute of every vertex (`size` bytes at `offset` in
 * GltfVertex), as a buffer view and its accessor. */
static cgltf_accessor* write_attribute(GltfBuild* gb, const BuiltMesh* bm, size_t offset, size_t size,
									   cgltf_type type) {
	const size_t start = opt_gltf_bin_append(gb, NULL, 0);
	for (uint32_t i = 0; i < bm->vertex_count; ++i)
		opt_gltf_bin_append(gb, (const char*)&bm->vertices[i] + offset, size);
	cgltf_buffer_view* view =
		opt_gltf_new_view(gb, start, bm->vertex_count * size, cgltf_buffer_view_type_vertices);
	return opt_gltf_new_accessor(gb, view, bm->vertex_count, type, cgltf_component_type_r_32f);
}

/* The material texture `ti` shows: its own, else the untextured one. */
static cgltf_material* texture_material_of(const Build* b, int32_t ti) {
	return &b->gb->materials[ti >= 0 && ti < b->opt->texture_count ? (size_t)ti : b->untextured];
}

/* The variant mappings of a primitive drawing face group `grp`: per
 * variant giving it another material, that material. A state variant shows
 * its state (the original OPT walker clamps each switch to its last child);
 * a given variant its material, else its state. Materials of different
 * alpha classes are promoted to the most transparent one. */
static bool primitive_mappings(Build* b, cgltf_primitive* prim, const opt_face_group_t* grp, int32_t mi,
							   int32_t g) {
	GltfBuild*       gb        = b->gb;
	const size_t     vc        = gb->variants_count;
	const int32_t    t0        = grp->texture_index;
	const int        sc        = grp->state_count;
	const bool       switched  = sc > 1 && grp->state_textures;
	cgltf_material** materials = (cgltf_material**)calloc(vc ? vc : 1, sizeof *materials);
	if (!materials) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	int rank = opt_gltf_alpha_class_rank(prim->material);
	for (size_t v = 0; v < vc; ++v) {
		size_t  state     = v + 1;
		int32_t composite = -1;
		if (v >= b->state_variant_count) {
			const size_t sv = v - b->state_variant_count;
			state           = b->variants->variants[sv].state;
			if (t0 >= 0 && t0 < b->opt->texture_count)
				composite = b->variants->material_of[sv * (size_t)b->opt->texture_count + (size_t)t0];
		}
		if (composite >= 0)
			materials[v] = &gb->materials[b->untextured + 1 + (size_t)composite];
		else
			materials[v] = texture_material_of(
				b, switched ? grp->state_textures[state < (size_t)sc ? state : (size_t)sc - 1] : t0);
		if (opt_gltf_alpha_class_rank(materials[v]) > rank)
			rank = opt_gltf_alpha_class_rank(materials[v]);
	}
	int mixed = opt_gltf_alpha_class_rank(prim->material) != rank;
	for (size_t v = 0; v < vc; ++v)
		mixed |= opt_gltf_alpha_class_rank(materials[v]) != rank;
	if (mixed) {
		prim->material =
			opt_gltf_promoted_material(gb, prim->material, rank, b->base_material_count, b->promoted);
		for (size_t v = 0; v < vc; ++v)
			materials[v] =
				opt_gltf_promoted_material(gb, materials[v], rank, b->base_material_count, b->promoted);
		fprintf(stderr, "opt2gltf: %s: mesh %d face group %d switch states mix alpha classes; all use %s\n",
				b->basename, (int)mi, (int)g, rank == 2 ? "BLEND" : "MASK");
	}
	size_t mapped = 0;
	for (size_t v = 0; v < vc; ++v)
		mapped += materials[v] != prim->material;
	if (mapped) {
		cgltf_material_mapping* maps = (cgltf_material_mapping*)opt_gltf_push_array(
			(void**)&gb->mappings, &gb->mappings_count, &gb->mappings_cap, sizeof *maps, mapped);
		size_t k = 0;
		for (size_t v = 0; v < vc; ++v) {
			if (materials[v] == prim->material)
				continue;
			maps[k].variant    = (cgltf_size)v;
			maps[k++].material = materials[v];
		}
		prim->mappings       = maps;
		prim->mappings_count = (cgltf_size)mapped;
	}
	free(materials);
	return true;
}

/* The primitive of face group `g` of mesh `mi`: its indices (16-bit while
 * every vertex fits), the mesh's vertex attributes, its material and
 * variant mappings. */
static bool write_primitive(Build* b, int32_t mi, int32_t g, cgltf_accessor* const vertex_attributes[4]) {
	GltfBuild*              gb            = b->gb;
	const BuiltMesh*        bm            = &b->built[mi];
	const opt_face_group_t* grp           = &b->opt->meshes[mi].lods[0].groups[g];
	const uint32_t          ic            = bm->fg_index_count_arr[g];
	const uint32_t*         group_indices = bm->indices + bm->fg_index_start[g];
	size_t                  idx_off, idx_bytes;
	cgltf_component_type    idx_type;
	if (bm->vertex_count <= 65536u) {
		uint16_t* narrow = (uint16_t*)malloc((size_t)ic * sizeof *narrow);
		if (!narrow)
			return false;
		for (uint32_t i = 0; i < ic; ++i)
			narrow[i] = (uint16_t)group_indices[i];
		idx_bytes = (size_t)ic * sizeof *narrow;
		idx_off   = opt_gltf_bin_append(gb, narrow, idx_bytes);
		idx_type  = cgltf_component_type_r_16u;
		free(narrow);
	} else {
		idx_bytes = (size_t)ic * sizeof *group_indices;
		idx_off   = opt_gltf_bin_append(gb, group_indices, idx_bytes);
		idx_type  = cgltf_component_type_r_32u;
	}
	cgltf_buffer_view* idx_view = opt_gltf_new_view(gb, idx_off, idx_bytes, cgltf_buffer_view_type_indices);
	cgltf_accessor*    idx_acc  = opt_gltf_new_accessor(gb, idx_view, ic, cgltf_type_scalar, idx_type);

	cgltf_primitive* prim = (cgltf_primitive*)opt_gltf_push_array(
		(void**)&gb->primitives, &gb->primitives_count, &gb->primitives_cap, sizeof *prim, 1);
	prim->type    = cgltf_primitive_type_triangles;
	prim->indices = idx_acc;

	static const struct {
		const char*          name;
		cgltf_attribute_type type;
	} kAttributes[4] = {
		{ "POSITION", cgltf_attribute_type_position },
		{ "NORMAL", cgltf_attribute_type_normal },
		{ "TEXCOORD_0", cgltf_attribute_type_texcoord },
		{ "TANGENT", cgltf_attribute_type_tangent },
	};

	const size_t     attr_count = mesh_attribute_count(bm);
	cgltf_attribute* attrs      = (cgltf_attribute*)opt_gltf_push_array(
		(void**)&gb->attributes, &gb->attributes_count, &gb->attributes_cap, sizeof *attrs, attr_count);
	for (size_t a = 0; a < attr_count; ++a) {
		attrs[a].name  = opt_gltf_keep_string(gb, xstrdup(kAttributes[a].name));
		attrs[a].type  = kAttributes[a].type;
		attrs[a].index = 0;
		attrs[a].data  = vertex_attributes[a];
	}
	prim->attributes       = attrs;
	prim->attributes_count = attr_count;

	/* Default material: state 0 (or the untextured one). */
	prim->material = texture_material_of(b, grp->texture_index);
	return !group_has_variants(b, grp) || primitive_mappings(b, prim, grp, mi, g);
}

/* Pass 2: mesh `mi`'s vertex attributes into the .bin, then one primitive
 * per face group with indices. */
static bool write_mesh(Build* b, int32_t mi) {
	GltfBuild*        gb   = b->gb;
	const BuiltMesh*  bm   = &b->built[mi];
	const opt_mesh_t* m    = &b->opt->meshes[mi];
	cgltf_mesh*       mesh = &gb->meshes[mi];
	mesh->name             = opt_gltf_keep_string(
		gb, xprintf_dup("Mesh%02d_%s", (int)mi, opt_mesh_type_name(m->descriptor.mesh_type)));

	float pmin[3] = { +INFINITY, +INFINITY, +INFINITY };
	float pmax[3] = { -INFINITY, -INFINITY, -INFINITY };
	for (uint32_t i = 0; i < bm->vertex_count; ++i) {
		for (int k = 0; k < 3; ++k) {
			pmin[k] = bm->vertices[i].pos[k] < pmin[k] ? bm->vertices[i].pos[k] : pmin[k];
			pmax[k] = bm->vertices[i].pos[k] > pmax[k] ? bm->vertices[i].pos[k] : pmax[k];
		}
	}
	cgltf_accessor* attributes[4] = { NULL };
	attributes[0] =
		write_attribute(gb, bm, offsetof(GltfVertex, pos), sizeof bm->vertices[0].pos, cgltf_type_vec3);
	attributes[0]->has_min = 1;
	attributes[0]->has_max = 1;
	memcpy(attributes[0]->min, pmin, sizeof pmin);
	memcpy(attributes[0]->max, pmax, sizeof pmax);
	attributes[1] =
		write_attribute(gb, bm, offsetof(GltfVertex, nrm), sizeof bm->vertices[0].nrm, cgltf_type_vec3);
	attributes[2] =
		write_attribute(gb, bm, offsetof(GltfVertex, uv), sizeof bm->vertices[0].uv, cgltf_type_vec2);
	if (bm->has_tangents)
		attributes[3] =
			write_attribute(gb, bm, offsetof(GltfVertex, tan), sizeof bm->vertices[0].tan, cgltf_type_vec4);

	mesh->primitives = gb->primitives + gb->primitives_count;
	for (int32_t g = 0; g < m->lods[0].group_count; ++g) {
		if (bm->fg_index_count_arr[g] == 0)
			continue;
		if (!write_primitive(b, mi, g, attributes))
			return false;
		mesh->primitives_count++;
	}
	return true;
}

/* A variant that changes no primitive's material (one without materials
 * of its own for the textures drawn) still maps the first primitive to its own material, so that
 * editors writing only mapped variants (Blender's glTF exporter) keep it
 * for the markings list and missions to name. */
static bool map_every_variant(Build* b) {
	GltfBuild* gb = b->gb;
	if (!gb->variants_count || !gb->primitives_count)
		return true;
	bool* mapped = (bool*)calloc(gb->variants_count, sizeof *mapped);
	if (!mapped) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	size_t unmapped = 0;
	for (size_t k = 0; k < gb->mappings_count; ++k)
		mapped[gb->mappings[k].variant] = true;
	for (size_t v = 0; v < gb->variants_count; ++v)
		unmapped += !mapped[v];
	if (unmapped) {
		/* cgltf points primitives at their mappings: rebuild the array. */
		const size_t            count    = gb->mappings_count + unmapped;
		cgltf_material_mapping* mappings = (cgltf_material_mapping*)calloc(count, sizeof *mappings);
		if (!mappings) {
			free(mapped);
			fprintf(stderr, "opt2gltf: oom\n");
			return false;
		}
		size_t k = 0;
		for (size_t p = 0; p < gb->primitives_count; ++p) {
			cgltf_primitive*        prim  = &gb->primitives[p];
			cgltf_material_mapping* first = mappings + k;
			for (size_t m = 0; m < prim->mappings_count; ++m)
				mappings[k++] = prim->mappings[m];
			for (size_t v = 0; p == 0 && v < gb->variants_count; ++v) {
				if (!mapped[v]) {
					mappings[k].variant    = (cgltf_size)v;
					mappings[k++].material = prim->material;
				}
			}
			prim->mappings_count = (cgltf_size)(mappings + k - first);
			prim->mappings       = prim->mappings_count ? first : NULL;
		}
		free(gb->mappings);
		gb->mappings       = mappings;
		gb->mappings_count = count;
		gb->mappings_cap   = count;
	}
	free(mapped);
	return true;
}

static bool write_meshes(Build* b) {
	if (!allocate_geometry(b))
		return false;
	for (int32_t mi = 0; mi < b->opt->mesh_count; ++mi)
		if (!write_mesh(b, mi))
			return false;
	return map_every_variant(b);
}

/* ---- Nodes -------------------------------------------------------------- */

/* The model root's AERON_flight_model object: the articulations (in option
 * order; speedBack only when it differs), articulation settings, component
 * sets, markings and the materials' common energy (bits of kAnimEnergies);
 * NULL when out of memory. */
static cJSON* root_extension(const Build* b, unsigned energy) {
	const OptGltfBuildOptions* o    = b->options;
	cJSON*                     root = cJSON_CreateObject();
	bool                       ok   = OptJson_Add(root, "role", cJSON_CreateString("model"));
	cJSON* list = ok && o->model.articulation_count ? OptJson_AddArray(root, "articulations") : NULL;
	ok          = ok && (list || !o->model.articulation_count);
	for (size_t a = 0; ok && a < o->model.articulation_count; ++a) {
		const OptGltfComponentArticulation* art    = &o->model.articulations[a];
		cJSON*                              object = OptJson_AddObject(list, NULL);
		ok = OptJson_Add(object, "component", cJSON_CreateNumber(art->component)) &&
			 OptJson_Add(object, "group", cJSON_CreateString(art->group)) &&
			 OptJson_Add(object, "angle", cJSON_CreateNumber(art->angle)) &&
			 OptJson_Add(object, "speed", cJSON_CreateNumber(art->speed));
		if (ok && art->speed_back != art->speed)
			ok = OptJson_Add(object, "speedBack", cJSON_CreateNumber(art->speed_back));
		if (ok && art->behavior[0])
			ok = OptJson_Add(object, "behavior", cJSON_CreateString(art->behavior));
	}
	cJSON* settings = ok && o->model.setting_count ? OptJson_AddObject(root, "articulationSettings") : NULL;
	ok              = ok && (settings || !o->model.setting_count);
	for (size_t s = 0; ok && s < o->model.setting_count; ++s)
		ok = OptJson_Add(settings, o->model.settings[s].name, cJSON_CreateNumber(o->model.settings[s].value));
	cJSON* sets = ok && o->model.set_count ? OptJson_AddObject(root, "componentSets") : NULL;
	ok          = ok && (sets || !o->model.set_count);
	for (size_t s = 0; ok && s < o->model.set_count; ++s) {
		const OptGltfComponentSet* set     = &o->model.sets[s];
		cJSON*                     members = OptJson_AddArray(sets, set->name);
		ok                                 = members != NULL;
		for (int component = 0; ok && component < 255; ++component)
			if (set->members[component / 32] & (1u << (component % 32)))
				ok = OptJson_Add(members, NULL, cJSON_CreateNumber(component));
	}
	ok = ok && markings_json(root, b->variants, b->max_state_count) &&
		 (!energy || opt_gltf_add_energy(root, energy));
	if (!ok) {
		cJSON_Delete(root);
		return NULL;
	}
	return root;
}

/* Adds `values` (OPT axes and units) to `object` as `key`, in glTF axes
 * and metres. */
static bool add_position(cJSON* object, const char* key, const opt_vec3_t* values) {
	float v[3];
	swap_axis_v3(values, v);
	for (int k = 0; k < 3; ++k)
		v[k] *= OPT_METERS_PER_UNIT;
	return OptJson_Add(object, key, OptJson_Floats(v, 3));
}

/* Mesh `mi`'s component AERON_flight_model object; NULL when out of
 * memory. */
static cJSON* component_extension(const Build* b, int32_t mi) {
	const OptGltfBuildOptions* o     = b->options;
	const opt_mesh_t*          m     = &b->opt->meshes[mi];
	const float                scale = OPT_METERS_PER_UNIT;
	cJSON*                     ext   = cJSON_CreateObject();
	bool                       ok =
		OptJson_Add(ext, "role", cJSON_CreateString("component")) &&
		OptJson_Add(ext, "meshType", cJSON_CreateNumber((int)m->descriptor.mesh_type)) &&
		OptJson_Add(ext, "explosionFlags", cJSON_CreateNumber((unsigned)m->descriptor.explosion_type)) &&
		OptJson_Add(ext, "targetId", cJSON_CreateNumber((int)m->descriptor.target_id));
	if (ok && m->has_descriptor) {
		const float span[3]       = { m->descriptor.span.x * scale, m->descriptor.span.z * scale,
									  m->descriptor.span.y * scale };
		const float bounds_min[3] = { -m->descriptor.bbox_max.x * scale, m->descriptor.bbox_min.z * scale,
									  -m->descriptor.bbox_max.y * scale };
		const float bounds_max[3] = { -m->descriptor.bbox_min.x * scale, m->descriptor.bbox_max.z * scale,
									  -m->descriptor.bbox_min.y * scale };
		ok                        = OptJson_Add(ext, "span", OptJson_Floats(span, 3)) &&
									add_position(ext, "center", &m->descriptor.center) &&
									OptJson_Add(ext, "boundsMin", OptJson_Floats(bounds_min, 3)) &&
									OptJson_Add(ext, "boundsMax", OptJson_Floats(bounds_max, 3));
	}
	if (ok && m->descriptor.target_id != 0)
		ok = add_position(ext, "target", &m->descriptor.target);
	/* Every mesh with a rotation frame has it: the default axis frame is
	 * not static, XWA rotates default-frame meshes about their pivot
	 * whenever their runtime angle is nonzero (hangar droid antennas spin
	 * about the default vertical axis). */
	if (ok && m->has_rotation_scale) {
		float rax[3], dax[3], uax[3];
		swap_axis_v3(&m->rotation_scale.rotation_axis, rax);
		swap_axis_v3(&m->rotation_scale.direction_axis, dax);
		swap_axis_v3(&m->rotation_scale.up_axis, uax);
		/* The pivot is a position, scaled; the axes are directions. */
		for (int axis = 0; axis < 3; ++axis) {
			rax[axis] /= OPT_Q15_TO_UNIT;
			dax[axis] /= OPT_Q15_TO_UNIT;
			uax[axis] /= OPT_Q15_TO_UNIT;
		}
		cJSON* rotation = OptJson_AddObject(ext, "rotation");
		ok              = add_position(rotation, "pivot", &m->rotation_scale.pivot) &&
						  OptJson_Add(rotation, "rotationAxis", OptJson_Floats(rax, 3)) &&
						  OptJson_Add(rotation, "directionAxis", OptJson_Floats(dax, 3)) &&
						  OptJson_Add(rotation, "upAxis", OptJson_Floats(uax, 3));
	}
	/* Tags in option order, duplicates dropped. */
	cJSON* tags = NULL;
	for (size_t t = 0; ok && t < o->model.tag_count; ++t) {
		const OptGltfComponentTag* tag = &o->model.tags[t];
		if (tag->mesh_index != (uint32_t)mi)
			continue;
		bool duplicate = false;
		for (size_t previous = 0; previous < t && !duplicate; ++previous)
			duplicate = o->model.tags[previous].mesh_index == (uint32_t)mi &&
						strcmp(o->model.tags[previous].tag, tag->tag) == 0;
		if (duplicate)
			continue;
		if (!tags)
			tags = OptJson_AddArray(ext, "tags");
		ok = OptJson_Add(tags, NULL, cJSON_CreateString(tag->tag));
	}
	if (!ok) {
		cJSON_Delete(ext);
		return NULL;
	}
	return ext;
}

/* A child node at `position` (OPT axes and units) under `parent`. */
static void child_node(cgltf_node* node, cgltf_node* parent, const opt_vec3_t* position) {
	float pos[3];
	swap_axis_v3(position, pos);
	node->parent          = parent;
	node->has_translation = 1;
	for (int k = 0; k < 3; ++k)
		node->translation[k] = pos[k] * OPT_METERS_PER_UNIT;
}

static bool hardpoint_node(Build* b, cgltf_node* node, cgltf_node* parent, const opt_hardpoint_t* hp) {
	node->name = opt_gltf_keep_string(b->gb, xprintf_dup("hardpoint_%d", (int)hp->type));
	child_node(node, parent, &hp->pos);
	cJSON* extension = cJSON_CreateObject();
	if (!OptJson_Add(extension, "role", cJSON_CreateString("hardpoint")) ||
		!OptJson_Add(extension, "type", cJSON_CreateNumber((int)hp->type))) {
		cJSON_Delete(extension);
		return false;
	}
	return opt_gltf_set_extension(b->gb, node, extension);
}

static bool engine_glow_node(Build* b, cgltf_node* node, cgltf_node* parent, const opt_engine_glow_t* eg,
							 int e) {
	node->name = opt_gltf_keep_string(b->gb, xprintf_dup("engine_glow_%d", e));
	child_node(node, parent, &eg->position);
	float look[3], up[3], right[3], rotation[4];
	swap_axis_v3(&eg->look_axis, look);
	swap_axis_v3(&eg->up_axis, up);
	swap_axis_v3(&eg->right_axis, right);
	quaternion_from_axes(right, up, look, rotation);
	node->has_rotation = 1;
	memcpy(node->rotation, rotation, sizeof rotation);
	node->has_scale          = 1;
	node->scale[0]           = eg->dimensions.x * OPT_METERS_PER_UNIT;
	node->scale[1]           = eg->dimensions.y * OPT_METERS_PER_UNIT;
	node->scale[2]           = eg->dimensions.z * OPT_METERS_PER_UNIT;
	const uint32_t colors[2] = { eg->core_color, eg->outer_color };
	float          rgba[2][4];
	for (int color = 0; color < 2; ++color) {
		rgba[color][0] = (float)((colors[color] >> 16) & 0xff) / 255.0f;
		rgba[color][1] = (float)((colors[color] >> 8) & 0xff) / 255.0f;
		rgba[color][2] = (float)(colors[color] & 0xff) / 255.0f;
		rgba[color][3] = (float)((colors[color] >> 24) & 0xff) / 255.0f;
	}
	cJSON* extension = cJSON_CreateObject();
	if (!OptJson_Add(extension, "role", cJSON_CreateString("engineGlow")) ||
		!OptJson_Add(extension, "enabled", cJSON_CreateBool(!eg->is_disabled)) ||
		!OptJson_Add(extension, "coreColor", OptJson_Floats(rgba[0], 4)) ||
		!OptJson_Add(extension, "outerColor", OptJson_Floats(rgba[1], 4))) {
		cJSON_Delete(extension);
		return false;
	}
	return opt_gltf_set_extension(b->gb, node, extension);
}

/* Light `li` (a point light) and its node. */
static bool light_node(Build* b, cgltf_node* node, cgltf_node* parent, size_t li) {
	const OptGltfLight* source = &b->options->model.lights[li];
	cgltf_light*        light  = &b->gb->lights[li];
	light->name                = opt_gltf_keep_string(b->gb, xprintf_dup("light_%zu", li));
	light->type                = cgltf_light_type_point;
	memcpy(light->color, source->color, sizeof light->color);
	light->intensity          = source->intensity;
	light->range              = source->range;
	node->name                = light->name;
	node->light               = light;
	const opt_vec3_t position = { source->position[0], source->position[1], source->position[2] };
	child_node(node, parent, &position);
	cJSON* extension = cJSON_CreateObject();
	if (!OptJson_Add(extension, "role", cJSON_CreateString("light"))) {
		cJSON_Delete(extension);
		return false;
	}
	return opt_gltf_set_extension(b->gb, node, extension);
}

/* Mesh `mi`'s node, with its child nodes from *next_child: hardpoints,
 * engine glows, then lights (empties, so that Blender and other tools show
 * them). */
static bool mesh_node(Build* b, int32_t mi, cgltf_node* node, cgltf_node** next_child) {
	const opt_mesh_t* m    = &b->opt->meshes[mi];
	cgltf_node*       root = &b->gb->nodes[0];
	root->children[mi]     = node;
	node->parent           = root;
	node->mesh             = &b->gb->meshes[mi];
	node->name             = b->gb->meshes[mi].name; /* already kept */
	if (!opt_gltf_set_extension(b->gb, node, component_extension(b, mi)))
		return false;

	const size_t light_count = b->options->model.light_count;
	int          mesh_lights = 0;
	for (size_t li = 0; li < light_count; ++li)
		mesh_lights += b->options->model.lights[li].mesh == mi;
	const int child_total = m->hardpoint_count + m->engine_glow_count + mesh_lights;
	if (child_total == 0)
		return true;
	cgltf_node** children = (cgltf_node**)calloc(child_total, sizeof *children);
	if (!children) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	node->children       = children;
	node->children_count = child_total;
	int ci               = 0;
	for (int h = 0; h < m->hardpoint_count; ++h) {
		children[ci] = (*next_child)++;
		if (!hardpoint_node(b, children[ci++], node, &m->hardpoints[h]))
			return false;
	}
	for (int e = 0; e < m->engine_glow_count; ++e) {
		children[ci] = (*next_child)++;
		if (!engine_glow_node(b, children[ci++], node, &m->engine_glows[e], e))
			return false;
	}
	for (size_t li = 0; li < light_count; ++li) {
		if (b->options->model.lights[li].mesh != mi)
			continue;
		children[ci] = (*next_child)++;
		if (!light_node(b, children[ci++], node, li))
			return false;
	}
	return true;
}

/* The nodes: the model root, one per mesh, then each mesh's children. */
static bool build_nodes(Build* b) {
	const opt_file_t*          opt            = b->opt;
	const OptGltfBuildOptions* o              = b->options;
	GltfBuild*                 gb             = b->gb;
	size_t                     children_total = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
		children_total += (size_t)opt->meshes[mi].hardpoint_count + (size_t)opt->meshes[mi].engine_glow_count;
	for (size_t li = 0; li < o->model.light_count; ++li)
		if (o->model.lights[li].mesh < 0 || o->model.lights[li].mesh >= opt->mesh_count)
			return opt_gltf_build_fail(b, "light %zu names OPT mesh %d of %d", li,
									   (int)o->model.lights[li].mesh, (int)opt->mesh_count);
	if (o->model.light_count) {
		gb->lights = (cgltf_light*)calloc(o->model.light_count, sizeof *gb->lights);
		if (!gb->lights)
			return false;
		gb->lights_count = o->model.light_count;
	}
	const size_t total = 1 + (size_t)opt->mesh_count + children_total + o->model.light_count;
	gb->nodes          = (cgltf_node*)calloc(total, sizeof *gb->nodes);
	if (!gb->nodes) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	gb->nodes_count = total;
	/* cgltf transform helpers read the implicit glTF defaults from these. */
	for (size_t i = 0; i < total; ++i) {
		gb->nodes[i].rotation[3] = 1.0f;
		set_color(gb->nodes[i].scale, 1.0f, 1.0f, 1.0f);
	}

	cgltf_node* root = &gb->nodes[0];
	root->name       = opt_gltf_keep_string(gb, xprintf_dup("OPT_%s", b->basename));
	root->children   = (cgltf_node**)calloc((size_t)opt->mesh_count, sizeof *root->children);
	if (!root->children) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	root->children_count   = (size_t)opt->mesh_count;
	cgltf_node* next_child = &gb->nodes[1 + opt->mesh_count];
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
		if (!mesh_node(b, mi, &gb->nodes[1 + mi], &next_child))
			return false;
	return true;
}

/* ---- Document ----------------------------------------------------------- */

static bool finish_document(Build* b) {
	OptGltfDocument* document = b->document;
	GltfBuild*       gb       = b->gb;
	document->scene.name      = opt_gltf_keep_string(gb, xstrdup("Scene"));
	/* The scene has exactly one root: the model root. */
	document->scene_roots = (cgltf_node**)calloc(1, sizeof *document->scene_roots);
	if (!document->scene_roots)
		return false;
	document->scene_roots[0]    = &gb->nodes[0];
	document->scene.nodes       = document->scene_roots;
	document->scene.nodes_count = 1;
	gb->buffer.size             = gb->bin_size;
	gb->buffer.data             = gb->bin;
	/* The model root's object takes the materials' common energy. */
	unsigned energy = 0;
	if (!opt_gltf_prune_unused(document) || !opt_gltf_material_extensions(document, &energy) ||
		!opt_gltf_set_extension(gb, &gb->nodes[0], root_extension(b, energy)))
		return false;

	cgltf_data* data         = &document->data;
	data->asset.version      = "2.0";
	data->asset.generator    = "opt2gltf";
	data->buffers            = &gb->buffer;
	data->buffers_count      = 1;
	data->buffer_views       = gb->views;
	data->buffer_views_count = gb->views_count;
	data->accessors          = gb->accessors;
	data->accessors_count    = gb->accessors_count;
	data->images             = gb->images;
	data->images_count       = gb->images_count;
	data->textures           = gb->textures;
	data->textures_count     = gb->textures_count;
	data->samplers           = &gb->sampler;
	data->samplers_count     = 1;
	data->materials          = gb->materials;
	data->materials_count    = gb->materials_count;
	data->meshes             = gb->meshes;
	data->meshes_count       = gb->meshes_count;
	data->nodes              = gb->nodes;
	data->nodes_count        = gb->nodes_count;
	data->scenes             = &document->scene;
	data->scenes_count       = 1;
	data->scene              = &document->scene;
	data->variants           = gb->variants;
	data->variants_count     = gb->variants_count;
	data->lights             = gb->lights;
	data->lights_count       = gb->lights_count;
	/* The flight contract, and the material variants when there are any. */
	const size_t extension_count = gb->variants_count > 0 ? 2u : 1u;
	data->extensions_used        = (char**)calloc(extension_count, sizeof(char*));
	if (!data->extensions_used)
		return false;
	data->extensions_used[0] = (char*)"AERON_flight_model";
	if (gb->variants_count > 0)
		data->extensions_used[1] = (char*)"KHR_materials_variants";
	data->extensions_used_count = extension_count;
	return true;
}

static bool build_memory(const opt_file_t* opt, const char* basename, const OptGltfBuildOptions* options,
						 OptGltfDocument** out_document, opt_error_t* error) {
	if (out_document)
		*out_document = NULL;
	if (error)
		error->msg[0] = '\0';
	if (!opt || !basename || !out_document)
		return false;
	const OptGltfBuildOptions* o = options ? options : &kDefaultOptions;
	Build                      b = {
		.opt       = opt,
		.basename  = basename,
		.options   = o,
		.error     = error,
		.materials = o->material.source == OPT_GLTF_MATERIALS_GIVEN ? o->material.given : NULL,
		.variants  = o->material.source == OPT_GLTF_MATERIALS_GIVEN ? o->material.variants : NULL,
		.screens   = o->material.screens,
		.smooth_angle =
			o->geometry.normals == OPT_GLTF_NORMALS_SMOOTHED ? o->geometry.smooth_angle_degrees : -1.0f,
		.repair_normals      = o->geometry.normals == OPT_GLTF_NORMALS_CLASSIC,
		.fullbright          = o->material.source == OPT_GLTF_MATERIALS_FULLBRIGHT,
		.fullbright_strength = o->material.fullbright_strength,
		.emissive            = o->material.source == OPT_GLTF_MATERIALS_STOCK && o->material.emissive,
	};
	if (!validate_options(&b))
		return false;
	b.document = (OptGltfDocument*)calloc(1, sizeof *b.document);
	if (!b.document)
		return opt_gltf_build_fail(&b, "out of memory");
	b.gb             = &b.document->gb;
	b.gb->buffer.uri = opt_gltf_keep_string(b.gb, xprintf_dup("%s.bin", basename));
	/* One sampler: repeat, trilinear. */
	b.gb->sampler.mag_filter = 9729;  /* LINEAR */
	b.gb->sampler.min_filter = 9987;  /* LINEAR_MIPMAP_LINEAR */
	b.gb->sampler.wrap_s     = 10497; /* REPEAT */
	b.gb->sampler.wrap_t     = 10497; /* REPEAT */

	const bool ok = opt_gltf_prepare_textures(&b) && opt_gltf_build_materials(&b) && build_geometry(&b) &&
					build_variants(&b) && write_meshes(&b) && build_nodes(&b) && finish_document(&b);
	build_free(&b);
	if (!ok) {
		if (error && !error->msg[0])
			snprintf(error->msg, sizeof error->msg, "out of memory while converting %s", basename);
		OptGltf_Free(b.document);
		return false;
	}
	*out_document = b.document;
	return true;
}

bool OptGltf_BuildMemory(const opt_file_t* opt, const char* basename, const OptGltfBuildOptions* options,
						 OptGltfDocument** out_document, opt_error_t* error) {
	if (!opt)
		return build_memory(opt, basename, options, out_document, error);
	FacelessPatch     patch;
	const opt_file_t* source = opt_gltf_faceless_patch_apply(opt, &patch);
	if (!source) {
		if (out_document)
			*out_document = NULL;
		if (error)
			snprintf(error->msg, sizeof error->msg, "out of memory");
		return false;
	}
	const bool ok = build_memory(source, basename, options, out_document, error);
	opt_gltf_faceless_patch_free(&patch);
	return ok;
}
