#ifndef AERON_SCENE_INTERNAL_H
#define AERON_SCENE_INTERNAL_H

/* Shared internals of the aeron_scene translation units. NOT a public
 * API — games use the aeron_scene public headers only. */

#include "aeron/log.h"
#include "aeron/scene/scene3d.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/mesh_overlay.h"

#define AERON_SCENE_MAX_INSTANCES 2048
#define AERON_SCENE_MAX_LIGHTS AERON_SCENE_POINT_LIGHT_CAPACITY
#define AERON_SCENE_CLUSTER_MAX_LIGHTS AERON_SCENE_CLUSTER_LIGHT_CAPACITY
#define AERON_SCENE_CLUSTER_MAX_GLOBAL_LIGHTS 4u
#define AERON_SCENE_CLUSTER_DEFAULT_TILE_SIZE 32u
#define AERON_SCENE_CLUSTER_MAX_TILE_SIZE 64u
#define AERON_SCENE_CLUSTER_TILE_ALIGNMENT 8u
#define AERON_SCENE_CLUSTER_REFERENCE_HEIGHT 1080u
#define AERON_SCENE_CLUSTER_BRUTE_FORCE_MAX_LIGHTS 16u
#define AERON_SCENE_CLUSTER_DEFAULT_DEPTH_SLICES 24u
#define AERON_SCENE_CLUSTER_THREADS 64u
#define AERON_SCENE_MAX_BILLBOARDS 2304
#define AERON_SCENE_MAX_FRAME_UNIFORMS 4
#define AERON_SCENE_FRAME_UNIFORM_CAP AERON_MAX_UNIFORM_DATA_SIZE
#define AERON_SCENE_MAX_MESH_OVERLAYS 64
#define AERON_SCENE_MAX_OVERLAY_VERTS (256u * 1024u)
#define AERON_SCENE_MAX_SHADOW_ONLY AERON_SCENE_MAX_INSTANCES
#define AERON_SCENE_MAX_SHADOW_CASTERS (AERON_SCENE_MAX_INSTANCES + AERON_SCENE_MAX_SHADOW_ONLY)
#define AERON_SCENE_MAX_MESH_TABLES \
	(1 + AERON_SCENE_MAX_INSTANCES * 2 + AERON_SCENE_MAX_SHADOW_ONLY + AERON_SCENE_MAX_MESH_OVERLAYS)
#define AERON_SCENE_MESH_TABLE_HASH_CAP 4096
#define AERON_SCENE_SHADOW_GUARD_TEXELS 4
#define AERON_SCENE_RECEIVER_LOCAL_SHADOW_SIZE 1024

/* Pipeline kinds for AeronScenePbr_Pipeline / _DrawInstances. Color
 * pipelines are single-target; the prepass variants own normal, velocity
 * and temporal-depth outputs. */
enum {
	AERON_PBR_PIPE_MESH = 0,         /* monolithic color, depth write */
	AERON_PBR_PIPE_FORWARD,          /* color, depth-EQUAL/no-write */
	AERON_PBR_PIPE_PREPASS,          /* normal only */
	AERON_PBR_PIPE_PREPASS_VEL,      /* normal + velocity */
	AERON_PBR_PIPE_MESH_MASK,        /* alpha mask, monolithic color */
	AERON_PBR_PIPE_FORWARD_MASK,     /* alpha mask, deferred color */
	AERON_PBR_PIPE_PREPASS_MASK,     /* alpha mask normal only */
	AERON_PBR_PIPE_PREPASS_MASK_VEL, /* alpha mask normal + velocity */
	AERON_PBR_PIPE_MESH_BLEND,       /* blend range of MESH */
	AERON_PBR_PIPE_FORWARD_BLEND,    /* blend range of FORWARD (test GE) */
	AERON_PBR_PIPE_PREPASS_STAMP,    /* normal masked + velocity written */
	AERON_PBR_PIPE_PREPASS_TEMPORAL, /* normal + velocity + R32 depth export */
	AERON_PBR_PIPE_PREPASS_MASK_TEMPORAL, /* alpha mask normal + velocity + depth */
	AERON_PBR_PIPE_KIND_COUNT
};

/* One AeronScene_SetFrameUniformData blob (copied at submit). */
typedef struct AeronSceneFrameUniform {
	uint8_t  stage; /* AeronShaderStage */
	uint8_t  _pad[3];
	uint32_t slot;
	uint32_t size;
	uint8_t  data[AERON_SCENE_FRAME_UNIFORM_CAP];
} AeronSceneFrameUniform;

/* Compact per-frame record of one AeronScene_AddBillboard submission
 * (pointers from the desc are copied by value — no borrowing). */
typedef struct AeronSceneBb3dEntry {
	AeronTexture* texture;
	uint8_t       blend;
	uint8_t       stage;
	uint8_t       has_prev;
	float         depth_bias;
	float         corners[4][3];
	float         prev_corners[4][3];
	float         uv[4][2];
	float         colors[4][4];
	float         center_color[4];
	float         anchor[3]; /* LENS: flare source, world space */
} AeronSceneBb3dEntry;

typedef struct AeronSceneMeshOverlayEntry {
	AeronTexture*              texture;
	const AeronSceneMeshTable* mesh_table;
	uint32_t                   first_vertex;
	uint32_t                   vertex_count;
	uint32_t                   mesh_table_index;
	float                      transform[16];
	float                      uv_xform[4];
	float                      uv_rect[4];
	float                      color[4];
	float                      depth_bias;
	uint8_t                    blend;
	uint8_t                    cull_mode;
} AeronSceneMeshOverlayEntry;

typedef struct AeronScenePreparedInstance {
	uint32_t current_table_index;
	uint32_t previous_table_index;
	uint32_t local_light_base;
	uint32_t local_light_count;
} AeronScenePreparedInstance;

typedef struct AeronScenePointLightGPU {
	float position_range[4];
	float color[4];
} AeronScenePointLightGPU;

typedef struct AeronSceneClusterLightGPU {
	float view_position_range[4];
	uint32_t point_light_index;
	float    luminance;
	float    _pad[2];
} AeronSceneClusterLightGPU;

typedef struct AeronSceneClusterHeaderGPU {
	uint32_t count;
	uint32_t candidate_count;
} AeronSceneClusterHeaderGPU;

typedef struct AeronSceneClusterUniformGPU {
	float    camera_position[3];
	float    near_z;
	float    camera_forward[3];
	float    slice_scale;
	float    tan_h_half;
	float    tan_v_half;
	float    proj_x_offset;
	float    proj_y_offset;
	uint32_t viewport_x;
	uint32_t viewport_y;
	uint32_t viewport_width;
	uint32_t viewport_height;
	uint32_t grid_x;
	uint32_t grid_y;
	uint32_t grid_z;
	uint32_t point_light_count;
	float    point_min_distance;
	float    point_contribution_cap;
	uint32_t tile_size;
	uint32_t flags;
	uint32_t global_light_indices[AERON_SCENE_CLUSTER_MAX_GLOBAL_LIGHTS];
} AeronSceneClusterUniformGPU;

enum {
	AERON_SCENE_CLUSTER_ENABLED    = 1u << 0,
	AERON_SCENE_CLUSTER_DEBUG_VIEW = 1u << 1,
};

/* FS b1, shared by the scene shadow module and the PBR draw path. */
typedef struct AeronSceneDirectionalShadowUniform {
	float view_proj[AERON_SCENE_SHADOW_MAX_CASCADES][16];
	float atlas_scale_bias[AERON_SCENE_SHADOW_MAX_CASCADES][4];
	float atlas_clamp[AERON_SCENE_SHADOW_MAX_CASCADES][4];
	/* near, far, transition start, normalized depth bias per texel. */
	float split_data[AERON_SCENE_SHADOW_MAX_CASCADES][4];
	/* world units/texel XY, normalized depth/texel XY. */
	float texel_data[AERON_SCENE_SHADOW_MAX_CASCADES][4];
	/* enabled, cascade count, filter quality, cascade debug. */
	float params[4];
	/* xyz = position; w reserved. */
	float camera_pos[4];
	/* xyz = camera forward; w = quality-2 filter radius in atlas texels. */
	float camera_forward[4];
	/* normal bias texels, depth bias texels, reserved, max distance. */
	float bias[4];
	/* distance fade start/end, inverse atlas size, reserved. */
	float fade[4];
	/* enabled, tan(light angular radius), max/min radius in atlas texels. */
	float pcss[4];
	/* enabled, FSR temporal phase, reserved, reserved. */
	float pcss_temporal[4];
	/* xyz = normalized surface-to-light direction. */
	float light_dir[4];
} AeronSceneDirectionalShadowUniform;
typedef char AeronSceneDirectionalShadowUniformSizeCheck
	[sizeof(AeronSceneDirectionalShadowUniform) == 640 ? 1 : -1];

typedef struct AeronSceneShadowFitHistory {
	int      valid;
	uint32_t fit_mode;
	uint32_t tile_size;
	float    light_dir[3];
	float    x_axis[3];
	float    reference_extent;
	float    extent[2];
	uint16_t shrink_frames[2];
} AeronSceneShadowFitHistory;

struct AeronScene3D {
	AeronRenderTarget* color_rt;
	AeronDepthTarget*  depth_rt;
	AeronRenderTarget* normal_rt; /* NULL when not requested */
	AeronRenderTarget* msaa_color_rt;
	AeronDepthTarget*  msaa_depth_rt;
	int                output_w, output_h;
	int                render_w, render_h;
	int                with_normal_rt;
	AeronTextureFormat color_format;
	AeronSampleCount   sample_count;

	AeronSceneCamera camera;        /* render-resolution viewport */
	AeronSceneCamera output_camera; /* caller's output-resolution viewport */
	float            jittered_view_proj[16];
	float            unjittered_view_proj[16];
	float            clear_rgba[4];

	AeronScenePassHookFn hook_fn[AERON_SCENE_HOOK_COUNT];
	void*                hook_user[AERON_SCENE_HOOK_COUNT];
	AeronSceneAfterMeshesFn after_meshes_fn;
	void*                   after_meshes_user;

	AeronSceneMeshInstance instances[AERON_SCENE_MAX_INSTANCES];
	AeronScenePreparedInstance prepared_instances[AERON_SCENE_MAX_INSTANCES];
	int                    instance_count;
	int                    instances_dropped;
	AeronSceneLight        lights[AERON_SCENE_MAX_LIGHTS];
	int                    light_count;
	int                    lights_dropped;
	AeronSceneFrameUniform frame_uniforms[AERON_SCENE_MAX_FRAME_UNIFORMS];
	int                    frame_uniform_count;

	/* Stabilized cascaded shadow map for the key directional light. */
	AeronSceneDirectionalShadowDesc    directional_shadow;
	AeronSceneDirectionalShadowUniform shadow_uniform;
	AeronSceneDirectionalShadowStats   shadow_stats;
	AeronSceneMeshInstance             shadow_only[AERON_SCENE_MAX_SHADOW_ONLY];
	AeronScenePreparedInstance         prepared_shadow_only[AERON_SCENE_MAX_SHADOW_ONLY];
	int                                shadow_only_count;
	int                                shadow_only_dropped;
	uint16_t shadow_casters[AERON_SCENE_SHADOW_MAX_CASCADES][AERON_SCENE_MAX_SHADOW_CASTERS];
	uint16_t shadow_caster_count[AERON_SCENE_SHADOW_MAX_CASCADES];
	AeronSceneShadowFitHistory         shadow_fit_history[AERON_SCENE_SHADOW_MAX_CASCADES];
	AeronSceneDirectionalShadowUniform receiver_local_shadow_uniform;
	uint16_t                           receiver_local_shadow_casters[AERON_SCENE_MAX_SHADOW_CASTERS];
	uint16_t                           receiver_local_shadow_caster_count;
	AeronDepthTarget*                  receiver_local_shadow_atlas;
	int                                shadow_tried;
	uint32_t                           shadow_resource_atlas_size;
	AeronDepthTarget*                  shadow_atlas;
	AeronDepthTarget*                  shadow_fallback;
	AeronSampler*                      shadow_sampler;
	AeronSampler*                      shadow_depth_sampler;
	AeronShader*                       shadow_vs;
	AeronShader*                       shadow_fs;
	AeronGraphicsPipeline*             shadow_pipes[3];
	AeronShader*                       shadow_mask_vs;
	AeronShader*                       shadow_mask_fs;
	AeronGraphicsPipeline*             shadow_mask_pipes[3];
	int                                shadow_debug_tried;
	AeronShader*                       shadow_debug_vs;
	AeronShader*                       shadow_debug_fs;
	AeronGraphicsPipeline*             shadow_debug_pipe;

	/* One pre-pass upload shared by PBR, shadows, and overlays. Mesh tables
	 * are deduplicated by borrowed pointer; local and point lights are packed. */
	const AeronSceneMeshTable* mesh_table_keys[AERON_SCENE_MESH_TABLE_HASH_CAP];
	uint32_t                   mesh_table_values[AERON_SCENE_MESH_TABLE_HASH_CAP];
	AeronSceneMeshTable*       mesh_table_staging;
	uint32_t                   mesh_table_count;
	uint32_t                   mesh_table_staging_cap;
	AeronSceneLightGPU*        local_light_staging;
	uint32_t                   local_light_count;
	uint32_t                   local_light_staging_cap;
	AeronScenePointLightGPU*   point_light_staging;
	uint32_t                   point_light_count;
	uint32_t                   point_light_staging_cap;
	AeronSceneClusterLightGPU* cluster_light_staging;
	uint32_t                   cluster_light_staging_cap;
	AeronBuffer*               mesh_table_buffer;
	uint32_t                   mesh_table_buffer_cap;
	AeronBuffer*               local_light_buffer;
	uint32_t                   local_light_buffer_cap;
	AeronBuffer*               point_light_buffer;
	uint32_t                   point_light_buffer_cap;
	AeronBuffer*               cluster_light_buffer;
	uint32_t                   cluster_light_buffer_cap;
	int                        storage_ready;
	int                        storage_error_logged;

	/* Clustered-forward point-light allocation. */
	AeronSceneClusteredLightDesc cluster_desc;
	AeronSceneClusterUniformGPU  cluster_uniform;
	AeronBuffer*                 cluster_header_buffer;
	uint32_t                     cluster_header_buffer_cap;
	AeronBuffer*                 cluster_index_buffer;
	uint32_t                     cluster_index_buffer_cap;
	AeronComputePipeline*        cluster_build_pipeline;
	float                        cluster_far_z;
	float                        cluster_near_z;
	uint32_t                     cluster_far_shrink_frames;
	uint32_t                     cluster_count;
	uint32_t                     cluster_global_count;
	uint32_t                     cluster_global_indices[AERON_SCENE_CLUSTER_MAX_GLOBAL_LIGHTS];
	uint32_t                     cluster_light_count;
	int                          cluster_active;
	int                          cluster_tried;
	int                          cluster_ready;

	/* Compact receiver-local mesh overlays (decals, highlights, etc.). */
	AeronSceneMeshOverlayEntry   overlays[AERON_SCENE_MAX_MESH_OVERLAYS];
	uint32_t                     overlay_count;
	uint32_t                     overlays_dropped;
	AeronSceneMeshOverlayVertex* overlay_vertices;
	uint32_t                     overlay_vertex_count;
	uint32_t                     overlay_vertex_cap;
	AeronBuffer*                 overlay_vb;
	uint32_t                     overlay_vb_cap;
	int                          overlay_frame_ready;
	int                          overlay_tried;
	AeronShader*                 overlay_vs;
	AeronShader*                 overlay_fs;
	AeronGraphicsPipeline*       overlay_pipes[3][3];

	/* Post stack (post.c). */
	AeronScenePostDesc     post;
	AeronSceneTemporalDesc temporal;
	AeronTemporalMode      temporal_active_mode;
	uint64_t               temporal_phase;
	float                  view_space_to_meters;
	float                  temporal_jitter[2];
	int                    temporal_tried;
	int                    temporal_active;
	AeronTemporalUpscaler* temporal_upscaler;
	AeronRenderTarget*     temporal_depth_rt;
	AeronRenderTarget*     temporal_output_rt;
	int                    temporal_output_allocation_failed;
	AeronShader*           temporal_sky_velocity_ps;
	AeronGraphicsPipeline* temporal_sky_velocity_pipeline;
	AeronShader*           temporal_copy_ps;
	AeronGraphicsPipeline* temporal_copy_pipeline;
	AeronSampler*          temporal_copy_sampler;
	float                  mb_prev_view_proj[16];
	float                  mb_prev_ori[4]; /* previous frame's camera orientation (rotation gate) */
	int                    mb_velocity_regen;
	int                    mb_velocity_valid;
	AeronRenderTarget*     scene_rt_out; /* color_rt or mb_rt after Render */
	int                    scene_rt_out_borrowed;
	int                    post_ssao_tried;
	int                    post_mb_tried;
	AeronShader*           fullscreen_vs;       /* scene_fullscreen_quad.vert, shared */
	AeronSampler*          post_point_sampler;  /* NEAREST clamp (depth/velocity) */
	AeronSampler*          post_linear_sampler; /* LINEAR clamp (ao/color) */
	/* Half-resolution SSAO + opaque directional-shadow visibility. */
	AeronShader*           ssao_ps;
	AeronShader*           ssao_blur_ps;
	AeronShader*           ssao_blur_lq_ps;
	AeronGraphicsPipeline* ssao_pipeline;
	AeronGraphicsPipeline* ssao_blur_pipeline;
	AeronGraphicsPipeline* ssao_blur_lq_pipeline;
	AeronRenderTarget*     ao_rt;
	AeronRenderTarget*     ao_blur_rt;
	int                    ao_rt_w, ao_rt_h;
	AeronTexture*          ssao_noise_tex;
#ifdef AERON_DEBUG_UI
	int                    post_ssao_debug_tried;
	AeronShader*           ssao_debug_ps;
	AeronGraphicsPipeline* ssao_debug_pipeline;
#endif
	/* Motion blur */
	AeronShader*           mb_camera_fill_ps;
	AeronShader*           mb_velocity_viz_ps;
	AeronShader*           mb_temporal_velocity_ps;
	AeronShader*           mb_reconstruct_ps;
	AeronShader*           mb_neighbormax_ps;
	AeronGraphicsPipeline* mb_camera_fill_pipeline;
	AeronGraphicsPipeline* mb_velocity_viz_pipeline;
	AeronGraphicsPipeline* mb_velocity_viz_output_pipeline;
	AeronGraphicsPipeline* mb_temporal_velocity_pipeline;
	AeronComputePipeline*  mb_temporal_tilemax_pipeline;
	AeronComputePipeline*  mb_fsr_tilemax_pipeline;
	AeronComputePipeline*  mb_tilemax_compute_pipeline;
	AeronGraphicsPipeline* mb_reconstruct_pipeline;
	AeronGraphicsPipeline* mb_neighbormax_pipeline;
	AeronRenderTarget*     velocity_rt;
	AeronRenderTarget*     mb_temporal_velocity_rt;
	int                    mb_temporal_motion_valid;
	int                    mb_temporal_motion_direct;
	int                    mb_temporal_tile_valid;
	AeronRenderTarget*     mb_rt;
	AeronRenderTarget*     mb_tile_rt;
	AeronRenderTarget*     mb_neighbor_rt;
	int                    mb_tile_w, mb_tile_h;

	/* Batched billboards (billboards3d.c). Entry array +
	 * CPU vertex scratch are lazily allocated on first AddBillboard;
	 * pipelines/shaders on first Render with entries. */
	AeronSceneBb3dEntry*   bb_entries;
	int                    bb_count;
	int                    bb_dropped;
	void*                  bb_verts; /* Bb3dVert scratch, entries*12 */
	AeronBuffer*           bb_vb;
	uint32_t               bb_vb_cap;
	int                    bb_tried;
	AeronShader*           bb_vs;
	AeronShader*           bb_fs;
	AeronShader*           bb_vel_vs;
	AeronShader*           bb_vel_fs;
	AeronGraphicsPipeline* bb_pipes[3]; /* [AeronSceneBillboardBlend] */
	AeronGraphicsPipeline* bb_vel_pipe;
	AeronGraphicsPipeline* bb_temporal_vel_pipe;
	AeronSampler*          bb_sampler;
	uint32_t               bb_frame_verts; /* built this frame */
	int                    bb_frame_has_vel;
	/* LENS stage (lazily built on first lens entry; games that never
	 * submit one pay nothing). */
	int                    bb_lens_tried;
	AeronShader*           bb_lens_vs;
	AeronShader*           bb_lens_fs;
	AeronGraphicsPipeline* bb_lens_pipes[3]; /* [AeronSceneBillboardBlend] */
	AeronSampler*          bb_depth_sampler; /* NEAREST clamp */
	int                    bb_frame_has_lens;

	/* Sky cube (AeronScene_SetSkyCube). Cube pointer + basis + exposure
	 * are per-frame (reset by Begin); shaders/pipelines are lazily
	 * created on the first Render with a sky and kept. Two pipeline
	 * variants mirror the color-pass RT layouts (normal RT riding along
	 * write-masked vs single target). */
	AeronTexture*          sky_cube; /* borrowed for the frame; NULL = no sky */
	float                  sky_world_to_cube[9];
	float                  sky_exposure;
	int                    sky_tried;
	AeronShader*           sky_vs;
	AeronShader*           sky_fs;
	AeronGraphicsPipeline* sky_pipe;

	/* PBR material class (pbr.c), lazily initialized on the first frame that
	 * requires mesh or geometry-backed post-processing. */
	int pbr_tried;
	int pbr_debug_views;
	/* Immutable world samplers for OFF/Native AA/Quality/Balanced/Performance.
	 * pbr_sampler and mesh_sampler select the active entries. */
	AeronSampler* pbr_samplers[AERON_TEMPORAL_PERFORMANCE + 1];
	AeronSampler* pbr_sampler;
	AeronSampler* mesh_sampler_source; /* borrowed identity; caller retains ownership */
	AeronSampler* mesh_samplers[AERON_TEMPORAL_PERFORMANCE + 1];
	AeronSampler* mesh_sampler;
	AeronShader*  pbr_vs;
	AeronShader*  pbr_prepass_vs;
	AeronShader*  pbr_prepass_mask_vs;
	AeronShader*  pbr_prepass_stamp_vs;
	AeronShader*  pbr_fs;
	AeronShader*  pbr_mask_fs;
#ifdef AERON_DEBUG_UI
	int          pbr_debug_tried;
	AeronShader* pbr_debug_fs;
	AeronShader* pbr_mask_debug_fs;
#endif
	AeronShader* pbr_prepass_fs;
	AeronShader* pbr_prepass_mask_fs;
	/* Pipeline matrix: kind x cull mode. Cull-NONE variants are created
	 * by Ensure (the long-standing behavior); other cull modes are
	 * created lazily on first use (per-instance cull_mode). */
	AeronShader*           pbr_prepass_stamp_fs;
	AeronGraphicsPipeline* pbr_pipes[AERON_PBR_PIPE_KIND_COUNT][3];
#ifdef AERON_DEBUG_UI
	AeronGraphicsPipeline* pbr_debug_pipes[AERON_PBR_PIPE_KIND_COUNT][3];
#endif
};

/* Shared small helpers (util.c). */
AeronShader* AeronSceneInternal_CompileShader(const char* name, AeronShaderStage stage, uint32_t samplers,
											  uint32_t ubs, uint32_t sbs);
AeronRenderTarget*         AeronSceneInternal_CreateColorRt(AeronTextureFormat fmt, int w, int h,
															const char* debug_name);
AeronBlendStateDesc        AeronSceneInternal_BlendOpaque(void);
AeronTexture*              AeronSceneInternal_WhiteTexture(void);
const AeronSceneMeshTable* AeronScenePbr_IdentityTable(void);
int                        AeronSceneStorage_Prepare(struct AeronScene3D* s,
													AeronCommandBuffer* cmd);
void                       AeronSceneStorage_Release(struct AeronScene3D* s);
uint32_t                   AeronSceneStorage_ShadowTableIndex(
						  const struct AeronScene3D* s, uint16_t encoded_caster);

/* clustered_lights.c */
void AeronSceneClusteredLights_Classify(struct AeronScene3D* s);
int  AeronSceneClusteredLights_Ensure(struct AeronScene3D* s);
int  AeronSceneClusteredLights_Build(struct AeronScene3D* s, AeronCommandBuffer* cmd);
void AeronSceneClusteredLights_Bind(struct AeronScene3D* s, AeronRenderPass* pass);
void AeronSceneClusteredLights_Release(struct AeronScene3D* s);

/* pbr.c */
int  AeronScenePbr_Ensure(struct AeronScene3D* s);
void AeronScenePbr_Release(struct AeronScene3D* s);
/* Pipeline for (kind, cull), lazily created; NULL on create failure.
 * Falls back to the cull-NONE variant if the requested cull mode
 * cannot be built. */
AeronGraphicsPipeline* AeronScenePbr_Pipeline(struct AeronScene3D* s, int kind, AeronCullMode cull);
/* Draw every submitted instance with the `pipe_kind` variant matching
 * each instance's cull_mode (rebinds only when it changes; instances
 * are typically uniform). `depth_only` skips the fragment-side binds
 * the prepass FS doesn't consume. */
int AeronScenePbr_DrawInstances(struct AeronScene3D* s, AeronCommandBuffer* cmd, AeronRenderPass* pass,
								int pipe_kind, int depth_only, int velocity, AeronTexture* ao_tex);
int AeronScenePbr_DrawTransparentInstances(struct AeronScene3D* s, AeronRenderPass* pass, int pipe_kind,
										   AeronTexture* ao_tex);

/* directional_shadow.c */
int AeronSceneDirectionalShadow_Prepare(struct AeronScene3D* scene);
int AeronSceneDirectionalShadow_Render(struct AeronScene3D* scene, AeronCommandBuffer* command_buffer);
void AeronSceneDirectionalShadow_Bind(struct AeronScene3D* scene, AeronRenderPass* render_pass);
void AeronSceneDirectionalShadow_BindForInstance(struct AeronScene3D* scene, AeronRenderPass* render_pass,
												 uint8_t shadow_flags);
void AeronSceneDirectionalShadow_BindScreen(struct AeronScene3D* scene, AeronRenderPass* render_pass);
void AeronSceneDirectionalShadow_BindDisabled(struct AeronScene3D* scene, AeronRenderPass* render_pass);
int  AeronSceneDirectionalShadow_DebugVisualize(struct AeronScene3D* scene,
												AeronCommandBuffer*  command_buffer);
void AeronSceneDirectionalShadow_Release(struct AeronScene3D* scene);

/* billboards3d.c — the batched AddBillboard machinery.
 * Prepare builds + uploads the frame VB (copy pass; call before any
 * render pass opens) and lazily creates pipelines; DrawStage records
 * one stage's batches into the open color pass; DrawVelocity stamps
 * OVERLAY entries carrying prev corners into the 2-RT velocity
 * prepass. All are cheap no-ops with zero entries. */
int  AeronSceneBb3d_Prepare(struct AeronScene3D* s, AeronCommandBuffer* cmd);
void AeronSceneBb3d_DrawStage(struct AeronScene3D* s, AeronRenderPass* pass, AeronSceneBillboardStage stage);
void AeronSceneBb3d_DrawVelocity(struct AeronScene3D* s, AeronRenderPass* pass);
/* Draw the LENS-stage entries (per-entry draws — the anchor rides a
 * per-draw uniform; entry counts are tiny). `pass` must have NO depth
 * attachment: the VS samples the scene depth texture. */
void AeronSceneBb3d_DrawLens(struct AeronScene3D* s, AeronRenderPass* pass);
void AeronSceneBb3d_Release(struct AeronScene3D* s);

/* mesh_overlay.c */
int  AeronSceneMeshOverlay_Prepare(struct AeronScene3D* scene, AeronCommandBuffer* command_buffer);
void AeronSceneMeshOverlay_Draw(struct AeronScene3D* scene, AeronRenderPass* render_pass);
void AeronSceneMeshOverlay_Release(struct AeronScene3D* scene);

/* post.c */
int  AeronScenePost_EnsureSsao(struct AeronScene3D* s);
int  AeronScenePost_EnsureMb(struct AeronScene3D* s);
void AeronScenePost_Release(struct AeronScene3D* s);
int  AeronScenePost_RunSsao(struct AeronScene3D* s, AeronCommandBuffer* cmd);
int  AeronScenePost_DebugVisualizeSsao(struct AeronScene3D* s, AeronCommandBuffer* cmd);
int  AeronScenePost_MbPrepareTemporalMotion(struct AeronScene3D* s, AeronCommandBuffer* cmd);
int  AeronScenePost_MbVisualizeTemporal(struct AeronScene3D* s, AeronCommandBuffer* cmd, AeronTexture* motion,
										int direct_fsr_motion);
int  AeronScenePost_MbResolve(struct AeronScene3D* s, AeronCommandBuffer* cmd, AeronRenderTarget* color,
							  AeronTexture* velocity, int direct_fsr_motion);
int  AeronSceneTemporal_Ensure(struct AeronScene3D* s);
void AeronSceneTemporal_Release(struct AeronScene3D* s);
int  AeronSceneTemporal_Dispatch(struct AeronScene3D* s, AeronCommandBuffer* cmd,
								 int update_retained_motion_vectors);
int  AeronSceneTemporal_EnsureMutableOutput(struct AeronScene3D* s, AeronCommandBuffer* cmd);
/* Shared fullscreen helper (own pass on `dst`, binds pairs, pushes FS
 * cbuffer 0, draws the 4-vertex strip). */
int AeronScenePost_Fullscreen(AeronCommandBuffer* cmd, AeronGraphicsPipeline* pipe, AeronRenderTarget* dst,
							  AeronTexture* const* textures, AeronSampler* const* samplers,
							  uint32_t num_binds, const void* uniform, uint32_t uniform_size,
							  const char* debug_label);
/* util */
void AeronSceneInternal_QuatToMat3(const float q[4], float m[9]);

#endif /* AERON_SCENE_INTERNAL_H */
