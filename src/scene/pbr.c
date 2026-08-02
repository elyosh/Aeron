/*
 * aeron_scene "pbr" material class — shaders, pipelines, and the
 * per-instance draw walk for AeronSceneMesh instances. Games retain
 * snapshot semantics such as eligibility, mesh-table construction,
 * and light-pool culling, then submit generic instances.
 *
 * Shader ABI (scene_pbr_mesh, scene_pbr_prepass, and stamp variants):
 *   VS b0 space1: AeronScenePbrVsBlock (transforms + storage selection)
 *   VS storage 0: packed current/previous articulation tables
 *   VS storage 1: packed per-instance local lights
 *   FS storage 0: per-mesh material entries
 *   FS storage 1: per-mesh packed variant map
 *   FS storage 2: frame point lights
 *   FS storage 3: clustered-light headers
 *   FS storage 4: clustered-light indices
 *   FS b0 space3: AeronSceneDirectionalShadowUniform
 *   FS b1 space3: fixed PBR environment and tuning
 *   FS b2 space3: clustered-light grid and camera data
 *   FS t0..t3: channel atlases, t4: AO, t5/t6: compared/raw directional shadow depth
 */

#include "internal.h"

#include <string.h>

/* Per-instance VS cbuffer (b0 space1) — mirrors `cbuffer
 * GltfMeshVSUniforms` in scene_pbr_mesh.vert.hlsl. */
typedef struct AeronScenePbrVsBlock {
	float    view_proj[16];
	float    unjittered_view_proj[16];
	float    model_to_world[16];
	uint32_t variant_row_base;
	uint32_t variant_group_count;
	uint32_t material_count;
	uint32_t camera_mb;
	float    base_color_emissive_strength;
	uint32_t receive_shadow;
	uint32_t screen_shadow;
	uint32_t current_table_index;
	uint32_t previous_table_index;
	uint32_t local_light_base;
	uint32_t local_light_count;
	uint32_t _pad_storage;
	float    prev_view_proj[16];
	float    prev_model_to_world[16];
} AeronScenePbrVsBlock; /* 368 B */
typedef char AeronScenePbrVsBlockSizeCheck[sizeof(AeronScenePbrVsBlock) == 368 ? 1 : -1];

static AeronShader* pbr_shader(const char* name, AeronShaderStage stage, uint32_t samplers,
							   uint32_t ubs, uint32_t sbs) {
	AeronShader* sh = Aeron_CreateShader(&(AeronShaderDesc) {
		.name = name, .stage = stage, .sampler_count = samplers,
		.uniform_buffer_count = ubs, .storage_buffer_count = sbs });
	if (!sh) {
		Aeron_LogError("aeron.scene", "PBR shader load failed: %s", name);
	}
	return sh;
}

static void pbr_vertex_input(AeronVertexAttributeDesc attrs[6], AeronVertexBufferLayoutDesc* vbd) {
	attrs[0] = (AeronVertexAttributeDesc) { .location    = 0,
											.buffer_slot = 0,
											.format      = AERON_VERTEX_FORMAT_FLOAT3,
											.offset      = (uint32_t)offsetof(AeronGltfVertex, pos) };
	attrs[1] = (AeronVertexAttributeDesc) { .location    = 1,
											.buffer_slot = 0,
											.format      = AERON_VERTEX_FORMAT_FLOAT3,
											.offset      = (uint32_t)offsetof(AeronGltfVertex, normal) };
	attrs[2] = (AeronVertexAttributeDesc) { .location    = 2,
											.buffer_slot = 0,
											.format      = AERON_VERTEX_FORMAT_FLOAT4,
											.offset      = (uint32_t)offsetof(AeronGltfVertex, tangent) };
	attrs[3] = (AeronVertexAttributeDesc) { .location    = 3,
											.buffer_slot = 0,
											.format      = AERON_VERTEX_FORMAT_FLOAT2,
											.offset      = (uint32_t)offsetof(AeronGltfVertex, uv) };
	attrs[4] = (AeronVertexAttributeDesc) { .location    = 4,
											.buffer_slot = 0,
											.format      = AERON_VERTEX_FORMAT_FLOAT,
											.offset      = (uint32_t)offsetof(AeronGltfVertex, mesh_index) };
	attrs[5] = (AeronVertexAttributeDesc) { .location    = 5,
											.buffer_slot = 0,
											.format      = AERON_VERTEX_FORMAT_UINT,
											.offset      = (uint32_t)offsetof(AeronGltfVertex, prim_id) };
	*vbd     = (AeronVertexBufferLayoutDesc) { .slot = 0, .stride = (uint32_t)sizeof(AeronGltfVertex) };
}

typedef struct PbrTargetDesc {
	AeronTextureFormat format;
	uint8_t            write_mask; /* 0 = declared-but-masked */
} PbrTargetDesc;

static AeronGraphicsPipeline* pbr_pipeline(AeronShader* vs, AeronShader* ps, const PbrTargetDesc* targets,
										   uint32_t num_targets, int depth_test, int depth_write,
										   AeronCompareOp compare, AeronCullMode cull, int blend,
										   AeronSampleCount sample_count) {
	AeronVertexAttributeDesc    attrs[6];
	AeronVertexBufferLayoutDesc vbd;
	pbr_vertex_input(attrs, &vbd);

	AeronColorTargetStateDesc cts[3] = { 0 };
	for (uint32_t i = 0; i < num_targets; ++i) {
		cts[i].format                        = targets[i].format;
		cts[i].blend.enabled                 = 0;
		cts[i].blend.color_write_mask_enable = 1;
		cts[i].blend.color_write_mask        = targets[i].write_mask;
	}
	if (blend) {
		/* Alpha-BLEND range (canopy glass): classic transparent OPT
		 * faces alpha-blend over the already-drawn hull. Color target 0
		 * blends; dst alpha keeps coverage (ONE / INV_SRC_ALPHA). */
		cts[0].blend.enabled   = 1;
		cts[0].blend.src_color = AERON_BLEND_SRC_ALPHA;
		cts[0].blend.dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA;
		cts[0].blend.color_op  = AERON_BLEND_OP_ADD;
		cts[0].blend.src_alpha = AERON_BLEND_ONE;
		cts[0].blend.dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA;
		cts[0].blend.alpha_op  = AERON_BLEND_OP_ADD;
	}

	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader       = vs,
		.fragment_shader     = ps,
		.primitive_type      = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode           = cull,
		.vertex_buffers      = &vbd,
		.vertex_buffer_count = 1,
		.attributes          = attrs,
		.attribute_count     = 6,
		.depth_format        = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.depth               = { .depth_test = depth_test, .depth_write = depth_write, .compare = compare },
		.color_target_count  = num_targets,
		.color_targets       = cts,
		.sample_count        = sample_count,
	});
}

/* Create the (kind, cull) variant. Kind fixes shaders, targets and
 * depth state; cull is free per variant. */
static AeronGraphicsPipeline* pbr_pipeline_create(struct AeronScene3D* s, int kind, AeronCullMode cull,
												  AeronShader* color_fs) {
	const PbrTargetDesc normal_rw = { AERON_TEXTURE_FORMAT_R16G16_SNORM, 0xF };
	const PbrTargetDesc normal_wm = { AERON_TEXTURE_FORMAT_R16G16_SNORM, 0x0 };
	const PbrTargetDesc vel_rw    = { AERON_TEXTURE_FORMAT_R16G16_FLOAT, 0xF };
	const PbrTargetDesc depth_rw  = { AERON_TEXTURE_FORMAT_R32_FLOAT, 0x1 };
	const PbrTargetDesc color_rw  = { s->color_format, 0xF };
	const PbrTargetDesc pre1[1]   = { normal_rw };
	const PbrTargetDesc pre2[2]   = { normal_rw, vel_rw };
	const PbrTargetDesc pre3[3]   = { normal_rw, vel_rw, depth_rw };

	switch (kind) {
		case AERON_PBR_PIPE_MESH:
			return pbr_pipeline(s->pbr_vs, color_fs, &color_rw, 1, 1, 1, AERON_COMPARE_GREATER_EQUAL, cull, 0,
								s->sample_count);
		case AERON_PBR_PIPE_FORWARD:
			return pbr_pipeline(s->pbr_vs, color_fs, &color_rw, 1, 1, s->sample_count != AERON_SAMPLE_COUNT_1,
								s->sample_count != AERON_SAMPLE_COUNT_1 ? AERON_COMPARE_GREATER_EQUAL
																		: AERON_COMPARE_EQUAL,
								cull, 0, s->sample_count);
		case AERON_PBR_PIPE_PREPASS:
			return s->pbr_prepass_vs && s->pbr_prepass_fs
					   ? pbr_pipeline(s->pbr_prepass_vs, s->pbr_prepass_fs, pre1, 1, 1, 1,
									  AERON_COMPARE_GREATER_EQUAL, cull, 0, AERON_SAMPLE_COUNT_1)
					   : NULL;
		case AERON_PBR_PIPE_PREPASS_VEL:
			return s->pbr_prepass_vs && s->pbr_prepass_fs
					   ? pbr_pipeline(s->pbr_prepass_vs, s->pbr_prepass_fs, pre2, 2, 1, 1,
									  AERON_COMPARE_GREATER_EQUAL, cull, 0, AERON_SAMPLE_COUNT_1)
					   : NULL;
		case AERON_PBR_PIPE_PREPASS_STAMP:
			/* Velocity stamp of alpha-BLEND ranges (instance velocity_stamp
			 * flag): 2-RT velocity-prepass layout with the normal target
			 * write-masked, depth GE test against the laid opaque depth, NO
			 * write; the FS alpha-tests the base-color atlas so only the
			 * geometry's visible texels receive motion vectors. */
			if (!s->pbr_prepass_stamp_vs || !s->pbr_prepass_stamp_fs) {
				return NULL;
			}
			{
				const PbrTargetDesc stamp2[2] = { normal_wm, vel_rw };
				return pbr_pipeline(s->pbr_prepass_stamp_vs, s->pbr_prepass_stamp_fs, stamp2, 2, 1, 0,
									AERON_COMPARE_GREATER_EQUAL, cull, 0, AERON_SAMPLE_COUNT_1);
			}
		case AERON_PBR_PIPE_PREPASS_TEMPORAL:
			return s->pbr_prepass_vs && s->pbr_prepass_fs
					   ? pbr_pipeline(s->pbr_prepass_vs, s->pbr_prepass_fs, pre3, 3, 1, 1,
									  AERON_COMPARE_GREATER_EQUAL, cull, 0, AERON_SAMPLE_COUNT_1)
					   : NULL;
		/* Blend ranges: depth test GE (also for the deferred FORWARD path —
		 * glass is excluded from the depth prepass, so EQUAL cannot match),
		 * no depth write, alpha blending on. */
		case AERON_PBR_PIPE_MESH_BLEND:
			return pbr_pipeline(s->pbr_vs, color_fs, &color_rw, 1, 1, 0, AERON_COMPARE_GREATER_EQUAL, cull, 1,
								s->sample_count);
		case AERON_PBR_PIPE_FORWARD_BLEND:
			return pbr_pipeline(s->pbr_vs, color_fs, &color_rw, 1, 1, 0, AERON_COMPARE_GREATER_EQUAL, cull, 1,
								s->sample_count);
		default:
			return NULL;
	}
}

AeronGraphicsPipeline* AeronScenePbr_Pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull) {
	if (kind < 0 || kind >= AERON_PBR_PIPE_KIND_COUNT || (unsigned)cull > AERON_CULL_BACK) {
		return NULL;
	}
	if (!s->pbr_pipes[kind][cull]) {
		s->pbr_pipes[kind][cull] = pbr_pipeline_create(s, kind, cull, s->pbr_fs);
		if (!s->pbr_pipes[kind][cull] && cull != AERON_CULL_NONE) {
			Aeron_LogWarn("aeron.scene", "PBR pipeline creation failed (kind %d, cull %d); using no-cull variant",
						  kind, (int)cull);
			return s->pbr_pipes[kind][AERON_CULL_NONE];
		}
	}
	return s->pbr_pipes[kind][cull];
}

#ifdef AERON_DEBUG_UI
static int pbr_debug_color_kind(int kind) {
	return kind == AERON_PBR_PIPE_MESH || kind == AERON_PBR_PIPE_FORWARD ||
		   kind == AERON_PBR_PIPE_MESH_BLEND || kind == AERON_PBR_PIPE_FORWARD_BLEND;
}

static int pbr_debug_active(const struct AeronScene3D* s) {
	return s->pbr_debug_views || s->directional_shadow.debug_cascades;
}

static AeronGraphicsPipeline* pbr_debug_pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull) {
	if (!pbr_debug_color_kind(kind) || !pbr_debug_active(s)) {
		return NULL;
	}
	if (!s->pbr_debug_tried) {
		s->pbr_debug_tried = 1;
		s->pbr_debug_fs =
			pbr_shader("scene_pbr_mesh_debug.frag", AERON_SHADER_STAGE_FRAGMENT, 7, 3, 5);
	}
	if (!s->pbr_debug_fs) {
		return NULL;
	}
	if (!s->pbr_debug_pipes[kind][cull]) {
		s->pbr_debug_pipes[kind][cull] = pbr_pipeline_create(s, kind, cull, s->pbr_debug_fs);
		if (!s->pbr_debug_pipes[kind][cull] && cull != AERON_CULL_NONE) {
			return pbr_debug_pipeline(s, kind, AERON_CULL_NONE);
		}
	}
	return s->pbr_debug_pipes[kind][cull];
}
#endif

static AeronGraphicsPipeline* pbr_draw_pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull) {
#ifdef AERON_DEBUG_UI
	AeronGraphicsPipeline* debug = pbr_debug_pipeline(s, kind, cull);
	if (debug) {
		return debug;
	}
#endif
	return AeronScenePbr_Pipeline(s, kind, cull);
}

int AeronScenePbr_Ensure(struct AeronScene3D* s) {
	if (s->pbr_tried) {
		return s->pbr_pipes[AERON_PBR_PIPE_MESH][AERON_CULL_NONE] != NULL;
	}
	s->pbr_tried = 1;

	s->pbr_vs         = pbr_shader("scene_pbr_mesh.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 2);
	s->pbr_prepass_vs =
		pbr_shader("scene_pbr_prepass.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 1);
	s->pbr_prepass_stamp_vs =
		pbr_shader("scene_pbr_prepass_stamp.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 1);
	s->pbr_fs         = pbr_shader("scene_pbr_mesh.frag", AERON_SHADER_STAGE_FRAGMENT, 7, 3, 5);
	s->pbr_prepass_fs = pbr_shader("scene_pbr_prepass.frag", AERON_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	/* Velocity stamping reads the same mesh-owned material resources. */
	s->pbr_prepass_stamp_fs =
		pbr_shader("scene_pbr_prepass_stamp.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 0, 2);
	if (!s->pbr_vs || !s->pbr_fs) {
		return 0;
	}

	/* Cull-NONE variants up front (long-standing default); other cull
	 * modes are created lazily via AeronScenePbr_Pipeline. */
	for (int kind = 0; kind < AERON_PBR_PIPE_KIND_COUNT; kind++) {
		s->pbr_pipes[kind][AERON_CULL_NONE] = pbr_pipeline_create(s, kind, AERON_CULL_NONE, s->pbr_fs);
	}
	if (!s->pbr_pipes[AERON_PBR_PIPE_MESH][AERON_CULL_NONE]) {
		Aeron_LogError("aeron.scene", "PBR pipeline creation failed");
	}
	return s->pbr_pipes[AERON_PBR_PIPE_MESH][AERON_CULL_NONE] != NULL;
}

void AeronScenePbr_Release(struct AeronScene3D* s) {
	for (int kind = 0; kind < AERON_PBR_PIPE_KIND_COUNT; kind++) {
		for (int cull = 0; cull < 3; cull++) {
			if (s->pbr_pipes[kind][cull]) {
				Aeron_DestroyGraphicsPipeline(s->pbr_pipes[kind][cull]);
				s->pbr_pipes[kind][cull] = NULL;
			}
#ifdef AERON_DEBUG_UI
			if (s->pbr_debug_pipes[kind][cull]) {
				Aeron_DestroyGraphicsPipeline(s->pbr_debug_pipes[kind][cull]);
				s->pbr_debug_pipes[kind][cull] = NULL;
			}
#endif
		}
	}
	if (s->pbr_vs)
		Aeron_DestroyShader(s->pbr_vs);
	if (s->pbr_prepass_vs)
		Aeron_DestroyShader(s->pbr_prepass_vs);
	if (s->pbr_prepass_stamp_vs)
		Aeron_DestroyShader(s->pbr_prepass_stamp_vs);
	if (s->pbr_fs)
		Aeron_DestroyShader(s->pbr_fs);
	if (s->pbr_prepass_fs)
		Aeron_DestroyShader(s->pbr_prepass_fs);
	if (s->pbr_prepass_stamp_fs)
		Aeron_DestroyShader(s->pbr_prepass_stamp_fs);
#ifdef AERON_DEBUG_UI
	if (s->pbr_debug_fs)
		Aeron_DestroyShader(s->pbr_debug_fs);
	s->pbr_debug_fs    = NULL;
	s->pbr_debug_tried = 0;
#endif
}

/* Identity mesh table pushed for instances without a custom one. */
const AeronSceneMeshTable* AeronScenePbr_IdentityTable(void);
const AeronSceneMeshTable* AeronScenePbr_IdentityTable(void) {
	static AeronSceneMeshTable t;
	static int                 built;
	if (!built) {
		for (int mi = 0; mi < AERON_MAX_MESH_SLOTS; ++mi) {
			t.rows[mi][0][0]                     = 1.0f;
			t.rows[mi][1][1]                     = 1.0f;
			t.rows[mi][2][2]                     = 1.0f;
			t.visibility_packed[mi >> 2][mi & 3] = 1.0f;
			t.emissive_packed[mi >> 2][mi & 3]   = 1.0f;
		}
		built = 1;
	}
	return &t;
}

/* Per-instance uniform pushes shared by the opaque walk and blend sweep. */
static void pbr_push_instance_uniforms(struct AeronScene3D* s, AeronRenderPass* pass,
									   const AeronSceneMeshInstance* in, const AeronSceneMesh* m,
									   const AeronScenePreparedInstance* prepared,
									   int velocity, int depth_only, int screen_shadow) {
	const int            vel = velocity && !in->zero_velocity;
	AeronScenePbrVsBlock vsb;
	memset(&vsb, 0, sizeof vsb);
	memcpy(vsb.view_proj, s->jittered_view_proj, sizeof vsb.view_proj);
	memcpy(vsb.unjittered_view_proj, s->unjittered_view_proj, sizeof vsb.unjittered_view_proj);
	memcpy(vsb.model_to_world, in->transform, sizeof vsb.model_to_world);
	memcpy(vsb.prev_view_proj, vel ? s->mb_prev_view_proj : s->unjittered_view_proj,
		   sizeof vsb.prev_view_proj);
	memcpy(vsb.prev_model_to_world, vel ? in->prev_transform : in->transform, sizeof vsb.prev_model_to_world);
	uint32_t variant = in->variant;
	if (variant >= m->variant_slots) {
		variant = m->variant_slots ? m->variant_slots - 1u : 0u;
	}
	vsb.variant_row_base             = variant * m->variant_groups_per_row;
	vsb.variant_group_count          = m->variant_groups_per_row;
	vsb.material_count               = m->material_count;
	vsb.camera_mb                    = (velocity && (s->temporal_active || s->post.mb_camera_blur)) ? 1u : 0u;
	vsb.base_color_emissive_strength = in->base_color_emissive_strength;
	vsb.receive_shadow       = (in->shadow_flags & AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW) == 0 ? 1u : 0u;
	const int receiver_local = (in->shadow_flags & AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW) != 0;
	vsb.screen_shadow        = screen_shadow && vsb.receive_shadow && !receiver_local ? 1u : 0u;
	vsb.current_table_index  = prepared->current_table_index;
	vsb.previous_table_index = vel ? prepared->previous_table_index
									 : prepared->current_table_index;
	vsb.local_light_base     = prepared->local_light_base;
	vsb.local_light_count    = prepared->local_light_count;
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &vsb, sizeof vsb);

	(void)depth_only;
}

/* Blend-range pipeline kind for a base kind (-1 = range not drawn by
 * this pass — the depth prepasses exclude transparent prims). */
static int pbr_blend_kind(int pipe_kind) {
	switch (pipe_kind) {
		case AERON_PBR_PIPE_MESH:
			return AERON_PBR_PIPE_MESH_BLEND;
		case AERON_PBR_PIPE_FORWARD:
			return AERON_PBR_PIPE_FORWARD_BLEND;
		default:
			return -1;
	}
}

static void pbr_bind_ao(struct AeronScene3D* s, AeronRenderPass* pass, AeronTexture* ao_tex) {
	AeronTexture* ao = ao_tex ? ao_tex : AeronSceneInternal_WhiteTexture();
	if (ao) {
		Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 4, ao,
								 ao_tex ? s->post_linear_sampler : s->pbr_sampler);
	}
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 2,
							s->point_light_buffer);
	AeronSceneClusteredLights_Bind(s, pass);
}

int AeronScenePbr_DrawInstances(struct AeronScene3D* s, AeronCommandBuffer* cmd, AeronRenderPass* pass,
								int pipe_kind, int depth_only, int velocity, AeronTexture* ao_tex) {
	(void)cmd;
	if (!s || !pass || !s->storage_ready) {
		return 0;
	}
	if (s->instance_count == 0) {
		return 1;
	}
	AeronGraphicsPipeline* pipeline = pbr_draw_pipeline(s, pipe_kind, AERON_CULL_NONE);
	if (!pipeline) {
		return 0;
	}
	AeronCullMode bound_cull = AERON_CULL_NONE;
	Aeron_BindGraphicsPipeline(pass, pipeline);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, s->mesh_table_buffer);
	if (!depth_only) {
		Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 1, s->local_light_buffer);
	}
	/* AO input at t4 (pbr FS declares it unconditionally); a white 1x1
	 * placeholder keeps AO reading as 1 when SSAO is off. Prepass FS
	 * samples nothing. */
	if (!depth_only) {
		pbr_bind_ao(s, pass, ao_tex);
		AeronSceneDirectionalShadow_Bind(s, pass);
	}

	AeronSampler* atlas_sampler = s->mesh_sampler ? s->mesh_sampler : s->pbr_sampler;
	const int     screen_shadow = !depth_only && pipe_kind == AERON_PBR_PIPE_FORWARD && ao_tex &&
								  s->shadow_stats.active && !s->directional_shadow.debug_cascades;

	const AeronSceneMesh* bound_mesh                  = NULL;
	int                   bound_receiver_local_shadow = 0;
	for (int i = 0; i < s->instance_count; ++i) {
		const AeronSceneMeshInstance* in = &s->instances[i];
		const AeronSceneMesh*         m  = in->mesh;
		if (!m || !m->vbo || !m->ibo || m->index_count == 0) {
			continue;
		}
		if (!depth_only) {
			const int receiver_local_shadow =
				(in->shadow_flags & AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW) != 0;
			if (receiver_local_shadow != bound_receiver_local_shadow) {
				AeronSceneDirectionalShadow_BindForInstance(s, pass, in->shadow_flags);
				bound_receiver_local_shadow = receiver_local_shadow;
			}
		}

		/* Per-instance cull variant — rebind only on change (resource
		 * bindings persist across pipeline binds within the pass). */
		const AeronCullMode cull = (AeronCullMode)in->cull_mode;
		if (cull != bound_cull) {
			AeronGraphicsPipeline* p = pbr_draw_pipeline(s, pipe_kind, cull);
			if (!p) {
				return 0;
			}
			if (p != pipeline) {
				pipeline = p;
				Aeron_BindGraphicsPipeline(pass, pipeline);
			}
			bound_cull = cull;
		}

		if (m != bound_mesh) {
			bound_mesh = m;
			Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
			Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT16, 0);
			if (!depth_only) {
				for (uint32_t c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
					/* Unauthored channel → 1x1 white; the FS gates on the
					 * material flags so it is never actually read, but the
					 * slot must be bound. */
					Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, c,
											 m->atlas[c] ? m->atlas[c] : AeronSceneInternal_WhiteTexture(),
											 atlas_sampler);
				}
				Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 0, m->material_buffer);
				Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 1, m->variant_buffer);
			}
		}

		/* Velocity prepass projects through the previous camera; other
		 * passes use prev = current (zero velocity, VS prev unused). A
		 * zero_velocity instance stays prev = current even there. */
		pbr_push_instance_uniforms(s, pass, in, m, &s->prepared_instances[i],
								   velocity, depth_only, screen_shadow);

		/* Opaque range only; the alpha-BLEND tail draws in the sweep
		 * below (and never in the depth prepasses). */
		Aeron_DrawIndexed(pass, m->opaque_index_count, 0, 0);
	}

	/* ---- Velocity STAMP sweep (velocity prepass only): the alpha-BLEND
	 * ranges of velocity_stamp instances, after ALL opaque prepass
	 * geometry so the GE depth test sees the complete laid depth. Writes
	 * motion vectors for the visible (alpha-tested) texels only; depth
	 * and normal stay untouched. */
	if (depth_only && velocity) {
		bound_mesh                        = NULL;
		bound_cull                        = AERON_CULL_NONE;
		AeronGraphicsPipeline* stamp_pipe = NULL;
		for (int i = 0; i < s->instance_count; ++i) {
			const AeronSceneMeshInstance* in = &s->instances[i];
			const AeronSceneMesh*         m  = in->mesh;
			if (!in->velocity_stamp || !m || !m->vbo || !m->ibo || m->opaque_index_count >= m->index_count) {
				continue;
			}
			const AeronCullMode cull = (AeronCullMode)in->cull_mode;
			if (!stamp_pipe || cull != bound_cull) {
				AeronGraphicsPipeline* p = AeronScenePbr_Pipeline(s, AERON_PBR_PIPE_PREPASS_STAMP, cull);
				if (!p) {
					return 0;
				}
				if (p != stamp_pipe) {
					stamp_pipe = p;
					Aeron_BindGraphicsPipeline(pass, stamp_pipe);
				}
				bound_cull = cull;
			}
			if (m != bound_mesh) {
				bound_mesh = m;
				Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
				Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT16, 0);
				Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0,
										 m->atlas[0] ? m->atlas[0] : AeronSceneInternal_WhiteTexture(),
										 atlas_sampler);
				Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 0, m->material_buffer);
				Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 1, m->variant_buffer);
			}
			pbr_push_instance_uniforms(s, pass, in, m, &s->prepared_instances[i],
									   /*velocity=*/1, /*depth_only=*/1,
									   /*screen_shadow=*/0);
			Aeron_DrawIndexed(pass, m->index_count - m->opaque_index_count, m->opaque_index_count, 0);
		}
	}
	return 1;
}

int AeronScenePbr_DrawTransparentInstances(struct AeronScene3D* s, AeronRenderPass* pass, int pipe_kind,
										   AeronTexture* ao_tex) {
	const int blend_kind = pbr_blend_kind(pipe_kind);
	if (!s || !pass || blend_kind < 0 || !s->storage_ready) {
		return 0;
	}
	if (s->instance_count == 0) {
		return 1;
	}
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, s->mesh_table_buffer);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 1, s->local_light_buffer);
	pbr_bind_ao(s, pass, ao_tex);
	AeronSceneDirectionalShadow_Bind(s, pass);
	AeronSampler*          atlas_sampler               = s->mesh_sampler ? s->mesh_sampler : s->pbr_sampler;
	const AeronSceneMesh*  bound_mesh                  = NULL;
	AeronCullMode          bound_cull                  = AERON_CULL_NONE;
	AeronGraphicsPipeline* blend_pipe                  = NULL;
	int                    bound_receiver_local_shadow = 0;
	for (int i = 0; i < s->instance_count; ++i) {
		const AeronSceneMeshInstance* in = &s->instances[i];
		const AeronSceneMesh*         m  = in->mesh;
		if (!m || !m->vbo || !m->ibo || m->opaque_index_count >= m->index_count) {
			continue;
		}
		const int receiver_local_shadow =
			(in->shadow_flags & AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW) != 0;
		if (receiver_local_shadow != bound_receiver_local_shadow) {
			AeronSceneDirectionalShadow_BindForInstance(s, pass, in->shadow_flags);
			bound_receiver_local_shadow = receiver_local_shadow;
		}
		const AeronCullMode cull = (AeronCullMode)in->cull_mode;
		if (!blend_pipe || cull != bound_cull) {
			AeronGraphicsPipeline* p = pbr_draw_pipeline(s, blend_kind, cull);
			if (!p) {
				return 0;
			}
			if (p != blend_pipe) {
				blend_pipe = p;
				Aeron_BindGraphicsPipeline(pass, blend_pipe);
			}
			bound_cull = cull;
		}
		if (m != bound_mesh) {
			bound_mesh = m;
			Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
			Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT16, 0);
			for (uint32_t c = 0; c < AERON_GLTF_CHANNEL_COUNT; c++) {
				Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, c,
										 m->atlas[c] ? m->atlas[c] : AeronSceneInternal_WhiteTexture(),
										 atlas_sampler);
			}
			Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 0, m->material_buffer);
			Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 1, m->variant_buffer);
		}
		pbr_push_instance_uniforms(s, pass, in, m, &s->prepared_instances[i],
								   /*velocity=*/0, /*depth_only=*/0,
								   /*screen_shadow=*/0);
		Aeron_DrawIndexed(pass, m->index_count - m->opaque_index_count, m->opaque_index_count, 0);
	}
	return 1;
}
