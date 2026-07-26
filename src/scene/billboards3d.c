/*
 * Batched scene billboards. Games submit world-space fans via
 * AeronScene_AddBillboard; this module owns batching (one dynamic VB
 * per frame, one draw per consecutive (texture, blend) run within a
 * stage), single-target color pipelines, and velocity stamping into the
 * motion-blur prepass for entries carrying previous-frame corners.
 *
 * Shaders: scene_billboard3d(.vert/.frag), scene_billboard3d_vel.
 */

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Interleaved vertex — both pipelines bind subsets of one layout. */
typedef struct Bb3dVert {
	float pos[3];
	float uv[2];
	float color[4];
	float bias;
	float prev[3];
} Bb3dVert; /* 52 B */

#define BB3D_VERTS_PER_ENTRY 12 /* 4-triangle fan, non-indexed */

void AeronScene_AddBillboard(AeronScene3D* s, const AeronSceneBillboardDesc* d) {
	if (!s || !d || !d->texture) {
		return;
	}
	if (!s->bb_entries) {
		s->bb_entries = (AeronSceneBb3dEntry*)malloc(AERON_SCENE_MAX_BILLBOARDS * sizeof *s->bb_entries);
		s->bb_verts   = malloc((size_t)AERON_SCENE_MAX_BILLBOARDS * BB3D_VERTS_PER_ENTRY * sizeof(Bb3dVert));
		if (!s->bb_entries || !s->bb_verts) {
			free(s->bb_entries);
			free(s->bb_verts);
			s->bb_entries = NULL;
			s->bb_verts   = NULL;
			return;
		}
	}
	if (s->bb_count >= AERON_SCENE_MAX_BILLBOARDS) {
		if (!s->bb_dropped) {
			fprintf(stderr, "[aeron_scene] billboard cap (%d) hit; dropping\n", AERON_SCENE_MAX_BILLBOARDS);
		}
		s->bb_dropped++;
		return;
	}
	AeronSceneBb3dEntry* e = &s->bb_entries[s->bb_count++];
	e->texture             = d->texture;
	e->blend               = (uint8_t)d->blend;
	e->stage               = (uint8_t)d->stage;
	e->depth_bias          = d->depth_bias_view;
	memcpy(e->corners, d->corners, sizeof e->corners);
	memcpy(e->uv, d->uv, sizeof e->uv);
	memcpy(e->colors, d->colors, sizeof e->colors);
	if (d->center_color) {
		memcpy(e->center_color, d->center_color, sizeof e->center_color);
	} else {
		for (int c = 0; c < 4; c++) {
			e->center_color[c] =
				0.25f * (d->colors[0][c] + d->colors[1][c] + d->colors[2][c] + d->colors[3][c]);
		}
	}
	e->has_prev = d->prev_corners != NULL;
	if (e->has_prev) {
		memcpy(e->prev_corners, d->prev_corners, sizeof e->prev_corners);
	} else {
		memcpy(e->prev_corners, d->corners, sizeof e->prev_corners);
	}
	memcpy(e->anchor, d->anchor_world, sizeof e->anchor);
}

static AeronGraphicsPipeline* bb3d_pipeline(struct AeronScene3D* s, AeronShader* vs, AeronShader* ps,
											int velocity_targets, int blend) {
	AeronVertexAttributeDesc attrs[5] = {
		{ .location    = 0,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT3,
		  .offset      = (uint32_t)offsetof(Bb3dVert, pos) },
		{ .location    = 1,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT2,
		  .offset      = (uint32_t)offsetof(Bb3dVert, uv) },
		{ .location    = 2,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT4,
		  .offset      = (uint32_t)offsetof(Bb3dVert, color) },
		{ .location    = 3,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT,
		  .offset      = (uint32_t)offsetof(Bb3dVert, bias) },
		{ .location    = 4,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT3,
		  .offset      = (uint32_t)offsetof(Bb3dVert, prev) },
	};
	AeronVertexBufferLayoutDesc vbd = { .slot = 0, .stride = (uint32_t)sizeof(Bb3dVert) };

	AeronColorTargetStateDesc cts[3] = { 0 };
	uint32_t                  num_targets;
	if (velocity_targets) {
		/* Temporal adds a masked depth-export target after normal + velocity. */
		cts[0].format                        = AERON_TEXTURE_FORMAT_R16G16_SNORM;
		cts[0].blend.color_write_mask_enable = 1;
		cts[0].blend.color_write_mask        = 0x0;
		cts[1].format                        = AERON_TEXTURE_FORMAT_R16G16_FLOAT;
		if (velocity_targets == 3) {
			cts[2].format                        = AERON_TEXTURE_FORMAT_R32_FLOAT;
			cts[2].blend.color_write_mask_enable = 1;
			cts[2].blend.color_write_mask        = 0x0;
		}
		num_targets = (uint32_t)velocity_targets;
	} else {
		cts[0].format = s->color_format;
		if (blend == AERON_SCENE_BILLBOARD_BLEND_ADDITIVE) {
			cts[0].blend = (AeronBlendStateDesc) { .enabled   = 1,
												   .src_color = AERON_BLEND_ONE,
												   .dst_color = AERON_BLEND_ONE,
												   .color_op  = AERON_BLEND_OP_ADD,
												   .src_alpha = AERON_BLEND_ZERO,
												   .dst_alpha = AERON_BLEND_ONE,
												   .alpha_op  = AERON_BLEND_OP_ADD };
		} else if (blend == AERON_SCENE_BILLBOARD_BLEND_PMA) {
			cts[0].blend = (AeronBlendStateDesc) { .enabled   = 1,
												   .src_color = AERON_BLEND_ONE,
												   .dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
												   .color_op  = AERON_BLEND_OP_ADD,
												   .src_alpha = AERON_BLEND_ONE,
												   .dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
												   .alpha_op  = AERON_BLEND_OP_ADD };
		} else {
			cts[0].blend = (AeronBlendStateDesc) { .enabled   = 1,
												   .src_color = AERON_BLEND_SRC_ALPHA,
												   .dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
												   .color_op  = AERON_BLEND_OP_ADD,
												   .src_alpha = AERON_BLEND_ONE,
												   .dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
												   .alpha_op  = AERON_BLEND_OP_ADD };
		}
		num_targets = 1;
	}

	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader       = vs,
		.fragment_shader     = ps,
		.primitive_type      = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode           = AERON_CULL_NONE,
		.vertex_buffers      = &vbd,
		.vertex_buffer_count = 1,
		.attributes          = attrs,
		.attribute_count     = 5,
		.depth_format        = AERON_TEXTURE_FORMAT_D32_FLOAT,
		/* Reversed-Z test against scene depth, never write — the flat
		 * per-quad depth (see the VS) mirrors classic sprite sorting. */
		.depth              = { .depth_test = 1, .depth_write = 0, .compare = AERON_COMPARE_GREATER_EQUAL },
		.color_target_count = num_targets,
		.color_targets      = cts,
		.sample_count       = velocity_targets ? AERON_SAMPLE_COUNT_1 : s->sample_count,
	});
}

/* LENS pipeline: single scene-format target, NO depth attachment (the
 * lens pass leaves depth unbound so the VS can sample it), same
 * vertex layout / blend variants as the color pipelines, the regular
 * billboard FS. */
static AeronGraphicsPipeline* bb3d_lens_pipeline(struct AeronScene3D* s, int blend) {
	AeronVertexAttributeDesc attrs[5] = {
		{ .location    = 0,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT3,
		  .offset      = (uint32_t)offsetof(Bb3dVert, pos) },
		{ .location    = 1,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT2,
		  .offset      = (uint32_t)offsetof(Bb3dVert, uv) },
		{ .location    = 2,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT4,
		  .offset      = (uint32_t)offsetof(Bb3dVert, color) },
		{ .location    = 3,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT,
		  .offset      = (uint32_t)offsetof(Bb3dVert, bias) },
		{ .location    = 4,
		  .buffer_slot = 0,
		  .format      = AERON_VERTEX_FORMAT_FLOAT3,
		  .offset      = (uint32_t)offsetof(Bb3dVert, prev) },
	};
	AeronVertexBufferLayoutDesc vbd = { .slot = 0, .stride = (uint32_t)sizeof(Bb3dVert) };

	AeronColorTargetStateDesc cts[1] = { 0 };
	cts[0].format                    = s->color_format;
	if (blend == AERON_SCENE_BILLBOARD_BLEND_ADDITIVE) {
		cts[0].blend = (AeronBlendStateDesc) { .enabled   = 1,
											   .src_color = AERON_BLEND_ONE,
											   .dst_color = AERON_BLEND_ONE,
											   .color_op  = AERON_BLEND_OP_ADD,
											   .src_alpha = AERON_BLEND_ZERO,
											   .dst_alpha = AERON_BLEND_ONE,
											   .alpha_op  = AERON_BLEND_OP_ADD };
	} else if (blend == AERON_SCENE_BILLBOARD_BLEND_PMA) {
		cts[0].blend = (AeronBlendStateDesc) { .enabled   = 1,
											   .src_color = AERON_BLEND_ONE,
											   .dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
											   .color_op  = AERON_BLEND_OP_ADD,
											   .src_alpha = AERON_BLEND_ONE,
											   .dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
											   .alpha_op  = AERON_BLEND_OP_ADD };
	} else {
		cts[0].blend = (AeronBlendStateDesc) { .enabled   = 1,
											   .src_color = AERON_BLEND_SRC_ALPHA,
											   .dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
											   .color_op  = AERON_BLEND_OP_ADD,
											   .src_alpha = AERON_BLEND_ONE,
											   .dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
											   .alpha_op  = AERON_BLEND_OP_ADD };
	}

	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader       = s->bb_lens_vs,
		.fragment_shader     = s->bb_lens_fs,
		.primitive_type      = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode           = AERON_CULL_NONE,
		.vertex_buffers      = &vbd,
		.vertex_buffer_count = 1,
		.attributes          = attrs,
		.attribute_count     = 5,
		.depth_format        = AERON_TEXTURE_FORMAT_UNKNOWN,
		.color_target_count  = 1,
		.color_targets       = cts,
	});
}

static int bb3d_lens_ensure(struct AeronScene3D* s) {
	if (s->bb_lens_tried) {
		return s->bb_lens_pipes[0] != NULL;
	}
	s->bb_lens_tried = 1;
	/* Occlusion samples depth in the FRAGMENT stage (the proven
	 * binding path — vertex-stage depth sampling reads zero on some
	 * backends); the VS only projects the anchor. */
	s->bb_lens_vs =
		AeronSceneInternal_CompileShader("scene_billboard3d_lens.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 0);
	s->bb_lens_fs =
		AeronSceneInternal_CompileShader("scene_billboard3d_lens.frag", AERON_SHADER_STAGE_FRAGMENT, 2, 1, 0);
	s->bb_depth_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_NEAREST,
		.mag_filter = AERON_FILTER_NEAREST,
		.mip_filter = AERON_FILTER_NEAREST,
		.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
	});
	if (!s->bb_lens_vs || !s->bb_lens_fs || !s->bb_depth_sampler) {
		fprintf(stderr, "[aeron_scene] billboard3d lens shader/sampler create failed\n");
		return 0;
	}
	s->bb_lens_pipes[AERON_SCENE_BILLBOARD_BLEND_ALPHA] =
		bb3d_lens_pipeline(s, AERON_SCENE_BILLBOARD_BLEND_ALPHA);
	s->bb_lens_pipes[AERON_SCENE_BILLBOARD_BLEND_ADDITIVE] =
		bb3d_lens_pipeline(s, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE);
	s->bb_lens_pipes[AERON_SCENE_BILLBOARD_BLEND_PMA] =
		bb3d_lens_pipeline(s, AERON_SCENE_BILLBOARD_BLEND_PMA);
	if (!s->bb_lens_pipes[0] || !s->bb_lens_pipes[1] || !s->bb_lens_pipes[2]) {
		fprintf(stderr, "[aeron_scene] billboard3d lens pipeline create failed\n");
	}
	return s->bb_lens_pipes[0] != NULL;
}

static int bb3d_ensure(struct AeronScene3D* s) {
	if (s->bb_tried) {
		return s->bb_pipes[0] != NULL;
	}
	s->bb_tried = 1;
	s->bb_vs = AeronSceneInternal_CompileShader("scene_billboard3d.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 0);
	s->bb_fs =
		AeronSceneInternal_CompileShader("scene_billboard3d.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 0, 0);
	s->bb_vel_vs =
		AeronSceneInternal_CompileShader("scene_billboard3d_vel.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 0);
	s->bb_vel_fs =
		AeronSceneInternal_CompileShader("scene_billboard3d_vel.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 0, 0);
	s->bb_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_LINEAR,
		.mag_filter = AERON_FILTER_LINEAR,
		.mip_filter = AERON_FILTER_LINEAR,
		.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.max_lod    = 1000.0f,
	});
	if (!s->bb_vs || !s->bb_fs || !s->bb_sampler) {
		fprintf(stderr, "[aeron_scene] billboard3d shader/sampler create failed\n");
		return 0;
	}
	s->bb_pipes[AERON_SCENE_BILLBOARD_BLEND_ALPHA] =
		bb3d_pipeline(s, s->bb_vs, s->bb_fs, 0, AERON_SCENE_BILLBOARD_BLEND_ALPHA);
	s->bb_pipes[AERON_SCENE_BILLBOARD_BLEND_ADDITIVE] =
		bb3d_pipeline(s, s->bb_vs, s->bb_fs, 0, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE);
	s->bb_pipes[AERON_SCENE_BILLBOARD_BLEND_PMA] =
		bb3d_pipeline(s, s->bb_vs, s->bb_fs, 0, AERON_SCENE_BILLBOARD_BLEND_PMA);
	if (s->bb_vel_vs && s->bb_vel_fs) {
		s->bb_vel_pipe          = bb3d_pipeline(s, s->bb_vel_vs, s->bb_vel_fs, 2, 0);
		s->bb_temporal_vel_pipe = bb3d_pipeline(s, s->bb_vel_vs, s->bb_vel_fs, 3, 0);
	}
	if (!s->bb_pipes[0] || !s->bb_pipes[1] || !s->bb_pipes[2]) {
		fprintf(stderr, "[aeron_scene] billboard3d pipeline create failed\n");
	}
	return s->bb_pipes[0] != NULL;
}

/* Emit one entry's 12 fan vertices (center + rim, triangles C-c0-c1,
 * C-c1-c2, C-c2-c3, C-c3-c0; cull is off so winding is free). */
static void bb3d_emit(Bb3dVert* v, const AeronSceneBb3dEntry* e) {
	Bb3dVert corner[5]; /* [0] = center */
	for (int a = 0; a < 3; a++) {
		corner[0].pos[a] =
			0.25f * (e->corners[0][a] + e->corners[1][a] + e->corners[2][a] + e->corners[3][a]);
		corner[0].prev[a] = 0.25f * (e->prev_corners[0][a] + e->prev_corners[1][a] + e->prev_corners[2][a] +
									 e->prev_corners[3][a]);
	}
	corner[0].uv[0] = 0.25f * (e->uv[0][0] + e->uv[1][0] + e->uv[2][0] + e->uv[3][0]);
	corner[0].uv[1] = 0.25f * (e->uv[0][1] + e->uv[1][1] + e->uv[2][1] + e->uv[3][1]);
	memcpy(corner[0].color, e->center_color, sizeof corner[0].color);
	corner[0].bias = e->depth_bias;
	for (int c = 0; c < 4; c++) {
		memcpy(corner[1 + c].pos, e->corners[c], sizeof corner[0].pos);
		memcpy(corner[1 + c].prev, e->prev_corners[c], sizeof corner[0].prev);
		memcpy(corner[1 + c].uv, e->uv[c], sizeof corner[0].uv);
		memcpy(corner[1 + c].color, e->colors[c], sizeof corner[0].color);
		corner[1 + c].bias = e->depth_bias;
	}
	for (int t = 0; t < 4; t++) {
		v[t * 3 + 0] = corner[0];
		v[t * 3 + 1] = corner[1 + t];
		v[t * 3 + 2] = corner[1 + ((t + 1) & 3)];
	}
}

int AeronSceneBb3d_Prepare(struct AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s || s->bb_count == 0) {
		return 0;
	}
	if (!bb3d_ensure(s) || !s->bb_verts) {
		return 0;
	}
	Bb3dVert* verts      = (Bb3dVert*)s->bb_verts;
	s->bb_frame_verts    = 0;
	s->bb_frame_has_vel  = 0;
	s->bb_frame_has_lens = 0;
	for (int i = 0; i < s->bb_count; i++) {
		bb3d_emit(&verts[s->bb_frame_verts], &s->bb_entries[i]);
		s->bb_frame_verts += BB3D_VERTS_PER_ENTRY;
		if (s->bb_entries[i].has_prev && s->bb_entries[i].stage == AERON_SCENE_BILLBOARD_STAGE_OVERLAY) {
			s->bb_frame_has_vel = 1;
		}
		if (s->bb_entries[i].stage == AERON_SCENE_BILLBOARD_STAGE_LENS) {
			s->bb_frame_has_lens = 1;
		}
	}
	const uint32_t need = s->bb_frame_verts * (uint32_t)sizeof(Bb3dVert);
	if (!s->bb_vb || s->bb_vb_cap < need) {
		if (s->bb_vb) {
			Aeron_DestroyBuffer(s->bb_vb);
		}
		uint32_t cap = s->bb_vb_cap ? s->bb_vb_cap : 64u * 1024u;
		while (cap < need) {
			cap *= 2u;
		}
		s->bb_vb     = Aeron_CreateBuffer(&(AeronBufferDesc) {
			.size = cap, .usage = AERON_BUFFER_USAGE_VERTEX, .debug_name = "scene.billboards.vertices" });
		s->bb_vb_cap = s->bb_vb ? cap : 0;
	}
	if (!s->bb_vb || !Aeron_UploadBufferDataCmd(cmd, s->bb_vb, 0, verts, need)) {
		s->bb_frame_verts = 0;
		return 0;
	}
	return 1;
}

/* Walk the entry list for `stage`, batching consecutive same-state
 * runs. `velocity` selects the prepass pipeline + prev-corner filter. */
static void bb3d_draw(struct AeronScene3D* s, AeronRenderPass* pass, int stage, int velocity) {
	if (!s || !pass || s->bb_frame_verts == 0 || !s->bb_vb) {
		return;
	}
	Aeron_BindVertexBuffer(pass, 0, s->bb_vb, 0);

	if (velocity) {
		AeronGraphicsPipeline* velocity_pipe = s->temporal_active ? s->bb_temporal_vel_pipe : s->bb_vel_pipe;
		if (!velocity_pipe) {
			return;
		}
		Aeron_BindGraphicsPipeline(pass, velocity_pipe);
		struct {
			float view_proj[16];
			float unjittered_view_proj[16];
			float prev_view_proj[16];
			float params[4];
		} u;
		memcpy(u.view_proj, s->jittered_view_proj, sizeof u.view_proj);
		memcpy(u.unjittered_view_proj, s->unjittered_view_proj, sizeof u.unjittered_view_proj);
		memcpy(u.prev_view_proj, s->mb_prev_view_proj, sizeof u.prev_view_proj);
		u.params[0] = s->temporal_active || s->post.mb_camera_blur ? 1.0f : 0.0f;
		u.params[1] = u.params[2] = u.params[3] = 0.0f;
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &u, sizeof u);
	} else {
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, s->jittered_view_proj, 16 * sizeof(float));
	}

	AeronTexture*          run_tex   = NULL;
	int                    run_blend = -1;
	uint32_t               run_first = 0, run_count = 0;
	AeronGraphicsPipeline* bound =
		velocity ? (s->temporal_active ? s->bb_temporal_vel_pipe : s->bb_vel_pipe) : NULL;

	for (int i = 0; i <= s->bb_count; i++) {
		const AeronSceneBb3dEntry* e   = i < s->bb_count ? &s->bb_entries[i] : NULL;
		const uint32_t             fv  = (uint32_t)i * BB3D_VERTS_PER_ENTRY;
		const int                  use = e && e->stage == stage && (!velocity || e->has_prev);
		if (use && e->texture == run_tex && (velocity || (int)e->blend == run_blend) && run_count &&
			fv == run_first + run_count) {
			run_count += BB3D_VERTS_PER_ENTRY;
			continue;
		}
		if (run_count) {
			Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0, run_tex, s->pbr_sampler);
			Aeron_Draw(pass, run_count, run_first);
			run_count = 0;
		}
		if (use) {
			if (!velocity && (int)e->blend != run_blend) {
				AeronGraphicsPipeline* p = e->blend < 3 ? s->bb_pipes[e->blend] : NULL;
				if (!p) {
					run_blend = -1;
					continue;
				}
				if (p != bound) {
					Aeron_BindGraphicsPipeline(pass, p);
					bound = p;
				}
				run_blend = (int)e->blend;
			}
			run_tex   = e->texture;
			run_first = fv;
			run_count = BB3D_VERTS_PER_ENTRY;
		}
	}
}

void AeronSceneBb3d_DrawStage(struct AeronScene3D* s, AeronRenderPass* pass, AeronSceneBillboardStage stage) {
	bb3d_draw(s, pass, (int)stage, /*velocity=*/0);
}

void AeronSceneBb3d_DrawVelocity(struct AeronScene3D* s, AeronRenderPass* pass) {
	if (s && s->bb_frame_has_vel) {
		bb3d_draw(s, pass, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, /*velocity=*/1);
	}
}

void AeronSceneBb3d_DrawLens(struct AeronScene3D* s, AeronRenderPass* pass) {
	if (!s || !pass || !s->bb_frame_has_lens || s->bb_frame_verts == 0 || !s->bb_vb || !bb3d_lens_ensure(s)) {
		return;
	}
	AeronTexture* depth_tex = Aeron_DepthTargetGetTexture(s->depth_rt);
	if (!depth_tex) {
		return;
	}
	Aeron_BindVertexBuffer(pass, 0, s->bb_vb, 0);

	struct {
		float view_proj[16];
		float anchor_world[4];
	} vu;
	memcpy(vu.view_proj, s->unjittered_view_proj, sizeof vu.view_proj);
	float fu[4];
	fu[0] = 2.0f; /* kernel radius, texels */
	fu[1] = (float)s->render_w;
	fu[2] = (float)s->render_h;
	fu[3] = 0.0f;

	int bound_blend = -1;
	for (int i = 0; i < s->bb_count; i++) {
		const AeronSceneBb3dEntry* e = &s->bb_entries[i];
		if (e->stage != AERON_SCENE_BILLBOARD_STAGE_LENS) {
			continue;
		}
		if ((int)e->blend != bound_blend) {
			AeronGraphicsPipeline* p = e->blend < 3 ? s->bb_lens_pipes[e->blend] : NULL;
			if (!p) {
				continue;
			}
			Aeron_BindGraphicsPipeline(pass, p);
			Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, fu, sizeof fu);
			Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 1, depth_tex, s->bb_depth_sampler);
			bound_blend = (int)e->blend;
		}
		vu.anchor_world[0] = e->anchor[0];
		vu.anchor_world[1] = e->anchor[1];
		vu.anchor_world[2] = e->anchor[2];
		vu.anchor_world[3] = 1.0f;
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &vu, sizeof vu);
		Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0, e->texture, s->bb_sampler);
		Aeron_Draw(pass, BB3D_VERTS_PER_ENTRY, (uint32_t)i * BB3D_VERTS_PER_ENTRY);
	}
}

void AeronSceneBb3d_Release(struct AeronScene3D* s) {
	if (!s) {
		return;
	}
	for (int i = 0; i < 3; i++) {
		if (s->bb_pipes[i]) {
			Aeron_DestroyGraphicsPipeline(s->bb_pipes[i]);
			s->bb_pipes[i] = NULL;
		}
	}
	if (s->bb_vel_pipe)
		Aeron_DestroyGraphicsPipeline(s->bb_vel_pipe);
	if (s->bb_temporal_vel_pipe)
		Aeron_DestroyGraphicsPipeline(s->bb_temporal_vel_pipe);
	for (int i = 0; i < 3; i++) {
		if (s->bb_lens_pipes[i]) {
			Aeron_DestroyGraphicsPipeline(s->bb_lens_pipes[i]);
			s->bb_lens_pipes[i] = NULL;
		}
	}
	if (s->bb_vs)
		Aeron_DestroyShader(s->bb_vs);
	if (s->bb_fs)
		Aeron_DestroyShader(s->bb_fs);
	if (s->bb_vel_vs)
		Aeron_DestroyShader(s->bb_vel_vs);
	if (s->bb_vel_fs)
		Aeron_DestroyShader(s->bb_vel_fs);
	if (s->bb_lens_vs)
		Aeron_DestroyShader(s->bb_lens_vs);
	if (s->bb_lens_fs)
		Aeron_DestroyShader(s->bb_lens_fs);
	if (s->bb_sampler)
		Aeron_DestroySampler(s->bb_sampler);
	if (s->bb_depth_sampler)
		Aeron_DestroySampler(s->bb_depth_sampler);
	if (s->bb_vb)
		Aeron_DestroyBuffer(s->bb_vb);
	free(s->bb_entries);
	free(s->bb_verts);
	s->bb_entries = NULL;
	s->bb_verts   = NULL;
}
