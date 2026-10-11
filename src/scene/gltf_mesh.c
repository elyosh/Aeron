/*
 * gltf_mesh — the render half of an AeronFlightModel.
 *
 * Consumes cooked glTF data from `aeron_gltf_cook` (tools/gltf_cook/): a
 * shipped .glb with BC7/BC5 KTX2 channel atlases, or the RGBA8 atlases of the
 * in-memory cook that converts an OPT at runtime. Both carry the atlases
 * through KHR_texture_basisu and per-material UV transforms through
 * KHR_texture_transform. Its caller has already validated the
 * AERON_flight_model hierarchy. The loader walks the cgltf graph and:
 *   - lifts each atlas KTX2 blob out of the GLB BIN chunk and copies
 *     it into a AeronGltfChannelKtx2-owned buffer the SDL_GPU side
 *     decodes via ktx2_open_mem at upload time
 *   - reads per-material PBR factors + per-binding KHR_texture_transform
 *     into AeronGltfMaterial.uv_xform[ch]
 *   - decodes per-primitive vertex / index / variant tables into one
 *     merged ship-level buffer the renderer issues a single indexed
 *     draw against
 *   - assigns each component node's model-local ordinal (its order under
 *     the model root) to its render vertices
 *
 * PNG decoding, atlas packing, and mip generation happen offline in
 * aeron_gltf_cook.
 *
 * Diagnostics flow through SDL_Log (host-side abstraction provides
 * the symbol). Errors return false; partially-built ships are freed
 * before returning.
 */

#include "aeron/scene/gltf_mesh.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cgltf.h"

/* Host-provided log (SDL3 via SDL_Log; standalone tools stub it). */
extern void SDL_Log(const char* fmt, ...);

/* glTF +Y up / +Z front to Aeron +X right / -Y forward / +Z up. */
static inline void gltf_to_aeron3(const float in[3], float out[3]) {
	out[0] = -in[0];
	out[1] = -in[2];
	out[2] = in[1];
}

/* The handedness-changing frame conversion also reverses tangent.w. */
static inline void gltf_to_aeron_tangent(const float in[4], float out[4]) {
	out[0] = -in[0];
	out[1] = -in[2];
	out[2] = in[1];
	out[3] = -in[3];
}

static bool json_object_string_equals(const char* json, const char* key, const char* expected) {
	if (!json || !key || !expected)
		return false;
	cJSON*       object = cJSON_ParseWithOpts(json, NULL, true);
	const cJSON* value  = cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
	const bool   matches =
		cJSON_IsString(value) && value->valuestring && strcmp(value->valuestring, expected) == 0;
	cJSON_Delete(object);
	return matches;
}

static void transform_point(const float matrix[16], const float in[3], float out[3]) {
	out[0] = matrix[0] * in[0] + matrix[4] * in[1] + matrix[8] * in[2] + matrix[12];
	out[1] = matrix[1] * in[0] + matrix[5] * in[1] + matrix[9] * in[2] + matrix[13];
	out[2] = matrix[2] * in[0] + matrix[6] * in[1] + matrix[10] * in[2] + matrix[14];
}

static void transform_direction(const float matrix[16], const float in[3], float out[3]) {
	out[0]             = matrix[0] * in[0] + matrix[4] * in[1] + matrix[8] * in[2];
	out[1]             = matrix[1] * in[0] + matrix[5] * in[1] + matrix[9] * in[2];
	out[2]             = matrix[2] * in[0] + matrix[6] * in[1] + matrix[10] * in[2];
	const float length = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	if (length > 1.0e-9f) {
		out[0] /= length;
		out[1] /= length;
		out[2] /= length;
	}
}

/* ===== cgltf accessor decoders ===================================== */

static void decode_vec3(const cgltf_accessor* a, float* out, uint32_t n) {
	if (!a || a->count != n)
		return;
	for (uint32_t i = 0; i < n; i++)
		cgltf_accessor_read_float(a, i, out + i * 3, 3);
}

static void decode_vec2(const cgltf_accessor* a, float* out, uint32_t n) {
	if (!a || a->count != n)
		return;
	for (uint32_t i = 0; i < n; i++)
		cgltf_accessor_read_float(a, i, out + i * 2, 2);
}

static void decode_vec4(const cgltf_accessor* a, float* out, uint32_t n) {
	if (!a || a->count != n)
		return;
	for (uint32_t i = 0; i < n; i++)
		cgltf_accessor_read_float(a, i, out + i * 4, 4);
}

/* Find an attribute accessor by glTF attribute type. */
static const cgltf_accessor* prim_attr(const cgltf_primitive* p, cgltf_attribute_type t) {
	for (cgltf_size i = 0; i < p->attributes_count; i++) {
		if (p->attributes[i].type == t && p->attributes[i].index == 0)
			return p->attributes[i].data;
	}
	return NULL;
}

/* ===== Channel KTX2 extraction =====================================
 *
 * aeron_gltf_cook names each atlas image `atlas_<channel>` with mimeType
 * `image/ktx2` and stores the KTX2 bytes in a buffer_view. We match by
 * name and copy the bytes out of the GLB BIN chunk into a
 * AeronGltfChannelKtx2-owned buffer so the SDL_GPU side has a stable
 * pointer after cgltf_free.
 *
 * Returns NULL when no image carries the expected name; caller decides
 * whether that's fatal. */
static const cgltf_image* find_atlas_image(const cgltf_data* data, const char* atlas_name) {
	for (cgltf_size i = 0; i < data->images_count; i++) {
		const cgltf_image* im = &data->images[i];
		if (im->name && strcmp(im->name, atlas_name) == 0)
			return im;
	}
	return NULL;
}

static bool copy_channel_ktx2(const cgltf_data* data, const char* atlas_name, AeronGltfChannelKtx2* out) {
	const cgltf_image* im = find_atlas_image(data, atlas_name);
	if (!im) {
		SDL_Log("[flight_gltf] missing atlas image '%s'", atlas_name);
		return false;
	}
	if (!im->buffer_view || !im->buffer_view->buffer || !im->buffer_view->buffer->data) {
		SDL_Log("[flight_gltf] atlas '%s' has no buffer_view data", atlas_name);
		return false;
	}
	const uint8_t* src  = (const uint8_t*)im->buffer_view->buffer->data + im->buffer_view->offset;
	size_t         size = im->buffer_view->size;
	uint8_t*       copy = (uint8_t*)malloc(size);
	if (!copy)
		return false;
	memcpy(copy, src, size);
	out->data = copy;
	out->size = size;
	return true;
}

/* The cooker's atlas image names, by AERON_GLTF_CHANNEL_*; a variant group
 * names its channel without the prefix. */
static const char* kAtlasNames[AERON_GLTF_CHANNEL_COUNT] = {
	"atlas_base_color",
	"atlas_normal",
	"atlas_metallic_roughness",
	"atlas_emissive",
};

/* ===== Variants and their groups ====================================
 *
 * The asset extras of a model with skin variants (aeron_gltf_cook):
 *   "aeronAssetVariants": N, "aeronSkinMarkings": [name or null, ...],
 *   "aeronVariantGroups": [{"bufferView": b, "layers": l, "variants": [...]}]
 * The groups' KTX2 blocks are copied like the core atlases. */
static char* copy_string(const char* s) {
	const size_t n    = strlen(s) + 1;
	char*        copy = (char*)malloc(n);
	if (copy)
		memcpy(copy, s, n);
	return copy;
}

static uint32_t variant_by_name(const AeronGltfModel* out, const char* name) {
	for (uint32_t v = 0; v < out->variant_count; v++)
		if (out->variant_names[v] && strcmp(out->variant_names[v], name) == 0)
			return v;
	return AERON_GLTF_NO_VARIANT;
}

static bool read_variants(const cgltf_data* data, const char* source_label, AeronGltfModel* out) {
	out->variant_count       = (uint32_t)data->variants_count;
	out->variant_columns     = out->variant_count > 0 ? out->variant_count : 1u;
	out->asset_variant_count = out->variant_count;
	if (out->variant_count) {
		out->variant_names = (char**)calloc(out->variant_count, sizeof *out->variant_names);
		if (!out->variant_names)
			return false;
		for (uint32_t v = 0; v < out->variant_count; v++) {
			const char* name = data->variants[v].name;
			if (name && !(out->variant_names[v] = copy_string(name)))
				return false;
		}
	}
	const cgltf_extras* extras = &data->asset.extras;
	cJSON*              root   = NULL;
	if (extras->data)
		root = cJSON_Parse(extras->data);
	else if (data->json && extras->end_offset > extras->start_offset)
		root = cJSON_ParseWithLength(data->json + extras->start_offset,
									 (size_t)(extras->end_offset - extras->start_offset));
	if (!root)
		return true;
	bool         ok       = true;
	const cJSON* assets   = cJSON_GetObjectItemCaseSensitive(root, "aeronAssetVariants");
	const cJSON* markings = cJSON_GetObjectItemCaseSensitive(root, "aeronSkinMarkings");
	const cJSON* groups   = cJSON_GetObjectItemCaseSensitive(root, "aeronVariantGroups");
	if (assets) {
		ok = cJSON_IsNumber(assets) && assets->valuedouble >= 1 && assets->valuedouble <= out->variant_count;
		if (ok)
			out->asset_variant_count = (uint32_t)assets->valuedouble;
	}
	if (ok && cJSON_IsArray(markings)) {
		out->skin_markings_count = (uint32_t)cJSON_GetArraySize(markings);
		out->skin_markings       = (uint32_t*)calloc(out->skin_markings_count ? out->skin_markings_count : 1,
													 sizeof *out->skin_markings);
		ok                       = out->skin_markings != NULL;
		uint32_t     m           = 0;
		const cJSON* entry       = NULL;
		cJSON_ArrayForEach(entry, markings) {
			if (!ok)
				break;
			out->skin_markings[m] = AERON_GLTF_NO_VARIANT;
			if (cJSON_IsString(entry)) {
				out->skin_markings[m] = variant_by_name(out, entry->valuestring);
				ok                    = out->skin_markings[m] != AERON_GLTF_NO_VARIANT;
			} else {
				ok = cJSON_IsNull(entry);
			}
			m++;
		}
	}
	if (ok && cJSON_IsArray(groups)) {
		out->group_count = (uint32_t)cJSON_GetArraySize(groups);
		out->groups =
			(AeronGltfVariantGroup*)calloc(out->group_count ? out->group_count : 1, sizeof *out->groups);
		ok                 = out->groups != NULL;
		uint32_t     g     = 0;
		const cJSON* entry = NULL;
		cJSON_ArrayForEach(entry, groups) {
			if (!ok)
				break;
			AeronGltfVariantGroup* group    = &out->groups[g++];
			const cJSON*           channel  = cJSON_GetObjectItemCaseSensitive(entry, "channel");
			const cJSON*           view     = cJSON_GetObjectItemCaseSensitive(entry, "bufferView");
			const cJSON*           layers   = cJSON_GetObjectItemCaseSensitive(entry, "layers");
			const cJSON*           variants = cJSON_GetObjectItemCaseSensitive(entry, "variants");
			ok = cJSON_IsNumber(view) && view->valuedouble >= 0 &&
				 view->valuedouble < (double)data->buffer_views_count && cJSON_IsNumber(layers) &&
				 layers->valuedouble >= 1 && layers->valuedouble <= AERON_GLTF_ATLAS_MAX_PAGE &&
				 cJSON_IsArray(variants) && cJSON_GetArraySize(variants) > 0;
			/* The channel, base colour when absent. */
			group->channel = AERON_GLTF_CHANNEL_BASE_COLOR;
			if (ok && channel) {
				ok = false;
				for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT && cJSON_IsString(channel); c++) {
					if (strcmp(channel->valuestring, kAtlasNames[c] + strlen("atlas_")) == 0) {
						group->channel = (uint32_t)c;
						ok             = true;
					}
				}
			}
			if (!ok)
				break;
			const cgltf_buffer_view* bv = &data->buffer_views[(size_t)view->valuedouble];
			group->layers               = (uint32_t)layers->valuedouble;
			group->variant_count        = (uint32_t)cJSON_GetArraySize(variants);
			group->variants             = (uint32_t*)calloc(group->variant_count, sizeof *group->variants);
			group->ktx2.data            = (uint8_t*)malloc(bv->size ? bv->size : 1);
			ok = group->variants && group->ktx2.data && bv->buffer && bv->buffer->data && bv->size > 0;
			if (!ok)
				break;
			memcpy(group->ktx2.data, (const uint8_t*)bv->buffer->data + bv->offset, bv->size);
			group->ktx2.size  = bv->size;
			uint32_t     v    = 0;
			const cJSON* item = NULL;
			cJSON_ArrayForEach(item, variants) {
				ok = ok && cJSON_IsNumber(item) && item->valuedouble >= out->asset_variant_count &&
					 item->valuedouble < out->variant_count;
				if (ok)
					group->variants[v++] = (uint32_t)item->valuedouble;
			}
		}
	}
	cJSON_Delete(root);
	if (!ok)
		SDL_Log("[flight_gltf] '%s' has invalid skin variant data in its asset extras", source_label);
	return ok;
}

/* ===== Per-material read ============================================
 *
 * Pulls factors + per-channel UV transform out of cgltf_material.
 * Channel ordering matches AERON_GLTF_CHANNEL_* — keep in sync with
 * the cooker and the FS. */
static const cgltf_texture_view* material_channel_view(const cgltf_material* m, int channel) {
	switch (channel) {
		case AERON_GLTF_CHANNEL_BASE_COLOR:
			return m->has_pbr_metallic_roughness ? &m->pbr_metallic_roughness.base_color_texture : NULL;
		case AERON_GLTF_CHANNEL_METALLIC_ROUGHNESS:
			return m->has_pbr_metallic_roughness ? &m->pbr_metallic_roughness.metallic_roughness_texture
												 : NULL;
		case AERON_GLTF_CHANNEL_NORMAL:
			return &m->normal_texture;
		case AERON_GLTF_CHANNEL_EMISSIVE:
			return &m->emissive_texture;
		default:
			return NULL;
	}
}

/* The cooker names atlas textures `tex_<channel>_p<page>_l<max_lod>`. A name
 * without that suffix keeps page 0 and no LOD limit; a malformed or
 * out-of-range suffix rejects the material. */
static bool parse_atlas_texture_name(const char* name, uint32_t* page, uint32_t* max_lod) {
	const char* lod = name ? strrchr(name, '_') : NULL;
	if (!lod || lod[1] != 'l' || lod == name)
		return true;
	const char* p = lod - 1;
	while (p > name && *p != '_')
		p--;
	if (*p != '_' || p[1] != 'p')
		return true;
	char*               end         = NULL;
	const unsigned long parsed_page = strtoul(p + 2, &end, 10);
	if (end == p + 2 || end != lod)
		return true;
	const unsigned long parsed_lod = strtoul(lod + 2, &end, 10);
	if (end == lod + 2 || *end != '\0')
		return true;
	if (parsed_page > AERON_GLTF_ATLAS_MAX_PAGE || parsed_lod > AERON_GLTF_ATLAS_UNCLAMPED_LOD) {
		SDL_Log("[flight_gltf] atlas texture '%s' is out of range", name);
		return false;
	}
	*page    = (uint32_t)parsed_page;
	*max_lod = (uint32_t)parsed_lod;
	return true;
}

static bool read_material(const cgltf_material* m, AeronGltfMaterial* out) {
	/* Factor defaults per glTF spec. */
	out->base_color_factor[0] = 1.0f;
	out->base_color_factor[1] = 1.0f;
	out->base_color_factor[2] = 1.0f;
	out->base_color_factor[3] = 1.0f;
	out->emissive_factor[0]   = 0.0f;
	out->emissive_factor[1]   = 0.0f;
	out->emissive_factor[2]   = 0.0f;
	out->emissive_strength    = 1.0f;
	out->metallic_factor      = 0.0f;
	out->roughness_factor     = 1.0f;
	out->double_sided         = 0u;
	out->alpha_mode           = AERON_GLTF_ALPHA_OPAQUE;
	out->alpha_cutoff         = 0.5f;
	out->emissive_mode        = AERON_GLTF_EMISSIVE_ADDITIVE;
	/* uv_xform sentinel zero = "channel not authored" — FS falls back
	 * to factor. Overwritten below for channels that do bind. */
	memset(out->uv_xform, 0, sizeof out->uv_xform);
	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
		out->atlas_page[c]    = 0;
		out->atlas_max_lod[c] = AERON_GLTF_ATLAS_UNCLAMPED_LOD;
	}
	out->specular_factor          = 1.0f;
	out->specular_color_factor[0] = 1.0f;
	out->specular_color_factor[1] = 1.0f;
	out->specular_color_factor[2] = 1.0f;
	out->transmission_factor      = 0.0f;

	if (!m)
		return true;

	out->double_sided = m->double_sided ? 1u : 0u;
	switch (m->alpha_mode) {
		case cgltf_alpha_mode_opaque:
			out->alpha_mode = AERON_GLTF_ALPHA_OPAQUE;
			break;
		case cgltf_alpha_mode_mask:
			if (!isfinite(m->alpha_cutoff) || m->alpha_cutoff < 0.0f || m->alpha_cutoff > 1.0f)
				return false;
			out->alpha_mode   = AERON_GLTF_ALPHA_MASK;
			out->alpha_cutoff = m->alpha_cutoff;
			break;
		case cgltf_alpha_mode_blend:
			out->alpha_mode = AERON_GLTF_ALPHA_BLEND;
			break;
		default:
			return false;
	}
	if (json_object_string_equals(m->extras.data, "aeronEmissiveMode", "legacy_srgb_srcalpha")) {
		out->emissive_mode = AERON_GLTF_EMISSIVE_LEGACY_SRGB_SRCALPHA;
	}

	if (m->has_pbr_metallic_roughness) {
		const cgltf_pbr_metallic_roughness* pbr = &m->pbr_metallic_roughness;
		memcpy(out->base_color_factor, pbr->base_color_factor, sizeof out->base_color_factor);
		out->metallic_factor  = pbr->metallic_factor;
		out->roughness_factor = pbr->roughness_factor;
	}
	out->emissive_factor[0] = m->emissive_factor[0];
	out->emissive_factor[1] = m->emissive_factor[1];
	out->emissive_factor[2] = m->emissive_factor[2];
	if (m->has_emissive_strength)
		out->emissive_strength = m->emissive_strength.emissive_strength;
	if (m->has_specular) {
		out->specular_factor = m->specular.specular_factor;
		memcpy(out->specular_color_factor, m->specular.specular_color_factor,
			   sizeof out->specular_color_factor);
		if (!isfinite(out->specular_factor) || out->specular_factor < 0.0f || out->specular_factor > 1.0f)
			return false;
		for (int i = 0; i < 3; i++)
			if (!isfinite(out->specular_color_factor[i]) || out->specular_color_factor[i] < 0.0f)
				return false;
	}
	if (m->has_transmission) {
		const float factor = m->transmission.transmission_factor;
		if (!isfinite(factor) || factor < 0.0f || factor > 1.0f)
			return false;
		out->transmission_factor = factor;
		/* Glass blends over the scene: only the blend range draws it. */
		if (factor > 0.0f)
			out->alpha_mode = AERON_GLTF_ALPHA_BLEND;
	}

	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
		const cgltf_texture_view* tv = material_channel_view(m, c);
		if (!tv || !tv->texture || !tv->has_transform)
			continue;
		out->uv_xform[c][0] = tv->transform.offset[0];
		out->uv_xform[c][1] = tv->transform.offset[1];
		out->uv_xform[c][2] = tv->transform.scale[0];
		out->uv_xform[c][3] = tv->transform.scale[1];
		if (!parse_atlas_texture_name(tv->texture->name, &out->atlas_page[c], &out->atlas_max_lod[c]))
			return false;
	}
	return true;
}

/* ===== Screen surfaces ============================================== */

static bool json_floats(const cJSON* array, float* out, int count) {
	if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != count)
		return false;
	for (int i = 0; i < count; i++) {
		const cJSON* item = cJSON_GetArrayItem(array, i);
		if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble))
			return false;
		out[i] = (float)item->valuedouble;
	}
	return true;
}

static bool json_bool(const cJSON* object, const char* key) {
	return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, key));
}

static bool json_uint(const cJSON* object, const char* key, uint32_t limit, uint32_t* out) {
	const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
	if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0.0) || item->valuedouble > (double)limit ||
		item->valuedouble != floor(item->valuedouble))
		return false;
	*out = (uint32_t)item->valuedouble;
	return true;
}

static bool read_screen_rect(const cJSON* json, AeronGltfScreenRect* r) {
	memset(r, 0, sizeof *r);
	const cJSON* gain = cJSON_GetObjectItemCaseSensitive(json, "gain");
	if (!json_uint(json, "source", AERON_GLTF_SCREEN_MAX_SOURCES - 1u, &r->source) ||
		!json_uint(json, "layers", 0x7u, &r->layers) ||
		!json_floats(cJSON_GetObjectItemCaseSensitive(json, "dst"), r->dst, 4) ||
		!json_floats(cJSON_GetObjectItemCaseSensitive(json, "bg"), r->bg, 3) ||
		!json_floats(cJSON_GetObjectItemCaseSensitive(json, "highlight"), r->highlight, 3) ||
		!json_floats(cJSON_GetObjectItemCaseSensitive(json, "lock"), r->lock, 3) || !cJSON_IsNumber(gain) ||
		!isfinite(gain->valuedouble) || gain->valuedouble < 0.0)
		return false;
	r->gain = (float)gain->valuedouble;
	if ((r->layers & AERON_GLTF_SCREEN_LAYER_BAR) &&
		!json_floats(cJSON_GetObjectItemCaseSensitive(json, "bar"), r->bar, 3))
		return false;
	return true;
}

/* Reads a material's `aeronScreen` extras into `out` and the model's
 * rectangle array. A malformed screen is reported and dropped: the material
 * stays an ordinary one. Returns false only when memory runs out. */
static bool read_screen(const cgltf_material* m, AeronGltfModel* model, uint32_t* rect_capacity,
						AeronGltfMaterial* out, const char* source_label, uint32_t material_index) {
	out->screen_first = out->screen_count = out->screen_flags = 0;
	if (!m || !m->extras.data || !strstr(m->extras.data, "aeronScreen"))
		return true;
	cJSON*       root   = cJSON_ParseWithOpts(m->extras.data, NULL, true);
	const cJSON* screen = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "aeronScreen") : NULL;
	const cJSON* rects  = screen ? cJSON_GetObjectItemCaseSensitive(screen, "rects") : NULL;
	uint32_t     hide   = 0;
	const int    count  = cJSON_IsArray(rects) ? cJSON_GetArraySize(rects) : -1;
	if (!cJSON_IsObject(screen) || !json_uint(screen, "hide", UINT32_MAX, &hide) || count < 0 ||
		count > (int)AERON_GLTF_SCREEN_MAX_RECTS_PER_MATERIAL) {
		SDL_Log("[flight_gltf] '%s' material %u: malformed aeronScreen ignored", source_label,
				material_index);
		cJSON_Delete(root);
		return true;
	}
	if (model->screen_rect_count + (uint32_t)count > *rect_capacity) {
		uint32_t capacity = *rect_capacity ? *rect_capacity * 2u : 32u;
		while (capacity < model->screen_rect_count + (uint32_t)count)
			capacity *= 2u;
		AeronGltfScreenRect* grown =
			(AeronGltfScreenRect*)realloc(model->screen_rects, capacity * sizeof *model->screen_rects);
		if (!grown) {
			cJSON_Delete(root);
			return false;
		}
		model->screen_rects = grown;
		*rect_capacity      = capacity;
	}
	const uint32_t first = model->screen_rect_count;
	for (int i = 0; i < count; i++) {
		if (!read_screen_rect(cJSON_GetArrayItem(rects, i), &model->screen_rects[first + (uint32_t)i])) {
			SDL_Log("[flight_gltf] '%s' material %u: malformed aeronScreen rectangle %d ignored",
					source_label, material_index, i);
			cJSON_Delete(root);
			return true;
		}
	}
	model->screen_hide_mask |= hide;
	if (count > 0) {
		model->screen_rect_count += (uint32_t)count;
		out->screen_first = first;
		out->screen_count = (uint32_t)count;
		out->screen_flags = (json_bool(screen, "cover") ? AERON_GLTF_SCREEN_COVER : 0u) |
							(json_bool(screen, "transparent") ? AERON_GLTF_SCREEN_TRANSPARENT : 0u) |
							(json_bool(screen, "alwaysVisible") ? AERON_GLTF_SCREEN_ALWAYS_VISIBLE : 0u) |
							(json_bool(screen, "hologram") ? AERON_GLTF_SCREEN_HOLOGRAM : 0u) |
							(json_bool(screen, "noisy") ? AERON_GLTF_SCREEN_NOISY : 0u);
	}
	cJSON_Delete(root);
	return true;
}

/* ===== Material animation =========================================== */

/* Flipbook state names, indexed by AERON_ANIM_STATE_*. */
static const char* const kAnimStates[AERON_ANIM_STATE_ALWAYS + 1] = {
	"threat.laser",
	"threat.turret",
	"threat.beam",
	"threat.missile_locking",
	"threat.missile_locked",
	"cannon.1",
	"cannon.2",
	"cannon.3",
	"cannon.4",
	"cannon.5",
	"cannon.6",
	"cannon.7",
	"cannon.8",
	"broken.cmd",
	"broken.laser_ion",
	"broken.beam_weapon",
	"broken.shields",
	"broken.throttle",
	"broken.sensors",
	"broken.laser_recharge",
	"broken.engine_power",
	"broken.shield_recharge",
	"broken.beam_recharge",
	"hull.25",
	"hull.50",
	"hull.75",
	"laser_lock",
	"warhead.locking",
	"warhead.locked",
	"hangar",
	"target",
	"always",
};

static bool json_string_index(const cJSON* object, const char* key, const char* const* names, uint32_t count,
							  uint32_t* out) {
	const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
	for (uint32_t i = 0; cJSON_IsString(item) && i < count; i++) {
		if (strcmp(item->valuestring, names[i]) == 0) {
			*out = i;
			return true;
		}
	}
	return false;
}

typedef struct FlipbookCapacity {
	uint32_t flipbooks, frames, damages;
} FlipbookCapacity;

static bool grow(void** items, uint32_t* capacity, uint32_t needed, size_t size) {
	if (needed <= *capacity)
		return true;
	uint32_t cap = *capacity ? *capacity * 2u : 32u;
	while (cap < needed)
		cap *= 2u;
	void* grown = realloc(*items, (size_t)cap * size);
	if (!grown)
		return false;
	*items    = grown;
	*capacity = cap;
	return true;
}

/* One flipbook appended to the model; false when malformed. Frames are
 * kept only with the flipbook. */
static bool read_flipbook(const cJSON* json, AeronGltfModel* model, FlipbookCapacity* cap, bool* oom) {
	const cJSON*             frames   = cJSON_GetObjectItemCaseSensitive(json, "frames");
	const cJSON*             library  = cJSON_GetObjectItemCaseSensitive(json, "library");
	const cJSON*             uv       = cJSON_GetObjectItemCaseSensitive(json, "uv");
	const cJSON*             strength = cJSON_GetObjectItemCaseSensitive(json, "emissiveStrength");
	const cJSON*             unlit    = cJSON_GetObjectItemCaseSensitive(json, "unlit");
	static const char* const layers[] = { "colour", "illumination" };
	static const char* const modes[]  = { "loop", "once" };
	AeronGltfFlipbook        fb       = { 0 };
	const int                count    = cJSON_IsArray(frames) ? cJSON_GetArraySize(frames) : -1;
	if (!json_string_index(json, "state", kAnimStates, AERON_ANIM_STATE_ALWAYS + 1, &fb.state) ||
		!json_string_index(json, "layer", layers, 2, &fb.layer) ||
		!json_string_index(json, "mode", modes, 2, &fb.mode) || count < 0 || count > 0xFFFF ||
		(count > 0 && !cJSON_IsString(library)))
		return false;
	fb.emissive_strength = 1.0f;
	if (strength) {
		if (!cJSON_IsNumber(strength) || !isfinite(strength->valuedouble) || strength->valuedouble < 0.0)
			return false;
		fb.emissive_strength = (float)strength->valuedouble;
	}
	if (unlit) {
		if (!cJSON_IsNumber(unlit) || !(unlit->valuedouble >= 0.0 && unlit->valuedouble <= 1.0))
			return false;
		fb.unlit = (float)unlit->valuedouble;
	}
	if (uv) {
		float scale[2], offset[2];
		if (!json_floats(cJSON_GetObjectItemCaseSensitive(uv, "scale"), scale, 2) ||
			!json_floats(cJSON_GetObjectItemCaseSensitive(uv, "offset"), offset, 2))
			return false;
		fb.has_uv       = true;
		fb.uv_scale[0]  = scale[0];
		fb.uv_scale[1]  = scale[1];
		fb.uv_offset[0] = offset[0];
		fb.uv_offset[1] = offset[1];
		fb.uv_clamp     = json_bool(uv, "clamp");
	}
	for (int i = 0; i < count; i++) {
		const cJSON* frame   = cJSON_GetArrayItem(frames, i);
		const cJSON* key     = cJSON_GetArrayItem(frame, 0);
		const cJSON* seconds = cJSON_GetArrayItem(frame, 1);
		if (cJSON_GetArraySize(frame) != 2 || !cJSON_IsString(key) || !cJSON_IsNumber(seconds) ||
			!isfinite(seconds->valuedouble) || seconds->valuedouble < 0.0)
			return false;
	}
	if (!grow((void**)&model->flipbooks, &cap->flipbooks, model->flipbook_count + 1u,
			  sizeof *model->flipbooks) ||
		!grow((void**)&model->flipbook_frames, &cap->frames, model->flipbook_frame_count + (uint32_t)count,
			  sizeof *model->flipbook_frames)) {
		*oom = true;
		return false;
	}
	fb.frame_first = model->flipbook_frame_count;
	fb.library     = count > 0 ? copy_string(library->valuestring) : NULL;
	if (count > 0 && !fb.library) {
		*oom = true;
		return false;
	}
	for (int i = 0; i < count; i++) {
		const cJSON*            frame = cJSON_GetArrayItem(frames, i);
		AeronGltfFlipbookFrame* out   = &model->flipbook_frames[model->flipbook_frame_count];
		out->key                      = copy_string(cJSON_GetArrayItem(frame, 0)->valuestring);
		out->seconds                  = (float)cJSON_GetArrayItem(frame, 1)->valuedouble;
		if (!out->key) {
			*oom = true;
			free(fb.library);
			return false;
		}
		model->flipbook_frame_count++;
		fb.frame_count++;
	}
	model->flipbooks[model->flipbook_count++] = fb;
	return true;
}

static void free_damage(AeronGltfDamage* d) {
	free(d->library);
	for (int s = 0; s < 3; s++)
		free(d->stages[s]);
	memset(d, 0, sizeof *d);
}

/* One damage alternative; false when malformed (or *oom). */
static bool read_damage_alternative(const cJSON* json, const char* library, AeronGltfDamage* out, bool* oom) {
	memset(out, 0, sizeof *out);
	const cJSON* stages = cJSON_GetObjectItemCaseSensitive(json, "stages");
	const cJSON* scale  = cJSON_GetObjectItemCaseSensitive(json, "scale");
	const cJSON* offset = cJSON_GetObjectItemCaseSensitive(json, "offset");
	if (!cJSON_IsArray(stages) || cJSON_GetArraySize(stages) != 3)
		return false;
	out->library = copy_string(library);
	*oom         = !out->library;
	for (int s = 0; !*oom && s < 3; s++) {
		const cJSON* stage = cJSON_GetArrayItem(stages, s);
		if (cJSON_IsNull(stage))
			continue;
		if (!cJSON_IsString(stage))
			return false;
		out->stages[s] = copy_string(stage->valuestring);
		*oom           = !out->stages[s];
	}
	if (*oom)
		return false;
	if (scale) {
		if (!json_floats(scale, out->scale, 2))
			return false;
		out->has_scale = true;
	}
	if (cJSON_IsString(offset)) {
		if (strcmp(offset->valuestring, "inside") != 0)
			return false;
		out->offset_mode = AERON_ANIM_OFFSET_INSIDE;
	} else if (offset) {
		if (!json_floats(offset, out->offset, 4))
			return false;
		out->offset_mode = AERON_ANIM_OFFSET_RANGE;
	}
	return true;
}

/* A material's `aeronDamage`: appended to the model's alternatives. A
 * malformed one is reported and dropped. Returns false only on OOM. */
static bool read_damage(const cgltf_material* m, AeronGltfModel* model, FlipbookCapacity* cap,
						AeronGltfMaterial* out, const char* source_label, uint32_t material_index) {
	if (!m || !m->extras.data || !strstr(m->extras.data, "aeronDamage"))
		return true;
	cJSON*         root    = cJSON_ParseWithOpts(m->extras.data, NULL, true);
	const cJSON*   damage  = cJSON_GetObjectItemCaseSensitive(root, "aeronDamage");
	const cJSON*   library = cJSON_GetObjectItemCaseSensitive(damage, "library");
	const cJSON*   alts    = cJSON_GetObjectItemCaseSensitive(damage, "alternatives");
	const int      count   = cJSON_IsArray(alts) ? cJSON_GetArraySize(alts) : -1;
	uint32_t       choice = 0, placement = 0;
	bool           oom   = false;
	bool           valid = cJSON_IsString(library) && count > 0 && count <= 0xFFFF &&
						   json_uint(damage, "choice", 0xFFFu, &choice) &&
						   json_uint(damage, "placement", 0xFFFu, &placement);
	const uint32_t first = model->damage_count;
	if (valid &&
		!grow((void**)&model->damages, &cap->damages, first + (uint32_t)count, sizeof *model->damages)) {
		cJSON_Delete(root);
		return false;
	}
	for (int i = 0; valid && i < count; i++) {
		valid = read_damage_alternative(cJSON_GetArrayItem(alts, i), library->valuestring,
										&model->damages[model->damage_count], &oom);
		if (valid)
			model->damage_count++;
		else
			free_damage(&model->damages[model->damage_count]);
	}
	cJSON_Delete(root);
	if (!valid) {
		while (model->damage_count > first)
			free_damage(&model->damages[--model->damage_count]);
		if (oom)
			return false;
		SDL_Log("[flight_gltf] '%s' material %u: malformed aeronDamage ignored", source_label,
				material_index);
		return true;
	}
	out->damage_first     = first;
	out->damage_count     = (uint32_t)count;
	out->damage_choice    = choice;
	out->damage_placement = placement;
	return true;
}

/* A material's `aeronEnergy` into its energy mask; unknown names are
 * reported and ignored. */
static void read_energy(const cgltf_material* m, AeronGltfMaterial* out, const char* source_label,
						uint32_t material_index) {
	out->energy_mask = 0;
	if (!m || !m->extras.data || !strstr(m->extras.data, "aeronEnergy"))
		return;
	static const char* const names[] = { "shields", "tractor", "jamming" };
	cJSON*                   root    = cJSON_ParseWithOpts(m->extras.data, NULL, true);
	const cJSON*             energy  = cJSON_GetObjectItemCaseSensitive(root, "aeronEnergy");
	const cJSON*             item;
	cJSON_ArrayForEach(item, energy) {
		uint32_t e = 0;
		while (e < 3 && !(cJSON_IsString(item) && strcmp(item->valuestring, names[e]) == 0))
			e++;
		if (e < 3)
			out->energy_mask |= 1u << e;
		else
			SDL_Log("[flight_gltf] '%s' material %u: unknown aeronEnergy entry ignored", source_label,
					material_index);
	}
	cJSON_Delete(root);
}

/* Reads a material's `aeronFlipbooks` extras. A malformed entry is reported
 * and the material keeps no animation. Returns false only when memory runs
 * out. */
static bool read_animation(const cgltf_material* m, AeronGltfModel* model, FlipbookCapacity* cap,
						   AeronGltfMaterial* out, const char* source_label, uint32_t material_index) {
	if (!m || !m->extras.data || !strstr(m->extras.data, "aeronFlipbooks"))
		return true;
	cJSON*       root  = cJSON_ParseWithOpts(m->extras.data, NULL, true);
	const cJSON* books = cJSON_GetObjectItemCaseSensitive(root, "aeronFlipbooks");
	const int    count = cJSON_IsArray(books) ? cJSON_GetArraySize(books) : -1;
	bool         valid = count >= 0;
	const uint32_t first_book = model->flipbook_count, first_frame = model->flipbook_frame_count;
	bool           oom = false;
	for (int i = 0; valid && i < count; i++)
		valid = read_flipbook(cJSON_GetArrayItem(books, i), model, cap, &oom);
	cJSON_Delete(root);
	if (!valid) {
		for (uint32_t b = first_book; b < model->flipbook_count; b++)
			free(model->flipbooks[b].library);
		for (uint32_t f = first_frame; f < model->flipbook_frame_count; f++)
			free(model->flipbook_frames[f].key);
		model->flipbook_count       = first_book;
		model->flipbook_frame_count = first_frame;
		if (oom)
			return false;
		SDL_Log("[flight_gltf] '%s' material %u: malformed aeronFlipbooks ignored", source_label,
				material_index);
		return true;
	}
	out->flipbook_first = first_book;
	out->flipbook_count = model->flipbook_count - first_book;
	return true;
}

static AeronGltfAlphaMode material_alpha_mode(const AeronGltfModel* model, uint32_t material) {
	if (material == AERON_GLTF_NO_MATERIAL)
		return AERON_GLTF_ALPHA_OPAQUE;
	if (!model || material >= model->material_count)
		return (AeronGltfAlphaMode)-1;
	return model->materials[material].alpha_mode;
}

static bool material_is_screen(const AeronGltfModel* model, uint32_t material) {
	return material < model->material_count &&
		   (model->materials[material].screen_count != 0 || model->materials[material].screen_flags != 0);
}

/* A primitive goes to the screen tail of its range when any variant
 * gives it a screen material. */
static bool primitive_is_screen(const AeronGltfModel* model, uint32_t prim_id) {
	for (uint32_t variant = 0; variant < model->variant_columns; ++variant) {
		if (material_is_screen(
				model, model->prim_variant_material[(size_t)prim_id * model->variant_columns + variant]))
			return true;
	}
	return false;
}

/* ===== Vertex append ===============================================
 *
 * Append decoded vertices + indices for one glTF primitive into the
 * ship's merged buffers. Index values are biased to reference the
 * primitive's range relative to its global vertex_offset so the
 * renderer issues a single indexed draw across all primitives.
 *
 * Also bakes per-vertex `component` (= component ordinal) and
 * `prim_id` (= global prim slot). */
static bool append_primitive_vertices(const cgltf_primitive* p, const cgltf_node* node,
									  uint16_t component_index, uint32_t prim_id, AeronGltfVertex* verts,
									  uint32_t* indices, uint32_t* voff_io, uint32_t* ioff_io) {
	const cgltf_accessor* pos = prim_attr(p, cgltf_attribute_type_position);
	const cgltf_accessor* nrm = prim_attr(p, cgltf_attribute_type_normal);
	const cgltf_accessor* uv  = prim_attr(p, cgltf_attribute_type_texcoord);
	const cgltf_accessor* tan = prim_attr(p, cgltf_attribute_type_tangent);
	const cgltf_accessor* idx = p->indices;
	if (!pos || p->type != cgltf_primitive_type_triangles)
		return false;

	uint32_t vcount = (uint32_t)pos->count;
	uint32_t icount = idx ? (uint32_t)idx->count : vcount;
	uint32_t voff   = *voff_io;
	uint32_t ioff   = *ioff_io;

	float* positions = (float*)malloc((size_t)vcount * 3 * sizeof(float));
	float* normals   = (float*)calloc(vcount, 3 * sizeof(float));
	float* uvs       = (float*)calloc(vcount, 2 * sizeof(float));
	float* tangents  = (float*)calloc(vcount, 4 * sizeof(float));
	if (!positions || !normals || !uvs || !tangents) {
		free(positions);
		free(normals);
		free(uvs);
		free(tangents);
		return false;
	}
	decode_vec3(pos, positions, vcount);
	if (nrm)
		decode_vec3(nrm, normals, vcount);
	else
		for (uint32_t i = 0; i < vcount; i++)
			normals[i * 3 + 2] = 1.0f;
	if (uv)
		decode_vec2(uv, uvs, vcount);
	if (tan)
		decode_vec4(tan, tangents, vcount);
	else
		for (uint32_t i = 0; i < vcount; i++)
			tangents[i * 4 + 3] = 1.0f;

	float matrix[16];
	cgltf_node_transform_world(node, matrix);
	for (uint32_t i = 0; i < vcount; i++) {
		AeronGltfVertex* v = &verts[voff + i];
		float            transformed[3];
		transform_point(matrix, &positions[i * 3], transformed);
		gltf_to_aeron3(transformed, v->pos);
		transform_direction(matrix, &normals[i * 3], transformed);
		gltf_to_aeron3(transformed, v->normal);
		transform_direction(matrix, &tangents[i * 4], transformed);
		const float tangent[4] = { transformed[0], transformed[1], transformed[2], tangents[i * 4 + 3] };
		gltf_to_aeron_tangent(tangent, v->tangent);
		v->uv[0]      = uvs[i * 2 + 0];
		v->uv[1]      = uvs[i * 2 + 1];
		v->component  = (float)component_index;
		v->prim_id    = prim_id;
	}
	free(positions);
	free(normals);
	free(uvs);
	free(tangents);

	/* Indices — biased to the primitive's range inside the ship's
	 * merged buffer. */
	uint32_t* dst_idx = indices + ioff;
	for (uint32_t i = 0; i < icount; i++) {
		cgltf_size raw = idx ? cgltf_accessor_read_index(idx, i) : i;
		dst_idx[i]     = (uint32_t)raw + voff;
	}

	*voff_io = voff + vcount;
	*ioff_io = ioff + icount;
	return true;
}

/* ===== Public API =================================================== */

bool Aeron_GltfMeshBuildData(const cgltf_data* data, const char* source_label, AeronGltfModel* out) {
	if (!data || !out)
		return false;
	if (!source_label)
		source_label = "<memory glTF>";
	memset(out, 0, sizeof *out);

	bool succeeded = false;

	typedef struct NodePlan {
		const cgltf_node* node;
		uint16_t          opt_mi;
	} NodePlan;

	NodePlan* plans = NULL;

	/* ---- Channel KTX2 atlases ------------------------------------- */
	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
		if (!copy_channel_ktx2(data, kAtlasNames[c], &out->channels[c]))
			goto cleanup;
	}

	/* ---- Variants ------------------------------------------------- */
	if (!read_variants(data, source_label, out))
		goto cleanup;

	/* ---- Materials (factors + per-channel UV transform) ----------- */
	if (data->materials_count > AERON_GLTF_MAX_MATERIALS) {
		SDL_Log("[flight_gltf] '%s' has %zu materials (over cap %u)", source_label, data->materials_count,
				(unsigned)AERON_GLTF_MAX_MATERIALS);
		goto cleanup;
	}
	if (data->materials_count > 0) {
		out->materials = (AeronGltfMaterial*)calloc(data->materials_count, sizeof *out->materials);
		if (!out->materials)
			goto cleanup;
		out->material_count = (uint32_t)data->materials_count;
		uint32_t         screen_rect_capacity = 0;
		FlipbookCapacity flipbook_capacity    = { 0 };
		for (uint32_t i = 0; i < out->material_count; i++) {
			if (!read_material(&data->materials[i], &out->materials[i])) {
				SDL_Log("[flight_gltf] '%s' material %u has invalid alpha state", source_label, i);
				goto cleanup;
			}
			if (!read_screen(&data->materials[i], out, &screen_rect_capacity, &out->materials[i],
							 source_label, i) ||
				!read_animation(&data->materials[i], out, &flipbook_capacity, &out->materials[i],
								source_label, i) ||
				!read_damage(&data->materials[i], out, &flipbook_capacity, &out->materials[i], source_label,
							 i))
				goto cleanup;
			read_energy(&data->materials[i], &out->materials[i], source_label, i);
			/* Holograms and transparent screens blend over the scene, and
			 * the PBR shader leaves them transparent when hidden: only the
			 * blend range can draw them. */
			AeronGltfMaterial* material = &out->materials[i];
			if ((material->screen_flags & (AERON_GLTF_SCREEN_HOLOGRAM | AERON_GLTF_SCREEN_TRANSPARENT)) != 0 &&
				material->alpha_mode != AERON_GLTF_ALPHA_BLEND) {
				SDL_Log("[flight_gltf] '%s' material %u: hologram or transparent screen drawn as BLEND",
						source_label, i);
				material->alpha_mode = AERON_GLTF_ALPHA_BLEND;
			}
		}
	}

	/* ---- Pass 1: count merged vertex / index / primitive totals and
	 * record component ordinals. */
	uint32_t total_v = 0, total_i = 0, total_prims = 0;
	if (!data->scene || data->scene->nodes_count != 1)
		goto cleanup;
	const cgltf_node* root = data->scene->nodes[0];
	plans = (NodePlan*)calloc(root->children_count, sizeof *plans);
	if (!plans)
		goto cleanup;
	uint32_t plan_count = 0;

	for (cgltf_size i = 0; i < root->children_count; i++) {
		const cgltf_node* n               = root->children[i];
		const uint16_t    component_index = (uint16_t)i;
		if (!n->mesh || i >= AERON_MAX_MESH_COMPONENTS)
			goto cleanup;

		NodePlan* plan = &plans[plan_count++];
		plan->node     = n;
		plan->opt_mi   = component_index;

		for (cgltf_size pi = 0; pi < n->mesh->primitives_count; pi++) {
			const cgltf_primitive* p   = &n->mesh->primitives[pi];
			const cgltf_accessor*  pos = prim_attr(p, cgltf_attribute_type_position);
			const cgltf_accessor*  idx = p->indices;
			if (!pos || p->type != cgltf_primitive_type_triangles)
				goto cleanup;
			const cgltf_size prim_i = idx ? idx->count : pos->count;
			if (pos->count > UINT32_MAX - total_v || prim_i > UINT32_MAX - total_i) {
				SDL_Log("[flight_gltf] '%s' merged geometry exceeds 32-bit counts", source_label);
				goto cleanup;
			}
			total_v += (uint32_t)pos->count;
			total_i += (uint32_t)prim_i;
			total_prims += 1u;
		}
	}

	if (total_v > 0 && total_i > 0) {
		out->vertices = (AeronGltfVertex*)calloc(total_v, sizeof *out->vertices);
		out->indices  = (uint32_t*)calloc(total_i, sizeof *out->indices);
		if (!out->vertices || !out->indices)
			goto cleanup;
		out->vertex_count = total_v;
		out->index_count  = total_i;
	}
	out->total_prim_count = total_prims;
	if (total_prims > 0) {
		out->prim_variant_material =
			(uint32_t*)malloc((size_t)total_prims * out->variant_columns * sizeof(uint32_t));
		if (!out->prim_variant_material)
			goto cleanup;
		for (size_t k = 0; k < (size_t)total_prims * out->variant_columns; k++)
			out->prim_variant_material[k] = AERON_GLTF_NO_MATERIAL;
	}

	/* ---- Pass 2: decode + interleave + populate variant table. ---- */
	uint32_t voff = 0, ioff = 0, prim_id = 0;
	for (uint32_t pi = 0; pi < plan_count; pi++) {
		const NodePlan*   plan = &plans[pi];
		const cgltf_mesh* mesh = plan->node->mesh;
		for (cgltf_size si = 0; si < mesh->primitives_count; si++) {
			const cgltf_primitive* p   = &mesh->primitives[si];
			const cgltf_accessor*  pos = prim_attr(p, cgltf_attribute_type_position);
			if (!pos)
				goto cleanup;
			if (!append_primitive_vertices(p, plan->node, plan->opt_mi, prim_id, out->vertices, out->indices,
										   &voff, &ioff))
				goto cleanup;
			uint32_t default_mat =
				p->material ? (uint32_t)cgltf_material_index(data, p->material) : AERON_GLTF_NO_MATERIAL;
			for (uint32_t vs = 0; vs < out->variant_columns; vs++)
				out->prim_variant_material[(size_t)prim_id * out->variant_columns + vs] = default_mat;
			for (cgltf_size mi = 0; mi < p->mappings_count; mi++) {
				const cgltf_material_mapping* mp = &p->mappings[mi];
				if (mp->variant < out->variant_columns) {
					uint32_t mat = mp->material ? (uint32_t)cgltf_material_index(data, mp->material)
												: AERON_GLTF_NO_MATERIAL;
					out->prim_variant_material[(size_t)prim_id * out->variant_columns + mp->variant] = mat;
				}
			}
			prim_id++;
		}
	}

	/* A fixed index partition requires every runtime material variant of a
	 * primitive to remain in the same render class. */
	for (uint32_t pid = 0; pid < out->total_prim_count; ++pid) {
		const uint32_t default_material = out->prim_variant_material[(size_t)pid * out->variant_columns];
		const AeronGltfAlphaMode default_mode = material_alpha_mode(out, default_material);
		if ((int)default_mode < 0) {
			SDL_Log("[flight_gltf] '%s' primitive %u references invalid material %u", source_label, pid,
					default_material);
			goto cleanup;
		}
		for (uint32_t variant = 1; variant < out->variant_columns; ++variant) {
			const uint32_t material =
				out->prim_variant_material[(size_t)pid * out->variant_columns + variant];
			const AeronGltfAlphaMode mode = material_alpha_mode(out, material);
			if ((int)mode < 0 || mode != default_mode) {
				SDL_Log("[flight_gltf] '%s' primitive %u variant %u changes alpha class "
						"(material %u mode %d -> material %u mode %d)",
						source_label, pid, variant, default_material, (int)default_mode, material, (int)mode);
				goto cleanup;
			}
		}
	}

	/* ---- Partition indices: OPAQUE (screens last), MASK, BLEND (screens
	 * last), stable within each class. Primitive alpha class is invariant
	 * across variants above. ---- */
	out->opaque_index_count        = 0;
	out->mask_index_offset         = 0;
	out->mask_index_count          = 0;
	out->blend_index_offset        = 0;
	out->blend_index_count         = 0;
	out->opaque_screen_index_count = 0;
	out->blend_screen_index_count  = 0;
	if (out->index_count > 0) {
		uint32_t* sorted = (uint32_t*)malloc((size_t)out->index_count * sizeof *sorted);
		uint8_t*  prim_screen =
			(uint8_t*)malloc(out->total_prim_count ? out->total_prim_count : 1u);
		if (!sorted || !prim_screen) {
			free(sorted);
			free(prim_screen);
			goto cleanup;
		}
		for (uint32_t pid = 0; pid < out->total_prim_count; ++pid)
			prim_screen[pid] = primitive_is_screen(out, pid);
		/* screen: 0 = no screen material, 1 = screen, 2 = either. */
		static const struct {
			AeronGltfAlphaMode mode;
			int                screen;
		} classes[] = {
			{ AERON_GLTF_ALPHA_OPAQUE, 0 }, { AERON_GLTF_ALPHA_OPAQUE, 1 }, { AERON_GLTF_ALPHA_MASK, 2 },
			{ AERON_GLTF_ALPHA_BLEND, 0 },  { AERON_GLTF_ALPHA_BLEND, 1 },
		};
		enum { CLASS_COUNT = sizeof classes / sizeof classes[0] };
		uint32_t w = 0;
		uint32_t starts[CLASS_COUNT + 1];
		for (int c = 0; c < CLASS_COUNT; ++c) {
			starts[c] = w;
			for (uint32_t t = 0; t + 2 < out->index_count; t += 3) {
				const uint32_t pid = out->vertices[out->indices[t]].prim_id;
				const uint32_t mat = out->prim_variant_material[(size_t)pid * out->variant_columns];
				if (material_alpha_mode(out, mat) != classes[c].mode ||
					(classes[c].screen != 2 && prim_screen[pid] != classes[c].screen))
					continue;
				sorted[w + 0] = out->indices[t + 0];
				sorted[w + 1] = out->indices[t + 1];
				sorted[w + 2] = out->indices[t + 2];
				w += 3;
			}
		}
		free(prim_screen);
		starts[CLASS_COUNT]            = w;
		out->opaque_index_count        = starts[2];
		out->opaque_screen_index_count = starts[2] - starts[1];
		out->mask_index_offset         = starts[2];
		out->mask_index_count          = starts[3] - starts[2];
		out->blend_index_offset        = starts[3];
		out->blend_index_count         = starts[5] - starts[3];
		out->blend_screen_index_count  = starts[5] - starts[4];
		if (w != out->index_count) {
			free(sorted);
			SDL_Log("[flight_gltf] '%s' index partition retained %u of %u indices", source_label, w,
					out->index_count);
			goto cleanup;
		}
		memcpy(out->indices, sorted, (size_t)out->index_count * sizeof *sorted);
		free(sorted);
	}

	SDL_Log("[flight_gltf] %s: %u verts, %u indices (%u opaque, %u mask, "
			"%u blend; %u + %u screen), %u prims, "
			"%u materials, %u variants, atlases base=%zu nrm=%zu mr=%zu "
			"emis=%zu",
			source_label, out->vertex_count, out->index_count, out->opaque_index_count, out->mask_index_count,
			out->blend_index_count, out->opaque_screen_index_count, out->blend_screen_index_count,
			out->total_prim_count, out->material_count, out->variant_count,
			out->channels[0].size, out->channels[1].size, out->channels[2].size, out->channels[3].size);
	if (out->group_count > 0)
		SDL_Log("[flight_gltf] %s: %u asset variants, %u skin markings, %u variant groups", source_label,
				out->asset_variant_count, out->skin_markings_count, out->group_count);
	if (out->screen_rect_count > 0 || out->screen_hide_mask != 0)
		SDL_Log("[flight_gltf] %s: %u screen rectangles, hide mask 0x%x", source_label,
				out->screen_rect_count, out->screen_hide_mask);

	succeeded = true;

cleanup:
	free(plans);
	if (!succeeded)
		Aeron_GltfMeshFree(out);
	return succeeded;
}

void Aeron_GltfMeshFree(AeronGltfModel* m) {
	if (!m)
		return;
	free(m->vertices);
	free(m->indices);
	free(m->materials);
	free(m->prim_variant_material);
	free(m->screen_rects);
	for (uint32_t b = 0; m->flipbooks && b < m->flipbook_count; b++)
		free(m->flipbooks[b].library);
	free(m->flipbooks);
	for (uint32_t f = 0; m->flipbook_frames && f < m->flipbook_frame_count; f++)
		free(m->flipbook_frames[f].key);
	free(m->flipbook_frames);
	for (uint32_t d = 0; m->damages && d < m->damage_count; d++)
		free_damage(&m->damages[d]);
	free(m->damages);
	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++)
		free(m->channels[c].data);
	for (uint32_t v = 0; m->variant_names && v < m->variant_count; v++)
		free(m->variant_names[v]);
	free(m->variant_names);
	free(m->skin_markings);
	for (uint32_t g = 0; m->groups && g < m->group_count; g++) {
		free(m->groups[g].ktx2.data);
		free(m->groups[g].variants);
	}
	free(m->groups);
	memset(m, 0, sizeof *m);
}
