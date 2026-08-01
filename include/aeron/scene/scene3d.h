#ifndef AERON_SCENE_SCENE3D_H
#define AERON_SCENE_SCENE3D_H

/*
 * AeronScene3D — per-frame immediate-mode 3D scene.
 *
 * Games translate their snapshots into generic primitives each frame:
 * a camera, mesh instances, lights, billboards, a sky, and a post
 * stack. The scene owns its render targets and pass chain; the game
 * submits the color RT as an Aeron texture layer afterwards.
 *
 * Game-specific shading enters through registered material classes
 * (game supplies shader names + param-block layout); anything that
 * resists the schema enters through pass hooks that drop to the Aeron
 * GPU HAL directly.
 */

#include <stdint.h>

#include "aeron/render.h"
#include "aeron/temporal.h"
#include "aeron/scene/mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronScene3D AeronScene3D;

/* Per-mesh-slot payload packed into the scene's per-frame storage
 * buffer: per-slot 3x4 affine + packed visibility / highlight / markings /
 * emissive lanes. Instances without one get the identity default.
 * Field order and sizes are the GPU layout — shaders address this
 * record with the float4 offsets in mesh_table_layout.hlsli. */
typedef struct AeronSceneMeshTable {
	float rows[AERON_MAX_MESH_SLOTS][3][4];
	float visibility_packed[AERON_MESH_PACKED_LANES][4];
	float highlight_packed[AERON_MESH_PACKED_LANES][4];
	float markings_packed[AERON_MESH_PACKED_LANES][4];
	float emissive_packed[AERON_MESH_PACKED_LANES][4];
} AeronSceneMeshTable;

/* One point light for this frame (world position, radius in world
 * units, linear-HDR color premultiplied by intensity). */
typedef struct AeronSceneLight {
	float pos[3];
	float radius;
	float color[3];
} AeronSceneLight;

/* CPU submission record for one per-instance culled light. The scene packs
 * active records into its per-frame local-light storage buffer. */
typedef struct AeronSceneLightGPU {
	float pos[3];
	float range;
	float color[3];
	float falloff_sq;
} AeronSceneLightGPU;

typedef struct AeronSceneLightBlock {
	uint32_t           light_count;
	uint32_t           _pad[3];
	AeronSceneLightGPU lights[16];
} AeronSceneLightBlock;

enum {
	AERON_SCENE_INSTANCE_NO_CAST_SHADOW                     = 1u << 0,
	AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW                  = 1u << 1,
	AERON_SCENE_INSTANCE_EXCLUDE_FROM_RECEIVER_LOCAL_SHADOW = 1u << 2,
	AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW          = 1u << 3,
};

/* One mesh instance for this frame. `transform`/`prev_transform` are
 * row-major model->world. Equal transforms and articulation yield zero
 * velocity when the motion-vector prepass consumes them. `variant` selects
 * the material variant column (TIE: decal_color). Mesh-table pointers are
 * borrowed for the frame (NULL = identity/all-visible). */
typedef struct AeronSceneMeshInstance {
	const AeronSceneMesh* mesh;
	float                 transform[16];
	float                 prev_transform[16];
	uint32_t              variant;
	/* When positive, render the sampled base color as unlit HDR emission
	 * at this strength instead of applying PBR/material emission. Intended
	 * for game-side semantic overrides such as original projectile meshes. */
	float                      base_color_emissive_strength;
	const AeronSceneMeshTable* mesh_table;
	/* Optional previous-frame articulation used only for velocity. NULL
	 * reuses mesh_table, which is correct for rigid or unchanged meshes. */
	const AeronSceneMeshTable* prev_mesh_table;
	/* Per-instance culled light block, borrowed for the frame (NULL = unlit).
	 * The scene copies active entries into its packed per-frame storage. */
	const AeronSceneLightBlock* lights;
	/* Skip receive-side local lighting (TIE: projectile bolts, which
	 * are themselves light sources). */
	int no_local_lights;
	/* Force zero velocity in the motion-vector prepass: project prev
	 * through the CURRENT camera with the current transform (TIE:
	 * spawn-frame objects with no matched previous pose — camera-
	 * induced velocity would be a bogus streak for them). */
	int zero_velocity;
	/* AeronCullMode for this instance's rasterization; default (0) is
	 * AERON_CULL_NONE — two-sided, the long-standing behavior. Which
	 * winding is "front"/"back" depends on the caller's projection
	 * handedness (a y-flipping projection reverses screen winding), so
	 * the game driver picks the mode that matches its content. */
	uint8_t cull_mode;
	/* Stamp this instance's alpha-BLEND index range into the velocity
	 * buffer during the motion-blur prepass (alpha-tested against the
	 * base-color atlas, depth GE test, no depth write) — the mesh
	 * analog of billboard velocity stamping. For blend-classified
	 * geometry that never enters the depth prepass (XWA: soft-alpha
	 * projectile ribbons) and would otherwise stay blur-sharp. */
	uint8_t velocity_stamp;
	/* Ordinary mesh instances cast and receive directional shadows by
	 * default. A receiver-local shadow map can preserve environment
	 * shadows while excluding selected casters from selected receivers. */
	uint8_t shadow_flags;
} AeronSceneMeshInstance;

typedef void (*AeronSceneAfterMeshesFn)(AeronCommandBuffer* cmd, AeronRenderPass* pass,
									   AeronRenderTarget* color_target, void* user);

#define AERON_SCENE_SHADOW_MAX_CASCADES 4

typedef enum AeronSceneShadowFitMode {
	/* Rotation-invariant sphere projected into a square. */
	AERON_SCENE_SHADOW_FIT_STABLE = 0,
	/* Tight light-space rectangle around the complete camera split. */
	AERON_SCENE_SHADOW_FIT_FRUSTUM = 1,
	/* Tight light-space rectangle around relevant receivers and casters. */
	AERON_SCENE_SHADOW_FIT_SCENE_DEPENDENT = 2,
} AeronSceneShadowFitMode;

typedef struct AeronSceneDirectionalShadowDesc {
	int   enabled;
	float light_dir[3]; /* normalized surface-to-light direction */
	/* Absolute origin added to scene-local positions for stable cascade
	 * snapping. Double precision preserves large rebased game worlds. */
	double   world_origin[3];
	uint32_t atlas_size;
	uint32_t cascade_count;
	uint32_t fit_mode; /* AeronSceneShadowFitMode */
	float    max_distance;
	float    split_lambda;
	int      explicit_splits;
	/* Normalized over [camera near, max_distance]; first cascade_count - 1 are active. */
	float split_positions[AERON_SCENE_SHADOW_MAX_CASCADES - 1];
	/* 0=hard. Fixed PCF uses 3x3/5x5/7x7 optimized tents; PCSS uses
	 * 8/16/24 comparison taps after its blocker search. */
	uint32_t filter_quality;
	float    filter_radius;
	int      contact_hardening;
	float    light_angular_radius_degrees; /* source half-angle */
	float    max_filter_radius;            /* atlas texels */
	float    pcss_min_filter_radius;       /* atlas texels */
	float    normal_bias;
	int      normal_bias_face_normal;
	float    depth_bias_texels;
	float    slope_bias;
	float    receiver_plane_bias;
	float    caster_constant_bias;
	float    caster_slope_bias;
	float    transition_fraction;
	float    distance_fade_fraction;
	int      debug_cascades;
	/* Fullscreen atlas diagnostic, drawn after temporal reconstruction.
	 * cascade -1 displays the complete atlas; 0..3 isolates one tile. */
	int debug_atlas;
	int debug_atlas_cascade;
} AeronSceneDirectionalShadowDesc;

typedef struct AeronSceneDirectionalShadowStats {
	int      active;
	uint32_t atlas_size;
	uint32_t cascade_count;
	uint32_t fit_mode;
	float    split_near[AERON_SCENE_SHADOW_MAX_CASCADES];
	float    split_far[AERON_SCENE_SHADOW_MAX_CASCADES];
	/* Maximum of the two axes, retained for compact callers. */
	float world_units_per_texel[AERON_SCENE_SHADOW_MAX_CASCADES];
	float world_units_per_texel_x[AERON_SCENE_SHADOW_MAX_CASCADES];
	float world_units_per_texel_y[AERON_SCENE_SHADOW_MAX_CASCADES];
	/* Linear texel-density improvement over the stable sphere fit. */
	float    texel_density_gain[AERON_SCENE_SHADOW_MAX_CASCADES];
	uint32_t receiver_count[AERON_SCENE_SHADOW_MAX_CASCADES];
	uint32_t candidate_count;
	uint32_t caster_count[AERON_SCENE_SHADOW_MAX_CASCADES];
	uint32_t triangle_count[AERON_SCENE_SHADOW_MAX_CASCADES];
	uint32_t dropped_shadow_only;
	int      receiver_local_active;
	uint32_t receiver_local_size;
	uint32_t receiver_local_caster_count;
	uint32_t receiver_local_triangle_count;
} AeronSceneDirectionalShadowStats;

/* Scene RT configuration. Pass 0/0 dims for the legacy 4K default.
 * `color_format` is the HDR scene format (games have shipped
 * R11G11B10_UFLOAT); the depth target is D32_FLOAT and created
 * sampled (SSAO / depth-aware hooks read it). `with_normal_rt`
 * allocates the R16G16_SNORM octahedral world-normal G-buffer the
 * SSAO path consumes. MSAA also allocates it for the single-sample
 * prepass used by depth-aware post effects. */
typedef struct AeronScene3DDesc {
	int                rt_width;
	int                rt_height;
	AeronTextureFormat color_format;
	int                with_normal_rt;
	AeronSampleCount   sample_count;
	AeronTemporalMode  temporal_mode;
	float              temporal_sharpness;
	float              view_space_to_meters;
} AeronScene3DDesc;

/* Per-frame camera. World-space position + world→eye rotation
 * quaternion (w, x, y, z — scene_quat_to_mat3's order), half-FOV
 * angles in radians, reversed-Z
 * near plane, and optional NDC X/Y offsets baked into the projection
 * for off-center classic projections (TIE's reticle alignment uses Y;
 * XWA's viewport center uses both; 0 for none). The viewport rect
 * selects the sub-rect of the scene RT the 3D view renders into —
 * full RT when w/h are 0. */
typedef struct AeronSceneCamera {
	float      pos[3];
	float      ori[4];
	float      h_half_rad;
	float      v_half_rad;
	float      near_z;
	float      proj_x_offset;
	float      proj_y_offset;
	AeronRectI viewport;
} AeronSceneCamera;

/* Pass-hook slots. Hooks receive the live command buffer and — for
 * the in-pass slots — the open scene render pass, so game code can
 * record extra draws (TIE: cockpit overlay, hyperspace streaks) or
 * whole extra passes (BEFORE_POST: motion blur, SSAO while those
 * remain game-side) without aeron_scene knowing their shape. */
typedef enum AeronScenePassSlot {
	AERON_SCENE_HOOK_AFTER_OPAQUE      = 0, /* in scene pass, after opaque mesh ranges */
	AERON_SCENE_HOOK_AFTER_TRANSPARENT = 1, /* in the FINAL color pass, last */
	AERON_SCENE_HOOK_BEFORE_POST       = 2, /* between scene pass end and post */
	AERON_SCENE_HOOK_BEFORE_OPAQUE     = 3, /* in scene pass, before meshes (sky, game-class geometry) */
	AERON_SCENE_HOOK_PREPASS           = 4, /* in the depth/normal(+velocity) prepass, after meshes */
	/* Output-resolution HDR hook after temporal reconstruction and world
	 * post effects. It has no render-resolution depth attachment. */
	AERON_SCENE_HOOK_AFTER_UPSCALE = 5,
	AERON_SCENE_HOOK_COUNT         = 6
} AeronScenePassSlot;

typedef void (*AeronScenePassHookFn)(AeronCommandBuffer* cmd, AeronRenderPass* pass_or_null, int rt_w,
									 int rt_h, void* user);

AeronScene3D* AeronScene_Create(const AeronScene3DDesc* desc);
void          AeronScene_Destroy(AeronScene3D* scene);
/* Effective scene sample count after exact color/depth capability fallback. */
AeronSampleCount AeronScene_SampleCount(const AeronScene3D* scene);

/* Frame start: latch the camera and reset per-frame submissions. Returns zero
 * when a pending render-mode change cannot prepare its required resources. */
int AeronScene_Begin(AeronScene3D* scene, const AeronSceneCamera* camera);

/* Per-frame submissions (reset by AeronScene_Begin). Instances and
 * lights are copied; referenced meshes/tables are borrowed until
 * Render() completes. Over-capacity submissions are dropped with a
 * once-per-frame log. */
void AeronScene_AddMeshInstance(AeronScene3D* scene, const AeronSceneMeshInstance* instance);
void AeronScene_AddLight(AeronScene3D* scene, const AeronSceneLight* light);

/* Configure the key directional shadow for this frame. Visible ordinary
 * instances cast automatically. AddShadowCaster queues hidden ordinary
 * geometry (for example the player exterior in an internal view). */
void AeronScene_SetDirectionalShadow(AeronScene3D* scene, const AeronSceneDirectionalShadowDesc* shadow);
void AeronScene_AddShadowCaster(AeronScene3D* scene, const AeronSceneMeshInstance* instance);
void AeronScene_GetDirectionalShadowStats(const AeronScene3D* scene, AeronSceneDirectionalShadowStats* out);

/* Clear color of the scene pass (linear HDR). Deep-space default. */
void AeronScene_SetClearColor(AeronScene3D* scene, const float rgba[4]);

/* Per-frame sky cube map (reset by Begin; no call = no sky draw, which
 * expresses game-side gating directly). Drawn as the first draw of the
 * color pass — under the BEFORE_OPAQUE hook, SKY billboards, and all
 * meshes — at the reversed-Z far plane (depth GE test, no write). The
 * cube texture is borrowed until Render() completes (load one via
 * Aeron_ImageLoadCubemapKtx2). `world_to_cube` is the game's row-major
 * world-axes → cube-sampling-basis rotation (cube convention: +X right,
 * +Y up, -Z forward); NULL = identity. `exposure` scales the sample —
 * 1.0 for LDR cubes, calibrated >1 for dim HDR sources. */
void AeronScene_SetSkyCube(AeronScene3D* scene, AeronTexture* cube, const float world_to_cube[9],
						   float exposure);

void AeronScene_SetPassHook(AeronScene3D* scene, AeronScenePassSlot slot, AeronScenePassHookFn fn,
							void* user);

/* Optional in-pass callback after opaque, transparent, and articulated mesh
 * geometry, but before scene billboards. Begin clears it. */
void AeronScene_SetAfterMeshes(AeronScene3D* scene, AeronSceneAfterMeshesFn fn, void* user);

/* Sampler used for the pbr channel-atlas binds. NULL restores the scene's
 * default (linear, clamp). Games swap in their own to carry anisotropy settings
 * or a point-sampled retro style (TIE: XvT). Aeron copies the immutable state
 * into mode-specific variants with AMD's recommended temporal mip bias. The
 * source sampler remains owned by the caller and must stay alive while selected. */
/* Selects a caller-owned base sampler and creates mode-specific variants.
 * Returns zero without changing the current sampler set on failure. */
int AeronScene_SetMeshSampler(AeronScene3D* scene, AeronSampler* sampler);

/* Select the diagnostic PBR shader variant for this frame. Begin resets
 * the selection; non-debug builds retain the call as a no-op so game code
 * does not need a build-mode conditional. */
void AeronScene_SetPbrDebugViews(AeronScene3D* scene, int enabled);

/* Queue a per-frame uniform blob (copied; reset by Begin) that the
 * scene binds to (stage, slot) in the color pass AFTER the
 * BEFORE_OPAQUE hook and before the sky billboards / instance walk —
 * the declarative form of the games' pass-level shading environment.
 * The PBR b1 block is scene-owned shadow data; game lighting uses b3.
 * Binding after the hook means hook-side
 * pushes to the same slots (e.g. an unlit env for hook-drawn
 * geometry) cannot leak into the instance walk. */
void AeronScene_SetFrameUniformData(AeronScene3D* scene, AeronShaderStage stage, uint32_t slot,
									const void* data, uint32_t size);

/* Post-stack configuration (SSAO + motion blur). Persistent until the
 * next SetPost. Quality 0 disables a stage; resources allocate lazily
 * on first use. `mb_shutter` <= 0 suppresses the blur for the frame
 * (pause) while velocity generation continues per SetMotionContext. */
typedef struct AeronScenePostDesc {
	int   ssao_quality; /* 0=off, 1=low (8-tap), 2=high (rotated 16-tap + separable blur) */
	float ssao_intensity;
	float ssao_power;
	float ssao_radius_view;
	float ssao_bias_view;
	float ssao_direct;
	int   ssao_debug_viz;
	/* Screen-space radius clamp (fractions of NDC half-width; 0 = off). A
	 * fixed view-space radius projects to a distance-varying screen footprint;
	 * too small a footprint aliases the kernel into a grid on distant hulls,
	 * too large separates the taps on near surfaces. ssao_min_screen_frac
	 * raises the effective radius on distant surfaces (removes the grid);
	 * ssao_max_screen_frac caps it on near ones. */
	float ssao_min_screen_frac;
	float ssao_max_screen_frac;
	/* Per-pixel kernel-radius jitter [0,1]; breaks the discrete taps into
	 * blur-smoothable noise so a large footprint does not resolve into
	 * separate offset occlusions on near surfaces. 0 = off. */
	float ssao_sample_jitter;
	int   mb_quality; /* 0=off, 1=low (own velocity), 2=high (TileMax/NeighborMax dilation) */
	float mb_shutter;
	int   mb_camera_blur;
	int   mb_velocity_viz;
	/* Consume retained render-resolution FSR motion without materializing an
	 * output-resolution velocity texture. */
	int mb_fsr_direct_motion;
} AeronScenePostDesc;

void AeronScene_SetPost(AeronScene3D* scene, const AeronScenePostDesc* post);

/* Temporal reconstruction settings for the next frame. Call before
 * AeronScene_Begin. Mode changes are applied at that frame boundary and reset
 * history. Positive sharpness enables RCAS. */
typedef struct AeronSceneTemporalDesc {
	AeronTemporalMode mode;
	float             frame_time_delta_ms;
	float             sharpness;
	int               reset_history;
	int               debug_view;
} AeronSceneTemporalDesc;

void AeronScene_SetTemporal(AeronScene3D* scene, const AeronSceneTemporalDesc* temporal);

/* Returns the selected FSR shader profile while a temporal context is active.
 * Borrowed strings remain valid until the scene changes temporal mode or is
 * destroyed. */
int AeronScene_GetTemporalProfileInfo(const AeronScene3D* scene, AeronTemporalProfileInfo* info);

/* Per-frame motion context for velocity generation (call after Begin).
 * `prev_view_proj` NULL => prev = current (zero camera velocity).
 * `velocity_regen` 0 holds the previous velocity buffer (the sim did
 * not advance this host frame); 1 regenerates it. */
void AeronScene_SetMotionContext(AeronScene3D* scene, const float prev_view_proj[16], int velocity_regen);

/* After Render(): the output-resolution RT holding the finished scene. */
AeronRenderTarget* AeronScene_SceneRt(AeronScene3D* scene);

/* Record the frame's passes into `cmd` (no pass may be open on it). Returns
 * zero after recording a sticky command-buffer failure.
 * Opens the scene pass with shared color and reversed-Z depth clears,
 * runs the in-pass hooks, then closes it and runs BEFORE_POST. */
int AeronScene_Render(AeronScene3D* scene, AeronCommandBuffer* cmd);

/* Borrowed views for layer submission / game-side hook passes. */
AeronTexture*      AeronScene_ColorTexture(AeronScene3D* scene);
AeronRenderTarget* AeronScene_ColorRt(AeronScene3D* scene);
AeronDepthTarget*  AeronScene_DepthRt(AeronScene3D* scene);
AeronRenderTarget* AeronScene_NormalRt(AeronScene3D* scene); /* prepass output; NULL when not allocated */
/* RtDims is the stable output size. RenderDims is the mode-derived internal
 * size used by color/depth/normal/velocity passes. */
void AeronScene_RtDims(const AeronScene3D* scene, int* w, int* h);
void AeronScene_RenderDims(const AeronScene3D* scene, int* w, int* h);

/* Stable unjittered view-projection for compatibility/UI projection. */
const float* AeronScene_ViewProj(const AeronScene3D* scene); /* float[16] */
/* Jittered view-projection used by render-resolution scene rasterization. */
const float* AeronScene_JitteredViewProj(const AeronScene3D* scene); /* float[16] */

/* Row-major view-projection for an arbitrary camera — the exact math
 * Begin() runs on the latched camera (reversed-Z off-center
 * perspective x quaternion view). Games use it for PREVIOUS-frame
 * matrices (motion blur) and for projecting through cameras the scene
 * hasn't latched, instead of keeping drift-prone private copies of
 * the projection math. */
void AeronScene_ComputeViewProj(const AeronSceneCamera* camera, float out[16]);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_SCENE3D_H */
