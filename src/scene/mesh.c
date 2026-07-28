/*
 * AeronSceneMesh — GPU-resident model upload. See
 * aeron/scene/mesh.h.
 */

#include "aeron/scene/mesh.h"

#include "aeron/log.h"
#include "aeron/scene/image_cache.h"
#include "aeron/scene/ktx2_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char AeronPbrMaterialEntrySizeCheck[sizeof(AeronPbrMaterialEntry) == 128 ? 1 : -1];

static const char* channel_name(int c) {
	static const char* names[AERON_GLTF_CHANNEL_COUNT] = { "base_color", "normal",
														   "metallic_rough", "emissive" };
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
	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
		if (m->atlas[c]) {
			Aeron_DestroyTexture(m->atlas[c]);
		}
	}
	free(m->engine_glows);
	free(m->cpu_vertices);
	free(m->cpu_indices);
	free(m);
}

/* Copy factors and atlas transforms into the shader's 128-byte storage
 * record. The temporary array is released immediately after upload. */
static uint32_t populate_material_entries(AeronPbrMaterialEntry* entries,
										  const AeronGltfModel* src) {
	uint32_t n = src->material_count;
	if (n > AERON_GLTF_MAX_MATERIALS) {
		n = AERON_GLTF_MAX_MATERIALS;
	}
	memset(entries, 0, (size_t)(n ? n : 1u) * sizeof *entries);
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
		if (m->alpha_blend) {
			flags |= 0x8u; /* FS: alpha = tex.a * factor.a (blend prims) */
		}
		if (m->emissive_mode == AERON_GLTF_EMISSIVE_LEGACY_SRGB_SRCALPHA) {
			flags |= 0x10u; /* FS: legacy sRGB filtering + SRCALPHA composition */
		}
		e->flags = flags;
	}
	return n;
}

static int build_material_storage(AeronSceneMesh* s, const AeronGltfModel* model, const char* name,
								  AeronPbrMaterialEntry** out_entries, uint32_t* out_bytes) {
	const uint32_t allocation_count =
		model->material_count > 0
			? (model->material_count > AERON_GLTF_MAX_MATERIALS ? AERON_GLTF_MAX_MATERIALS
																 : model->material_count)
			: 1u;
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
	s->material_count = populate_material_entries(entries, model);
	const uint32_t bytes = allocation_count * (uint32_t)sizeof *entries;
	s->material_buffer = Aeron_CreateBuffer(&(AeronBufferDesc){
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

static int build_variant_storage(AeronSceneMesh* s, const AeronGltfModel* model, const char* name,
								 uint32_t** out_values, uint32_t* out_bytes) {
	uint32_t primitive_count = model->total_prim_count;
	if (!out_values || !out_bytes) {
		return 0;
	}
	*out_values = NULL;
	*out_bytes  = 0;
	if (primitive_count > 256u) {
		primitive_count = 256u;
	}
	const uint32_t groups = primitive_count > 0 ? (primitive_count + 3u) / 4u : 1u;
	const uint32_t rows   = model->variant_slots > 0 ? model->variant_slots : 1u;
	const size_t value_count = (size_t)groups * rows * 4u;
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
	s->variant_buffer = Aeron_CreateBuffer(&(AeronBufferDesc){
		.size       = bytes,
		.usage      = AERON_BUFFER_USAGE_STORAGE,
		.debug_name = name,
	});
	if (!s->variant_buffer) {
		free(values);
		return 0;
	}
	s->variant_groups_per_row = groups;
	*out_values = values;
	*out_bytes  = bytes;
	return 1;
}

AeronSceneMesh* AeronScene_MeshCreate(AeronCommandBuffer* cmd, const AeronGltfModel* model,
									  const char* debug_name,
									  AeronSceneMeshCreateStatus* status) {
	if (status) {
		*status = AERON_SCENE_MESH_CREATE_RESOURCE_FAILURE;
	}
	if (!cmd || !model) {
		return NULL;
	}
	const char*     name = debug_name ? debug_name : "<mesh>";
	Ktx2* channel_payloads[AERON_GLTF_CHANNEL_COUNT] = { 0 };
	int have_any_channel = 0;
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
		channel_payloads[c] =
			ktx2_open_mem_status(blob->data, blob->size, label, &open_status);
		if (!channel_payloads[c]) {
			if (status) {
				*status = open_status == KTX2_OPEN_RESOURCE_FAILURE
							  ? AERON_SCENE_MESH_CREATE_RESOURCE_FAILURE
							  : AERON_SCENE_MESH_CREATE_INVALID_SOURCE;
			}
			close_channel_payloads(channel_payloads);
			return NULL;
		}
	}

	AeronSceneMesh* s = (AeronSceneMesh*)calloc(1, sizeof *s);
	AeronPbrMaterialEntry* material_entries = NULL;
	uint32_t* variant_values = NULL;
	uint32_t material_bytes = 0;
	uint32_t variant_bytes = 0;
	uint32_t vbo_bytes = 0;
	uint32_t ibo_bytes = 0;
	if (!s) {
		close_channel_payloads(channel_payloads);
		return NULL;
	}

	/* ---- Merged VBO / IBO upload ---- */
	if (model->vertex_count > 0 && model->index_count > 0) {
		vbo_bytes = model->vertex_count * (uint32_t)sizeof(AeronGltfVertex);
		ibo_bytes = model->index_count * (uint32_t)sizeof(uint16_t);
		s->vbo             = Aeron_CreateBuffer(&(AeronBufferDesc){
						.usage = AERON_BUFFER_USAGE_VERTEX, .size = vbo_bytes });
		s->ibo             = Aeron_CreateBuffer(&(AeronBufferDesc){
						.usage = AERON_BUFFER_USAGE_INDEX, .size = ibo_bytes });
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

		s->cpu_vertices =
			(AeronSceneMeshCpuVertex*)malloc((size_t)model->vertex_count * sizeof *s->cpu_vertices);
		s->cpu_indices =
			(uint16_t*)malloc((size_t)model->index_count * sizeof *s->cpu_indices);
		if (!s->cpu_vertices || !s->cpu_indices) {
			Aeron_LogError("aeron.scene", "%s: retained geometry allocation failed", name);
			close_channel_payloads(channel_payloads);
			AeronScene_MeshDestroy(s);
			return NULL;
		}
		for (uint32_t vertex_index = 0; vertex_index < model->vertex_count; vertex_index++) {
			const AeronGltfVertex* source_vertex = &model->vertices[vertex_index];
			AeronSceneMeshCpuVertex* retained_vertex = &s->cpu_vertices[vertex_index];
			memcpy(retained_vertex->pos, source_vertex->pos, sizeof retained_vertex->pos);
			memcpy(retained_vertex->normal, source_vertex->normal, sizeof retained_vertex->normal);
			retained_vertex->mesh_index = source_vertex->mesh_index;
		}
		memcpy(s->cpu_indices, model->indices,
			   (size_t)model->index_count * sizeof *s->cpu_indices);
	}

	/* ---- Engine glows (owned copy; XWA OPTs) ---- */
	if (model->engine_glow_count > 0 && model->engine_glows) {
		s->engine_glows = (AeronGltfEngineGlow*)malloc(model->engine_glow_count *
													   sizeof *s->engine_glows);
		if (!s->engine_glows) {
			Aeron_LogError("aeron.scene", "%s: engine-glow allocation failed", name);
			close_channel_payloads(channel_payloads);
			AeronScene_MeshDestroy(s);
			return NULL;
		}
		memcpy(s->engine_glows, model->engine_glows,
			   model->engine_glow_count * sizeof *s->engine_glows);
		s->engine_glow_count = model->engine_glow_count;
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
		s->atlas[c] = Aeron_ImageUploadKtx2(cmd, channel_payloads[c], label);
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
	s->variant_count    = model->variant_count;
	s->variant_slots    = model->variant_slots;
	s->total_prim_count = model->total_prim_count;
	char material_name[512];
	char variant_name[512];
	snprintf(material_name, sizeof material_name, "%s.materials", name);
	snprintf(variant_name, sizeof variant_name, "%s.material_variants", name);
	if (!build_material_storage(s, model, material_name, &material_entries, &material_bytes) ||
		!build_variant_storage(s, model, variant_name, &variant_values, &variant_bytes)) {
		free(material_entries);
		free(variant_values);
		Aeron_LogError("aeron.scene", "%s: material storage build failed", name);
		AeronScene_MeshDestroy(s);
		return NULL;
	}
	AeronBufferUploadDesc buffer_uploads[4];
	uint32_t buffer_upload_count = 0;
	if (s->vbo) {
		buffer_uploads[buffer_upload_count++] = (AeronBufferUploadDesc){
			.buffer = s->vbo, .data = model->vertices, .size = vbo_bytes };
		buffer_uploads[buffer_upload_count++] = (AeronBufferUploadDesc){
			.buffer = s->ibo, .data = model->indices, .size = ibo_bytes };
	}
	buffer_uploads[buffer_upload_count++] = (AeronBufferUploadDesc){
		.buffer = s->material_buffer, .data = material_entries, .size = material_bytes };
	buffer_uploads[buffer_upload_count++] = (AeronBufferUploadDesc){
		.buffer = s->variant_buffer, .data = variant_values, .size = variant_bytes };
	const int buffers_uploaded = Aeron_UploadBufferBatchCmd(cmd, buffer_uploads, buffer_upload_count);
	free(material_entries);
	free(variant_values);
	if (!buffers_uploaded) {
		Aeron_LogError("aeron.scene", "%s: geometry/material batch upload failed", name);
		AeronScene_MeshDestroy(s);
		return NULL;
	}

	/* ---- Articulation + bounds ---- */
	memcpy(s->mesh_rot, model->mesh_rot, sizeof s->mesh_rot);
	for (uint32_t i = 0; i < AERON_MAX_MESH_SLOTS; i++) {
		if (s->mesh_rot[i].has_rotation) {
			s->has_any_rotation = true;
			break;
		}
	}
	memcpy(s->bound_min, model->bound_min, sizeof s->bound_min);
	memcpy(s->bound_max, model->bound_max, sizeof s->bound_max);
	s->bound_radius = model->bound_radius;
	if (status) {
		*status = AERON_SCENE_MESH_CREATE_SUCCESS;
	}
	return s;
}
