/* The area meshes of a headset's eyes.
 *
 * The lenses of a headset never show the corners of an eye image. The
 * runtime gives the shape of that hidden area as a triangle mesh for each
 * eye, and the shape of the rest as a second mesh.
 *
 * - The scene draws the hidden-area mesh at the near depth before anything
 *   else, in the depth prepass and in the colour pass, so every pixel under
 *   it fails the depth test and isn't shaded.
 * - The full-screen passes draw the visible-area mesh in place of the whole
 *   target (AeronScene_FullscreenMesh), so they skip the same pixels.
 *
 * The meshes of all views are kept in one vertex buffer, already projected
 * to clip space. They change very rarely, so the buffer is only filled again
 * when a view's mesh or projection changes. */

#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void AeronSceneViewArea_Reset(struct AeronScene3D* s) { s->view_area_active = 0; }

/* Takes one mesh of a view's description. Returns whether it changed. */
static int view_area_set_mesh(AeronSceneViewAreaMesh* mesh, const float* xy, int vertex_count,
							  uint32_t generation) {
	const uint32_t count   = xy && vertex_count >= 3 ? (uint32_t)(vertex_count - vertex_count % 3) : 0;
	const int      changed = mesh->count != count || mesh->generation != generation;
	mesh->xy               = count ? xy : NULL;
	mesh->count            = count;
	mesh->generation       = generation;
	return changed;
}

void AeronSceneViewArea_SetView(struct AeronScene3D* s, int view, const AeronSceneViewDesc* desc) {
	AeronSceneViewArea* state         = &s->view_areas[view];
	const float         projection[4] = { desc->camera.h_half_rad, desc->camera.v_half_rad,
										  desc->camera.proj_x_offset, desc->camera.proj_y_offset };
	int                 changed       = memcmp(state->projection, projection, sizeof projection) != 0;
	changed |= view_area_set_mesh(&state->hidden, desc->hidden_area_xy, desc->hidden_area_vertex_count,
								  desc->hidden_area_generation);
	changed |= view_area_set_mesh(&state->visible, desc->visible_area_xy, desc->visible_area_vertex_count,
								  desc->visible_area_generation);
	if (changed) {
		s->view_area_dirty = 1;
	}
	memcpy(state->projection, projection, sizeof projection);
	s->view_area_active = 1;
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
	const AeronTextureFormat formats[2]   = { AERON_TEXTURE_FORMAT_R16G16_SNORM, s->color_format };
	const AeronSampleCount   samples[2]   = { AERON_SAMPLE_COUNT_1, s->sample_count };
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

/* Projects a mesh to clip space, into `clip` from vertex `first`. The mesh
 * is on the plane one unit in front of the eye, with X right and Y up. */
static uint32_t view_area_project(AeronSceneViewAreaMesh* mesh, const float projection[4], float* clip,
								  uint32_t first) {
	mesh->first = first;
	AeronView_PlaneToClip(mesh->xy, (int)mesh->count, projection[0], projection[1], projection[2],
						  projection[3], &clip[2 * first]);
	return first + mesh->count;
}

int AeronSceneViewArea_Prepare(struct AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s->view_area_active) {
		return 1;
	}
	uint32_t total  = 0;
	uint32_t hidden = 0;
	for (int view = 0; view < s->view_count; ++view) {
		total += s->view_areas[view].hidden.count + s->view_areas[view].visible.count;
		hidden += s->view_areas[view].hidden.count;
	}
	if (total == 0) {
		s->view_area_active = 0;
		return 1;
	}
	if (hidden > 0 && !hidden_area_ensure_pipelines(s)) {
		s->view_area_active = 0;
		return 0;
	}
	if (!s->view_area_dirty && s->view_area_vb) {
		return 1;
	}
	if (!s->view_area_vb || s->view_area_vb_vertices < total) {
		if (s->view_area_vb) {
			Aeron_DestroyBuffer(s->view_area_vb);
		}
		s->view_area_vb          = Aeron_CreateBuffer(&(AeronBufferDesc) {
			.size       = total * 2u * (uint32_t)sizeof(float),
			.usage      = AERON_BUFFER_USAGE_VERTEX,
			.debug_name = "scene.view_area",
		});
		s->view_area_vb_vertices = s->view_area_vb ? total : 0;
	}
	float* clip = (float*)malloc((size_t)total * 2u * sizeof(float));
	if (!s->view_area_vb || !clip) {
		free(clip);
		s->view_area_active = 0;
		return 0;
	}
	uint32_t first = 0;
	for (int view = 0; view < s->view_count; ++view) {
		AeronSceneViewArea* state = &s->view_areas[view];
		first                     = view_area_project(&state->hidden, state->projection, clip, first);
		first                     = view_area_project(&state->visible, state->projection, clip, first);
		state->fullscreen         = (AeronSceneFullscreenMesh) {
			.vertices = s->view_area_vb,
			.first    = state->visible.first,
			.count    = state->visible.count,
		};
	}
	const int ok =
		Aeron_UploadBufferDataCmd(cmd, s->view_area_vb, 0, clip, total * 2u * (uint32_t)sizeof(float));
	free(clip);
	if (!ok) {
		s->view_area_active = 0;
		return 0;
	}
	s->view_area_dirty = 0;
	return 1;
}

void AeronSceneViewArea_DrawHidden(struct AeronScene3D* s, AeronRenderPass* pass, int color_pass) {
	if (!s->view_area_active || !pass) {
		return;
	}
	const AeronSceneViewAreaMesh* mesh = &s->view_areas[s->current_view].hidden;
	if (mesh->count == 0 || !s->view_area_vb) {
		return;
	}
	Aeron_BindGraphicsPipeline(pass,
							   color_pass ? s->hidden_area_color_pipeline : s->hidden_area_prepass_pipeline);
	Aeron_BindVertexBuffer(pass, 0, s->view_area_vb, 0);
	Aeron_Draw(pass, mesh->count, mesh->first);
}

const AeronSceneFullscreenMesh* AeronScene_FullscreenMesh(AeronScene3D* s) {
	if (s && s->view_area_active && s->view_area_vb && !s->view_area_dirty) {
		const AeronSceneViewArea* state = &s->view_areas[s->current_view];
		if (state->visible.count > 0 && state->fullscreen.vertices == s->view_area_vb) {
			return &state->fullscreen;
		}
	}
	return AeronSceneFullscreen_Whole();
}

void AeronSceneViewArea_Release(struct AeronScene3D* s) {
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
	if (s->view_area_vb) {
		Aeron_DestroyBuffer(s->view_area_vb);
	}
	s->hidden_area_prepass_pipeline = NULL;
	s->hidden_area_color_pipeline   = NULL;
	s->hidden_area_vs               = NULL;
	s->hidden_area_ps               = NULL;
	s->view_area_vb                 = NULL;
	s->view_area_vb_vertices        = 0;
	s->view_area_active             = 0;
	s->view_area_dirty              = 1;
}
