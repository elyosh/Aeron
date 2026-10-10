/*
 * AeronSceneMesh — GPU-resident model upload. See
 * aeron/scene/mesh.h.
 */

#include "aeron/scene/mesh.h"

#include "aeron/log.h"
#include "aeron/scene/image_cache.h"
#include "aeron/scene/ktx2_reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char AeronPbrMaterialEntrySizeCheck[sizeof(AeronPbrMaterialEntry) == 192 ? 1 : -1];
typedef char AeronPbrScreenRectSizeCheck[sizeof(AeronPbrScreenRect) == 96 ? 1 : -1];

static const char* channel_name(int c) {
	static const char* names[AERON_GLTF_CHANNEL_COUNT] = { "base_color", "normal", "metallic_rough",
														   "emissive" };
	return (c >= 0 && c < AERON_GLTF_CHANNEL_COUNT) ? names[c] : "?";
}

static void close_channel_payloads(Ktx2* channels[AERON_GLTF_CHANNEL_COUNT]) {
	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; ++c) {
		if (channels[c]) {
			ktx2_close(channels[c]);
			channels[c] = NULL;
		}
	}
}

void AeronScene_MeshDestroy(AeronSceneMesh* m) {
	if (!m) {
		return;
	}
	if (m->vbo) {
		Aeron_DestroyBuffer(m->vbo);
	}
	if (m->ibo) {
		Aeron_DestroyBuffer(m->ibo);
	}
	Aeron_DestroyBuffer(m->material_buffer);
	Aeron_DestroyBuffer(m->variant_buffer);
	Aeron_DestroyBuffer(m->screen_rect_buffer);
	Aeron_DestroyBuffer(m->anim_buffer);
	for (uint32_t i = 0; i < m->effect_library_count; i++)
		AeronEffectAtlas_Release(m->effect_atlas, m->effect_libraries[i]);
	free(m->effect_libraries);
	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
		if (m->atlas[c]) {
			Aeron_DestroyTexture(m->atlas[c]);
		}
	}
	free(m->engine_glows);
	free(m->lights);
	free(m->cpu_vertices);
	free(m->cpu_indices);
	AeronFaceBvh_Free(m->render_bvh);
	free(m);
}

/* ===== Variant choice ================================================
 *
 * Without a choice (`set` NULL) the variant indices are the model's asset
 * variants and no variant group is loaded; with one, variant index i shows
 * variant set->variants[i]
 * and the groups those variants use are loaded. A loaded group's base-colour
 * layers follow the core layers in group order, skipping groups not loaded. */
static uint32_t variant_index_count(const AeronGltfModel* model, const AeronSceneMeshVariants* set) {
	if (set)
		return set->count;
	return model->variant_count > 0 && model->asset_variant_count > 0 ? model->asset_variant_count : 1u;
}

/* The prim_variant_material column variant index `index` shows. */
static uint32_t variant_column(const AeronGltfModel* model, const AeronSceneMeshVariants* set,
							   uint32_t index) {
	if (set)
		return set->variants[index] == AERON_GLTF_NO_VARIANT ? 0u : set->variants[index];
	return model->variant_count > 0 ? index : 0u;
}

static bool group_loaded(const AeronGltfModel* model, const AeronSceneMeshVariants* set, uint32_t group) {
	if (!set)
		return false;
	const AeronGltfVariantGroup* g = &model->groups[group];
	for (uint32_t index = 0; index < set->count; ++index)
		for (uint32_t v = 0; v < g->variant_count; ++v)
			if (g->variants[v] == set->variants[index])
				return true;
	return false;
}

/* The uploaded layer of channel `channel`'s page `page`: a core page is
 * kept, a loaded group's page moves to its compacted layer, a page of a group
 * not loaded becomes 0 (no variant index shows its materials); UINT32_MAX
 * past the channel's groups. */
static uint32_t channel_layer(const AeronGltfModel* model, const AeronSceneMeshVariants* set, int channel,
							  uint32_t core_layers, uint32_t page) {
	if (page < core_layers)
		return page;
	uint32_t first = core_layers, compacted = core_layers;
	for (uint32_t g = 0; g < model->group_count; ++g) {
		if (model->groups[g].channel != (uint32_t)channel)
			continue;
		const bool loaded = group_loaded(model, set, g);
		if (page < first + model->groups[g].layers)
			return loaded ? compacted + (page - first) : 0u;
		first += model->groups[g].layers;
		compacted += loaded ? model->groups[g].layers : 0u;
	}
	return UINT32_MAX;
}

/* The material primitives without one use (pbr_default_material's values):
 * white base colour factor, no emission, metallic 0, roughness 1, no
 * channel textures. */
static void populate_default_material_entry(AeronPbrMaterialEntry* e) {
	memset(e, 0, sizeof *e);
	for (int c = 0; c < 4; c++) {
		e->base_color_factor[c] = 1.0f;
		e->specular[c]          = 1.0f;
		e->atlas[c]             = 15u << 12;
	}
	e->emissive_factor[3] = 1.0f;
	e->metal_rough[1]     = 1.0f;
	e->metal_rough[2]     = 0.5f;
}

/* Fills the mesh's materials, then the default one after them (index
 * material_count), so shaders read any primitive's material from the
 * buffer. Returns the material count. */
static uint32_t populate_material_entries(AeronPbrMaterialEntry* entries, const AeronGltfModel* src,
										  const AeronSceneMeshVariants* set,
										  const uint32_t                core_layers[AERON_GLTF_CHANNEL_COUNT],
										  const uint32_t (*anim)[3]) {
	uint32_t n = src->material_count;
	if (n > AERON_GLTF_MAX_MATERIALS) {
		n = AERON_GLTF_MAX_MATERIALS;
	}
	memset(entries, 0, (size_t)n * sizeof *entries);
	populate_default_material_entry(&entries[n]);
	for (uint32_t i = 0; i < n; i++) {
		const AeronGltfMaterial* m = &src->materials[i];
		AeronPbrMaterialEntry*   e = &entries[i];

		memcpy(e->base_rect, m->uv_xform[AERON_GLTF_CHANNEL_BASE_COLOR], sizeof e->base_rect);
		memcpy(e->normal_rect, m->uv_xform[AERON_GLTF_CHANNEL_NORMAL], sizeof e->normal_rect);
		memcpy(e->mr_rect, m->uv_xform[AERON_GLTF_CHANNEL_METALLIC_ROUGHNESS], sizeof e->mr_rect);
		memcpy(e->emissive_rect, m->uv_xform[AERON_GLTF_CHANNEL_EMISSIVE], sizeof e->emissive_rect);

		memcpy(e->base_color_factor, m->base_color_factor, sizeof e->base_color_factor);
		e->emissive_factor[0] = m->emissive_factor[0];
		e->emissive_factor[1] = m->emissive_factor[1];
		e->emissive_factor[2] = m->emissive_factor[2];
		e->emissive_factor[3] = m->emissive_strength;
		e->metal_rough[0]     = m->metallic_factor;
		e->metal_rough[1]     = m->roughness_factor;
		e->metal_rough[2]     = m->alpha_cutoff;
		e->specular[0]        = m->specular_color_factor[0];
		e->specular[1]        = m->specular_color_factor[1];
		e->specular[2]        = m->specular_color_factor[2];
		e->specular[3]        = m->specular_factor;
		for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
			const uint32_t page = src->group_count
									  ? channel_layer(src, set, c, core_layers[c], m->atlas_page[c])
									  : m->atlas_page[c];
			e->atlas[c]         = (page & 0xFFFu) | (m->atlas_max_lod[c] & 0xFu) << 12;
		}

		/* flags mirror UV-xform scale presence (scale.xy > 0), matching
		 * the FS sentinel: scale==0 means channel absent. */
		uint32_t flags = 0;
		if (e->normal_rect[2] > 0.0f || e->normal_rect[3] > 0.0f) {
			flags |= 0x1u;
		}
		if (e->mr_rect[2] > 0.0f || e->mr_rect[3] > 0.0f) {
			flags |= 0x2u;
		}
		if (e->emissive_rect[2] > 0.0f || e->emissive_rect[3] > 0.0f) {
			flags |= 0x4u;
		}
		if (m->alpha_mode == AERON_GLTF_ALPHA_BLEND) {
			flags |= 0x8u; /* FS: alpha = tex.a * factor.a (blend prims) */
		}
		if (m->emissive_mode == AERON_GLTF_EMISSIVE_LEGACY_SRGB_SRCALPHA) {
			flags |= 0x10u; /* FS: legacy sRGB filtering + SRCALPHA composition */
		}
		if (m->alpha_mode == AERON_GLTF_ALPHA_MASK) {
			flags |= 0x20u; /* FS: discard base alpha below metal_rough.z */
		}
		e->flags        = flags;
		e->screen_first = m->screen_first;
		e->screen_count = m->screen_count;
		e->screen_flags = m->screen_flags;
		if (anim && (anim[i][0] || anim[i][1] || anim[i][2])) {
			const uint32_t page = src->group_count ? channel_layer(src, set, AERON_GLTF_CHANNEL_BASE_COLOR,
																   core_layers[AERON_GLTF_CHANNEL_BASE_COLOR],
																   m->anim_base_page)
												   : m->anim_base_page;
			memcpy(e->anim_base_rect, m->anim_base_xform, sizeof e->anim_base_rect);
			e->anim[0] = (page & 0xFFFu) | (m->anim_base_max_lod & 0xFu) << 12;
			e->anim[1] = anim[i][0];
			e->anim[2] = anim[i][1];
			e->anim[3] = anim[i][2];
		}
	}
	return n;
}

static int build_material_storage(AeronSceneMesh* s, const AeronGltfModel* model,
								  const AeronSceneMeshVariants* set,
								  const uint32_t                core_layers[AERON_GLTF_CHANNEL_COUNT],
								  const uint32_t (*anim)[3], const char* name,
								  AeronPbrMaterialEntry** out_entries, uint32_t* out_bytes) {
	/* The mesh's materials, then the default one. */
	const uint32_t allocation_count =
		(model->material_count > AERON_GLTF_MAX_MATERIALS ? AERON_GLTF_MAX_MATERIALS : model->material_count) +
		1u;
	if (!out_entries || !out_bytes) {
		return 0;
	}
	*out_entries = NULL;
	*out_bytes   = 0;
	AeronPbrMaterialEntry* entries =
		(AeronPbrMaterialEntry*)malloc((size_t)allocation_count * sizeof *entries);
	if (!entries) {
		return 0;
	}
	s->material_count    = populate_material_entries(entries, model, set, core_layers, anim);
	const uint32_t bytes = allocation_count * (uint32_t)sizeof *entries;
	s->material_buffer   = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size       = bytes,
		.usage      = AERON_BUFFER_USAGE_STORAGE,
		.debug_name = name,
	});
	if (!s->material_buffer) {
		free(entries);
		return 0;
	}
	*out_entries = entries;
	*out_bytes   = bytes;
	return 1;
}

/* Screen rectangles in their shader record; NULL storage without any. */
static int build_screen_storage(AeronSceneMesh* s, const AeronGltfModel* model, const char* name,
								AeronPbrScreenRect** out_rects, uint32_t* out_bytes) {
	*out_rects          = NULL;
	*out_bytes          = 0;
	s->screen_hide_mask   = model->screen_hide_mask;
	s->screen_source_mask = 0;
	if (model->screen_rect_count == 0) {
		return 1;
	}
	AeronPbrScreenRect* rects = (AeronPbrScreenRect*)calloc(model->screen_rect_count, sizeof *rects);
	if (!rects) {
		return 0;
	}
	for (uint32_t i = 0; i < model->screen_rect_count; ++i) {
		const AeronGltfScreenRect* src = &model->screen_rects[i];
		AeronPbrScreenRect*        dst = &rects[i];
		memcpy(dst->dst, src->dst, sizeof dst->dst);
		memcpy(dst->bg_gain, src->bg, sizeof src->bg);
		dst->bg_gain[3] = src->gain;
		memcpy(dst->highlight, src->highlight, sizeof src->highlight);
		memcpy(dst->lock, src->lock, sizeof src->lock);
		memcpy(dst->bar, src->bar, sizeof src->bar);
		dst->info[0] = src->layers;
		dst->info[1] = src->source;
		if ((src->layers & AERON_GLTF_SCREEN_LAYER_BAR) == 0 && src->source < AERON_GLTF_SCREEN_MAX_SOURCES)
			s->screen_source_mask |= (uint64_t)1 << src->source;
	}
	const uint32_t bytes  = model->screen_rect_count * (uint32_t)sizeof *rects;
	s->screen_rect_buffer = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size       = bytes,
		.usage      = AERON_BUFFER_USAGE_STORAGE,
		.debug_name = name,
	});
	if (!s->screen_rect_buffer) {
		free(rects);
		return 0;
	}
	s->screen_rect_count = model->screen_rect_count;
	*out_rects           = rects;
	*out_bytes           = bytes;
	return 1;
}

/* ===== Material animation ============================================
 *
 * The records of material_anim_layout.hlsli: the header, then per animated
 * material its flipbooks (each followed by its uv record when it has one),
 * then their frames. */
typedef struct AnimRecords {
	uint32_t (*items)[4];
	uint32_t count, capacity;
} AnimRecords;

static uint32_t* anim_record(AnimRecords* r) {
	if (r->count == r->capacity) {
		const uint32_t capacity = r->capacity ? r->capacity * 2u : 64u;
		uint32_t (*grown)[4]    = (uint32_t (*)[4])realloc(r->items, capacity * sizeof *r->items);
		if (!grown)
			return NULL;
		r->items    = grown;
		r->capacity = capacity;
	}
	uint32_t* record = r->items[r->count++];
	memset(record, 0, sizeof *r->items);
	return record;
}

static uint32_t float_bits(float f) {
	uint32_t u;
	memcpy(&u, &f, sizeof u);
	return u;
}

/* The mesh's effect library index of `name`, acquiring it once; UINT32_MAX
 * when it cannot be loaded (its frames show the static material). `names`
 * are the acquired libraries' names, `failed` those that failed. */
static uint32_t anim_library(AeronSceneMesh* s, AeronCommandBuffer* cmd, const char* name, const char** names,
							 const char** failed, uint32_t* failed_count) {
	for (uint32_t i = 0; i < s->effect_library_count; i++)
		if (strcmp(names[i], name) == 0)
			return s->effect_libraries[i];
	for (uint32_t i = 0; i < *failed_count; i++)
		if (strcmp(failed[i], name) == 0)
			return UINT32_MAX;
	uint32_t library;
	if (!AeronEffectAtlas_Acquire(s->effect_atlas, cmd, name, &library)) {
		failed[(*failed_count)++] = name;
		return UINT32_MAX;
	}
	names[s->effect_library_count]                 = name;
	s->effect_libraries[s->effect_library_count++] = library;
	return library;
}

/* A frame record (two for illumination, the second with the emission
 * strength) of image `key`; the layer AERON_ANIM_NO_LAYER when the library
 * lacks it. Returns false on OOM. */
static bool anim_frame(AnimRecords* r, const AeronEffectAtlas* atlas, uint32_t library, uint32_t layer,
					   const char* key, float end, float emissive_strength, uint32_t* missing) {
	AeronEffectImage image;
	const bool       found = library != UINT32_MAX && AeronEffectAtlas_Find(atlas, library, key, &image) &&
							 image.present[AERON_EFFECT_LAYER] &&
							 (layer != AERON_ANIM_LAYER_ILLUMINATION || image.present[AERON_EFFECT_EMISSIVE]);
	*missing += !found;
	for (uint32_t t = 0; t < AERON_ANIM_FRAME_RECORDS(layer); t++) {
		uint32_t* record = anim_record(r);
		if (!record)
			return false;
		record[0] = AERON_ANIM_NO_LAYER;
		record[3] = float_bits(t == 0 ? end : emissive_strength);
		if (found) {
			record[0] = (image.layer[t] & 0xFFFu) | (image.lod[t] & 0xFu) << 12;
			record[1] = image.rect[t][0] | image.rect[t][1] << 16;
			record[2] = image.rect[t][2] | image.rect[t][3] << 16;
		}
	}
	return true;
}

/* A damage alternative's records: its three stage frames (layer
 * AERON_ANIM_NO_LAYER for none or a missing image), then its placement.
 * Returns false on OOM. */
static bool anim_damage(AnimRecords* r, const AeronEffectAtlas* atlas, uint32_t library,
						const AeronGltfDamage* damage, uint32_t* missing) {
	for (uint32_t stage = 0; stage < AERON_ANIM_DAMAGE_STAGES; stage++) {
		uint32_t* record = anim_record(r);
		if (!record)
			return false;
		record[0] = AERON_ANIM_NO_LAYER;
		AeronEffectImage image;
		if (!damage->stages[stage])
			continue;
		if (library == UINT32_MAX || !AeronEffectAtlas_Find(atlas, library, damage->stages[stage], &image) ||
			!image.present[AERON_EFFECT_LAYER]) {
			(*missing)++;
			continue;
		}
		record[0] = (image.layer[0] & 0xFFFu) | (image.lod[0] & 0xFu) << 12;
		record[1] = image.rect[0][0] | image.rect[0][1] << 16;
		record[2] = image.rect[0][2] | image.rect[0][3] << 16;
	}
	uint32_t* placement = anim_record(r);
	if (!placement)
		return false;
	placement[0]    = float_bits(damage->scale[0]);
	placement[1]    = float_bits(damage->scale[1]);
	placement[2]    = damage->has_scale ? 1u : 0u;
	placement[3]    = damage->offset_mode;
	uint32_t* range = anim_record(r);
	if (!range)
		return false;
	for (int k = 0; k < 4; k++)
		range[k] = float_bits(damage->offset[k]);
	return true;
}

/* Builds the records of the model's flipbooks and damage alternatives, and
 * each material's anim.y, z and w (all 0 without animation) into
 * `out_anim`; no records when nothing animates. Returns 0 on OOM. */
/* The energy effects' records, from header record 0.x: per effect a
 * flipbook record (mode, first frame, frame count, total seconds), then
 * asuint(strength), asuint(tile size in metres), then its frames, one
 * record each (the emissive image). Returns false on OOM. */
static bool anim_energy(AnimRecords* r, const AeronEffectData* data, float extent, uint32_t* missing) {
	r->items[0][0] = r->count;
	uint32_t book[AERON_EFFECT_ENERGY_COUNT];
	for (int e = 0; e < AERON_EFFECT_ENERGY_COUNT; e++) {
		AeronEffectEnergy energy = { 0 };
		AeronEffectData_Energy(data, e, &energy);
		book[e] = r->count;
		if (!anim_record(r) || !anim_record(r))
			return false;
		uint32_t* record = r->items[book[e]];
		uint32_t* params = r->items[book[e] + 1];
		record[0]        = (energy.once ? AERON_ANIM_MODE_ONCE : AERON_ANIM_MODE_LOOP)
						   << AERON_ANIM_FLIPBOOK_MODE_SHIFT;
		record[2]        = energy.frame_count;
		params[0]        = float_bits(energy.strength);
		params[1]        = float_bits(energy.tile * extent);
	}
	for (int e = 0; e < AERON_EFFECT_ENERGY_COUNT; e++) {
		r->items[book[e]][1] = r->count;
		double end           = 0.0;
		for (uint32_t f = 0; f < r->items[book[e]][2]; f++) {
			AeronEffectImage image;
			const int        found = AeronEffectData_EnergyFrame(data, e, f, &image, &end);
			if (!anim_record(r))
				return false;
			uint32_t* record = r->items[r->count - 1];
			record[0]        = AERON_ANIM_NO_LAYER;
			record[3]        = float_bits((float)end);
			*missing += !found;
			if (found) {
				record[0] = (image.layer[AERON_EFFECT_EMISSIVE] & 0xFFFu) |
							(image.lod[AERON_EFFECT_EMISSIVE] & 0xFu) << 12;
				record[1] = image.rect[AERON_EFFECT_EMISSIVE][0] | image.rect[AERON_EFFECT_EMISSIVE][1] << 16;
				record[2] = image.rect[AERON_EFFECT_EMISSIVE][2] | image.rect[AERON_EFFECT_EMISSIVE][3] << 16;
			}
		}
		r->items[book[e]][3] = float_bits((float)end);
	}
	return true;
}

static int build_anim_records(AeronSceneMesh* s, AeronCommandBuffer* cmd, const AeronGltfModel* model,
							  const AeronEffectData* effect_data, float extent, const char* name,
							  AnimRecords* r, uint32_t (**out_anim)[3]) {
	*out_anim            = NULL;
	uint32_t energy_mask = 0;
	for (uint32_t m = 0; effect_data && m < model->material_count; m++)
		energy_mask |= model->materials[m].energy_mask;
	if (!s->effect_atlas || (!model->flipbook_count && !model->damage_count && !energy_mask))
		return 1;
	const uint32_t library_capacity = model->flipbook_count + model->damage_count;
	uint32_t (*anim)[3]             = (uint32_t (*)[3])calloc(model->material_count, sizeof *anim);
	const char** names              = (const char**)calloc(library_capacity, sizeof *names);
	const char** failed             = (const char**)calloc(library_capacity, sizeof *failed);
	s->effect_libraries             = (uint32_t*)calloc(library_capacity, sizeof *s->effect_libraries);
	int      ok           = anim && names && failed && s->effect_libraries && anim_record(r);
	uint32_t failed_count = 0, missing = 0, frames = 0;
	if (ok && energy_mask)
		ok = anim_energy(r, effect_data, extent, &missing);
	for (uint32_t m = 0; ok && m < model->material_count; m++) {
		const AeronGltfMaterial* material = &model->materials[m];
		const uint32_t           energy   = effect_data ? material->energy_mask & 0x7u : 0u;
		if (!material->flipbook_count && !material->damage_count && !energy)
			continue;
		/* The flipbooks, then their frames, then the damage alternatives. */
		const uint32_t first = r->count;
		for (uint32_t b = 0; ok && b < material->flipbook_count; b++) {
			const AeronGltfFlipbook* fb     = &model->flipbooks[material->flipbook_first + b];
			uint32_t*                record = anim_record(r);
			ok                              = record != NULL;
			if (ok) {
				record[0] = fb->state | fb->layer << AERON_ANIM_FLIPBOOK_LAYER_SHIFT |
							fb->mode << AERON_ANIM_FLIPBOOK_MODE_SHIFT |
							(fb->has_uv ? AERON_ANIM_FLIPBOOK_HAS_UV : 0u) |
							(fb->uv_clamp ? AERON_ANIM_FLIPBOOK_UV_CLAMP : 0u);
				record[2] = fb->frame_count;
			}
			if (ok && fb->has_uv) {
				record = anim_record(r);
				ok     = record != NULL;
				if (ok) {
					record[0] = float_bits(fb->uv_scale[0]);
					record[1] = float_bits(fb->uv_scale[1]);
					record[2] = float_bits(fb->uv_offset[0]);
					record[3] = float_bits(fb->uv_offset[1]);
				}
			}
		}
		for (uint32_t b = 0, book = first; ok && b < material->flipbook_count; b++) {
			const AeronGltfFlipbook* fb = &model->flipbooks[material->flipbook_first + b];
			const uint32_t library = fb->frame_count
										 ? anim_library(s, cmd, fb->library, names, failed, &failed_count)
										 : UINT32_MAX;
			float          end     = 0.0f;
			r->items[book][1]      = r->count;
			for (uint32_t f = 0; ok && f < fb->frame_count; f++) {
				const AeronGltfFlipbookFrame* frame = &model->flipbook_frames[fb->frame_first + f];
				end += frame->seconds;
				ok = anim_frame(r, s->effect_atlas, library, fb->layer, frame->key, end, fb->emissive_strength,
								&missing);
			}
			r->items[book][3] = float_bits(end);
			frames += fb->frame_count;
			book += fb->has_uv ? 2u : 1u;
		}
		const uint32_t damage_first = r->count;
		for (uint32_t d = 0; ok && d < material->damage_count; d++) {
			const AeronGltfDamage* damage = &model->damages[material->damage_first + d];
			const uint32_t library = anim_library(s, cmd, damage->library, names, failed, &failed_count);
			ok                     = anim_damage(r, s->effect_atlas, library, damage, &missing);
			frames += AERON_ANIM_DAMAGE_STAGES;
		}
		if (damage_first > 0xFFFFu) {
			Aeron_LogError("aeron.scene", "%s: too many animation records; material %u stays static", name,
						   m);
			continue;
		}
		if (material->flipbook_count)
			anim[m][0] = first | material->flipbook_count << 16;
		if (material->damage_count) {
			anim[m][1] = damage_first | material->damage_count << 16;
			anim[m][2] = (material->damage_choice & 0xFFFu) << 8 | (material->damage_placement & 0xFFFu)
																	   << 20;
		}
		anim[m][2] |= energy;
	}
	free((void*)names);
	free((void*)failed);
	if (!ok) {
		free(anim);
		return 0;
	}
	if (missing)
		Aeron_LogWarn("aeron.scene", "%s: %u of %u animation images are not in their libraries", name,
					  missing, frames);
	*out_anim = anim;
	return 1;
}

static int build_variant_storage(AeronSceneMesh* s, const AeronGltfModel* model,
								 const AeronSceneMeshVariants* set, const char* name, uint32_t** out_values,
								 uint32_t* out_bytes) {
	const uint32_t primitive_count = model->total_prim_count;
	if (!out_values || !out_bytes) {
		return 0;
	}
	*out_values = NULL;
	*out_bytes                 = 0;
	const uint32_t groups      = primitive_count > 0 ? (primitive_count + 3u) / 4u : 1u;
	const uint32_t rows        = variant_index_count(model, set);
	const size_t   value_count = (size_t)groups * rows * 4u;
	if (value_count > UINT32_MAX / sizeof(uint32_t)) {
		return 0;
	}
	uint32_t* values = (uint32_t*)malloc(value_count * sizeof *values);
	if (!values) {
		return 0;
	}
	for (size_t i = 0; i < value_count; ++i) {
		values[i] = AERON_GLTF_NO_MATERIAL;
	}
	if (model->prim_variant_material && model->variant_columns > 0) {
		for (uint32_t row = 0; row < rows; ++row) {
			/* Storage row `row` holds variant index `row`. */
			const uint32_t column = variant_column(model, set, row);
			for (uint32_t primitive = 0; primitive < primitive_count; ++primitive) {
				values[((size_t)row * groups + primitive / 4u) * 4u + primitive % 4u] =
					model->prim_variant_material[(size_t)primitive * model->variant_columns + column];
			}
		}
	}
	const uint32_t bytes = (uint32_t)(value_count * sizeof *values);
	s->variant_buffer    = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size       = bytes,
		.usage      = AERON_BUFFER_USAGE_STORAGE,
		.debug_name = name,
	});
	if (!s->variant_buffer) {
		free(values);
		return 0;
	}
	s->variant_groups_per_row = groups;
	*out_values               = values;
	*out_bytes                = bytes;
	return 1;
}

/* Channel `channel`'s atlas: the core layers, then those of the channel's
 * variant groups `set` uses. Returns NULL on failure (logged). */
static AeronTexture* upload_channel_groups(AeronCommandBuffer* cmd, const AeronGltfModel* model,
										   const AeronSceneMeshVariants* set, int channel, const Ktx2* core,
										   const char* label) {
	const Ktx2** parts = (const Ktx2**)calloc((size_t)model->group_count + 1u, sizeof *parts);
	if (!parts) {
		return NULL;
	}
	uint32_t count = 0, layers = 0, channel_groups = 0;
	size_t   bytes = 0;
	bool     ok    = true;
	parts[count++] = core;
	for (uint32_t g = 0; ok && g < model->group_count; ++g) {
		if (model->groups[g].channel != (uint32_t)channel) {
			continue;
		}
		channel_groups++;
		if (!group_loaded(model, set, g)) {
			continue;
		}
		const AeronGltfVariantGroup* group = &model->groups[g];
		Ktx2*                        part  = ktx2_open_mem(group->ktx2.data, group->ktx2.size, label);
		ok                                 = part && ktx2_layer_count(part) == (int)group->layers;
		if (part) {
			parts[count++] = part;
		}
		layers += group->layers;
		bytes += group->ktx2.size;
		if (!ok) {
			Aeron_LogError("aeron.scene", "%s: variant group %u is not a %u-layer KTX2", label, g,
						   group->layers);
		}
	}
	AeronTexture* texture = ok ? Aeron_ImageUploadKtx2Layers(cmd, parts, count, label) : NULL;
	Aeron_LogInfo("aeron.scene", "%s: %u variant indices, %u of %u variant groups (%u layers, %zu bytes)",
				  label, set->count, count - 1u, channel_groups, layers, bytes);
	for (uint32_t i = 1; i < count; ++i) {
		ktx2_close((Ktx2*)parts[i]);
	}
	free(parts);
	return texture;
}

static AeronSceneMesh* mesh_create(AeronCommandBuffer* cmd, const AeronFlightModel* flight_model,
								   const char* debug_name, const AeronSceneMeshVariants* set,
								   AeronEffectAtlas* effect_atlas, const AeronEffectData* effect_data,
								   AeronSceneMeshCreateStatus* status) {
	if (status) {
		*status = AERON_SCENE_MESH_CREATE_RESOURCE_FAILURE;
	}
	if (!cmd || !flight_model) {
		return NULL;
	}
	const AeronGltfModel* model = &flight_model->render;
	const char*           name  = debug_name ? debug_name : "<mesh>";
	if (set) {
		bool valid = set->variants && set->count > 0;
		for (uint32_t index = 0; valid && index < set->count; ++index)
			valid =
				set->variants[index] == AERON_GLTF_NO_VARIANT || set->variants[index] < model->variant_count;
		if (!valid) {
			Aeron_LogError("aeron.scene", "%s: invalid variant choice", name);
			if (status) {
				*status = AERON_SCENE_MESH_CREATE_INVALID_SOURCE;
			}
			return NULL;
		}
	}
	/* Base-colour core layers, known once that atlas is open. */
	/* Core layers of each channel, known once its atlas is open. */
	uint32_t core_layers[AERON_GLTF_CHANNEL_COUNT] = { 0 };
	if (model->mask_index_offset != model->opaque_index_count ||
		model->mask_index_offset > model->index_count ||
		model->mask_index_count > model->index_count - model->mask_index_offset ||
		model->blend_index_offset != model->mask_index_offset + model->mask_index_count ||
		model->blend_index_offset > model->index_count ||
		model->blend_index_count != model->index_count - model->blend_index_offset ||
		model->opaque_index_count % 3u != 0u || model->mask_index_offset % 3u != 0u ||
		model->mask_index_count % 3u != 0u || model->blend_index_offset % 3u != 0u ||
		model->blend_index_count % 3u != 0u || model->opaque_screen_index_count > model->opaque_index_count ||
		model->blend_screen_index_count > model->blend_index_count ||
		model->opaque_screen_index_count % 3u != 0u || model->blend_screen_index_count % 3u != 0u) {
		Aeron_LogError("aeron.scene", "%s: invalid opaque/mask/blend index ranges", name);
		if (status) {
			*status = AERON_SCENE_MESH_CREATE_INVALID_SOURCE;
		}
		return NULL;
	}
	Ktx2* channel_payloads[AERON_GLTF_CHANNEL_COUNT] = { 0 };
	int   have_any_channel                           = 0;
	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; ++c) {
		if (model->channels[c].data && model->channels[c].size) {
			have_any_channel = 1;
		}
	}
	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; ++c) {
		const AeronGltfChannelKtx2* blob = &model->channels[c];
		if (!blob->data || blob->size == 0) {
			if (have_any_channel) {
				Aeron_LogError("aeron.scene", "%s: missing %s atlas", name, channel_name(c));
				if (status) {
					*status = AERON_SCENE_MESH_CREATE_INVALID_SOURCE;
				}
				close_channel_payloads(channel_payloads);
				return NULL;
			}
			continue;
		}
		char label[96];
		snprintf(label, sizeof label, "%s.%s", name, channel_name(c));
		Ktx2OpenStatus open_status;
		channel_payloads[c] = ktx2_open_mem_status(blob->data, blob->size, label, &open_status);
		if (!channel_payloads[c]) {
			if (status) {
				*status = open_status == KTX2_OPEN_RESOURCE_FAILURE ? AERON_SCENE_MESH_CREATE_RESOURCE_FAILURE
																	: AERON_SCENE_MESH_CREATE_INVALID_SOURCE;
			}
			close_channel_payloads(channel_payloads);
			return NULL;
		}
		/* Every binding must name an existing layer of its atlas, after the
		 * variant groups' remapping. */
		uint32_t layers =
			ktx2_layer_count(channel_payloads[c]) > 0 ? (uint32_t)ktx2_layer_count(channel_payloads[c]) : 1u;
		core_layers[c] = layers;
		for (uint32_t g = 0; g < model->group_count; ++g)
			layers += model->groups[g].channel == (uint32_t)c && group_loaded(model, set, g)
						  ? model->groups[g].layers
						  : 0u;
		for (uint32_t index = 0; index < model->material_count; ++index) {
			const uint32_t page = model->group_count ? channel_layer(model, set, c, core_layers[c],
																	 model->materials[index].atlas_page[c])
													 : model->materials[index].atlas_page[c];
			if (page >= layers) {
				Aeron_LogError("aeron.scene", "%s: material %u samples %s atlas page %u of %u", name, index,
							   channel_name(c), model->materials[index].atlas_page[c], layers);
				if (status) {
					*status = AERON_SCENE_MESH_CREATE_INVALID_SOURCE;
				}
				close_channel_payloads(channel_payloads);
				return NULL;
			}
			/* The material's aeronAnimBaseColor is on the base atlas. */
			const AeronGltfMaterial* material = &model->materials[index];
			const uint32_t           base_page =
				c != AERON_GLTF_CHANNEL_BASE_COLOR || material->anim_base_xform[2] <= 0.0f ? 0u
				: model->group_count ? channel_layer(model, set, c, core_layers[c], material->anim_base_page)
									 : material->anim_base_page;
			if (base_page >= layers) {
				Aeron_LogError("aeron.scene", "%s: material %u anim base colour samples base atlas page %u of %u",
							   name, index, material->anim_base_page, layers);
				if (status) {
					*status = AERON_SCENE_MESH_CREATE_INVALID_SOURCE;
				}
				close_channel_payloads(channel_payloads);
				return NULL;
			}
		}
	}

	AeronSceneMesh*        s                = (AeronSceneMesh*)calloc(1, sizeof *s);
	AeronPbrMaterialEntry* material_entries = NULL;
	uint32_t*              variant_values   = NULL;
	uint32_t               material_bytes   = 0;
	uint32_t               variant_bytes    = 0;
	uint32_t               vbo_bytes        = 0;
	uint32_t               ibo_bytes        = 0;
	if (!s) {
		close_channel_payloads(channel_payloads);
		return NULL;
	}

	/* ---- Merged VBO / IBO upload ---- */
	if (model->vertex_count > 0 && model->index_count > 0) {
		if (model->vertex_count > UINT32_MAX / (uint32_t)sizeof(AeronGltfVertex) ||
			model->index_count > UINT32_MAX / (uint32_t)sizeof(uint32_t)) {
			Aeron_LogError("aeron.scene", "%s: geometry exceeds 4 GiB buffer limits", name);
			close_channel_payloads(channel_payloads);
			AeronScene_MeshDestroy(s);
			return NULL;
		}
		vbo_bytes = model->vertex_count * (uint32_t)sizeof(AeronGltfVertex);
		ibo_bytes = model->index_count * (uint32_t)sizeof(uint32_t);
		s->vbo =
			Aeron_CreateBuffer(&(AeronBufferDesc) { .usage = AERON_BUFFER_USAGE_VERTEX, .size = vbo_bytes });
		s->ibo =
			Aeron_CreateBuffer(&(AeronBufferDesc) { .usage = AERON_BUFFER_USAGE_INDEX, .size = ibo_bytes });
		if (s->vbo && s->ibo) {
			char buffer_name[512];
			snprintf(buffer_name, sizeof buffer_name, "%s.vertices", name);
			Aeron_GpuDebugNameBuffer(s->vbo, buffer_name);
			snprintf(buffer_name, sizeof buffer_name, "%s.indices", name);
			Aeron_GpuDebugNameBuffer(s->ibo, buffer_name);
		}
		if (!s->vbo || !s->ibo) {
			Aeron_LogError("aeron.scene", "%s: geometry buffer creation failed", name);
			close_channel_payloads(channel_payloads);
			AeronScene_MeshDestroy(s);
			return NULL;
		}
		s->vertex_count       = model->vertex_count;
		s->index_count        = model->index_count;
		s->opaque_index_count = model->opaque_index_count;
		s->mask_index_offset  = model->mask_index_offset;
		s->mask_index_count   = model->mask_index_count;
		s->blend_index_offset = model->blend_index_offset;
		s->blend_index_count  = model->blend_index_count;
		s->opaque_screen_index_count = model->opaque_screen_index_count;
		s->blend_screen_index_count  = model->blend_screen_index_count;

		s->cpu_vertices =
			(AeronSceneMeshCpuVertex*)malloc((size_t)model->vertex_count * sizeof *s->cpu_vertices);
		s->cpu_indices = (uint32_t*)malloc((size_t)model->index_count * sizeof *s->cpu_indices);
		if (!s->cpu_vertices || !s->cpu_indices) {
			Aeron_LogError("aeron.scene", "%s: retained geometry allocation failed", name);
			close_channel_payloads(channel_payloads);
			AeronScene_MeshDestroy(s);
			return NULL;
		}
		for (uint32_t vertex_index = 0; vertex_index < model->vertex_count; vertex_index++) {
			const AeronGltfVertex*   source_vertex   = &model->vertices[vertex_index];
			AeronSceneMeshCpuVertex* retained_vertex = &s->cpu_vertices[vertex_index];
			memcpy(retained_vertex->pos, source_vertex->pos, sizeof retained_vertex->pos);
			memcpy(retained_vertex->normal, source_vertex->normal, sizeof retained_vertex->normal);
			retained_vertex->component = source_vertex->component;
		}
		memcpy(s->cpu_indices, model->indices, (size_t)model->index_count * sizeof *s->cpu_indices);
		if (flight_model->render_bvh) {
			s->render_bvh = AeronFaceBvh_Clone(flight_model->render_bvh);
			if (!s->render_bvh) {
				Aeron_LogError("aeron.scene", "%s: render BVH copy failed", name);
				close_channel_payloads(channel_payloads);
				AeronScene_MeshDestroy(s);
				return NULL;
			}
		}
	}

	/* ---- Engine glows (owned copy; XWA OPTs) ---- */
	if (flight_model->engine_glow_count > 0 && flight_model->engine_glows) {
		s->engine_glows =
			(AeronFlightEngineGlow*)malloc(flight_model->engine_glow_count * sizeof *s->engine_glows);
		if (!s->engine_glows) {
			Aeron_LogError("aeron.scene", "%s: engine-glow allocation failed", name);
			close_channel_payloads(channel_payloads);
			AeronScene_MeshDestroy(s);
			return NULL;
		}
		memcpy(s->engine_glows, flight_model->engine_glows,
			   flight_model->engine_glow_count * sizeof *s->engine_glows);
		s->engine_glow_count = flight_model->engine_glow_count;
	}

	/* ---- Point lights (owned copy) ---- */
	if (flight_model->light_count > 0 && flight_model->lights) {
		s->lights = (AeronFlightLight*)malloc(flight_model->light_count * sizeof *s->lights);
		if (!s->lights) {
			Aeron_LogError("aeron.scene", "%s: light allocation failed", name);
			close_channel_payloads(channel_payloads);
			AeronScene_MeshDestroy(s);
			return NULL;
		}
		memcpy(s->lights, flight_model->lights, flight_model->light_count * sizeof *s->lights);
		s->light_count = flight_model->light_count;
	}

	/* ---- Channel atlases (4 BC7 KTX2 blobs from the GLB BIN chunk) ----
	 * A mesh with no channel blobs is a legitimate factor-only asset. A
	 * partial or malformed authored set was rejected before GPU work began. */
	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
		if (!channel_payloads[c]) {
			continue;
		}
		char label[96];
		snprintf(label, sizeof label, "%s.%s", name, channel_name(c));
		bool channel_grouped = false;
		for (uint32_t g = 0; set && g < model->group_count; ++g)
			channel_grouped |= model->groups[g].channel == (uint32_t)c;
		if (channel_grouped) {
			s->atlas[c] = upload_channel_groups(cmd, model, set, c, channel_payloads[c], label);
		} else {
			s->atlas[c] = Aeron_ImageUploadKtx2Array(cmd, channel_payloads[c], label);
		}
		ktx2_close(channel_payloads[c]);
		channel_payloads[c] = NULL;
		if (!s->atlas[c]) {
			Aeron_LogError("aeron.scene", "%s: %s atlas upload failed", name, channel_name(c));
			close_channel_payloads(channel_payloads);
			AeronScene_MeshDestroy(s);
			return NULL;
		}
	}

	/* ---- Immutable material and variant storage ---- */
	s->variant_rows               = variant_index_count(model, set);
	s->variant_count              = model->variant_count > 0 ? s->variant_rows : 0u;
	s->total_prim_count           = model->total_prim_count;
	s->all_materials_single_sided = true;
	for (uint32_t index = 0; index < model->material_count; ++index) {
		if (model->materials[index].double_sided) {
			s->all_materials_single_sided = false;
			break;
		}
	}
	char                material_name[512];
	char                variant_name[512];
	char                screen_name[512];
	char                anim_name[512];
	AeronPbrScreenRect* screen_rects = NULL;
	uint32_t            screen_bytes = 0;
	AnimRecords         anim_records = { 0 };
	uint32_t (*anim)[3]              = NULL;
	snprintf(material_name, sizeof material_name, "%s.materials", name);
	snprintf(variant_name, sizeof variant_name, "%s.material_variants", name);
	snprintf(screen_name, sizeof screen_name, "%s.screen_rects", name);
	snprintf(anim_name, sizeof anim_name, "%s.material_anim", name);
	s->effect_atlas = effect_atlas;
	/* Energy effects tile by the model's largest extent. */
	const float extent = fmaxf(flight_model->bounds.max.x - flight_model->bounds.min.x,
							   fmaxf(flight_model->bounds.max.y - flight_model->bounds.min.y,
									 flight_model->bounds.max.z - flight_model->bounds.min.z));
	int         built  = build_anim_records(s, cmd, model, effect_data, extent > 0.0f ? extent : 1.0f, name,
											&anim_records, &anim);
	if (built && anim_records.count > 1) {
		s->anim_buffer = Aeron_CreateBuffer(&(AeronBufferDesc) {
			.size       = anim_records.count * (uint32_t)sizeof *anim_records.items,
			.usage      = AERON_BUFFER_USAGE_STORAGE,
			.debug_name = anim_name,
		});
		built          = s->anim_buffer != NULL;
	}
	if (!built ||
		!build_material_storage(s, model, set, core_layers, (const uint32_t (*)[3])anim, material_name,
								&material_entries, &material_bytes) ||
		!build_variant_storage(s, model, set, variant_name, &variant_values, &variant_bytes) ||
		!build_screen_storage(s, model, screen_name, &screen_rects, &screen_bytes)) {
		free(material_entries);
		free(variant_values);
		free(screen_rects);
		free(anim_records.items);
		free(anim);
		Aeron_LogError("aeron.scene", "%s: material storage build failed", name);
		AeronScene_MeshDestroy(s);
		return NULL;
	}
	free(anim);
	AeronBufferUploadDesc buffer_uploads[6];
	uint32_t              buffer_upload_count = 0;
	if (s->vbo) {
		buffer_uploads[buffer_upload_count++] =
			(AeronBufferUploadDesc) { .buffer = s->vbo, .data = model->vertices, .size = vbo_bytes };
		buffer_uploads[buffer_upload_count++] =
			(AeronBufferUploadDesc) { .buffer = s->ibo, .data = model->indices, .size = ibo_bytes };
	}
	buffer_uploads[buffer_upload_count++] = (AeronBufferUploadDesc) { .buffer = s->material_buffer,
																	  .data   = material_entries,
																	  .size   = material_bytes };
	buffer_uploads[buffer_upload_count++] = (AeronBufferUploadDesc) { .buffer = s->variant_buffer,
																	  .data   = variant_values,
																	  .size   = variant_bytes };
	if (s->screen_rect_buffer) {
		buffer_uploads[buffer_upload_count++] = (AeronBufferUploadDesc) { .buffer = s->screen_rect_buffer,
																		  .data   = screen_rects,
																		  .size   = screen_bytes };
	}
	if (s->anim_buffer) {
		buffer_uploads[buffer_upload_count++] =
			(AeronBufferUploadDesc) { .buffer = s->anim_buffer,
									  .data   = anim_records.items,
									  .size   = anim_records.count * (uint32_t)sizeof *anim_records.items };
	}
	const int buffers_uploaded = Aeron_UploadBufferBatchCmd(cmd, buffer_uploads, buffer_upload_count);
	free(material_entries);
	free(variant_values);
	free(screen_rects);
	free(anim_records.items);
	if (!buffers_uploaded) {
		Aeron_LogError("aeron.scene", "%s: geometry/material batch upload failed", name);
		AeronScene_MeshDestroy(s);
		return NULL;
	}

	/* ---- Articulation + bounds ---- */
	s->component_count = flight_model->component_count < AERON_MAX_MESH_COMPONENTS
							 ? flight_model->component_count
							 : AERON_MAX_MESH_COMPONENTS;
	if (s->component_count == 0) {
		s->component_count = 1;
	}
	for (uint32_t i = 0; i < s->component_count; ++i) {
		const AeronFlightComponent* component = &flight_model->components[i];
		AeronMeshRot*               rotation  = &s->component_rot[i];
		rotation->mesh_type                   = (uint8_t)component->mesh_type;
		if (component->has_rotation) {
			rotation->has_rotation = 1;
			rotation->pivot[0]     = component->rotation.pivot.x;
			rotation->pivot[1]     = component->rotation.pivot.y;
			rotation->pivot[2]     = component->rotation.pivot.z;
			rotation->axis[0]      = component->rotation.rotation_axis.x;
			rotation->axis[1]      = component->rotation.rotation_axis.y;
			rotation->axis[2]      = component->rotation.rotation_axis.z;
		}
		s->component_bound_min[i][0] = component->bounds.min.x;
		s->component_bound_min[i][1] = component->bounds.min.y;
		s->component_bound_min[i][2] = component->bounds.min.z;
		s->component_bound_max[i][0] = component->bounds.max.x;
		s->component_bound_max[i][1] = component->bounds.max.y;
		s->component_bound_max[i][2] = component->bounds.max.z;
	}
	for (uint32_t i = 0; i < s->component_count; i++) {
		if (s->component_rot[i].has_rotation) {
			s->has_any_rotation = true;
			break;
		}
	}
	s->bound_min[0] = flight_model->bounds.min.x;
	s->bound_min[1] = flight_model->bounds.min.y;
	s->bound_min[2] = flight_model->bounds.min.z;
	s->bound_max[0] = flight_model->bounds.max.x;
	s->bound_max[1] = flight_model->bounds.max.y;
	s->bound_max[2] = flight_model->bounds.max.z;
	if (status) {
		*status = AERON_SCENE_MESH_CREATE_SUCCESS;
	}
	return s;
}

AeronSceneMesh* AeronScene_MeshCreate(AeronCommandBuffer* cmd, const AeronFlightModel* model,
									  const char* debug_name, AeronSceneMeshCreateStatus* status) {
	return mesh_create(cmd, model, debug_name, NULL, NULL, NULL, status);
}

AeronSceneMesh* AeronScene_MeshCreateWithVariants(AeronCommandBuffer* cmd, const AeronFlightModel* model,
												  const char*                   debug_name,
												  const AeronSceneMeshVariants* variants,
												  AeronSceneMeshCreateStatus*   status) {
	return mesh_create(cmd, model, debug_name, variants, NULL, NULL, status);
}

AeronSceneMesh* AeronScene_MeshCreateEx(AeronCommandBuffer* cmd, const AeronSceneMeshCreateInfo* info,
										AeronSceneMeshCreateStatus* status) {
	if (!info) {
		if (status)
			*status = AERON_SCENE_MESH_CREATE_RESOURCE_FAILURE;
		return NULL;
	}
	return mesh_create(cmd, info->model, info->debug_name, info->variants, info->effect_atlas,
					   info->effect_data, status);
}
