/* Generic articulated mesh-overlay pass. See mesh_overlay.h. */

#include "internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct OverlayVertexUniforms {
	float    view_proj[16];
	float    transform[16];
	float    params[4];
	uint32_t mesh_table_index;
	uint32_t _pad[3];
} OverlayVertexUniforms;
typedef char OverlayVertexUniformsSizeCheck[sizeof(OverlayVertexUniforms) == 160 ? 1 : -1];

typedef struct OverlayFragmentUniforms {
	float uv_xform[4];
	float uv_rect[4];
	float color[4];
} OverlayFragmentUniforms;

void AeronScene_AddMeshOverlay(AeronScene3D* scene, const AeronSceneMeshOverlayDesc* overlay) {
	if (!scene || !overlay || !overlay->vertices || overlay->vertex_count == 0 || !overlay->texture) {
		return;
	}
	if (scene->overlay_count >= AERON_SCENE_MAX_MESH_OVERLAYS ||
		overlay->vertex_count > AERON_SCENE_MAX_OVERLAY_VERTS - scene->overlay_vertex_count) {
		if (!scene->overlays_dropped) {
			fprintf(stderr, "[aeron_scene] mesh-overlay cap hit; dropping\n");
		}
		scene->overlays_dropped++;
		return;
	}
	const uint32_t required_vertex_count = scene->overlay_vertex_count + overlay->vertex_count;
	if (required_vertex_count > scene->overlay_vertex_cap) {
		uint32_t new_capacity = scene->overlay_vertex_cap ? scene->overlay_vertex_cap : 4096u;
		while (new_capacity < required_vertex_count) {
			new_capacity *= 2u;
		}
		if (new_capacity > AERON_SCENE_MAX_OVERLAY_VERTS) {
			new_capacity = AERON_SCENE_MAX_OVERLAY_VERTS;
		}
		void* resized_vertices =
			realloc(scene->overlay_vertices, (size_t)new_capacity * sizeof *scene->overlay_vertices);
		if (!resized_vertices) {
			scene->overlays_dropped++;
			return;
		}
		scene->overlay_vertices   = (AeronSceneMeshOverlayVertex*)resized_vertices;
		scene->overlay_vertex_cap = new_capacity;
	}
	AeronSceneMeshOverlayEntry* entry = &scene->overlays[scene->overlay_count++];
	memset(entry, 0, sizeof *entry);
	entry->texture      = overlay->texture;
	entry->mesh_table   = overlay->mesh_table;
	entry->first_vertex = scene->overlay_vertex_count;
	entry->vertex_count = overlay->vertex_count;
	entry->blend        = overlay->blend < 3 ? overlay->blend : AERON_SCENE_MESH_OVERLAY_BLEND_ALPHA;
	entry->cull_mode    = overlay->cull_mode <= AERON_CULL_BACK ? overlay->cull_mode : AERON_CULL_NONE;
	entry->depth_bias   = overlay->depth_bias_view;
	memcpy(entry->transform, overlay->transform, sizeof entry->transform);
	memcpy(entry->uv_xform, overlay->uv_xform, sizeof entry->uv_xform);
	memcpy(entry->uv_rect, overlay->uv_rect, sizeof entry->uv_rect);
	memcpy(entry->color, overlay->color, sizeof entry->color);
	if (entry->color[0] == 0.0f && entry->color[1] == 0.0f && entry->color[2] == 0.0f &&
		entry->color[3] == 0.0f) {
		entry->color[0] = entry->color[1] = entry->color[2] = entry->color[3] = 1.0f;
	}
	memcpy(&scene->overlay_vertices[scene->overlay_vertex_count], overlay->vertices,
		   (size_t)overlay->vertex_count * sizeof *overlay->vertices);
	scene->overlay_vertex_count = required_vertex_count;
}

static AeronGraphicsPipeline* create_overlay_pipeline(AeronScene3D* scene, int blend_mode,
													  AeronCullMode cull_mode) {
	AeronVertexAttributeDesc vertex_attributes[3] = {
		{ .location    = 0,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT3,
		  .offset      = (uint32_t)offsetof(AeronSceneMeshOverlayVertex, pos) },
		{ .location    = 1,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT2,
		  .offset      = (uint32_t)offsetof(AeronSceneMeshOverlayVertex, uv) },
		{ .location    = 2,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT,
		  .offset      = (uint32_t)offsetof(AeronSceneMeshOverlayVertex, mesh_index) },
	};
	AeronVertexBufferLayoutDesc vertex_buffer_layout = { .slot = 0,
														 .stride =
															 (uint32_t)sizeof(AeronSceneMeshOverlayVertex) };
	AeronColorTargetStateDesc   color_targets[1]     = { 0 };
	color_targets[0].format                          = scene->color_format;
	if (blend_mode == AERON_SCENE_MESH_OVERLAY_BLEND_ADDITIVE) {
		color_targets[0].blend = (AeronBlendStateDesc) { .enabled   = 1,
														 .src_color = AERON_BLEND_ONE,
														 .dst_color = AERON_BLEND_ONE,
														 .color_op  = AERON_BLEND_OP_ADD,
														 .src_alpha = AERON_BLEND_ZERO,
														 .dst_alpha = AERON_BLEND_ONE,
														 .alpha_op  = AERON_BLEND_OP_ADD };
	} else if (blend_mode == AERON_SCENE_MESH_OVERLAY_BLEND_PMA) {
		color_targets[0].blend = (AeronBlendStateDesc) { .enabled   = 1,
														 .src_color = AERON_BLEND_ONE,
														 .dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
														 .color_op  = AERON_BLEND_OP_ADD,
														 .src_alpha = AERON_BLEND_ONE,
														 .dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
														 .alpha_op  = AERON_BLEND_OP_ADD };
	} else {
		color_targets[0].blend = (AeronBlendStateDesc) { .enabled   = 1,
														 .src_color = AERON_BLEND_SRC_ALPHA,
														 .dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
														 .color_op  = AERON_BLEND_OP_ADD,
														 .src_alpha = AERON_BLEND_ONE,
														 .dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
														 .alpha_op  = AERON_BLEND_OP_ADD };
	}
	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader       = scene->overlay_vs,
		.fragment_shader     = scene->overlay_fs,
		.primitive_type      = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode           = cull_mode,
		.vertex_buffers      = &vertex_buffer_layout,
		.vertex_buffer_count = 1,
		.attributes          = vertex_attributes,
		.attribute_count     = 3,
		.depth_format        = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.depth               = { .depth_test = 1, .depth_write = 0, .compare = AERON_COMPARE_GREATER_EQUAL },
		.color_target_count  = 1,
		.color_targets       = color_targets,
		.sample_count        = scene->sample_count,
	});
}

static int ensure_overlay_resources(AeronScene3D* scene) {
	if (scene->overlay_tried) {
		return scene->overlay_pipes[0][0] != NULL;
	}
	scene->overlay_tried = 1;
	scene->overlay_vs =
		AeronSceneInternal_CompileShader("scene_mesh_overlay.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 1);
	scene->overlay_fs =
		AeronSceneInternal_CompileShader("scene_mesh_overlay.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
	if (!scene->overlay_vs || !scene->overlay_fs) {
		return 0;
	}
	for (int blend_mode = 0; blend_mode < 3; blend_mode++) {
		for (int cull_mode = 0; cull_mode < 3; cull_mode++) {
			scene->overlay_pipes[blend_mode][cull_mode] =
				create_overlay_pipeline(scene, blend_mode, (AeronCullMode)cull_mode);
		}
	}
	return scene->overlay_pipes[0][0] != NULL;
}

int AeronSceneMeshOverlay_Prepare(AeronScene3D* scene, AeronCommandBuffer* command_buffer) {
	if (!scene || scene->overlay_vertex_count == 0 || !ensure_overlay_resources(scene)) {
		return 0;
	}
	scene->overlay_frame_ready      = 0;
	const uint32_t vertex_data_size = scene->overlay_vertex_count * (uint32_t)sizeof *scene->overlay_vertices;
	if (!scene->overlay_vb || scene->overlay_vb_cap < vertex_data_size) {
		if (scene->overlay_vb) {
			Aeron_DestroyBuffer(scene->overlay_vb);
		}
		uint32_t new_capacity = scene->overlay_vb_cap ? scene->overlay_vb_cap : 64u * 1024u;
		while (new_capacity < vertex_data_size) {
			new_capacity *= 2u;
		}
		scene->overlay_vb =
			Aeron_CreateBuffer(&(AeronBufferDesc) { .size       = new_capacity,
													.usage      = AERON_BUFFER_USAGE_VERTEX,
													.debug_name = "scene.mesh_overlays.vertices" });
		scene->overlay_vb_cap = scene->overlay_vb ? new_capacity : 0;
	}
	scene->overlay_frame_ready =
		scene->overlay_vb && Aeron_UploadBufferDataCmd(command_buffer, scene->overlay_vb, 0,
													   scene->overlay_vertices, vertex_data_size);
	return scene->overlay_frame_ready;
}

void AeronSceneMeshOverlay_Draw(AeronScene3D* scene, AeronRenderPass* render_pass) {
	if (!scene || !render_pass || !scene->storage_ready || !scene->overlay_frame_ready || !scene->overlay_vb ||
		scene->overlay_count == 0) {
		return;
	}
	Aeron_BindVertexBuffer(render_pass, 0, scene->overlay_vb, 0);
	Aeron_BindStorageBuffer(render_pass, AERON_SHADER_STAGE_VERTEX, 0, scene->mesh_table_buffer);
	for (uint32_t overlay_index = 0; overlay_index < scene->overlay_count; overlay_index++) {
		const AeronSceneMeshOverlayEntry* overlay  = &scene->overlays[overlay_index];
		AeronGraphicsPipeline*            pipeline = scene->overlay_pipes[overlay->blend][overlay->cull_mode];
		if (!pipeline) {
			continue;
		}
		Aeron_BindGraphicsPipeline(render_pass, pipeline);
		OverlayVertexUniforms vertex_uniforms;
		memset(&vertex_uniforms, 0, sizeof vertex_uniforms);
		memcpy(vertex_uniforms.view_proj, scene->jittered_view_proj, sizeof vertex_uniforms.view_proj);
		memcpy(vertex_uniforms.transform, overlay->transform, sizeof vertex_uniforms.transform);
		vertex_uniforms.params[0]         = overlay->depth_bias;
		vertex_uniforms.mesh_table_index = overlay->mesh_table_index;
		Aeron_BindUniformData(render_pass, AERON_SHADER_STAGE_VERTEX, 0, &vertex_uniforms,
							  sizeof vertex_uniforms);
		OverlayFragmentUniforms fragment_uniforms;
		memcpy(fragment_uniforms.uv_xform, overlay->uv_xform, sizeof fragment_uniforms.uv_xform);
		memcpy(fragment_uniforms.uv_rect, overlay->uv_rect, sizeof fragment_uniforms.uv_rect);
		memcpy(fragment_uniforms.color, overlay->color, sizeof fragment_uniforms.color);
		Aeron_BindUniformData(render_pass, AERON_SHADER_STAGE_FRAGMENT, 0, &fragment_uniforms,
							  sizeof fragment_uniforms);
		Aeron_BindTextureSampler(render_pass, AERON_SHADER_STAGE_FRAGMENT, 0, overlay->texture,
								 scene->pbr_sampler);
		Aeron_Draw(render_pass, overlay->vertex_count, overlay->first_vertex);
	}
}

void AeronSceneMeshOverlay_Release(AeronScene3D* scene) {
	if (!scene) {
		return;
	}
	for (int blend_mode = 0; blend_mode < 3; blend_mode++) {
		for (int cull_mode = 0; cull_mode < 3; cull_mode++) {
			if (scene->overlay_pipes[blend_mode][cull_mode]) {
				Aeron_DestroyGraphicsPipeline(scene->overlay_pipes[blend_mode][cull_mode]);
			}
		}
	}
	if (scene->overlay_vs) {
		Aeron_DestroyShader(scene->overlay_vs);
	}
	if (scene->overlay_fs) {
		Aeron_DestroyShader(scene->overlay_fs);
	}
	if (scene->overlay_vb) {
		Aeron_DestroyBuffer(scene->overlay_vb);
	}
	free(scene->overlay_vertices);
}
