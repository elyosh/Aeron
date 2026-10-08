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

typedef char AeronPbrMaterialEntrySizeCheck[sizeof(AeronPbrMaterialEntry) == 160 ? 1 : -1];
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

/* Copy factors and atlas transforms into the shader's 160-byte storage
 * record. The temporary array is released immediately after upload. */
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
static uint32_t populate_material_entries(AeronPbrMaterialEntry* entries, const AeronGltfModel* src) {
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
			e->atlas[c] = (m->atlas_page[c] & 0xFFFu) | (m->atlas_max_lod[c] & 0xFu) << 12;
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
	}
	return n;
}

static int build_material_storage(AeronSceneMesh* s, const AeronGltfModel* model, const char* name,
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
	s->material_count    = populate_material_entries(entries, model);
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

static int build_variant_storage(AeronSceneMesh* s, const AeronGltfModel* model, const char* name,
								 uint32_t** out_values, uint32_t* out_bytes) {
	const uint32_t primitive_count = model->total_prim_count;
	if (!out_values || !out_bytes) {
		return 0;
	}
	*out_values = NULL;
	*out_bytes                 = 0;
	const uint32_t groups      = primitive_count > 0 ? (primitive_count + 3u) / 4u : 1u;
	const uint32_t rows        = model->variant_slots > 0 ? model->variant_slots : 1u;
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
	if (model->prim_variant_material && model->variant_slots > 0) {
		for (uint32_t variant = 0; variant < rows; ++variant) {
			for (uint32_t primitive = 0; primitive < primitive_count; ++primitive) {
				values[((size_t)variant * groups + primitive / 4u) * 4u + primitive % 4u] =
					model->prim_variant_material[(size_t)primitive * model->variant_slots + variant];
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

AeronSceneMesh* AeronScene_MeshCreate(AeronCommandBuffer* cmd, const AeronFlightModel* flight_model,
									  const char* debug_name, AeronSceneMeshCreateStatus* status) {
	if (status) {
		*status = AERON_SCENE_MESH_CREATE_RESOURCE_FAILURE;
	}
	if (!cmd || !flight_model) {
		return NULL;
	}
	const AeronGltfModel* model = &flight_model->render;
	const char*           name  = debug_name ? debug_name : "<mesh>";
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
		/* Every binding must name an existing layer of its atlas. */
		const uint32_t layers =
			ktx2_layer_count(channel_payloads[c]) > 0 ? (uint32_t)ktx2_layer_count(channel_payloads[c]) : 1u;
		for (uint32_t index = 0; index < model->material_count; ++index) {
			if (model->materials[index].atlas_page[c] >= layers) {
				Aeron_LogError("aeron.scene", "%s: material %u samples %s atlas page %u of %u", name, index,
							   channel_name(c), model->materials[index].atlas_page[c], layers);
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
			retained_vertex->mesh_index = source_vertex->mesh_index;
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
		s->atlas[c] = Aeron_ImageUploadKtx2Array(cmd, channel_payloads[c], label);
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
	s->variant_count              = model->variant_count;
	s->variant_slots              = model->variant_slots;
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
	AeronPbrScreenRect* screen_rects = NULL;
	uint32_t            screen_bytes = 0;
	snprintf(material_name, sizeof material_name, "%s.materials", name);
	snprintf(variant_name, sizeof variant_name, "%s.material_variants", name);
	snprintf(screen_name, sizeof screen_name, "%s.screen_rects", name);
	if (!build_material_storage(s, model, material_name, &material_entries, &material_bytes) ||
		!build_variant_storage(s, model, variant_name, &variant_values, &variant_bytes) ||
		!build_screen_storage(s, model, screen_name, &screen_rects, &screen_bytes)) {
		free(material_entries);
		free(variant_values);
		free(screen_rects);
		Aeron_LogError("aeron.scene", "%s: material storage build failed", name);
		AeronScene_MeshDestroy(s);
		return NULL;
	}
	AeronBufferUploadDesc buffer_uploads[5];
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
	const int buffers_uploaded = Aeron_UploadBufferBatchCmd(cmd, buffer_uploads, buffer_upload_count);
	free(material_entries);
	free(variant_values);
	free(screen_rects);
	if (!buffers_uploaded) {
		Aeron_LogError("aeron.scene", "%s: geometry/material batch upload failed", name);
		AeronScene_MeshDestroy(s);
		return NULL;
	}

	/* ---- Articulation + bounds ---- */
	s->slot_count = flight_model->component_count < AERON_MAX_MESH_SLOTS ? flight_model->component_count
																		 : AERON_MAX_MESH_SLOTS;
	if (s->slot_count == 0) {
		s->slot_count = 1;
	}
	for (uint32_t i = 0; i < s->slot_count; ++i) {
		const AeronFlightComponent* component = &flight_model->components[i];
		AeronMeshRot*               rotation  = &s->mesh_rot[i];
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
	}
	for (uint32_t i = 0; i < s->slot_count; i++) {
		if (s->mesh_rot[i].has_rotation) {
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
	const float ex  = fmaxf(fabsf(s->bound_min[0]), fabsf(s->bound_max[0]));
	const float ey  = fmaxf(fabsf(s->bound_min[1]), fabsf(s->bound_max[1]));
	const float ez  = fmaxf(fabsf(s->bound_min[2]), fabsf(s->bound_max[2]));
	s->bound_radius = sqrtf(ex * ex + ey * ey + ez * ez);
	if (status) {
		*status = AERON_SCENE_MESH_CREATE_SUCCESS;
	}
	return s;
}
