/*
 * aeron_scene "pbr" material class — shaders, pipelines, and the
 * per-instance draw walk for AeronSceneMesh instances. Games retain
 * snapshot semantics such as eligibility, mesh-table construction,
 * and light-pool culling, then submit generic instances.
 *
 * Shader ABI (scene_pbr_mesh, scene_pbr_prepass, and stamp variants):
 *   VS b0 space1: AeronScenePbrVsBlock (transforms + storage selection)
 *   VS storage 0: packed current/previous articulation tables
 *   FS sampler 7: optional diffuse environment cubemap
 *   FS storage 0: per-mesh material entries (t8)
 *   FS storage 1: per-mesh packed variant map (t9)
 *   FS storage 2: frame point lights (t10)
 *   FS storage 3: clustered-light depth bins and tile masks (t11)
 *   FS storage 4: unused, the same buffer as storage 3 (t12)
 *   FS b0 space3: AeronSceneDirectionalShadowUniform
 *   FS b1 space3: fixed PBR environment and tuning
 *   FS b2 space3: clustered-light grid and camera data
 *   FS t0..t3: channel atlases, t4: AO, t5/t6: compared/raw directional shadow depth
 *
 * The colour shaders with material animation (`_anim`, for meshes with
 * animation records while the scene has an effect atlas) add samplers 12
 * and 13 (effect atlas layer and emissive), storage 7 (the mesh's
 * animation records) and b3 space3 (AeronScenePbrAnimBlock). Those with
 * detail layers too (`_details`, for meshes with details) take the same
 * resources.
 *
 * The instanced vertex shaders (`_instanced`, which draw a batch) take
 * AeronScenePbrBatchVsBlock at b0 space1 and the batch's records
 * (AeronScenePbrInstanceRecord) at VS storage 1.
 */

#include "internal.h"

#include "aeron/scene/environment.h"

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
	uint32_t _pad;
	uint32_t receive_shadow;
	uint32_t screen_shadow;
	uint32_t current_table_base;
	uint32_t previous_table_base;
	uint32_t receiver_local_shadow;
	uint32_t hidden_tag_groups;
	uint32_t table_component_count;
	float    prev_view_proj[16];
	float    prev_model_to_world[16];
} AeronScenePbrVsBlock; /* 368 B */

typedef char AeronScenePbrVsBlockSizeCheck[sizeof(AeronScenePbrVsBlock) == 368 ? 1 : -1];

/* Animation FS cbuffer (b3 space3), per instance — mirrors `cbuffer
 * PbrAnimFS` in scene_pbr_mesh_impl.hlsli. */
typedef struct AeronScenePbrAnimBlock {
	float    time;            /* animation time, seconds */
	float    page_size;       /* effect atlas page, texels */
	float    meters_per_unit; /* the scene's metres per world unit, for detail fades */
	float    _pad;
	uint32_t state_mask, seed, damage_stage, energy;
	float    energy_start;
	float    _pad1[3];
	float    onset[32];
} AeronScenePbrAnimBlock; /* 176 B */

typedef char AeronScenePbrAnimBlockSizeCheck[sizeof(AeronScenePbrAnimBlock) == 176 ? 1 : -1];

/* Batch VS cbuffer (b0 space1) of the instanced vertex shaders — mirrors
 * `cbuffer PbrBatchVSUniforms` in scene_pbr_vertex_common.hlsli. The
 * instances' values are in their records (AeronScenePbrInstanceRecord, VS
 * storage 1). */
typedef struct AeronScenePbrBatchVsBlock {
	float    view_proj[16];
	float    unjittered_view_proj[16];
	float    moving_prev_view_proj[16];
	uint32_t variant_group_count;
	uint32_t material_count;
	uint32_t camera_mb;
	uint32_t instance_base;
} AeronScenePbrBatchVsBlock; /* 208 B */

typedef char AeronScenePbrBatchVsBlockSizeCheck[sizeof(AeronScenePbrBatchVsBlock) == 208 ? 1 : -1];

/* Second VS cbuffer (b1 space1) for emission batches. Mirrors
 * `cbuffer PbrEmissionVSUniforms` in scene_pbr_emission_widen.hlsli. */
typedef struct AeronScenePbrEmissionVsBlock {
	float center[3]; /* the mesh's bounds centre */
	float half_width_a;
	float width_axis_a[3];
	float min_width_px; /* AeronScene_SetEmissionMinWidth */
	float render_size[2];
	float max_widen;
	float emissive_scale; /* AeronScene_EmissiveScale */
	float fade_width_px;  /* AeronScene_SetEmissionFade */
	float fade_power;
	float half_width_b;
	float _pad0;
	float width_axis_b[3];
	float _pad1;
	float depth_map[4]; /* only for the bloom draw */
} AeronScenePbrEmissionVsBlock; /* 96 B */

typedef char AeronScenePbrEmissionVsBlockSizeCheck[sizeof(AeronScenePbrEmissionVsBlock) == 96 ? 1 : -1];

/* The widest an emission_only instance gets, as a multiple of its real
 * width. A ribbon seen almost edge-on is about invisible anyway, and
 * widening it further would only stretch it along the view direction. */
#define PBR_EMISSION_MAX_WIDEN 32.0f
typedef char AeronScenePbrInstanceRecordSizeCheck[sizeof(AeronScenePbrInstanceRecord) == 160 ? 1 : -1];

static AeronShader* pbr_shader(const char* name, AeronShaderStage stage, uint32_t samplers, uint32_t ubs,
							   uint32_t sbs) {
	AeronShader* sh = Aeron_CreateShader(&(AeronShaderDesc) { .name                 = name,
															  .stage                = stage,
															  .sampler_count        = samplers,
															  .uniform_buffer_count = ubs,
															  .storage_buffer_count = sbs });
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
											.offset      = (uint32_t)offsetof(AeronGltfVertex, component) };
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

/* The `blend` argument of pbr_pipeline: 0 is off, 1 is the usual alpha
 * blending, and PBR_BLEND_PREMULTIPLIED adds a colour that already holds the
 * alpha. */
#define PBR_BLEND_PREMULTIPLIED 2

static AeronGraphicsPipeline* pbr_pipeline_rasterized(AeronShader* vs, AeronShader* ps,
													  const PbrTargetDesc* targets, uint32_t num_targets,
													  int depth_test, int depth_write, AeronCompareOp compare,
													  AeronCullMode cull, int blend,
													  AeronSampleCount                sample_count,
													  const AeronRasterizerStateDesc* rasterizer) {
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
		 * blends; dst alpha keeps coverage (ONE / INV_SRC_ALPHA). With
		 * PBR_BLEND_PREMULTIPLIED, the colour already holds the light the
		 * face adds, which isn't multiplied by alpha again. */
		cts[0].blend.enabled   = 1;
		cts[0].blend.src_color = blend == PBR_BLEND_PREMULTIPLIED ? AERON_BLEND_ONE : AERON_BLEND_SRC_ALPHA;
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
		.rasterizer          = *rasterizer,
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

static AeronGraphicsPipeline* pbr_pipeline(AeronShader* vs, AeronShader* ps, const PbrTargetDesc* targets,
										   uint32_t num_targets, int depth_test, int depth_write,
										   AeronCompareOp compare, AeronCullMode cull, int blend,
										   AeronSampleCount sample_count) {
	const AeronRasterizerStateDesc rasterizer = { 0 };
	return pbr_pipeline_rasterized(vs, ps, targets, num_targets, depth_test, depth_write, compare, cull,
								   blend, sample_count, &rasterizer);
}

/* Colour kind of a screen kind: the same pipeline state. */
static int pbr_screen_base_kind(int kind) {
	switch (kind) {
		case AERON_PBR_PIPE_MESH_SCREEN:
			return AERON_PBR_PIPE_MESH;
		case AERON_PBR_PIPE_FORWARD_SCREEN:
			return AERON_PBR_PIPE_FORWARD;
		case AERON_PBR_PIPE_MESH_BLEND_SCREEN:
			return AERON_PBR_PIPE_MESH_BLEND;
		case AERON_PBR_PIPE_FORWARD_BLEND_SCREEN:
			return AERON_PBR_PIPE_FORWARD_BLEND;
		case AERON_PBR_PIPE_MESH_MASK_SCREEN:
			return AERON_PBR_PIPE_MESH_MASK;
		case AERON_PBR_PIPE_FORWARD_MASK_SCREEN:
			return AERON_PBR_PIPE_FORWARD_MASK;
		default:
			return -1;
	}
}

/* Kind whose pipeline state a kind uses: screen kinds share their colour
 * kind's state and differ only by fragment shader. */
static int pbr_state_kind(int kind) {
	const int base = pbr_screen_base_kind(kind);
	return base >= 0 ? base : kind;
}

/* Screen kind drawing the screen primitives of a colour kind (-1 = the
 * kind draws whole ranges). */
static int pbr_screen_kind(int kind) {
	switch (kind) {
		case AERON_PBR_PIPE_MESH:
			return AERON_PBR_PIPE_MESH_SCREEN;
		case AERON_PBR_PIPE_FORWARD:
			return AERON_PBR_PIPE_FORWARD_SCREEN;
		case AERON_PBR_PIPE_MESH_BLEND:
			return AERON_PBR_PIPE_MESH_BLEND_SCREEN;
		case AERON_PBR_PIPE_FORWARD_BLEND:
			return AERON_PBR_PIPE_FORWARD_BLEND_SCREEN;
		case AERON_PBR_PIPE_MESH_MASK:
			return AERON_PBR_PIPE_MESH_MASK_SCREEN;
		case AERON_PBR_PIPE_FORWARD_MASK:
			return AERON_PBR_PIPE_FORWARD_MASK_SCREEN;
		default:
			return -1;
	}
}

/* Velocity stamp of alpha-BLEND ranges (instance velocity_stamp flag):
 * 2-RT velocity-prepass layout with the normal target write-masked, depth
 * GE test against the laid opaque depth, NO write; the FS alpha-tests the
 * base-color atlas so only the geometry's visible texels receive motion
 * vectors. */
static AeronGraphicsPipeline* pbr_stamp_pipeline(AeronShader* vs, AeronShader* fs, AeronCullMode cull) {
	const PbrTargetDesc stamp2[2] = { { AERON_TEXTURE_FORMAT_R16G16_SNORM, 0x0 },
									  { AERON_TEXTURE_FORMAT_R16G16_FLOAT, 0xF } };
	return vs && fs ? pbr_pipeline(vs, fs, stamp2, 2, 1, 0, AERON_COMPARE_GREATER_EQUAL, cull, 0,
								   AERON_SAMPLE_COUNT_1)
					: NULL;
}

/* Create the (kind, cull) variant. Kind fixes shaders, targets and
 * depth state; cull is free per variant. */
static AeronGraphicsPipeline* pbr_pipeline_create(struct AeronScene3D* s, int kind, AeronCullMode cull,
												  AeronShader* color_vs, AeronShader* color_fs) {
	const PbrTargetDesc normal_rw = { AERON_TEXTURE_FORMAT_R16G16_SNORM, 0xF };
	const PbrTargetDesc vel_rw    = { AERON_TEXTURE_FORMAT_R16G16_FLOAT, 0xF };
	const PbrTargetDesc depth_rw  = { AERON_TEXTURE_FORMAT_R32_FLOAT, 0x1 };
	const PbrTargetDesc color_rw  = { s->color_format, 0xF };
	const PbrTargetDesc planar_rw = { AERON_TEXTURE_FORMAT_RGBA16_FLOAT, 0xF };
	const PbrTargetDesc pre1[1]   = { normal_rw };
	const PbrTargetDesc pre2[2]   = { normal_rw, vel_rw };
	const PbrTargetDesc pre3[3]   = { normal_rw, vel_rw, depth_rw };

	switch (kind) {
		case AERON_PBR_PIPE_MESH:
			return pbr_pipeline(color_vs, color_fs, &color_rw, 1, 1, 1, AERON_COMPARE_GREATER_EQUAL, cull, 0,
								s->sample_count);
		case AERON_PBR_PIPE_FORWARD:
			return pbr_pipeline(color_vs, color_fs, &color_rw, 1, 1, s->sample_count != AERON_SAMPLE_COUNT_1,
								s->sample_count != AERON_SAMPLE_COUNT_1 ? AERON_COMPARE_GREATER_EQUAL
																		: AERON_COMPARE_EQUAL,
								cull, 0, s->sample_count);
		case AERON_PBR_PIPE_MESH_MASK:
			return pbr_pipeline(color_vs, color_fs, &color_rw, 1, 1, 1, AERON_COMPARE_GREATER_EQUAL, cull, 0,
								s->sample_count);
		case AERON_PBR_PIPE_PLANAR:
		case AERON_PBR_PIPE_PLANAR_MASK:
			return pbr_pipeline(color_vs, color_fs, &planar_rw, 1, 1, 1, AERON_COMPARE_GREATER_EQUAL, cull, 0,
								AERON_SAMPLE_COUNT_1);
		case AERON_PBR_PIPE_FORWARD_MASK:
			return pbr_pipeline(color_vs, color_fs, &color_rw, 1, 1, s->sample_count != AERON_SAMPLE_COUNT_1,
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
		case AERON_PBR_PIPE_PREPASS_MASK:
			return s->pbr_prepass_mask_vs && s->pbr_prepass_mask_fs
					   ? pbr_pipeline(s->pbr_prepass_mask_vs, s->pbr_prepass_mask_fs, pre1, 1, 1, 1,
									  AERON_COMPARE_GREATER_EQUAL, cull, 0, AERON_SAMPLE_COUNT_1)
					   : NULL;
		case AERON_PBR_PIPE_PREPASS_MASK_VEL:
			return s->pbr_prepass_mask_vs && s->pbr_prepass_mask_fs
					   ? pbr_pipeline(s->pbr_prepass_mask_vs, s->pbr_prepass_mask_fs, pre2, 2, 1, 1,
									  AERON_COMPARE_GREATER_EQUAL, cull, 0, AERON_SAMPLE_COUNT_1)
					   : NULL;
		case AERON_PBR_PIPE_PREPASS_STAMP_SCREEN:
			return pbr_stamp_pipeline(s->pbr_prepass_stamp_vs, s->pbr_prepass_stamp_screen_fs, cull);
		case AERON_PBR_PIPE_PREPASS_STAMP:
			return pbr_stamp_pipeline(s->pbr_prepass_stamp_vs, s->pbr_prepass_stamp_fs, cull);
		case AERON_PBR_PIPE_PREPASS_TEMPORAL:
			return s->pbr_prepass_vs && s->pbr_prepass_fs
					   ? pbr_pipeline(s->pbr_prepass_vs, s->pbr_prepass_fs, pre3, 3, 1, 1,
									  AERON_COMPARE_GREATER_EQUAL, cull, 0, AERON_SAMPLE_COUNT_1)
					   : NULL;
		case AERON_PBR_PIPE_PREPASS_MASK_TEMPORAL:
			return s->pbr_prepass_mask_vs && s->pbr_prepass_mask_fs
					   ? pbr_pipeline(s->pbr_prepass_mask_vs, s->pbr_prepass_mask_fs, pre3, 3, 1, 1,
									  AERON_COMPARE_GREATER_EQUAL, cull, 0, AERON_SAMPLE_COUNT_1)
					   : NULL;
		/* Back-face fill: strict GREATER (reversed-Z: nearer) with the back
		 * faces biased farther, so a front face at the same depth always
		 * wins, e.g. the twin of a two-sided sheet built from two coincident
		 * opposite triangles. Writes depth, so the filled interior also
		 * hides what is drawn later behind the model (engine glows,
		 * billboards, transparent meshes). */
		case AERON_PBR_PIPE_BACKFACE_FILL: {
			const AeronRasterizerStateDesc bias = { .depth_bias                 = 1,
													.depth_bias_constant_factor = -16.0f,
													.depth_bias_slope_factor    = -2.0f };
			return s->pbr_backface_vs && s->pbr_backface_fs
					   ? pbr_pipeline_rasterized(s->pbr_backface_vs, s->pbr_backface_fs, &color_rw, 1, 1, 1,
												 AERON_COMPARE_GREATER, cull, 0, s->sample_count, &bias)
					   : NULL;
		}
		/* Blend ranges: depth test GE (also for the deferred FORWARD path —
		 * glass is excluded from the depth prepass, so EQUAL cannot match),
		 * no depth write, alpha blending on. */
		case AERON_PBR_PIPE_MESH_BLEND:
			return pbr_pipeline(color_vs, color_fs, &color_rw, 1, 1, 0, AERON_COMPARE_GREATER_EQUAL, cull, 1,
								s->sample_count);
		case AERON_PBR_PIPE_FORWARD_BLEND:
			return pbr_pipeline(color_vs, color_fs, &color_rw, 1, 1, 0, AERON_COMPARE_GREATER_EQUAL, cull, 1,
								s->sample_count);
		default:
			return NULL;
	}
}

static int pbr_mask_color_kind(int kind) {
	return kind == AERON_PBR_PIPE_MESH_MASK || kind == AERON_PBR_PIPE_FORWARD_MASK ||
		   kind == AERON_PBR_PIPE_PLANAR_MASK;
}

/* Kinds shaded by the mesh colour shader, which has the debug and
 * image-based-lighting variants. */
static int pbr_color_kind(int kind) {
	return kind == AERON_PBR_PIPE_MESH || kind == AERON_PBR_PIPE_FORWARD ||
		   kind == AERON_PBR_PIPE_MESH_MASK || kind == AERON_PBR_PIPE_FORWARD_MASK ||
		   kind == AERON_PBR_PIPE_MESH_BLEND || kind == AERON_PBR_PIPE_FORWARD_BLEND ||
		   kind == AERON_PBR_PIPE_PLANAR || kind == AERON_PBR_PIPE_PLANAR_MASK ||
		   pbr_screen_base_kind(kind) >= 0;
}

/* Index of a colour kind's shader in pbr_anim_fs[ibl]: plain, mask, blend,
 * screen, mask screen. */
static int pbr_color_variant(int kind) {
	if (pbr_screen_base_kind(kind) >= 0)
		return pbr_mask_color_kind(pbr_screen_base_kind(kind)) ? 4 : 3;
	if (kind == AERON_PBR_PIPE_MESH_BLEND || kind == AERON_PBR_PIPE_FORWARD_BLEND)
		return 2;
	return pbr_mask_color_kind(kind) ? 1 : 0;
}

static AeronShader* pbr_color_shader(struct AeronScene3D* s, int kind, int debug, int ibl) {
#ifdef AERON_DEBUG_UI
	if (debug) {
		return pbr_mask_color_kind(pbr_state_kind(kind)) ? s->pbr_mask_debug_fs : s->pbr_debug_fs;
	}
#else
	(void)debug;
#endif
	if (pbr_screen_base_kind(kind) >= 0) {
		if (pbr_mask_color_kind(pbr_screen_base_kind(kind)))
			return ibl ? s->pbr_mask_screen_ibl_fs : s->pbr_mask_screen_fs;
		return ibl ? s->pbr_screen_ibl_fs : s->pbr_screen_fs;
	}
	if (kind == AERON_PBR_PIPE_MESH_BLEND || kind == AERON_PBR_PIPE_FORWARD_BLEND) {
		return ibl ? s->pbr_blend_ibl_fs : s->pbr_blend_fs;
	}
	if (pbr_mask_color_kind(kind)) {
		return ibl ? s->pbr_mask_ibl_fs : s->pbr_mask_fs;
	}
	return ibl ? s->pbr_ibl_fs : s->pbr_fs;
}

static AeronGraphicsPipeline* pbr_cached_pipeline(struct AeronScene3D* s, AeronGraphicsPipeline* (*pipes)[3],
												  int kind, AeronCullMode cull, int ibl) {
	if (!pipes[kind][cull]) {
		pipes[kind][cull] =
			pbr_pipeline_create(s, pbr_state_kind(kind), cull, s->pbr_vs, pbr_color_shader(s, kind, 0, ibl));
		if (!pipes[kind][cull] && cull != AERON_CULL_NONE) {
			Aeron_LogWarn("aeron.scene",
						  "PBR pipeline creation failed (kind %d, cull %d); using no-cull variant", kind,
						  (int)cull);
			return pbr_cached_pipeline(s, pipes, kind, AERON_CULL_NONE, ibl);
		}
	}
	return pipes[kind][cull];
}

/* Colour kinds take the image-based-lighting variant while an environment
 * is selected; without one, its code would still cost every pixel. */
AeronGraphicsPipeline* AeronScenePbr_Pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull) {
	if (kind < 0 || kind >= AERON_PBR_PIPE_KIND_COUNT || (unsigned)cull > AERON_CULL_BACK) {
		return NULL;
	}
	const int ibl = s->pbr_environment_set && pbr_color_kind(kind);
	return pbr_cached_pipeline(s, ibl ? s->pbr_ibl_pipes : s->pbr_pipes, kind, cull, ibl);
}

/* The highest animation level a mesh draws with: 0 without animation
 * records or without an effect atlas, 1 with them, 2 when it also has
 * detail layers. */
static int pbr_anim_level(const struct AeronScene3D* s, const AeronSceneMesh* mesh) {
	if (!mesh || !mesh->anim_buffer || !s->effect_atlas)
		return 0;
	return mesh->has_details ? 2 : 1;
}

/* The animation level an instance draws with this frame. It is 1 only when
 * the records can show something on the instance: a flipbook that always
 * plays or whose state is set, damage frames while the instance is damaged,
 * or its energy effect. Otherwise the `_anim` shaders would draw the plain
 * material, so the plain shaders draw the same image for less. Meshes with
 * detail layers always use their shaders. */
static int pbr_instance_anim_level(const struct AeronScene3D* s, const AeronSceneMeshInstance* in) {
	const AeronSceneMesh* mesh  = in ? in->mesh : NULL;
	const int             level = pbr_anim_level(s, mesh);
	if (level != 1)
		return level;
	if ((mesh->anim_states & (1u << AERON_ANIM_STATE_ALWAYS)) != 0u)
		return 1;
	const AeronSceneMaterialAnim* anim = in->anim;
	if (!anim)
		return 0;
	if ((anim->state_mask & mesh->anim_states) != 0u)
		return 1;
	if (anim->damage_stage > 0u && mesh->anim_damage)
		return 1;
	if (anim->energy >= 1u && anim->energy <= 3u && (mesh->anim_energy & (1u << (anim->energy - 1u))) != 0u)
		return 1;
	return 0;
}

/* The colour kind's pipeline with material animation, and with detail
 * layers when `details`, for image-based lighting or not (`ibl`); NULL when
 * its shaders do not load. The shaders of each are loaded on first use. */
static AeronGraphicsPipeline* pbr_anim_pipeline_ibl(struct AeronScene3D* s, int kind, AeronCullMode cull,
													int details, int ibl) {
	static const char* const names[2][2][5] = {
		{ { "scene_pbr_mesh_anim.frag", "scene_pbr_mesh_mask_anim.frag", "scene_pbr_mesh_blend_anim.frag",
			"scene_pbr_mesh_screen_anim.frag", "scene_pbr_mesh_mask_screen_anim.frag" },
		  { "scene_pbr_mesh_ibl_anim.frag", "scene_pbr_mesh_mask_ibl_anim.frag",
			"scene_pbr_mesh_blend_ibl_anim.frag", "scene_pbr_mesh_screen_ibl_anim.frag",
			"scene_pbr_mesh_mask_screen_ibl_anim.frag" } },
		{ { "scene_pbr_mesh_details.frag", "scene_pbr_mesh_mask_details.frag",
			"scene_pbr_mesh_blend_details.frag", "scene_pbr_mesh_screen_details.frag",
			"scene_pbr_mesh_mask_screen_details.frag" },
		  { "scene_pbr_mesh_ibl_details.frag", "scene_pbr_mesh_mask_ibl_details.frag",
			"scene_pbr_mesh_blend_ibl_details.frag", "scene_pbr_mesh_screen_ibl_details.frag",
			"scene_pbr_mesh_mask_screen_ibl_details.frag" } },
	};
	if (!s->pbr_anim_tried[details]) {
		s->pbr_anim_tried[details] = 1;
		if (!s->pbr_anim_vs)
			s->pbr_anim_vs = pbr_shader("scene_pbr_mesh_anim.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 2);
		for (int i = 0; i < 2; ++i)
			for (int v = 0; v < 5; ++v)
				s->pbr_anim_fs[details][i][v] =
					pbr_shader(names[details][i][v], AERON_SHADER_STAGE_FRAGMENT, 14, 4, 8);
	}
	AeronShader* shader = s->pbr_anim_fs[details][ibl][pbr_color_variant(kind)];
	if (!shader || !s->pbr_anim_vs)
		return NULL;
	AeronGraphicsPipeline** pipe = &s->pbr_anim_pipes[details][ibl][kind][cull];
	if (!*pipe) {
		*pipe = pbr_pipeline_create(s, pbr_state_kind(kind), cull, s->pbr_anim_vs, shader);
		if (!*pipe && cull != AERON_CULL_NONE)
			return pbr_anim_pipeline_ibl(s, kind, AERON_CULL_NONE, details, ibl);
	}
	return *pipe;
}

/* pbr_anim_pipeline_ibl with the scene's current lighting. */
static AeronGraphicsPipeline* pbr_anim_pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull,
												int details) {
	return pbr_anim_pipeline_ibl(s, kind, cull, details, s->pbr_environment_set);
}

/* The pipeline a colour kind draws with at animation level `anim` (0 to 2),
 * for image-based lighting or not, falling back as pbr_draw_pipeline does;
 * NULL when none can be created. */
static AeronGraphicsPipeline* pbr_level_pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull,
												 int anim, int ibl) {
	AeronGraphicsPipeline* pipeline = anim == 2 ? pbr_anim_pipeline_ibl(s, kind, cull, 1, ibl) : NULL;
	if (!pipeline && anim)
		pipeline = pbr_anim_pipeline_ibl(s, kind, cull, 0, ibl);
	return pipeline ? pipeline
					: pbr_cached_pipeline(s, ibl ? s->pbr_ibl_pipes : s->pbr_pipes, kind, cull, ibl);
}

int AeronScenePbr_PrepareMesh(struct AeronScene3D* s, const AeronSceneMesh* mesh, AeronCullMode cull) {
	/* The colour kinds of the opaque, mask and blend ranges, in both passes. */
	static const int kinds[3][2] = {
		{ AERON_PBR_PIPE_MESH, AERON_PBR_PIPE_FORWARD },
		{ AERON_PBR_PIPE_MESH_MASK, AERON_PBR_PIPE_FORWARD_MASK },
		{ AERON_PBR_PIPE_MESH_BLEND, AERON_PBR_PIPE_FORWARD_BLEND },
	};
	const uint32_t screens[3] = { mesh->opaque_screen_index_count, mesh->mask_screen_index_count,
								  mesh->blend_screen_index_count };
	const uint32_t plain[3]   = { mesh->opaque_index_count - screens[0], mesh->mask_index_count - screens[1],
								  mesh->blend_index_count - screens[2] };
	const int      anim       = pbr_anim_level(s, mesh);
	int            ok         = 1;
	/* Instances of an animated mesh draw without animation while nothing on
	 * them animates (pbr_instance_anim_level). Both levels are created here,
	 * so that a switch during flight doesn't compile a pipeline. */
	for (int level = anim == 1 ? 0 : anim; level <= anim; ++level) {
		for (int range = 0; range < 3; ++range) {
			for (int pass = 0; pass < 2; ++pass) {
				for (int ibl = 0; ibl < 2; ++ibl) {
					const int kind = kinds[range][pass];
					if (plain[range])
						ok &= pbr_level_pipeline(s, kind, cull, level, ibl) != NULL;
					if (screens[range])
						ok &= pbr_level_pipeline(s, pbr_screen_kind(kind), cull, level, ibl) != NULL;
				}
			}
		}
	}
	return ok;
}

/* The probe variant pipeline of a colour kind at animation level `anim`
 * (pbr_instance_anim_level); NULL for kinds without one (screens) or when
 * its shaders don't load. */
static AeronGraphicsPipeline* pbr_probe_pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull,
												 int anim) {
	static const char* const names[3][5] = {
		{ "scene_pbr_mesh_probes.frag", "scene_pbr_mesh_mask_probes.frag", "scene_pbr_mesh_blend_probes.frag",
		  "scene_pbr_mesh_mirror.frag", "scene_pbr_mesh_mask_mirror.frag" },
		{ "scene_pbr_mesh_probes_anim.frag", "scene_pbr_mesh_mask_probes_anim.frag",
		  "scene_pbr_mesh_blend_probes_anim.frag", "scene_pbr_mesh_mirror_anim.frag",
		  "scene_pbr_mesh_mask_mirror_anim.frag" },
		{ "scene_pbr_mesh_probes_details.frag", "scene_pbr_mesh_mask_probes_details.frag",
		  "scene_pbr_mesh_blend_probes_details.frag", "scene_pbr_mesh_mirror_details.frag",
		  "scene_pbr_mesh_mask_mirror_details.frag" },
	};
	if (!pbr_color_kind(kind) || pbr_screen_base_kind(kind) >= 0) {
		return NULL;
	}
	const int details = anim == 2;
	if (!s->pbr_probe_tried[details]) {
		s->pbr_probe_tried[details] = 1;
		if (!s->pbr_anim_vs)
			s->pbr_anim_vs = pbr_shader("scene_pbr_mesh_anim.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 2);
		for (int v = 0; v < 5; ++v) {
			if (details) {
				s->pbr_probe_fs[2][v] = pbr_shader(names[2][v], AERON_SHADER_STAGE_FRAGMENT, 16, 4, 8);
			} else {
				s->pbr_probe_fs[0][v] = pbr_shader(names[0][v], AERON_SHADER_STAGE_FRAGMENT, 14, 3, 7);
				s->pbr_probe_fs[1][v] = pbr_shader(names[1][v], AERON_SHADER_STAGE_FRAGMENT, 16, 4, 8);
			}
		}
	}
	AeronShader* vs = anim ? s->pbr_anim_vs : s->pbr_vs;
	/* The floor's mirrored view takes its own, lighter variants. */
	const int    variant = kind == AERON_PBR_PIPE_PLANAR        ? 3
						   : kind == AERON_PBR_PIPE_PLANAR_MASK ? 4
																: pbr_color_variant(kind);
	AeronShader* shader  = s->pbr_probe_fs[anim][variant];
	if (!vs || !shader) {
		return NULL;
	}
	AeronGraphicsPipeline** pipe = &s->pbr_probe_pipes[anim][kind][cull];
	if (!*pipe) {
		*pipe = pbr_pipeline_create(s, pbr_state_kind(kind), cull, vs, shader);
		if (!*pipe && cull != AERON_CULL_NONE)
			return pbr_probe_pipeline(s, kind, AERON_CULL_NONE, anim);
	}
	return *pipe;
}

#ifdef AERON_DEBUG_UI
static int pbr_debug_active(const struct AeronScene3D* s) {
	return s->pbr_debug_views || s->directional_shadow.debug_cascades || s->cluster_desc.debug_view;
}

static AeronGraphicsPipeline* pbr_debug_pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull) {
	if (!pbr_color_kind(kind) || !pbr_debug_active(s)) {
		return NULL;
	}
	if (!s->pbr_debug_tried) {
		s->pbr_debug_tried = 1;
		s->pbr_debug_fs    = pbr_shader("scene_pbr_mesh_debug.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
		s->pbr_mask_debug_fs =
			pbr_shader("scene_pbr_mesh_mask_debug.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	}
	if (!s->pbr_debug_fs || !s->pbr_mask_debug_fs) {
		return NULL;
	}
	if (!s->pbr_debug_pipes[kind][cull]) {
		s->pbr_debug_pipes[kind][cull] =
			pbr_pipeline_create(s, pbr_state_kind(kind), cull, s->pbr_vs, pbr_color_shader(s, kind, 1, 1));
		if (!s->pbr_debug_pipes[kind][cull] && cull != AERON_CULL_NONE) {
			return pbr_debug_pipeline(s, kind, AERON_CULL_NONE);
		}
	}
	return s->pbr_debug_pipes[kind][cull];
}
#endif

/* `anim`: the animation level of the drawn instance
 * (pbr_instance_anim_level). Debug views draw without animation, and so does
 * a scene whose animation shaders don't load. A mesh whose detail shaders
 * don't load draws without its details. */
static AeronGraphicsPipeline* pbr_draw_pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull,
												int anim) {
#ifdef AERON_DEBUG_UI
	AeronGraphicsPipeline* debug = pbr_debug_pipeline(s, kind, cull);
	if (debug) {
		return debug;
	}
#endif
	if (anim && s->effect_atlas && pbr_color_kind(kind)) {
		AeronGraphicsPipeline* pipeline = anim == 2 ? pbr_anim_pipeline(s, kind, cull, 1) : NULL;
		if (!pipeline)
			pipeline = pbr_anim_pipeline(s, kind, cull, 0);
		if (pipeline)
			return pipeline;
	}
	return AeronScenePbr_Pipeline(s, kind, cull);
}

/* The batch pipeline of a blend kind or the velocity stamp, with or
 * without IBL; created on the first call. NULL when its shaders do not
 * load. */
static AeronGraphicsPipeline* pbr_instanced_create(struct AeronScene3D* s, int kind, AeronCullMode cull,
												   int ibl) {
	if (!s->pbr_instanced_tried) {
		s->pbr_instanced_tried = 1;
		s->pbr_instanced_vs = pbr_shader("scene_pbr_mesh_instanced.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 3);
		s->pbr_instanced_stamp_vs =
			pbr_shader("scene_pbr_prepass_stamp_instanced.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 3);
	}
	AeronGraphicsPipeline** pipe = &s->pbr_instanced_pipes[ibl][kind][cull];
	if (!*pipe && kind == AERON_PBR_PIPE_PREPASS_STAMP) {
		*pipe = pbr_stamp_pipeline(s->pbr_instanced_stamp_vs, s->pbr_prepass_stamp_fs, cull);
	} else if (!*pipe && s->pbr_instanced_vs) {
		*pipe = pbr_pipeline_create(s, kind, cull, s->pbr_instanced_vs, pbr_color_shader(s, kind, 0, ibl));
	}
	return *pipe;
}

/* The pipeline that draws a batch in a blend kind or the velocity stamp;
 * NULL when its shaders do not load, and in debug views, so that the
 * batch's instances draw alone. */
static AeronGraphicsPipeline* pbr_instanced_pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull) {
#ifdef AERON_DEBUG_UI
	if (pbr_debug_pipeline(s, kind, cull)) {
		return NULL;
	}
#endif
	const int ibl = kind != AERON_PBR_PIPE_PREPASS_STAMP && s->pbr_environment_set;
	return pbr_instanced_create(s, kind, cull, ibl);
}

/* Returns the pipeline for emission batches in a blend kind, and creates it
 * on first use. Returns NULL when its shaders don't load. */
static AeronGraphicsPipeline* pbr_emission_pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull) {
	AeronGraphicsPipeline** pipe = &s->pbr_emission_pipes[kind][cull];
	if (*pipe || !s->pbr_emission_vs || !s->pbr_emission_fs ||
		(kind != AERON_PBR_PIPE_MESH_BLEND && kind != AERON_PBR_PIPE_FORWARD_BLEND))
		return *pipe;
	/* The states of the blend kinds in pbr_pipeline_create, with
	 * premultiplied blending: an emission texture holds the light each
	 * texel gives, already multiplied by its alpha. */
	const PbrTargetDesc color_rw = { s->color_format, 0xF };
	*pipe = pbr_pipeline(s->pbr_emission_vs, s->pbr_emission_fs, &color_rw, 1, 1, 0, AERON_COMPARE_GREATER_EQUAL,
						 cull, PBR_BLEND_PREMULTIPLIED, s->sample_count);
	return *pipe;
}

int AeronScenePbr_EmissionAvailable(const struct AeronScene3D* s) {
	if (!s->pbr_emission_vs || !s->pbr_emission_fs) {
		return 0;
	}
#ifdef AERON_DEBUG_UI
	/* The debug views show every instance with the debug shader. */
	if (pbr_debug_active(s)) {
		return 0;
	}
#endif
	return 1;
}

/* Returns the velocity-stamp pipeline for emission batches, and creates it
 * on first use. Returns NULL when its shaders don't load. */
static AeronGraphicsPipeline* pbr_emission_stamp_pipeline(struct AeronScene3D* s, AeronCullMode cull) {
	AeronGraphicsPipeline** pipe = &s->pbr_emission_stamp_pipes[cull];
	if (!*pipe && s->pbr_emission_stamp_vs)
		*pipe = pbr_stamp_pipeline(s->pbr_emission_stamp_vs, s->pbr_prepass_stamp_fs, cull);
	return *pipe;
}

/* Returns the pipeline of the bloom draw of emission batches, and creates
 * it on first use. It adds the emission, which already holds the alpha, to
 * a bloom level, which has no depth. Returns NULL when its shaders don't
 * load. */
static AeronGraphicsPipeline* pbr_emission_bloom_pipeline(struct AeronScene3D* s, AeronCullMode cull) {
	AeronGraphicsPipeline** pipe = &s->pbr_emission_bloom_pipes[cull];
	if (*pipe || !s->pbr_emission_bloom_vs || !s->pbr_emission_bloom_fs)
		return *pipe;
	AeronVertexAttributeDesc    attrs[6];
	AeronVertexBufferLayoutDesc vbd;
	pbr_vertex_input(attrs, &vbd);
	*pipe = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader       = s->pbr_emission_bloom_vs,
		.fragment_shader     = s->pbr_emission_bloom_fs,
		.primitive_type      = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode           = cull,
		.vertex_buffers      = &vbd,
		.vertex_buffer_count = 1,
		.attributes          = attrs,
		.attribute_count     = 6,
		.color_format        = AERON_SCENE_BLOOM_FORMAT,
		.blend =
			{
				.enabled   = 1,
				.src_color = AERON_BLEND_ONE,
				.dst_color = AERON_BLEND_ONE,
				.color_op  = AERON_BLEND_OP_ADD,
				.src_alpha = AERON_BLEND_ZERO,
				.dst_alpha = AERON_BLEND_ONE,
				.alpha_op  = AERON_BLEND_OP_ADD,
			},
	});
	return *pipe;
}

/* Pushes the second VS cbuffer for an emission batch of mesh `m`, with the
 * settings `p`, its minimum width in render pixels, and its emission
 * multiplied by `scale`. `bloom` describes the bloom draw; it is NULL for
 * the other draws. The width is measured along the two shorter axes of the
 * bounds, and the shader keeps the one that is wider on screen. A laser
 * bolt is a flat ribbon: its longest axis is its length and one short axis
 * has almost no extent. A warhead is round, and the short axis that points
 * at the camera looks thin on screen, so the other one must be measured. */
static void pbr_push_emission_uniforms(const struct AeronScene3D* s, AeronRenderPass* pass,
									   const AeronSceneMesh* m, const AeronSceneEmissionParams* p,
									   float min_width_px, float scale, const AeronSceneEmissionBloomDraw* bloom) {
	AeronScenePbrEmissionVsBlock block;
	memset(&block, 0, sizeof block);
	float extent[3];
	int   longest = 0;
	for (int axis = 0; axis < 3; ++axis) {
		extent[axis]       = m->bound_max[axis] - m->bound_min[axis];
		block.center[axis] = 0.5f * (m->bound_min[axis] + m->bound_max[axis]);
		if (extent[axis] > extent[longest])
			longest = axis;
	}
	const int a                = (longest + 1) % 3;
	const int b                = (longest + 2) % 3;
	block.half_width_a         = 0.5f * extent[a];
	block.width_axis_a[a]      = 1.0f;
	block.half_width_b         = 0.5f * extent[b];
	block.width_axis_b[b]      = 1.0f;
	const int has_width        = block.half_width_a > 0.0f || block.half_width_b > 0.0f;
	/* A mesh without a width has nothing to widen or fade. */
	block.min_width_px   = has_width ? min_width_px : 0.0f;
	block.fade_width_px  = has_width ? p->fade_width_px : 0.0f;
	block.fade_power     = p->fade_power;
	block.render_size[0] = (float)s->render_w;
	block.render_size[1] = (float)s->render_h;
	block.max_widen      = PBR_EMISSION_MAX_WIDEN;
	block.emissive_scale = AeronScene_EmissiveScale() * scale;
	if (bloom)
		memcpy(block.depth_map, bloom->depth_map, sizeof block.depth_map);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 1, &block, sizeof block);
}


static int pbr_required_pipelines_ready(const struct AeronScene3D* s) {
	static const int required[] = {
		AERON_PBR_PIPE_MESH,         AERON_PBR_PIPE_MESH_MASK,        AERON_PBR_PIPE_FORWARD_MASK,
		AERON_PBR_PIPE_PREPASS_MASK, AERON_PBR_PIPE_PREPASS_MASK_VEL, AERON_PBR_PIPE_PREPASS_MASK_TEMPORAL,
	};
	for (size_t index = 0; index < sizeof required / sizeof required[0]; ++index) {
		if (!s->pbr_pipes[required[index]][AERON_CULL_NONE])
			return 0;
	}
	return 1;
}

int AeronScenePbr_Ensure(struct AeronScene3D* s) {
	if (s->pbr_tried) {
		return pbr_required_pipelines_ready(s);
	}
	s->pbr_tried = 1;

	s->pbr_vs               = pbr_shader("scene_pbr_mesh.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 2);
	s->pbr_prepass_vs       = pbr_shader("scene_pbr_prepass.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 2);
	s->pbr_prepass_mask_vs  = pbr_shader("scene_pbr_prepass_mask.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 2);
	s->pbr_prepass_stamp_vs = pbr_shader("scene_pbr_prepass_stamp.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 2);
	s->pbr_fs               = pbr_shader("scene_pbr_mesh.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	s->pbr_mask_fs          = pbr_shader("scene_pbr_mesh_mask.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	s->pbr_blend_fs         = pbr_shader("scene_pbr_mesh_blend.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	s->pbr_screen_fs        = pbr_shader("scene_pbr_mesh_screen.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	s->pbr_mask_screen_fs =
		pbr_shader("scene_pbr_mesh_mask_screen.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	s->pbr_ibl_fs       = pbr_shader("scene_pbr_mesh_ibl.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	s->pbr_mask_ibl_fs  = pbr_shader("scene_pbr_mesh_mask_ibl.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	s->pbr_blend_ibl_fs = pbr_shader("scene_pbr_mesh_blend_ibl.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	s->pbr_screen_ibl_fs =
		pbr_shader("scene_pbr_mesh_screen_ibl.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	s->pbr_mask_screen_ibl_fs =
		pbr_shader("scene_pbr_mesh_mask_screen_ibl.frag", AERON_SHADER_STAGE_FRAGMENT, 12, 3, 7);
	s->pbr_prepass_fs      = pbr_shader("scene_pbr_prepass.frag", AERON_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	s->pbr_prepass_mask_fs = pbr_shader("scene_pbr_prepass_mask.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 0, 2);
	s->pbr_backface_vs     = pbr_shader("scene_pbr_backface.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 2);
	s->pbr_backface_fs     = pbr_shader("scene_pbr_backface.frag", AERON_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	/* Velocity stamping reads the same mesh-owned material resources. */
	s->pbr_prepass_stamp_fs =
		pbr_shader("scene_pbr_prepass_stamp.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 0, 2);
	s->pbr_prepass_stamp_screen_fs =
		pbr_shader("scene_pbr_prepass_stamp_screen.frag", AERON_SHADER_STAGE_FRAGMENT, 3, 0, 4);
	if (!s->pbr_vs || !s->pbr_fs || !s->pbr_mask_fs || !s->pbr_blend_fs || !s->pbr_screen_fs ||
		!s->pbr_mask_screen_fs || !s->pbr_ibl_fs || !s->pbr_mask_ibl_fs || !s->pbr_blend_ibl_fs ||
		!s->pbr_screen_ibl_fs || !s->pbr_mask_screen_ibl_fs || !AeronSceneInternal_WhiteCubeTexture() ||
		!AeronSceneInternal_BrdfLutTexture() || !s->pbr_prepass_mask_vs || !s->pbr_prepass_mask_fs) {
		return 0;
	}

	/* Cull-NONE variants up front (long-standing default); other cull
	 * modes are created lazily via AeronScenePbr_Pipeline. */
	for (int kind = 0; kind < AERON_PBR_PIPE_KIND_COUNT; kind++) {
		/* The back-face fill only culls front faces. */
		if (kind == AERON_PBR_PIPE_BACKFACE_FILL)
			continue;
		s->pbr_pipes[kind][AERON_CULL_NONE] = pbr_pipeline_create(s, pbr_state_kind(kind), AERON_CULL_NONE,
																  s->pbr_vs, pbr_color_shader(s, kind, 0, 0));
	}
	/* The cull-NONE batch pipelines too, with and without IBL, so that the
	 * first batch doesn't wait for them. */
	pbr_instanced_create(s, AERON_PBR_PIPE_PREPASS_STAMP, AERON_CULL_NONE, 0);
	for (int ibl = 0; ibl < 2; ++ibl) {
		pbr_instanced_create(s, AERON_PBR_PIPE_MESH_BLEND, AERON_CULL_NONE, ibl);
		pbr_instanced_create(s, AERON_PBR_PIPE_FORWARD_BLEND, AERON_CULL_NONE, ibl);
	}
	/* Load the emission shaders. They're optional: without them,
	 * emission_only instances draw with the other shaders. */
	s->pbr_emission_vs = pbr_shader("scene_pbr_emission_instanced.vert", AERON_SHADER_STAGE_VERTEX, 0, 2, 3);
	s->pbr_emission_fs = pbr_shader("scene_pbr_emission.frag", AERON_SHADER_STAGE_FRAGMENT, 2, 0, 2);
	s->pbr_emission_stamp_vs =
		pbr_shader("scene_pbr_emission_stamp_instanced.vert", AERON_SHADER_STAGE_VERTEX, 0, 2, 3);
	s->pbr_emission_bloom_vs =
		pbr_shader("scene_pbr_emission_bloom_instanced.vert", AERON_SHADER_STAGE_VERTEX, 0, 2, 3);
	s->pbr_emission_bloom_fs = pbr_shader("scene_pbr_emission_bloom.frag", AERON_SHADER_STAGE_FRAGMENT, 3, 0, 2);
	pbr_emission_pipeline(s, AERON_PBR_PIPE_MESH_BLEND, AERON_CULL_NONE);
	pbr_emission_pipeline(s, AERON_PBR_PIPE_FORWARD_BLEND, AERON_CULL_NONE);
	pbr_emission_stamp_pipeline(s, AERON_CULL_NONE);
	if (!pbr_required_pipelines_ready(s)) {
		Aeron_LogError("aeron.scene", "PBR pipeline creation failed");
	}
	return pbr_required_pipelines_ready(s);
}

void AeronScenePbr_Release(struct AeronScene3D* s) {
	for (int kind = 0; kind < AERON_PBR_PIPE_KIND_COUNT; kind++) {
		for (int cull = 0; cull < 3; cull++) {
			if (s->pbr_pipes[kind][cull]) {
				Aeron_DestroyGraphicsPipeline(s->pbr_pipes[kind][cull]);
				s->pbr_pipes[kind][cull] = NULL;
			}
			if (s->pbr_ibl_pipes[kind][cull]) {
				Aeron_DestroyGraphicsPipeline(s->pbr_ibl_pipes[kind][cull]);
				s->pbr_ibl_pipes[kind][cull] = NULL;
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
	if (s->pbr_prepass_mask_vs)
		Aeron_DestroyShader(s->pbr_prepass_mask_vs);
	if (s->pbr_prepass_stamp_vs)
		Aeron_DestroyShader(s->pbr_prepass_stamp_vs);
	if (s->pbr_fs)
		Aeron_DestroyShader(s->pbr_fs);
	if (s->pbr_mask_fs)
		Aeron_DestroyShader(s->pbr_mask_fs);
	if (s->pbr_blend_fs)
		Aeron_DestroyShader(s->pbr_blend_fs);
	if (s->pbr_screen_fs)
		Aeron_DestroyShader(s->pbr_screen_fs);
	if (s->pbr_mask_screen_fs)
		Aeron_DestroyShader(s->pbr_mask_screen_fs);
	if (s->pbr_ibl_fs)
		Aeron_DestroyShader(s->pbr_ibl_fs);
	if (s->pbr_mask_ibl_fs)
		Aeron_DestroyShader(s->pbr_mask_ibl_fs);
	if (s->pbr_blend_ibl_fs)
		Aeron_DestroyShader(s->pbr_blend_ibl_fs);
	if (s->pbr_screen_ibl_fs)
		Aeron_DestroyShader(s->pbr_screen_ibl_fs);
	if (s->pbr_mask_screen_ibl_fs)
		Aeron_DestroyShader(s->pbr_mask_screen_ibl_fs);
	if (s->pbr_prepass_fs)
		Aeron_DestroyShader(s->pbr_prepass_fs);
	if (s->pbr_prepass_mask_fs)
		Aeron_DestroyShader(s->pbr_prepass_mask_fs);
	if (s->pbr_prepass_stamp_fs)
		Aeron_DestroyShader(s->pbr_prepass_stamp_fs);
	if (s->pbr_prepass_stamp_screen_fs)
		Aeron_DestroyShader(s->pbr_prepass_stamp_screen_fs);
	if (s->pbr_backface_vs)
		Aeron_DestroyShader(s->pbr_backface_vs);
	if (s->pbr_backface_fs)
		Aeron_DestroyShader(s->pbr_backface_fs);
	for (int details = 0; details < 2; ++details) {
		for (int ibl = 0; ibl < 2; ++ibl) {
			for (int kind = 0; kind < AERON_PBR_PIPE_KIND_COUNT; kind++) {
				for (int cull = 0; cull < 3; cull++) {
					if (s->pbr_anim_pipes[details][ibl][kind][cull])
						Aeron_DestroyGraphicsPipeline(s->pbr_anim_pipes[details][ibl][kind][cull]);
					s->pbr_anim_pipes[details][ibl][kind][cull] = NULL;
				}
			}
			for (int v = 0; v < 5; ++v) {
				if (s->pbr_anim_fs[details][ibl][v])
					Aeron_DestroyShader(s->pbr_anim_fs[details][ibl][v]);
				s->pbr_anim_fs[details][ibl][v] = NULL;
			}
		}
		s->pbr_anim_tried[details]  = 0;
		s->pbr_probe_tried[details] = 0;
	}
	for (int anim = 0; anim < 3; ++anim) {
		for (int kind = 0; kind < AERON_PBR_PIPE_KIND_COUNT; kind++) {
			for (int cull = 0; cull < 3; cull++) {
				if (s->pbr_probe_pipes[anim][kind][cull])
					Aeron_DestroyGraphicsPipeline(s->pbr_probe_pipes[anim][kind][cull]);
				s->pbr_probe_pipes[anim][kind][cull] = NULL;
			}
		}
		for (int v = 0; v < 5; ++v) {
			if (s->pbr_probe_fs[anim][v])
				Aeron_DestroyShader(s->pbr_probe_fs[anim][v]);
			s->pbr_probe_fs[anim][v] = NULL;
		}
	}
	for (int ibl = 0; ibl < 2; ++ibl) {
		for (int kind = 0; kind < AERON_PBR_PIPE_KIND_COUNT; kind++) {
			for (int cull = 0; cull < 3; cull++) {
				if (s->pbr_instanced_pipes[ibl][kind][cull])
					Aeron_DestroyGraphicsPipeline(s->pbr_instanced_pipes[ibl][kind][cull]);
				s->pbr_instanced_pipes[ibl][kind][cull] = NULL;
			}
		}
	}
	for (int kind = 0; kind < AERON_PBR_PIPE_KIND_COUNT; kind++) {
		for (int cull = 0; cull < 3; cull++) {
			if (s->pbr_emission_pipes[kind][cull])
				Aeron_DestroyGraphicsPipeline(s->pbr_emission_pipes[kind][cull]);
			s->pbr_emission_pipes[kind][cull] = NULL;
		}
	}
	for (int cull = 0; cull < 3; cull++) {
		if (s->pbr_emission_stamp_pipes[cull])
			Aeron_DestroyGraphicsPipeline(s->pbr_emission_stamp_pipes[cull]);
		s->pbr_emission_stamp_pipes[cull] = NULL;
		if (s->pbr_emission_bloom_pipes[cull])
			Aeron_DestroyGraphicsPipeline(s->pbr_emission_bloom_pipes[cull]);
		s->pbr_emission_bloom_pipes[cull] = NULL;
	}
	if (s->pbr_emission_bloom_depth_sampler)
		Aeron_DestroySampler(s->pbr_emission_bloom_depth_sampler);
	s->pbr_emission_bloom_depth_sampler = NULL;
	if (s->pbr_instanced_vs)
		Aeron_DestroyShader(s->pbr_instanced_vs);
	if (s->pbr_instanced_stamp_vs)
		Aeron_DestroyShader(s->pbr_instanced_stamp_vs);
	if (s->pbr_emission_vs)
		Aeron_DestroyShader(s->pbr_emission_vs);
	if (s->pbr_emission_fs)
		Aeron_DestroyShader(s->pbr_emission_fs);
	if (s->pbr_emission_stamp_vs)
		Aeron_DestroyShader(s->pbr_emission_stamp_vs);
	if (s->pbr_emission_bloom_vs)
		Aeron_DestroyShader(s->pbr_emission_bloom_vs);
	if (s->pbr_emission_bloom_fs)
		Aeron_DestroyShader(s->pbr_emission_bloom_fs);
	s->pbr_emission_bloom_vs  = NULL;
	s->pbr_emission_bloom_fs  = NULL;
	s->pbr_instanced_vs       = NULL;
	s->pbr_instanced_stamp_vs = NULL;
	s->pbr_emission_vs        = NULL;
	s->pbr_emission_fs        = NULL;
	s->pbr_emission_stamp_vs  = NULL;
	s->pbr_instanced_tried    = 0;
	if (s->pbr_anim_vs)
		Aeron_DestroyShader(s->pbr_anim_vs);
	s->pbr_anim_vs = NULL;
#ifdef AERON_DEBUG_UI
	if (s->pbr_debug_fs)
		Aeron_DestroyShader(s->pbr_debug_fs);
	if (s->pbr_mask_debug_fs)
		Aeron_DestroyShader(s->pbr_mask_debug_fs);
	s->pbr_debug_fs      = NULL;
	s->pbr_mask_debug_fs = NULL;
	s->pbr_debug_tried   = 0;
#endif
}

/* Identity mesh table pushed for instances without a custom one. */
const AeronSceneMeshTable* AeronScenePbr_IdentityTable(void);

const AeronSceneMeshTable* AeronScenePbr_IdentityTable(void) {
	static AeronSceneMeshTable t;
	static int                 built;
	if (!built) {
		t.component_count = AERON_MAX_MESH_COMPONENTS;
		for (int mi = 0; mi < AERON_MAX_MESH_COMPONENTS; ++mi) {
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
									   const AeronScenePreparedInstance* prepared, int velocity,
									   int depth_only, int screen_shadow) {
	const int            vel = velocity && !in->zero_velocity;
	AeronScenePbrVsBlock vsb;
	memset(&vsb, 0, sizeof vsb);
	/* The floor's mirrored view draws through its own camera. */
	const float* view_proj =
		s->planar_reflection.drawing ? s->planar_reflection.view_proj : s->jittered_view_proj;
	const float* unjittered =
		s->planar_reflection.drawing ? s->planar_reflection.view_proj : s->unjittered_view_proj;
	memcpy(vsb.view_proj, view_proj, sizeof vsb.view_proj);
	memcpy(vsb.unjittered_view_proj, unjittered, sizeof vsb.unjittered_view_proj);
	memcpy(vsb.model_to_world, in->transform, sizeof vsb.model_to_world);
	memcpy(vsb.prev_view_proj, vel ? s->mb_prev_view_proj : unjittered, sizeof vsb.prev_view_proj);
	memcpy(vsb.prev_model_to_world, vel ? in->prev_transform : in->transform, sizeof vsb.prev_model_to_world);
	uint32_t variant = in->variant;
	if (variant >= m->variant_rows) {
		variant = m->variant_rows ? m->variant_rows - 1u : 0u;
	}
	vsb.variant_row_base             = variant * m->variant_groups_per_row;
	vsb.variant_group_count          = m->variant_groups_per_row;
	vsb.material_count               = m->material_count;
	vsb.camera_mb                    = (velocity && (s->temporal_active || s->post.mb_camera_blur)) ? 1u : 0u;
	vsb.receive_shadow       = (in->shadow_flags & AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW) == 0 ? 1u : 0u;
	const int receiver_local = (in->shadow_flags & AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW) != 0;
	/* Opaque receivers read the half-resolution screen pass, which takes
	 * receiver-local pixels from their own map by the prepass's flag. */
	vsb.screen_shadow         = screen_shadow && vsb.receive_shadow ? 1u : 0u;
	vsb.receiver_local_shadow = vsb.receive_shadow && receiver_local ? 1u : 0u;
	vsb.current_table_base    = prepared->current_table_base;
	vsb.previous_table_base   = vel ? prepared->previous_table_base : prepared->current_table_base;
	vsb.table_component_count = prepared->table_component_count;
	vsb.hidden_tag_groups     = in->hidden_tag_groups;
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &vsb, sizeof vsb);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 1, m->tag_group_buffer);

	/* The animation block of the `_anim` colour shaders: the instance's
	 * state. Only instances that draw with these shaders need it
	 * (pbr_instance_anim_level). */
	if (!depth_only && pbr_instance_anim_level(s, in) != 0) {
		AeronScenePbrAnimBlock anim;
		memset(&anim, 0, sizeof anim);
		anim.time      = s->animation_time;
		anim.page_size = (float)AeronEffectAtlas_PageSize(s->effect_atlas);
		anim.meters_per_unit = s->view_space_to_meters;
		if (in->anim) {
			anim.state_mask   = in->anim->state_mask;
			anim.seed         = in->anim->seed;
			anim.damage_stage = in->anim->damage_stage;
			anim.energy       = in->anim->energy;
			anim.energy_start = in->anim->energy_start;
			memcpy(anim.onset, in->anim->onset, sizeof anim.onset);
		}
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 3, &anim, sizeof anim);
	}
}

/* The batch's uniforms and its records, for a draw with the instanced
 * vertex shaders. */
static void pbr_push_batch_uniforms(struct AeronScene3D* s, AeronRenderPass* pass,
									const AeronScenePbrBatch* batch, int velocity) {
	AeronScenePbrBatchVsBlock block;
	memset(&block, 0, sizeof block);
	const float* view_proj =
		s->planar_reflection.drawing ? s->planar_reflection.view_proj : s->jittered_view_proj;
	const float* unjittered =
		s->planar_reflection.drawing ? s->planar_reflection.view_proj : s->unjittered_view_proj;
	memcpy(block.view_proj, view_proj, sizeof block.view_proj);
	memcpy(block.unjittered_view_proj, unjittered, sizeof block.unjittered_view_proj);
	memcpy(block.moving_prev_view_proj, velocity ? s->mb_prev_view_proj : unjittered,
		   sizeof block.moving_prev_view_proj);
	block.variant_group_count = batch->mesh->variant_groups_per_row;
	block.material_count      = batch->mesh->material_count;
	block.camera_mb           = (velocity && (s->temporal_active || s->post.mb_camera_blur)) ? 1u : 0u;
	block.instance_base       = batch->first;
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &block, sizeof block);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 1, s->instance_record_buffer);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 2, batch->mesh->tag_group_buffer);
}

/* Clears the batches' drawn flags before a walk that draws them. */
static void pbr_reset_batches(struct AeronScene3D* s) {
	for (uint32_t b = 0; b < s->pbr_batch_count; ++b) {
		s->pbr_batches[b].drawn = 0;
	}
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

static int pbr_mask_kind(int pipe_kind) {
	switch (pipe_kind) {
		case AERON_PBR_PIPE_MESH:
			return AERON_PBR_PIPE_MESH_MASK;
		case AERON_PBR_PIPE_FORWARD:
			return AERON_PBR_PIPE_FORWARD_MASK;
		case AERON_PBR_PIPE_PLANAR:
			return AERON_PBR_PIPE_PLANAR_MASK;
		case AERON_PBR_PIPE_PREPASS:
			return AERON_PBR_PIPE_PREPASS_MASK;
		case AERON_PBR_PIPE_PREPASS_VEL:
			return AERON_PBR_PIPE_PREPASS_MASK_VEL;
		case AERON_PBR_PIPE_PREPASS_TEMPORAL:
			return AERON_PBR_PIPE_PREPASS_MASK_TEMPORAL;
		default:
			return -1;
	}
}

/* The screen captures at t10 and t11. Without a capture the placeholder is
 * never sampled: the frame marks every source invalid. */
static void pbr_bind_screen_textures(const struct AeronScene3D* s, AeronRenderPass* pass) {
	AeronSampler* screen_sampler = s->post_linear_sampler ? s->post_linear_sampler : s->pbr_sampler;
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 10,
							 s->screen_graphics ? s->screen_graphics : AeronSceneInternal_WhiteTexture(),
							 screen_sampler);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 11,
							 s->screen_text ? s->screen_text : AeronSceneInternal_WhiteTexture(),
							 screen_sampler);
}

/* Material animation: the effect atlas (samplers 12, 13), read by the
 * `_anim` shaders only. */
static void pbr_bind_effect_textures(struct AeronScene3D* s, AeronRenderPass* pass) {
	s->pbr_occlusion_in_effect_slot = 0;
	if (s->effect_atlas) {
		AeronSampler* sampler = s->mesh_sampler ? s->mesh_sampler : s->pbr_sampler;
		for (int which = 0; which < 2; ++which) {
			AeronTexture* texture = AeronEffectAtlas_Texture(s->effect_atlas, which);
			Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 12u + (uint32_t)which,
									 texture ? texture : AeronSceneInternal_WhiteArrayTexture(), sampler);
		}
	}
}

static void pbr_bind_ao(struct AeronScene3D* s, AeronRenderPass* pass, AeronTexture* ao_tex) {
	AeronTexture* ao = ao_tex ? ao_tex : AeronSceneInternal_WhiteTexture();
	if (ao) {
		Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 4, ao,
								 ao_tex ? s->post_linear_sampler : s->pbr_sampler);
	}
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 7,
							 s->pbr_environment_set ? s->pbr_environment.diffuse
													: AeronSceneInternal_WhiteCubeTexture(),
							 s->pbr_environment_set ? s->pbr_environment.sampler : s->pbr_sampler);
	/* Specular image-based lighting: the prefiltered cube when selected
	 * (the shader samples it only when its strength and mip count are set)
	 * and the split-sum BRDF table. */
	const int specular = s->pbr_environment_set && s->pbr_environment.specular;
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 8,
							 specular ? s->pbr_environment.specular : AeronSceneInternal_WhiteCubeTexture(),
							 specular ? s->pbr_environment.sampler : s->pbr_sampler);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 9, AeronSceneInternal_BrdfLutTexture(),
							 s->pbr_sampler);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 2, s->point_light_buffer);
	AeronSceneClusteredLights_Bind(s, pass);
	/* Screen surfaces: the captured graphics and text (t10, t11) and the
	 * frame's sources (storage 6). */
	pbr_bind_screen_textures(s, pass);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 6, s->screen_frame_buffer);
	pbr_bind_effect_textures(s, pass);
}

static void pbr_bind_material_resources(const struct AeronScene3D* s, AeronRenderPass* pass,
										const AeronSceneMesh* mesh, AeronSampler* atlas_sampler,
										int all_channels) {
	const uint32_t channel_count = all_channels ? AERON_GLTF_CHANNEL_COUNT : 1u;
	for (uint32_t channel = 0; channel < channel_count; ++channel) {
		Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, channel,
								 mesh->atlas[channel] ? mesh->atlas[channel]
													  : AeronSceneInternal_WhiteArrayTexture(),
								 atlas_sampler);
	}
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 0, mesh->material_buffer);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 1, mesh->variant_buffer);
	/* Colour shaders read the screen rectangles (storage 5); a mesh without
	 * any binds the frame buffer in their place, which it never reads. The
	 * `_anim` shaders also read the animation records (storage 7). */
	if (all_channels) {
		Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 5,
								mesh->screen_rect_buffer ? mesh->screen_rect_buffer : s->screen_frame_buffer);
		if (mesh->anim_buffer && s->effect_atlas)
			Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 7, mesh->anim_buffer);
	}
}

/* Binds the (kind, cull) pipeline unless it is already bound; *bound is the
 * pass's current pipeline. Resource bindings and uniforms persist across
 * pipeline binds within the pass. */
static int pbr_bind_pipeline(struct AeronScene3D* s, AeronRenderPass* pass, int kind, AeronCullMode cull,
							 int anim, AeronGraphicsPipeline** bound) {
	AeronGraphicsPipeline* pipeline = pbr_draw_pipeline(s, kind, cull, anim);
	if (!pipeline) {
		return 0;
	}
	if (pipeline != *bound) {
		Aeron_BindGraphicsPipeline(pass, pipeline);
		*bound = pipeline;
	}
	return 1;
}

/* The probe-set key of instance `i`'s colour draws (pbr_bind_draw): -1
 * without a set, else the set times 2, plus 1 when the instance is the
 * owner of the first set and takes the floor's reflection. */
static int pbr_set_key(const struct AeronScene3D* s, int i) {
	const int set = s->prepared_instances[i].probe_set;
	if (set < 0) {
		return -1;
	}
	const int receiver = set == 0 && s->instances[i].probe_set_owner == 1 && s->planar_reflection.active &&
						 !s->planar_reflection.drawing;
	return set * 2 + receiver;
}

/* Binds a colour draw's pipeline. An instance with a probe set (`set_key`
 * >= 0, pbr_set_key) draws with the probe variant, its set bound in place
 * of the screen textures (t10, t11), its buffer at storage slot 4 and the
 * lighting block with its fields; `bound_set` tracks the key bound, -1 for
 * the screen textures, which screen draws get back. Debug views draw
 * without probes. */
static int pbr_bind_draw(struct AeronScene3D* s, AeronRenderPass* pass, int kind, AeronCullMode cull,
						 const AeronSceneMeshInstance* in, int set_key, AeronGraphicsPipeline** bound,
						 int* bound_set) {
	AeronGraphicsPipeline* probe = NULL;
	const int              anim  = pbr_instance_anim_level(s, in);
	const int              set   = set_key >= 0 ? set_key >> 1 : -1;
	if (set >= 0) {
#ifdef AERON_DEBUG_UI
		if (!pbr_debug_pipeline(s, kind, cull))
#endif
			probe = pbr_probe_pipeline(s, kind, cull, anim);
	}
	if (!probe) {
		if (pbr_screen_base_kind(kind) >= 0 && *bound_set >= 0) {
			pbr_bind_screen_textures(s, pass);
			*bound_set = -1;
		}
		if (anim && s->pbr_occlusion_in_effect_slot) {
			pbr_bind_effect_textures(s, pass);
		}
		return pbr_bind_pipeline(s, pass, kind, cull, anim, bound);
	}
	if (probe != *bound) {
		Aeron_BindGraphicsPipeline(pass, probe);
		*bound = probe;
		/* The probe occlusion grid and the floor's reflection follow the
		 * material and effect textures: t12 and t13, or t14 and t15 in the
		 * `_anim` shaders, whose effect textures at t12 and t13 they
		 * displace until an `_anim` draw binds them again. */
		if (anim && s->pbr_occlusion_in_effect_slot) {
			pbr_bind_effect_textures(s, pass);
		}
		Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, anim ? 14u : 12u,
								 AeronSceneProbeOcclusion_Grid(s), AeronSceneProbeOcclusion_Sampler(s));
		Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, anim ? 15u : 13u,
								 AeronScenePlanarReflection_Image(s), AeronScenePlanarReflection_Sampler(s));
		s->pbr_occlusion_in_effect_slot |= !anim;
	}
	if (set_key != *bound_set) {
		const AeronSceneProbeSet* p      = &s->probe_sets[set];
		const AeronProbeSet*      probes = p->probes;
		Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 10, probes->cubes, probes->sampler);
		Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 11, probes->grid, probes->sampler);
		Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 4, probes->buffer);
		AeronSceneLightingUniformGPU lighting = s->lighting_uniform;
		memcpy(lighting.probe_params, p->params, sizeof lighting.probe_params);
		memcpy(lighting.probe_strengths, p->strengths, sizeof lighting.probe_strengths);
		memcpy(lighting.world_to_model, p->world_to_model, sizeof lighting.world_to_model);
		memcpy(lighting.grid_scale, p->grid_scale, sizeof lighting.grid_scale);
		memcpy(lighting.grid_offset, p->grid_offset, sizeof lighting.grid_offset);
		AeronSceneProbeOcclusion_LightingFields(s, set, lighting.occlusion_origin, lighting.occlusion_dims);
		AeronScenePlanarReflection_LightingFields(s, set, set_key & 1, lighting.planar_rect,
												  lighting.planar_params, lighting.planar_eye,
												  lighting.planar_screen);
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 2, &lighting, sizeof lighting);
		*bound_set = set_key;
	}
	return 1;
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
	AeronGraphicsPipeline* bound = NULL;
	if (!pbr_bind_pipeline(s, pass, pipe_kind, AERON_CULL_NONE, 0, &bound)) {
		return 0;
	}
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, s->mesh_table_buffer);
	/* AO input at t4 (pbr FS declares it unconditionally); a white 1x1
	 * placeholder keeps AO reading as 1 when SSAO is off. Prepass FS
	 * samples nothing. */
	if (!depth_only) {
		pbr_bind_ao(s, pass, ao_tex);
		AeronSceneDirectionalShadow_Bind(s, pass);
	}

	AeronSampler* atlas_sampler = s->mesh_sampler ? s->mesh_sampler : s->pbr_sampler;
	const int     screen_kind   = depth_only ? -1 : pbr_screen_kind(pipe_kind);
	const int     screen_shadow = !depth_only && pipe_kind == AERON_PBR_PIPE_FORWARD && ao_tex &&
								  s->shadow_stats.active && !s->directional_shadow.debug_cascades;

	const AeronSceneMesh* bound_mesh                  = NULL;
	int                   bound_receiver_local_shadow = 0;
	int                   bound_set                   = -1;
	/* The floor's mirrored view draws its own instances. */
	const uint16_t* order = s->planar_reflection.drawing ? s->planar_reflection.order : s->camera_order;
	const int       order_count =
		s->planar_reflection.drawing ? s->planar_reflection.order_count : s->camera_order_count;
	for (int k = 0; k < order_count; ++k) {
		const int                     i  = order[k];
		const AeronSceneMeshInstance* in = &s->instances[i];
		const AeronSceneMesh*         m  = in->mesh;
		/* A mesh without opaque faces, such as a laser bolt, draws nothing
		 * here. */
		if (!m || !m->vbo || !m->ibo || m->opaque_index_count == 0) {
			continue;
		}
		/* The emission pipeline draws these in the transparent pass. They
		 * keep their depth prepass, which hides their own back faces. */
		if (!depth_only && !s->planar_reflection.drawing && s->prepared_instances[i].emission) {
			continue;
		}
		/* Small instances leave the prepass (AeronScene_SetSmallInstanceCull). */
		if (depth_only && s->small_cull_active && !s->planar_reflection.drawing &&
			s->prepared_instances[i].screen_size_px < s->small_cull_prepass_px) {
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

		/* Per-instance cull variant. */
		const AeronCullMode cull = (AeronCullMode)in->cull_mode;
		const int           set  = depth_only ? -1 : pbr_set_key(s, i);
		if (!pbr_bind_draw(s, pass, pipe_kind, cull, in, set, &bound, &bound_set)) {
			return 0;
		}

		if (m != bound_mesh) {
			bound_mesh = m;
			Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
			Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT32, 0);
			if (!depth_only) {
				pbr_bind_material_resources(s, pass, m, atlas_sampler, 1);
			}
		}

		/* Velocity prepass projects through the previous camera; other
		 * passes use prev = current (zero velocity, VS prev unused). A
		 * zero_velocity instance stays prev = current even there. */
		pbr_push_instance_uniforms(s, pass, in, m, &s->prepared_instances[i], velocity, depth_only,
								   screen_shadow);

		/* Opaque range only. Mask and blend ranges use their own sweeps.
		 * Colour passes draw the screen tail with the screen pipeline. */
		const uint32_t screen_count = screen_kind >= 0 ? m->opaque_screen_index_count : 0u;
		const uint32_t plain_count  = m->opaque_index_count - screen_count;
		if (plain_count != 0) {
			Aeron_DrawIndexed(pass, plain_count, 0, 0);
		}
		if (screen_count != 0) {
			if (!pbr_bind_draw(s, pass, screen_kind, cull, in, -1, &bound, &bound_set)) {
				return 0;
			}
			Aeron_DrawIndexed(pass, screen_count, plain_count, 0);
		}
	}

	/* Masked geometry is opaque after its material cutoff, but requires base
	 * alpha and variant lookup in every depth/color pass. Colour passes draw
	 * the screen tail with the mask-screen pipeline. */
	const int mask_kind        = pbr_mask_kind(pipe_kind);
	const int mask_screen_kind = depth_only || mask_kind < 0 ? -1 : pbr_screen_kind(mask_kind);
	if (mask_kind >= 0) {
		bound_mesh = NULL;
		for (int k = 0; k < order_count; ++k) {
			const int                     i  = order[k];
			const AeronSceneMeshInstance* in = &s->instances[i];
			const AeronSceneMesh*         m  = in->mesh;
			if (!m || !m->vbo || !m->ibo || m->mask_index_count == 0) {
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
			const AeronCullMode cull         = (AeronCullMode)in->cull_mode;
			const uint32_t      screen_count = mask_screen_kind >= 0 ? m->mask_screen_index_count : 0u;
			const uint32_t      plain_count  = m->mask_index_count - screen_count;
			const int           set          = depth_only ? -1 : pbr_set_key(s, i);
			if (!pbr_bind_draw(s, pass, plain_count != 0 ? mask_kind : mask_screen_kind, cull, in, set, &bound,
							   &bound_set)) {
				return 0;
			}
			if (m != bound_mesh) {
				bound_mesh = m;
				Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
				Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT32, 0);
				pbr_bind_material_resources(s, pass, m, atlas_sampler, !depth_only);
			}
			pbr_push_instance_uniforms(s, pass, in, m, &s->prepared_instances[i], velocity, depth_only,
									   screen_shadow);
			if (plain_count != 0) {
				Aeron_DrawIndexed(pass, plain_count, m->mask_index_offset, 0);
			}
			if (screen_count != 0) {
				if (!pbr_bind_draw(s, pass, mask_screen_kind, cull, in, -1, &bound, &bound_set)) {
					return 0;
				}
				Aeron_DrawIndexed(pass, screen_count, m->mask_index_offset + plain_count, 0);
			}
		}
	}

	/* ---- Velocity STAMP sweep (velocity prepass only): the alpha-BLEND
	 * ranges of velocity_stamp instances, after ALL opaque prepass
	 * geometry so the GE depth test sees the complete laid depth. Writes
	 * motion vectors for the visible (alpha-tested) texels only; depth
	 * and normal stay untouched. */
	if (depth_only && velocity) {
		bound_mesh = NULL;
		pbr_reset_batches(s);
		for (int k = 0; k < order_count; ++k) {
			const int                     i  = order[k];
			const AeronSceneMeshInstance* in = &s->instances[i];
			const AeronSceneMesh*         m  = in->mesh;
			if (!m || !m->vbo || !m->ibo || (m->blend_index_count == 0 && !s->prepared_instances[i].emission)) {
				continue;
			}
			if (in->velocity_stamp_screens && !in->velocity_stamp && m->blend_screen_index_count != 0) {
				/* The screens only: visibility from their content, which
				 * needs the screen textures and storage. */
				if (!pbr_bind_pipeline(s, pass, AERON_PBR_PIPE_PREPASS_STAMP_SCREEN,
									   (AeronCullMode)in->cull_mode, 0, &bound)) {
					return 0;
				}
				bound_mesh = NULL;
				Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
				Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT32, 0);
				Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0,
										 m->atlas[0] ? m->atlas[0] : AeronSceneInternal_WhiteArrayTexture(),
										 atlas_sampler);
				AeronSampler* screen_sampler =
					s->post_linear_sampler ? s->post_linear_sampler : s->pbr_sampler;
				Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 1,
										 s->screen_graphics ? s->screen_graphics
															: AeronSceneInternal_WhiteTexture(),
										 screen_sampler);
				Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 2,
										 s->screen_text ? s->screen_text : AeronSceneInternal_WhiteTexture(),
										 screen_sampler);
				Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 0, m->material_buffer);
				Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 1, m->variant_buffer);
				Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 2,
										m->screen_rect_buffer ? m->screen_rect_buffer
															  : s->screen_frame_buffer);
				Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 3, s->screen_frame_buffer);
				pbr_push_instance_uniforms(s, pass, in, m, &s->prepared_instances[i],
										   /*velocity=*/1, /*depth_only=*/1,
										   /*screen_shadow=*/0);
				Aeron_DrawIndexed(pass, m->blend_screen_index_count,
								  m->blend_index_offset + m->blend_index_count - m->blend_screen_index_count,
								  0);
				continue;
			}
			if (!in->velocity_stamp) {
				continue;
			}
			/* A batch draws all its instances where its first one comes. */
			const int32_t          batch_index = s->prepared_instances[i].batch;
			AeronScenePbrBatch*    batch       = batch_index >= 0 ? &s->pbr_batches[batch_index] : NULL;
			AeronGraphicsPipeline* instanced   = NULL;
			int                    emission    = 0;
			if (batch) {
				if (batch->drawn) {
					continue;
				}
				/* Widen these batches as their colour draw does
				 * (AeronScene_SetEmissionMinWidth). */
				if (batch->emission) {
					instanced = pbr_emission_stamp_pipeline(s, (AeronCullMode)in->cull_mode);
					emission  = instanced != NULL;
				}
				if (!instanced) {
					instanced =
						pbr_instanced_pipeline(s, AERON_PBR_PIPE_PREPASS_STAMP, (AeronCullMode)in->cull_mode);
				}
			}
			if (instanced) {
				if (instanced != bound) {
					Aeron_BindGraphicsPipeline(pass, instanced);
					bound = instanced;
				}
			} else if (!pbr_bind_pipeline(s, pass, AERON_PBR_PIPE_PREPASS_STAMP, (AeronCullMode)in->cull_mode,
										  0, &bound)) {
				return 0;
			}
			if (m != bound_mesh) {
				bound_mesh = m;
				Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
				Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT32, 0);
				Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0,
										 m->atlas[0] ? m->atlas[0] : AeronSceneInternal_WhiteArrayTexture(),
										 atlas_sampler);
				Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 0, m->material_buffer);
				Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 1, m->variant_buffer);
			}
			if (instanced) {
				batch->drawn = 1;
				pbr_push_batch_uniforms(s, pass, batch, /*velocity=*/1);
				if (emission) {
					pbr_push_emission_uniforms(s, pass, m, &s->emission, s->emission.min_width_px,
											   s->emission.scale, NULL);
					if (m->opaque_index_count != 0)
						Aeron_DrawIndexedInstanced(pass, m->opaque_index_count, batch->count, 0, 0);
				}
				if (m->blend_index_count != 0)
					Aeron_DrawIndexedInstanced(pass, m->blend_index_count, batch->count, m->blend_index_offset,
											   0);
				continue;
			}
			pbr_push_instance_uniforms(s, pass, in, m, &s->prepared_instances[i],
									   /*velocity=*/1, /*depth_only=*/1,
									   /*screen_shadow=*/0);
			Aeron_DrawIndexed(pass, m->blend_index_count, m->blend_index_offset, 0);
		}
	}
	return 1;
}

/* Draws the opaque or the blended faces of an emission batch with the
 * emission pipeline. Returns 0 when the pipeline isn't there. It binds the
 * emissive atlas at sampler slot 1, where the other shaders read the normal
 * atlas, so the next draw has to bind its mesh's resources again. */
static int pbr_draw_emission_batch(struct AeronScene3D* s, AeronRenderPass* pass, const AeronScenePbrBatch* batch,
								   int blend_kind, int opaque, AeronSampler* atlas_sampler,
								   AeronGraphicsPipeline** bound) {
	const AeronSceneMesh*  m        = batch->mesh;
	AeronGraphicsPipeline* pipeline = pbr_emission_pipeline(s, blend_kind, (AeronCullMode)batch->cull_mode);
	if (!pipeline) {
		return 0;
	}
	const uint32_t count  = opaque ? m->opaque_index_count : m->blend_index_count;
	const uint32_t offset = opaque ? 0u : m->blend_index_offset;
	if (count == 0) {
		return 1;
	}
	if (pipeline != *bound) {
		Aeron_BindGraphicsPipeline(pass, pipeline);
		*bound = pipeline;
	}
	Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
	Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT32, 0);
	pbr_bind_material_resources(s, pass, m, atlas_sampler, 0);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 1,
							 m->atlas[AERON_GLTF_CHANNEL_EMISSIVE] ? m->atlas[AERON_GLTF_CHANNEL_EMISSIVE]
																   : AeronSceneInternal_WhiteArrayTexture(),
							 atlas_sampler);
	pbr_push_batch_uniforms(s, pass, batch, /*velocity=*/0);
	pbr_push_emission_uniforms(s, pass, m, &s->emission, s->emission.min_width_px, s->emission.scale, NULL);
	Aeron_DrawIndexedInstanced(pass, count, batch->count, offset, 0);
	return 1;
}

int AeronScenePbr_DrawEmissionBloom(struct AeronScene3D* s, AeronRenderPass* pass,
									const AeronSceneEmissionParams* p, const AeronSceneEmissionBloomDraw* draw) {
	AeronTexture* depth = s->depth_rt ? Aeron_DepthTargetGetTexture(s->depth_rt) : NULL;
	if (!s->storage_ready || !depth) {
		return 0;
	}
	if (!s->pbr_emission_bloom_depth_sampler) {
		s->pbr_emission_bloom_depth_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
			.min_filter = AERON_FILTER_NEAREST,
			.mag_filter = AERON_FILTER_NEAREST,
			.mip_filter = AERON_FILTER_NEAREST,
			.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
		});
		if (!s->pbr_emission_bloom_depth_sampler) {
			return 0;
		}
	}
	Aeron_SetViewport(pass, &draw->viewport);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, s->mesh_table_buffer);
	/* Bindings persist across pipelines and draws in a pass. */
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 2, depth, s->pbr_emission_bloom_depth_sampler);
	AeronSampler*          atlas_sampler = s->mesh_sampler ? s->mesh_sampler : s->pbr_sampler;
	AeronGraphicsPipeline* bound         = NULL;
	for (uint32_t b = 0; b < s->pbr_batch_count; ++b) {
		const AeronScenePbrBatch* batch = &s->pbr_batches[b];
		const AeronSceneMesh*     m     = batch->mesh;
		if (!batch->emission || m->opaque_index_count + m->blend_index_count == 0) {
			continue;
		}
		AeronGraphicsPipeline* pipeline = pbr_emission_bloom_pipeline(s, (AeronCullMode)batch->cull_mode);
		if (!pipeline) {
			return 0;
		}
		if (pipeline != bound) {
			Aeron_BindGraphicsPipeline(pass, pipeline);
			bound = pipeline;
		}
		Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
		Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT32, 0);
		pbr_bind_material_resources(s, pass, m, atlas_sampler, 0);
		Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 1,
								 m->atlas[AERON_GLTF_CHANNEL_EMISSIVE] ? m->atlas[AERON_GLTF_CHANNEL_EMISSIVE]
																	   : AeronSceneInternal_WhiteArrayTexture(),
								 atlas_sampler);
		pbr_push_batch_uniforms(s, pass, batch, /*velocity=*/0);
		pbr_push_emission_uniforms(s, pass, m, p, draw->min_width_px, draw->scale, draw);
		if (m->opaque_index_count != 0)
			Aeron_DrawIndexedInstanced(pass, m->opaque_index_count, batch->count, 0, 0);
		if (m->blend_index_count != 0)
			Aeron_DrawIndexedInstanced(pass, m->blend_index_count, batch->count, m->blend_index_offset, 0);
	}
	return 1;
}

int AeronScenePbr_DrawTransparentInstances(struct AeronScene3D* s, AeronRenderPass* pass, int pipe_kind,
										   AeronTexture* ao_tex, int layer) {
	const int blend_kind = pbr_blend_kind(pipe_kind);
	if (!s || !pass || blend_kind < 0 || !s->storage_ready) {
		return 0;
	}
	if (s->instance_count == 0) {
		return 1;
	}
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, s->mesh_table_buffer);
	/* Screen-space AO belongs to the nearest opaque/masked depth layer, not
	 * this later transparent layer. Bind white while preserving the already
	 * shaded background's AO. */
	pbr_bind_ao(s, pass, NULL);
	AeronSceneDirectionalShadow_Bind(s, pass);
	AeronSampler*          atlas_sampler               = s->mesh_sampler ? s->mesh_sampler : s->pbr_sampler;
	const AeronSceneMesh*  bound_mesh                  = NULL;
	AeronGraphicsPipeline* bound                       = NULL;
	int                    bound_receiver_local_shadow = 0;
	int                    bound_set                   = -1;
	pbr_reset_batches(s);
	/* Draw the opaque faces of emission batches, such as warheads,
	 * first. Blended faces in front of them, such as a nearer laser bolt,
	 * must be drawn after them, as they are after the opaque pass. What is
	 * behind them is hidden by their depth from the prepass. */
	if (!layer) {
		for (uint32_t b = 0; b < s->pbr_batch_count; ++b) {
			const AeronScenePbrBatch* batch = &s->pbr_batches[b];
			if (batch->emission && batch->mesh->opaque_index_count != 0) {
				pbr_draw_emission_batch(s, pass, batch, blend_kind, /*opaque=*/1, atlas_sampler, &bound);
				bound_mesh = NULL;
			}
		}
	}
	/* Far to near, so overlapping transparent instances blend in order. A
	 * batch draws all its instances, far to near, where its farthest one
	 * comes. */
	for (int k = s->camera_order_count - 1; k >= 0; --k) {
		const int                     i  = s->camera_order[k];
		const AeronSceneMeshInstance* in = &s->instances[i];
		const AeronSceneMesh*         m  = in->mesh;
		if (!m || !m->vbo || !m->ibo || m->blend_index_count == 0) {
			continue;
		}
		/* The instance's screens go to the screen layer. */
		const int layered =
			s->screen_layer_active && in->screens_to_layer && m->blend_screen_index_count != 0;
		if (layer && !layered) {
			continue;
		}
		const int receiver_local_shadow =
			(in->shadow_flags & AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW) != 0;
		if (receiver_local_shadow != bound_receiver_local_shadow) {
			AeronSceneDirectionalShadow_BindForInstance(s, pass, in->shadow_flags);
			bound_receiver_local_shadow = receiver_local_shadow;
		}
		const AeronCullMode cull        = (AeronCullMode)in->cull_mode;
		const int32_t       batch_index = s->prepared_instances[i].batch;
		if (batch_index >= 0) {
			AeronScenePbrBatch* batch = &s->pbr_batches[batch_index];
			if (batch->drawn) {
				continue;
			}
			if (batch->emission &&
				pbr_draw_emission_batch(s, pass, batch, blend_kind, /*opaque=*/0, atlas_sampler, &bound)) {
				bound_mesh   = NULL;
				batch->drawn = 1;
				continue;
			}
			AeronGraphicsPipeline* instanced = pbr_instanced_pipeline(s, blend_kind, cull);
			if (instanced) {
				if (instanced != bound) {
					Aeron_BindGraphicsPipeline(pass, instanced);
					bound = instanced;
				}
				if (m != bound_mesh) {
					bound_mesh = m;
					Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
					Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT32, 0);
					pbr_bind_material_resources(s, pass, m, atlas_sampler, 1);
				}
				batch->drawn = 1;
				pbr_push_batch_uniforms(s, pass, batch, /*velocity=*/0);
				Aeron_DrawIndexedInstanced(pass, m->blend_index_count, batch->count, m->blend_index_offset,
										   0);
				continue;
			}
		}
		if (!pbr_bind_draw(s, pass, blend_kind, cull, in, pbr_set_key(s, i), &bound, &bound_set)) {
			return 0;
		}
		if (m != bound_mesh) {
			bound_mesh = m;
			Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
			Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT32, 0);
			pbr_bind_material_resources(s, pass, m, atlas_sampler, 1);
		}
		pbr_push_instance_uniforms(s, pass, in, m, &s->prepared_instances[i],
								   /*velocity=*/0, /*depth_only=*/0,
								   /*screen_shadow=*/0);
		/* Screens last, as they lie inside a cockpit's canopy. */
		const uint32_t plain_count = m->blend_index_count - m->blend_screen_index_count;
		if (plain_count != 0 && !layer) {
			Aeron_DrawIndexed(pass, plain_count, m->blend_index_offset, 0);
		}
		if (m->blend_screen_index_count != 0 && layer == layered) {
			if (!pbr_bind_draw(s, pass, pbr_screen_kind(blend_kind), cull, in, -1, &bound, &bound_set)) {
				return 0;
			}
			Aeron_DrawIndexed(pass, m->blend_screen_index_count, m->blend_index_offset + plain_count, 0);
		}
	}
	(void)ao_tex;
	return 1;
}

int AeronScenePbr_DrawBackfaceFill(struct AeronScene3D* s, AeronRenderPass* pass) {
	if (!s || !pass || !s->storage_ready) {
		return 0;
	}
	if (!s->backface_fill || s->instance_count == 0) {
		return 1;
	}
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, s->mesh_table_buffer);
	const AeronSceneMesh* bound_mesh = NULL;
	AeronCullMode         bound_cull = AERON_CULL_NONE;
	for (int k = 0; k < s->camera_order_count; ++k) {
		const int                     i  = s->camera_order[k];
		const AeronSceneMeshInstance* in = &s->instances[i];
		const AeronSceneMesh*         m  = in->mesh;
		/* Two-sided instances already draw their back faces. */
		if (!m || !m->vbo || !m->ibo || m->opaque_index_count == 0 || in->cull_mode == AERON_CULL_NONE) {
			continue;
		}
		/* The faces the instance culls are its back faces under the
		 * caller's winding convention, so cull the opposite ones. */
		const AeronCullMode cull = in->cull_mode == AERON_CULL_BACK ? AERON_CULL_FRONT : AERON_CULL_BACK;
		if (cull != bound_cull) {
			AeronGraphicsPipeline* pipeline = AeronScenePbr_Pipeline(s, AERON_PBR_PIPE_BACKFACE_FILL, cull);
			if (!pipeline) {
				/* Cosmetic: keep the frame and stop retrying. */
				Aeron_LogWarn("aeron.scene",
							  "PBR back-face fill pipeline creation failed; back-face fill disabled");
				s->backface_fill = 0;
				return 1;
			}
			Aeron_BindGraphicsPipeline(pass, pipeline);
			bound_cull = cull;
		}
		if (m != bound_mesh) {
			bound_mesh = m;
			Aeron_BindVertexBuffer(pass, 0, m->vbo, 0);
			Aeron_BindIndexBuffer(pass, m->ibo, AERON_INDEX_FORMAT_UINT32, 0);
		}
		pbr_push_instance_uniforms(s, pass, in, m, &s->prepared_instances[i], /*velocity=*/0,
								   /*depth_only=*/1, /*screen_shadow=*/0);
		Aeron_DrawIndexed(pass, m->opaque_index_count, 0, 0);
	}
	return 1;
}
