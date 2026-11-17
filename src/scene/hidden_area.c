/* The hidden-area mesh of a headset's eyes.
 *
 * The lenses of a headset never show the corners of an eye image. The
 * runtime gives the shape of that hidden area as a triangle mesh for each
 * eye. The scene draws it at the near depth before anything else, in the
 * depth prepass and in the colour pass, so every pixel under it fails the
 * depth test and isn't shaded.
 *
 * The meshes of all views are kept in one vertex buffer, already projected
 * to clip space. They change very rarely, so the buffer is only filled again
 * when a view's mesh or projection changes. */

#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void AeronSceneHiddenArea_Reset(struct AeronScene3D* s) { s->hidden_area_active = 0; }

void AeronSceneHiddenArea_SetView(struct AeronScene3D* s, int view, const AeronSceneViewDesc* desc) {
	AeronSceneHiddenAreaView* state = &s->hidden_area_views[view];
	const uint32_t count = desc->hidden_area_xy && desc->hidden_area_vertex_count >= 3
							   ? (uint32_t)(desc->hidden_area_vertex_count - desc->hidden_area_vertex_count % 3)
							   : 0;
	const float projection[4] = { tanf(desc->camera.h_half_rad), tanf(desc->camera.v_half_rad),
								  desc->camera.proj_x_offset, desc->camera.proj_y_offset };
	if (state->count != count || state->generation != desc->hidden_area_generation ||
		memcmp(state->projection, projection, sizeof projection) != 0) {
		s->hidden_area_dirty = 1;
	}
	state->xy         = count ? desc->hidden_area_xy : NULL;
	state->count      = count;
	state->generation = desc->hidden_area_generation;
	memcpy(state->projection, projection, sizeof projection);
	s->hidden_area_active = 1;
}

static int hidden_area_ensure_pipelines(struct AeronScene3D* s) {
	if (s->hidden_area_prepass_pipeline && s->hidden_area_color_pipeline) {
		return 1;
	}
	if (!s->hidden_area_vs) {
		s->hidden_area_vs =
			AeronSceneInternal_CompileShader("scene_hidden_area.vert", AERON_SHADER_STAGE_VERTEX, 0, 0, 0);
	}
	if (!s->hidden_area_ps) {
		s->hidden_area_ps =
			AeronSceneInternal_CompileShader("scene_hidden_area.frag", AERON_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	}
	if (!s->hidden_area_vs || !s->hidden_area_ps) {
		return 0;
	}
	static const AeronVertexBufferLayoutDesc layout    = { .slot = 0, .stride = 2 * sizeof(float) };
	static const AeronVertexAttributeDesc    attribute = { 0, 0, AERON_VERTEX_FORMAT_FLOAT2, 0 };
	/* Only depth is written: the colour targets keep their cleared values. */
	const AeronTextureFormat formats[2] = { AERON_TEXTURE_FORMAT_R16G16_SNORM, s->color_format };
	const AeronSampleCount   samples[2] = { AERON_SAMPLE_COUNT_1, s->sample_count };
	AeronGraphicsPipeline**  pipelines[2] = { &s->hidden_area_prepass_pipeline,
											  &s->hidden_area_color_pipeline };
	for (int i = 0; i < 2; ++i) {
		if (*pipelines[i]) {
			continue;
		}
		const AeronColorTargetStateDesc target = {
			.format = formats[i],
			.blend  = { .color_write_mask_enable = 1, .color_write_mask = 0 },
		};
		*pipelines[i] = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
			.vertex_shader       = s->hidden_area_vs,
			.fragment_shader     = s->hidden_area_ps,
			.primitive_type      = AERON_PRIMITIVE_TRIANGLES,
			.cull_mode           = AERON_CULL_NONE,
			.vertex_buffers      = &layout,
			.vertex_buffer_count = 1,
			.attributes          = &attribute,
			.attribute_count     = 1,
			.depth_format        = AERON_TEXTURE_FORMAT_D32_FLOAT,
			.depth               = { .depth_test = 1, .depth_write = 1, .compare = AERON_COMPARE_ALWAYS },
			.color_target_count  = 1,
			.color_targets       = &target,
			.sample_count        = samples[i],
		});
	}
	return s->hidden_area_prepass_pipeline && s->hidden_area_color_pipeline;
}

int AeronSceneHiddenArea_Prepare(struct AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s->hidden_area_active) {
		return 1;
	}
	uint32_t total = 0;
	for (int view = 0; view < s->view_count; ++view) {
		total += s->hidden_area_views[view].count;
	}
	if (total == 0) {
		s->hidden_area_active = 0;
		return 1;
	}
	if (!hidden_area_ensure_pipelines(s)) {
		s->hidden_area_active = 0;
		return 0;
	}
	if (!s->hidden_area_dirty && s->hidden_area_vb) {
		return 1;
	}
	if (!s->hidden_area_vb || s->hidden_area_vb_vertices < total) {
		if (s->hidden_area_vb) {
			Aeron_DestroyBuffer(s->hidden_area_vb);
		}
		s->hidden_area_vb = Aeron_CreateBuffer(&(AeronBufferDesc) {
			.size       = total * 2u * (uint32_t)sizeof(float),
			.usage      = AERON_BUFFER_USAGE_VERTEX,
			.debug_name = "scene.hidden_area",
		});
		s->hidden_area_vb_vertices = s->hidden_area_vb ? total : 0;
	}
	float* clip = (float*)malloc((size_t)total * 2u * sizeof(float));
	if (!s->hidden_area_vb || !clip) {
		free(clip);
		s->hidden_area_active = 0;
		return 0;
	}
	uint32_t first = 0;
	for (int view = 0; view < s->view_count; ++view) {
		AeronSceneHiddenAreaView* state = &s->hidden_area_views[view];
		state->first                    = first;
		/* The mesh is in the eye's tangent space: X right and Y up, one unit
		 * in front of the eye. The projection of scene_mat4_perspective
		 * takes it to clip space. */
		for (uint32_t i = 0; i < state->count; ++i) {
			clip[2 * (first + i) + 0] = state->xy[2 * i + 0] / state->projection[0] + state->projection[2];
			clip[2 * (first + i) + 1] = state->xy[2 * i + 1] / state->projection[1] + state->projection[3];
		}
		first += state->count;
	}
	const int ok =
		Aeron_UploadBufferDataCmd(cmd, s->hidden_area_vb, 0, clip, total * 2u * (uint32_t)sizeof(float));
	free(clip);
	if (!ok) {
		s->hidden_area_active = 0;
		return 0;
	}
	s->hidden_area_dirty = 0;
	return 1;
}

void AeronSceneHiddenArea_Draw(struct AeronScene3D* s, AeronRenderPass* pass, int color_pass) {
	if (!s->hidden_area_active || !pass) {
		return;
	}
	const AeronSceneHiddenAreaView* state = &s->hidden_area_views[s->current_view];
	if (state->count == 0 || !s->hidden_area_vb) {
		return;
	}
	Aeron_BindGraphicsPipeline(pass,
							   color_pass ? s->hidden_area_color_pipeline : s->hidden_area_prepass_pipeline);
	Aeron_BindVertexBuffer(pass, 0, s->hidden_area_vb, 0);
	Aeron_Draw(pass, state->count, state->first);
}

void AeronSceneHiddenArea_Release(struct AeronScene3D* s) {
	if (s->hidden_area_prepass_pipeline) {
		Aeron_DestroyGraphicsPipeline(s->hidden_area_prepass_pipeline);
	}
	if (s->hidden_area_color_pipeline) {
		Aeron_DestroyGraphicsPipeline(s->hidden_area_color_pipeline);
	}
	if (s->hidden_area_vs) {
		Aeron_DestroyShader(s->hidden_area_vs);
	}
	if (s->hidden_area_ps) {
		Aeron_DestroyShader(s->hidden_area_ps);
	}
	if (s->hidden_area_vb) {
		Aeron_DestroyBuffer(s->hidden_area_vb);
	}
	s->hidden_area_prepass_pipeline = NULL;
	s->hidden_area_color_pipeline   = NULL;
	s->hidden_area_vs               = NULL;
	s->hidden_area_ps               = NULL;
	s->hidden_area_vb               = NULL;
	s->hidden_area_vb_vertices      = 0;
	s->hidden_area_active           = 0;
}
