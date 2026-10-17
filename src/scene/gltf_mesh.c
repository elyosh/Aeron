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

#include "aeron/asset/flight_model.h"

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

/* ===== Extension objects ===========================================
 *
 * Returns the parsed extension `name` of an object (owned by the caller),
 * or NULL when the object does not have it. See
 * tools/extensions/AERON_flight_model, and AERON_flight_model_cooked for
 * cooked textures. */
static cJSON* extension_object(const cgltf_extension* extensions, cgltf_size count, const char* name) {
	for (cgltf_size e = 0; e < count; e++)
		if (extensions[e].name && extensions[e].data && strcmp(extensions[e].name, name) == 0)
			return cJSON_Parse(extensions[e].data);
	return NULL;
}

static cJSON* flight_model_object(const cgltf_extension* extensions, cgltf_size count) {
	return extension_object(extensions, count, AERON_FLIGHT_MODEL_EXTENSION);
}

/* ===== Variants and the markings list ===============================
 *
 * The model root's AERON_flight_model `markings`: [variant name or null,
 * ...], at most 256 entries. */
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

/* Reads the model root's markings list into `out->markings`. */
static bool read_markings(const cgltf_data* data, const char* source_label, AeronGltfModel* out) {
	const cgltf_node* root = data->scene && data->scene->nodes_count ? data->scene->nodes[0] : NULL;
	const char*       json = NULL;
	for (cgltf_size e = 0; root && e < root->extensions_count; e++)
		if (root->extensions[e].name && root->extensions[e].data &&
			strcmp(root->extensions[e].name, AERON_FLIGHT_MODEL_EXTENSION) == 0)
			json = root->extensions[e].data;
	cJSON* extension = json ? cJSON_Parse(json) : NULL;
	const cJSON* markings = cJSON_GetObjectItemCaseSensitive(extension, "markings");
	bool ok = !markings || (cJSON_IsArray(markings) && cJSON_GetArraySize(markings) <= 256);
	const uint32_t count = ok && markings ? (uint32_t)cJSON_GetArraySize(markings) : 0u;
	if (ok && count) {
		out->markings = (uint32_t*)malloc(count * sizeof *out->markings);
		ok            = out->markings != NULL;
		const cJSON* entry = NULL;
		cJSON_ArrayForEach(entry, markings) {
			if (!ok)
				break;
			uint32_t variant = AERON_GLTF_NO_VARIANT;
			if (cJSON_IsString(entry)) {
				variant = variant_by_name(out, entry->valuestring);
				if (variant == AERON_GLTF_NO_VARIANT) {
					SDL_Log("[flight_gltf] '%s': markings value %u names no variant '%s'", source_label,
							out->markings_count, entry->valuestring);
					ok = false;
				}
			} else {
				ok = cJSON_IsNull(entry);
			}
			out->markings[out->markings_count++] = variant;
		}
	}
	cJSON_Delete(extension);
	if (!ok)
		SDL_Log("[flight_gltf] '%s' has an invalid markings list", source_label);
	return ok;
}

static bool read_variants(const cgltf_data* data, const char* source_label, AeronGltfModel* out) {
	out->variant_count   = (uint32_t)data->variants_count;
	out->variant_columns = out->variant_count + 1u;
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
	return read_markings(data, source_label, out);
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

/* ===== Atlas images ==================================================
 *
 * A cooked texture's AERON_flight_model_cooked object names its image (a
 * KTX2 array of one channel's atlas pages) and its page in that image. An
 * image's channel is the material slot that references it. A channel's core image is
 * the one the primitives' own materials reference or, in a channel no own
 * material uses, its only image. The channel's other images are variant
 * groups (AeronGltfVariantGroup), of the variants whose materials reference
 * them, in image order. */
typedef struct AtlasTexture {
	uint32_t image;
	uint32_t layer;
	uint32_t max_lod;
} AtlasTexture;

/* Reads the image, page and mip limit of `texture`. Returns false when it
 * has no valid AERON_flight_model_cooked object. */
static bool read_atlas_texture(const cgltf_data* data, const cgltf_texture* texture, AtlasTexture* out) {
	cJSON*       object  = texture ? extension_object(texture->extensions, texture->extensions_count,
													  AERON_FLIGHT_MODEL_COOKED_EXTENSION)
								   : NULL;
	const cJSON* source  = cJSON_GetObjectItemCaseSensitive(object, "source");
	const cJSON* layer   = cJSON_GetObjectItemCaseSensitive(object, "layer");
	const cJSON* max_lod = cJSON_GetObjectItemCaseSensitive(object, "maxLod");
	const bool   ok =
		cJSON_IsNumber(source) && source->valuedouble >= 0.0 && source->valuedouble < (double)data->images_count &&
		source->valuedouble == floor(source->valuedouble) && cJSON_IsNumber(layer) && layer->valuedouble >= 0.0 &&
		layer->valuedouble <= AERON_GLTF_ATLAS_MAX_PAGE && layer->valuedouble == floor(layer->valuedouble) &&
		(!max_lod || (cJSON_IsNumber(max_lod) && max_lod->valuedouble >= 0.0 &&
					  max_lod->valuedouble < AERON_GLTF_ATLAS_UNCLAMPED_LOD &&
					  max_lod->valuedouble == floor(max_lod->valuedouble)));
	if (ok) {
		out->image   = (uint32_t)source->valuedouble;
		out->layer   = (uint32_t)layer->valuedouble;
		out->max_lod = max_lod ? (uint32_t)max_lod->valuedouble : AERON_GLTF_ATLAS_UNCLAMPED_LOD;
	}
	cJSON_Delete(object);
	return ok;
}

/* Copies image `image`'s KTX2 bytes into `out`, with its layer count. */
static bool copy_image_ktx2(const cgltf_image* image, AeronGltfChannelKtx2* out, uint32_t* layers) {
	static const uint8_t kIdentifier[12] = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n' };
	const cgltf_buffer_view* view = image->buffer_view;
	if (!view || !view->buffer || !view->buffer->data || view->size < 48)
		return false;
	const uint8_t* src = (const uint8_t*)view->buffer->data + view->offset;
	if (memcmp(src, kIdentifier, sizeof kIdentifier) != 0)
		return false;
	uint32_t count;
	memcpy(&count, src + 32, sizeof count); /* layerCount, little-endian */
	out->data = (uint8_t*)malloc(view->size);
	if (!out->data)
		return false;
	memcpy(out->data, src, view->size);
	out->size = view->size;
	*layers   = count ? count : 1u;
	return true;
}

/* Per image: its channel (-1 when no material references it) and the first
 * page it takes among its channel's (core, then groups). */
typedef struct AtlasImages {
	int32_t*  channel;
	uint32_t* page_base;
} AtlasImages;

static void atlas_images_free(AtlasImages* atlas) {
	free(atlas->channel);
	free(atlas->page_base);
	memset(atlas, 0, sizeof *atlas);
}

/* Marks the images material `m` references, for its own primitives
 * (`own`) or variant `variant` (`uses`, images × variants). */
static bool mark_atlas_images(const cgltf_data* data, const cgltf_material* m, AtlasImages* atlas, bool* own,
							  uint8_t* uses, size_t variant, bool is_own, const char* source_label) {
	for (int c = 0; m && c < AERON_GLTF_CHANNEL_COUNT; c++) {
		const cgltf_texture_view* tv = material_channel_view(m, c);
		AtlasTexture              texture;
		if (!tv || !tv->texture)
			continue;
		if (!read_atlas_texture(data, tv->texture, &texture)) {
			SDL_Log("[flight_gltf] '%s': a texture has no AERON_flight_model_cooked image and page (not a cooked model)",
					source_label);
			return false;
		}
		if (atlas->channel[texture.image] >= 0 && atlas->channel[texture.image] != c) {
			SDL_Log("[flight_gltf] '%s': image %u serves two channels", source_label, texture.image);
			return false;
		}
		atlas->channel[texture.image] = c;
		if (is_own)
			own[texture.image] = true;
		else
			uses[(size_t)texture.image * data->variants_count + variant] = 1;
	}
	return true;
}

/* Fills the model's core images and variant groups, and `atlas`, from the
 * images the materials reference. */
static bool read_atlases(const cgltf_data* data, const char* source_label, AeronGltfModel* out,
						 AtlasImages* atlas) {
	const size_t images   = data->images_count ? data->images_count : 1;
	const size_t variants = data->variants_count;
	atlas->channel        = (int32_t*)malloc(images * sizeof *atlas->channel);
	atlas->page_base      = (uint32_t*)calloc(images, sizeof *atlas->page_base);
	bool*    own          = (bool*)calloc(images, sizeof *own);
	uint8_t* uses         = (uint8_t*)calloc(images * (variants ? variants : 1), 1);
	bool     ok           = atlas->channel && atlas->page_base && own && uses;
	for (size_t i = 0; ok && i < data->images_count; i++)
		atlas->channel[i] = -1;
	for (size_t mi = 0; ok && mi < data->meshes_count; mi++) {
		for (size_t pi = 0; ok && pi < data->meshes[mi].primitives_count; pi++) {
			const cgltf_primitive* p = &data->meshes[mi].primitives[pi];
			ok = mark_atlas_images(data, p->material, atlas, own, uses, 0, true, source_label);
			for (size_t k = 0; ok && k < p->mappings_count; k++)
				ok = p->mappings[k].variant >= variants ||
					 mark_atlas_images(data, p->mappings[k].material, atlas, own, uses, p->mappings[k].variant,
									   false, source_label);
		}
	}
	bool own_channel[AERON_GLTF_CHANNEL_COUNT] = { false };
	for (size_t i = 0; ok && i < data->images_count; i++)
		if (own[i])
			own_channel[atlas->channel[i]] = true;
	/* Core images. */
	uint32_t core_layers[AERON_GLTF_CHANNEL_COUNT] = { 0 };
	for (size_t i = 0; ok && i < data->images_count; i++) {
		const int32_t c = atlas->channel[i];
		if (c < 0 || (!own[i] && own_channel[c]))
			continue;
		if (out->channels[c].data) {
			SDL_Log("[flight_gltf] '%s': channel %d has two core images", source_label, c);
			ok = false;
		} else if (!copy_image_ktx2(&data->images[i], &out->channels[c], &core_layers[c])) {
			SDL_Log("[flight_gltf] '%s': image %zu is not a KTX2 in a buffer view", source_label, i);
			ok = false;
		}
	}
	/* Variant groups, each image's pages after the core's and the earlier
	 * groups' of its channel. */
	uint32_t next_page[AERON_GLTF_CHANNEL_COUNT];
	memcpy(next_page, core_layers, sizeof next_page);
	for (size_t i = 0; ok && i < data->images_count; i++) {
		const int32_t c = atlas->channel[i];
		if (c < 0 || own[i] || !own_channel[c])
			continue;
		AeronGltfVariantGroup* grown =
			(AeronGltfVariantGroup*)realloc(out->groups, (out->group_count + 1u) * sizeof *grown);
		if (!grown) {
			ok = false;
			break;
		}
		out->groups              = grown;
		AeronGltfVariantGroup* g = &out->groups[out->group_count];
		memset(g, 0, sizeof *g);
		g->channel = (uint32_t)c;
		g->variants = (uint32_t*)malloc((variants ? variants : 1) * sizeof *g->variants);
		ok          = g->variants && copy_image_ktx2(&data->images[i], &g->ktx2, &g->layers);
		out->group_count++;
		for (size_t v = 0; ok && v < variants; v++)
			if (uses[i * variants + v])
				g->variants[g->variant_count++] = (uint32_t)v;
		atlas->page_base[i] = next_page[c];
		next_page[c] += g->layers;
		if (!ok)
			SDL_Log("[flight_gltf] '%s': image %zu is not a KTX2 in a buffer view", source_label, i);
	}
	free(own);
	free(uses);
	return ok;
}

static bool read_material(const cgltf_data* data, const AtlasImages* atlas, const cgltf_material* m,
						  AeronGltfMaterial* out) {
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
		AtlasTexture texture;
		if (!read_atlas_texture(data, tv->texture, &texture))
			return false;
		/* An image no primitive's material references is not loaded: a
		 * material no primitive uses samples nothing. */
		if (atlas->channel[texture.image] < 0) {
			memset(out->uv_xform[c], 0, sizeof out->uv_xform[c]);
			continue;
		}
		out->atlas_page[c]    = atlas->page_base[texture.image] + texture.layer;
		out->atlas_max_lod[c] = texture.max_lod;
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

/* Reads a material's `screen` into `out` and the model's rectangle array. A
 * malformed screen is reported and dropped: the material stays an ordinary
 * one. Returns false only when memory runs out. */
static bool read_screen(const cJSON* screen, AeronGltfModel* model, uint32_t* rect_capacity,
						AeronGltfMaterial* out, const char* source_label, uint32_t material_index) {
	out->screen_first = out->screen_count = out->screen_flags = 0;
	if (!screen)
		return true;
	const cJSON* rects = cJSON_GetObjectItemCaseSensitive(screen, "rects");
	uint32_t     hide   = 0;
	const int    count  = cJSON_IsArray(rects) ? cJSON_GetArraySize(rects) : -1;
	if (!cJSON_IsObject(screen) || !json_uint(screen, "hide", UINT32_MAX, &hide) || count < 0 ||
		count > (int)AERON_GLTF_SCREEN_MAX_RECTS_PER_MATERIAL) {
		SDL_Log("[flight_gltf] '%s' material %u: malformed screen ignored", source_label, material_index);
		return true;
	}
	if (model->screen_rect_count + (uint32_t)count > *rect_capacity) {
		uint32_t capacity = *rect_capacity ? *rect_capacity * 2u : 32u;
		while (capacity < model->screen_rect_count + (uint32_t)count)
			capacity *= 2u;
		AeronGltfScreenRect* grown =
			(AeronGltfScreenRect*)realloc(model->screen_rects, capacity * sizeof *model->screen_rects);
		if (!grown)
			return false;
		model->screen_rects = grown;
		*rect_capacity      = capacity;
	}
	const uint32_t first = model->screen_rect_count;
	for (int i = 0; i < count; i++) {
		if (!read_screen_rect(cJSON_GetArrayItem(rects, i), &model->screen_rects[first + (uint32_t)i])) {
			SDL_Log("[flight_gltf] '%s' material %u: malformed screen rectangle %d ignored", source_label,
					material_index, i);
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
	const cJSON*             strength = cJSON_GetObjectItemCaseSensitive(json, "emissiveStrength");
	const cJSON*             unlit    = cJSON_GetObjectItemCaseSensitive(json, "unlit");
	static const char* const layers[] = { "baseColor", "emissive" }; /* AERON_ANIM_LAYER_* */
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
		if (!cJSON_IsObject(offset) || !json_floats(cJSON_GetObjectItemCaseSensitive(offset, "min"), out->offset, 2) ||
			!json_floats(cJSON_GetObjectItemCaseSensitive(offset, "max"), out->offset + 2, 2))
			return false;
		out->offset_mode = AERON_ANIM_OFFSET_RANGE;
	}
	return true;
}

/* A material's `damage`: appended to the model's alternatives. A malformed
 * one is reported and dropped. Returns false only on OOM. */
static bool read_damage(const cJSON* damage, AeronGltfModel* model, FlipbookCapacity* cap,
						AeronGltfMaterial* out, const char* source_label, uint32_t material_index) {
	if (!damage)
		return true;
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
		!grow((void**)&model->damages, &cap->damages, first + (uint32_t)count, sizeof *model->damages))
		return false;
	for (int i = 0; valid && i < count; i++) {
		valid = read_damage_alternative(cJSON_GetArrayItem(alts, i), library->valuestring,
										&model->damages[model->damage_count], &oom);
		if (valid)
			model->damage_count++;
		else
			free_damage(&model->damages[model->damage_count]);
	}
	if (!valid) {
		while (model->damage_count > first)
			free_damage(&model->damages[--model->damage_count]);
		if (oom)
			return false;
		SDL_Log("[flight_gltf] '%s' material %u: malformed damage ignored", source_label, material_index);
		return true;
	}
	out->damage_first     = first;
	out->damage_count     = (uint32_t)count;
	out->damage_choice    = choice;
	out->damage_placement = placement;
	return true;
}

/* The AERON_EFFECT_ENERGY_* bits of an `energy` list; unknown names are
 * reported and ignored. */
static uint32_t energy_mask(const cJSON* energy, const char* source_label, const char* owner) {
	static const char* const names[] = { "shields", "tractor", "jamming" };
	uint32_t                 mask    = 0;
	const cJSON*             item;
	cJSON_ArrayForEach(item, energy) {
		uint32_t e = 0;
		while (e < 3 && !(cJSON_IsString(item) && strcmp(item->valuestring, names[e]) == 0))
			e++;
		if (e < 3)
			mask |= 1u << e;
		else
			SDL_Log("[flight_gltf] '%s' %s: unknown energy entry ignored", source_label, owner);
	}
	return mask;
}

/* The model root's `energy`, the default of materials without their own:
 * its bits in *mask, false when the model gives none. */
static bool read_energy_default(const cgltf_data* data, const char* source_label, uint32_t* mask) {
	const cgltf_node* root   = data->scene && data->scene->nodes_count ? data->scene->nodes[0] : NULL;
	cJSON*            object = root ? flight_model_object(root->extensions, root->extensions_count) : NULL;
	const cJSON*      energy = cJSON_GetObjectItemCaseSensitive(object, "energy");
	*mask                    = energy ? energy_mask(energy, source_label, "model root") : 0u;
	cJSON_Delete(object);
	return energy != NULL;
}

/* Reads a material's `flipbooks`. A malformed entry is reported and the
 * material keeps no animation. Returns false only when memory runs out. */
static bool read_animation(const cJSON* books, AeronGltfModel* model, FlipbookCapacity* cap,
						   AeronGltfMaterial* out, const char* source_label, uint32_t material_index) {
	if (!books)
		return true;
	const int    count = cJSON_IsArray(books) ? cJSON_GetArraySize(books) : -1;
	bool         valid = count >= 0;
	const uint32_t first_book = model->flipbook_count, first_frame = model->flipbook_frame_count;
	bool           oom = false;
	for (int i = 0; valid && i < count; i++)
		valid = read_flipbook(cJSON_GetArrayItem(books, i), model, cap, &oom);
	if (!valid) {
		for (uint32_t b = first_book; b < model->flipbook_count; b++)
			free(model->flipbooks[b].library);
		for (uint32_t f = first_frame; f < model->flipbook_frame_count; f++)
			free(model->flipbook_frames[f].key);
		model->flipbook_count       = first_book;
		model->flipbook_frame_count = first_frame;
		if (oom)
			return false;
		SDL_Log("[flight_gltf] '%s' material %u: malformed flipbooks ignored", source_label, material_index);
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
	for (uint32_t column = 0; column < model->variant_columns; ++column) {
		if (material_is_screen(
				model, model->prim_variant_material[(size_t)prim_id * model->variant_columns + column]))
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

	/* ---- Variants, then the atlas images the materials reference --- */
	AtlasImages atlas = { 0 };
	if (!read_variants(data, source_label, out) || !read_atlases(data, source_label, out, &atlas))
		goto cleanup;
	uint32_t   energy_default       = 0;
	const bool has_energy_default   = read_energy_default(data, source_label, &energy_default);

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
			const cgltf_material* m = &data->materials[i];
			if (!read_material(data, &atlas, m, &out->materials[i])) {
				SDL_Log("[flight_gltf] '%s' material %u has an invalid alpha state or texture", source_label, i);
				goto cleanup;
			}
			cJSON*       object = flight_model_object(m->extensions, m->extensions_count);
			const cJSON* energy = cJSON_GetObjectItemCaseSensitive(object, "energy");
			char         owner[32];
			snprintf(owner, sizeof owner, "material %u", i);
			out->materials[i].energy_mask = energy ? energy_mask(energy, source_label, owner)
												   : (has_energy_default ? energy_default : 0u);
			const bool read =
				read_screen(cJSON_GetObjectItemCaseSensitive(object, "screen"), out, &screen_rect_capacity,
							&out->materials[i], source_label, i) &&
				read_animation(cJSON_GetObjectItemCaseSensitive(object, "flipbooks"), out, &flipbook_capacity,
							   &out->materials[i], source_label, i) &&
				read_damage(cJSON_GetObjectItemCaseSensitive(object, "damage"), out, &flipbook_capacity,
							&out->materials[i], source_label, i);
			cJSON_Delete(object);
			if (!read)
				goto cleanup;
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
				if (mp->variant < out->variant_count) {
					uint32_t mat = mp->material ? (uint32_t)cgltf_material_index(data, mp->material)
												: AERON_GLTF_NO_MATERIAL;
					out->prim_variant_material[(size_t)prim_id * out->variant_columns + mp->variant + 1u] = mat;
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
		for (uint32_t column = 1; column < out->variant_columns; ++column) {
			const uint32_t material =
				out->prim_variant_material[(size_t)pid * out->variant_columns + column];
			const AeronGltfAlphaMode mode = material_alpha_mode(out, material);
			if ((int)mode < 0 || mode != default_mode) {
				SDL_Log("[flight_gltf] '%s' primitive %u variant %u changes alpha class "
						"(material %u mode %d -> material %u mode %d)",
						source_label, pid, column - 1u, default_material, (int)default_mode, material, (int)mode);
				goto cleanup;
			}
		}
	}

	/* ---- Partition indices: OPAQUE, MASK, BLEND, each with its screens
	 * last, stable within each class. Primitive alpha class is invariant
	 * across variants above. ---- */
	out->opaque_index_count        = 0;
	out->mask_index_offset         = 0;
	out->mask_index_count          = 0;
	out->blend_index_offset        = 0;
	out->blend_index_count         = 0;
	out->opaque_screen_index_count = 0;
	out->mask_screen_index_count   = 0;
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
		/* screen: 0 = no screen material, 1 = screen. */
		static const struct {
			AeronGltfAlphaMode mode;
			int                screen;
		} classes[] = {
			{ AERON_GLTF_ALPHA_OPAQUE, 0 }, { AERON_GLTF_ALPHA_OPAQUE, 1 }, { AERON_GLTF_ALPHA_MASK, 0 },
			{ AERON_GLTF_ALPHA_MASK, 1 },   { AERON_GLTF_ALPHA_BLEND, 0 },  { AERON_GLTF_ALPHA_BLEND, 1 },
		};
		enum { CLASS_COUNT = sizeof classes / sizeof classes[0] };
		uint32_t w = 0;
		uint32_t starts[CLASS_COUNT + 1];
		for (int c = 0; c < CLASS_COUNT; ++c) {
			starts[c] = w;
			for (uint32_t t = 0; t + 2 < out->index_count; t += 3) {
				const uint32_t pid = out->vertices[out->indices[t]].prim_id;
				const uint32_t mat = out->prim_variant_material[(size_t)pid * out->variant_columns];
				if (material_alpha_mode(out, mat) != classes[c].mode || prim_screen[pid] != classes[c].screen)
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
		out->mask_index_count          = starts[4] - starts[2];
		out->mask_screen_index_count   = starts[4] - starts[3];
		out->blend_index_offset        = starts[4];
		out->blend_index_count         = starts[6] - starts[4];
		out->blend_screen_index_count  = starts[6] - starts[5];
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
			"%u blend; %u + %u + %u screen), %u prims, "
			"%u materials, %u variants, atlases base=%zu nrm=%zu mr=%zu "
			"emis=%zu",
			source_label, out->vertex_count, out->index_count, out->opaque_index_count, out->mask_index_count,
			out->blend_index_count, out->opaque_screen_index_count, out->mask_screen_index_count,
			out->blend_screen_index_count, out->total_prim_count, out->material_count, out->variant_count,
			out->channels[0].size, out->channels[1].size, out->channels[2].size, out->channels[3].size);
	if (out->markings_count > 0 || out->group_count > 0)
		SDL_Log("[flight_gltf] %s: %u markings, %u variant groups", source_label, out->markings_count,
				out->group_count);
	if (out->screen_rect_count > 0 || out->screen_hide_mask != 0)
		SDL_Log("[flight_gltf] %s: %u screen rectangles, hide mask 0x%x", source_label,
				out->screen_rect_count, out->screen_hide_mask);

	succeeded = true;

cleanup:
	free(plans);
	atlas_images_free(&atlas);
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
	free(m->markings);
	for (uint32_t g = 0; m->groups && g < m->group_count; g++) {
		free(m->groups[g].ktx2.data);
		free(m->groups[g].variants);
	}
	free(m->groups);
	memset(m, 0, sizeof *m);
}

uint32_t Aeron_GltfMarkingsVariant(const AeronGltfModel* m, uint32_t markings) {
	if (!m || m->markings_count == 0)
		return AERON_GLTF_NO_VARIANT;
	return m->markings[markings < m->markings_count ? markings : m->markings_count - 1u];
}
