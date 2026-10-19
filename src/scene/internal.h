#ifndef AERON_SCENE_INTERNAL_H
#define AERON_SCENE_INTERNAL_H

/* Shared internals of the aeron_scene translation units. NOT a public
 * API — games use the aeron_scene public headers only. */

#include "aeron/log.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/mesh_overlay.h"
#include "aeron/scene/probe_set.h"
#include "aeron/scene/scene3d.h"

#define AERON_SCENE_BLOOM_REFERENCE_HEIGHT 2160

#define AERON_SCENE_MAX_INSTANCES 2048
#define AERON_SCENE_MAX_LIGHTS AERON_SCENE_POINT_LIGHT_CAPACITY
#define AERON_SCENE_CLUSTER_DEFAULT_TILE_SIZE 32u
#define AERON_SCENE_CLUSTER_MAX_TILE_SIZE 64u
#define AERON_SCENE_CLUSTER_TILE_ALIGNMENT 8u
#define AERON_SCENE_CLUSTER_REFERENCE_HEIGHT 1080u
#define AERON_SCENE_CLUSTER_DEFAULT_DEPTH_SLICES 24u
#define AERON_SCENE_CLUSTER_THREADS 64u
#define AERON_SCENE_MAX_BILLBOARDS 2304
#define AERON_SCENE_MAX_FRAME_UNIFORMS 4
#define AERON_SCENE_FRAME_UNIFORM_CAP AERON_MAX_UNIFORM_DATA_SIZE
#define AERON_SCENE_MAX_MESH_OVERLAYS 64
#define AERON_SCENE_MAX_OVERLAY_VERTS (256u * 1024u)
#define AERON_SCENE_MAX_SHADOW_ONLY AERON_SCENE_MAX_INSTANCES
#define AERON_SCENE_MAX_SHADOW_CASTERS (AERON_SCENE_MAX_INSTANCES + AERON_SCENE_MAX_SHADOW_ONLY)
#define AERON_SCENE_MAX_MESH_TABLES                                                                          \
	(1 + AERON_SCENE_MAX_INSTANCES * 2 + AERON_SCENE_MAX_SHADOW_ONLY + AERON_SCENE_MAX_MESH_OVERLAYS)
/* Open addressing over at most AERON_SCENE_MAX_MESH_TABLES keys: the power
 * of two above it. */
#define AERON_SCENE_MESH_TABLE_HASH_CAP 8192
#define AERON_SCENE_SHADOW_GUARD_TEXELS 4

/* Pipeline kinds for AeronScenePbr_Pipeline / _DrawInstances. Color
 * pipelines are single-target; the prepass variants own normal, velocity
 * and temporal-depth outputs. */
enum {
	AERON_PBR_PIPE_MESH = 0,              /* monolithic color, depth write */
	AERON_PBR_PIPE_FORWARD,               /* color, depth-EQUAL/no-write */
	AERON_PBR_PIPE_PREPASS,               /* normal only */
	AERON_PBR_PIPE_PREPASS_VEL,           /* normal + velocity */
	AERON_PBR_PIPE_MESH_MASK,             /* alpha mask, monolithic color */
	AERON_PBR_PIPE_FORWARD_MASK,          /* alpha mask, deferred color */
	AERON_PBR_PIPE_PREPASS_MASK,          /* alpha mask normal only */
	AERON_PBR_PIPE_PREPASS_MASK_VEL,      /* alpha mask normal + velocity */
	AERON_PBR_PIPE_MESH_BLEND,            /* blend range of MESH */
	AERON_PBR_PIPE_FORWARD_BLEND,         /* blend range of FORWARD (test GE) */
	AERON_PBR_PIPE_PREPASS_STAMP,         /* normal masked + velocity written */
	AERON_PBR_PIPE_PREPASS_TEMPORAL,      /* normal + velocity + R32 depth export */
	AERON_PBR_PIPE_PREPASS_MASK_TEMPORAL, /* alpha mask normal + velocity + depth */
	AERON_PBR_PIPE_BACKFACE_FILL,         /* back faces, dark color, strict biased test */
	/* A mesh's screen primitives in each colour kind: the shader with the
	 * screen code, which the others leave out. */
	AERON_PBR_PIPE_MESH_SCREEN,
	AERON_PBR_PIPE_FORWARD_SCREEN,
	AERON_PBR_PIPE_MESH_BLEND_SCREEN,
	AERON_PBR_PIPE_FORWARD_BLEND_SCREEN,
	AERON_PBR_PIPE_MESH_MASK_SCREEN,
	AERON_PBR_PIPE_FORWARD_MASK_SCREEN,
	AERON_PBR_PIPE_PREPASS_STAMP_SCREEN, /* velocity stamp of a blend range's screens */
	/* The floor's mirrored view (planar_reflection.c): MESH and MESH_MASK
	 * into its half-resolution target, single-sampled. */
	AERON_PBR_PIPE_PLANAR,
	AERON_PBR_PIPE_PLANAR_MASK,
	AERON_PBR_PIPE_KIND_COUNT
};

#define AERON_SCENE_MAX_SKY_SUNS 4

/* One prepared sky-sun disc: quad placement (gnomonic tangent plane) and
 * fragment radiances, all in tangent-of-angle units. */
typedef struct AeronSceneSkySun {
	float direction[3];
	float tangent0[3];
	float tangent1[3];
	float half_extent;
	float disc[4]; /* centre radiance rgb, limb-darkening coefficient */
	float glow[3]; /* peak glow radiance */
	float radius;
	float glow_radius;
} AeronSceneSkySun;

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
	int                        array; /* texture is a 2D array */
	uint32_t                   layer, max_lod;
	int                        clamp; /* array: clamp to uv_rect instead of discarding */
	const AeronSceneMeshTable* mesh_table;
	uint32_t                   first_vertex;
	uint32_t                   vertex_count;
	uint32_t                   mesh_table_base;
	uint32_t                   table_component_count;
	float                      transform[16];
	float                      uv_xform[4];
	float                      uv_rect[4];
	float                      color[4];
	float                      depth_bias;
	uint8_t                    blend;
	uint8_t                    cull_mode;
} AeronSceneMeshOverlayEntry;

typedef struct AeronScenePreparedInstance {
	/* float4 bases of the current and previous mesh tables in the frame's
	 * storage; both hold at least table_component_count components. */
	uint32_t current_table_base;
	uint32_t previous_table_base;
	uint32_t table_component_count;
	/* The bound intersects the camera frustum; camera passes skip the
	 * instance otherwise (shadow passes cull per cascade). */
	uint32_t camera_visible;
	/* The probe set the instance's colour draws use; -1 for none. */
	int32_t probe_set;
} AeronScenePreparedInstance;

/* A probe set placed for the frame (AeronScene_AddProbeSet). */
typedef struct AeronSceneProbeSet {
	const AeronProbeSet* probes;
	float                world_to_model[12]; /* rows: scene units to model metres */
	float                metres_per_unit;    /* the largest scale of world_to_model */
	/* The lighting block's probe fields (AeronSceneLightingUniformGPU). */
	float params[4];
	float strengths[4];
	float grid_scale[4];
	float grid_offset[4];
} AeronSceneProbeSet;

/* Occlusion of the first probe set's light by the instances inside its grid
 * box (probe_occlusion.c). */
#define AERON_SCENE_OCCLUSION_MAX_OCCLUDERS 64
#define AERON_SCENE_OCCUPANCY_MIPS          5     /* 0.25 m to 4 m */
#define AERON_SCENE_OCCUPANCY_CELL          0.25f /* metres */
#define AERON_SCENE_OCCLUSION_GRID_CELL     1.0f  /* metres */

typedef struct AeronSceneOccluder {
	const struct AeronSceneMesh* mesh;
	uint16_t                     caster; /* as the shadow lists encode it */
	uint64_t                     hash;   /* placement, articulation and visibility */
	float                        lo[3];  /* bounds, model metres */
	float                        hi[3];
} AeronSceneOccluder;

/* The floor's reflection (planar_reflection.c): what stands above the first
 * probe set's reflection plane, drawn mirrored across it at half
 * resolution, then its mips, which the floor reads. */
typedef struct AeronScenePlanarReflection {
	AeronRenderTarget*    color;
	AeronDepthTarget*     depth;
	AeronTexture*         image; /* 2 layers: colour x coverage, coverage; distance x coverage */
	AeronTexture*         placeholder;
	AeronComputePipeline* resolve; /* the image and its mips, in one pass */
	AeronSampler*         sampler;
	int                   width, height;
	int                   failed;
	/* This frame: the image holds the instances in `order`, drawn through
	 * `view_proj` from `eye` (world); `drawing` while the mirrored pass
	 * records, so the PBR draws take these. */
	int      active;
	int      drawing;
	float    view_proj[16];
	float    eye[3];
	uint16_t order[AERON_SCENE_MAX_INSTANCES];
	int      order_count;
	/* Where the floor reads it: x0, y0, x1, y1 in half-resolution texels. */
	float screen[4];
} AeronScenePlanarReflection;

typedef struct AeronSceneProbeOcclusion {
	/* The probe set the grids describe, and where it was placed. */
	const AeronProbeSet* probes;
	float                world_to_model[12];
	float                origin[3]; /* the grid box's low corner, model metres */
	int                                   occupancy_dims[3];
	int                                   grid_dims[3];
	/* 1: the GPU builds the occupancy from depth views of the occluders;
	 * 0: the CPU voxelizes them into `cells`, mip 0 of the occupancy, x
	 * fastest (255 inside an occluder), where compute cannot write it. */
	int                   gpu;
	uint8_t*              cells;
	AeronDepthTarget*     views;   /* the occluders' depth views */
	AeronTexture*         staging; /* the occupancy's mips 1 to 4 before their copy */
	AeronBuffer*          records; /* the views' boxes and tiles */
	AeronComputePipeline* compose;
	AeronComputePipeline* downsample;
	AeronTexture*         occupancy; /* R8, AERON_SCENE_OCCUPANCY_MIPS mips */
	/* RGBA16F, 2 * grid_dims[0] wide: per cell, the share of the probe
	 * irradiance past the occluders on the faces toward +x, -x, +y, -y,
	 * then in the right half toward +z, -z. */
	AeronTexture*         grid;
	AeronTexture*         unshaded;    /* the same before the floor's shading (its first pass) */
	AeronTexture*         placeholder; /* 1x1x1, bound while off */
	AeronSampler*         sampler;
	AeronComputePipeline* pipeline;
	AeronSceneOccluder    occluders[AERON_SCENE_OCCLUSION_MAX_OCCLUDERS];
	int                   occluder_count;
	int                   valid;  /* the grids hold `occluders` */
	int                   failed; /* resources could not be created: off */
	int                   active; /* this frame: the shaders read the grids */
	int                   frames_since_build;
	int                   dropped_warned;
} AeronSceneProbeOcclusion;

typedef struct AeronScenePointLightGPU {
	/* xyz: the position, w: the inverse square of the range. */
	float position_range[4];
	float color[4];
	/* xyz: a tube's half axis, w: its half length (zero for a sphere). */
	float half_axis[4];
	/* xyz: a one-sided light's unit direction (zero for a light emitting
	 * all around), w: the inverse of a tube's half length. Per-light values
	 * the shader would otherwise derive at every fragment. A one-sided
	 * light's color.w (its sphere radius) is negated, so lights emitting
	 * all around never load the direction. */
	float direction[4];
} AeronScenePointLightGPU;

/* A point light in view space, in the point light order. w bounds the
 * whole light: its range plus a tube's half length. */
typedef struct AeronSceneClusterLightGPU {
	float view_position_range[4];
} AeronSceneClusterLightGPU;

/* The cluster build's output, one uint buffer (scene_clustered_lights.hlsli):
 * per depth bin two words, the index ranges of the spheres and of the tubes
 * that reach it, then per screen tile a bitmask of the lights that reach
 * it, mask_words words long. */
#define AERON_SCENE_CLUSTER_BIN_WORDS 2u

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
	/* Point lights from tube_start on are tubes. */
	uint32_t tube_start;
	uint32_t mask_words;
	uint32_t tile_size;
	uint32_t flags;
} AeronSceneClusterUniformGPU;

/* Fragment b2: the scene lighting block. Mirrors ClusteredLightUniform
 * with AERON_SCENE_LIGHTING_ENVIRONMENT; the cluster build compute pass
 * binds only the cluster part. */
typedef struct AeronSceneLightingUniformGPU {
	AeronSceneClusterUniformGPU cluster;
	float                       environment_params[4];   /* diffuse, specular strength, specular mips, 0 */
	float                       environment_basis[3][4]; /* world-to-local rows */
	float                       debug_material[4];       /* enabled, metallic, roughness, 0 */
	float                       emission[4];             /* emissive scale, 0, 0, 0 */
	float                       render_size[4];          /* width, height, 1 / width, 1 / height */
	/* The drawn object's probe set, read by the probe variants only. */
	float probe_params[4];      /* probe count, cube mip count, grid points along x, y */
	float probe_strengths[4];   /* diffuse, specular, 0, 0 */
	float world_to_model[3][4]; /* rows: scene units to model metres */
	float grid_scale[4];        /* model metres to grid texels; w: grid points along z */
	float grid_offset[4];
	/* The probe occlusion grid (probe_occlusion.c): its low corner in model
	 * metres and 1 while it applies; its cells along x, y, z and their size
	 * in metres. */
	float occlusion_origin[4];
	float occlusion_dims[4];
	/* The reflecting floor (planar_reflection.c), in model metres: its
	 * centre and half size along x and y; its height, how far from it a
	 * surface takes its reflection, 1 while the drawn instance takes it, and
	 * the reflection's mip count. While the mirrored view draws, `eye` is
	 * its camera position (w = 1). `screen`: where the floor reads the
	 * reflection, x0, y0, x1, y1 in half-resolution texels. */
	float planar_rect[4];
	float planar_params[4];
	float planar_eye[4];
	float planar_screen[4];
} AeronSceneLightingUniformGPU;

/* Layout checks against the HLSL blocks (ClusteredLightUniform, 96 bytes;
 * with the environment, the debug material, the emission, the render size,
 * the probe set, its occlusion grid and the reflecting floor, 416 bytes). */
typedef char AeronSceneClusterUniformSizeCheck[sizeof(AeronSceneClusterUniformGPU) == 96 ? 1 : -1];
typedef char AeronSceneLightingUniformSizeCheck[sizeof(AeronSceneLightingUniformGPU) == 416 ? 1 : -1];

enum {
	AERON_SCENE_CLUSTER_DEBUG_VIEW = 1u << 0,
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
	/* enabled, cascade count, reserved, cascade debug. */
	float params[4];
	/* xyz = position; w reserved. */
	float camera_pos[4];
	/* xyz = camera forward; w reserved. */
	float camera_forward[4];
	/* normal bias texels, depth bias texels, reserved, max distance. */
	float bias[4];
	/* distance fade start/end, inverse atlas width and height (tiles are
	 * square; the receiver-local atlas is one row of them). */
	float fade[4];
	/* reserved, tan(light angular radius), max/min radius in atlas texels. */
	float pcss[4];
	/* enabled, FSR temporal phase, reserved, reserved. */
	float pcss_temporal[4];
	/* xyz = normalized surface-to-light direction. */
	float light_dir[4];
} AeronSceneDirectionalShadowUniform;

typedef char
	AeronSceneDirectionalShadowUniformSizeCheck[sizeof(AeronSceneDirectionalShadowUniform) == 640 ? 1 : -1];

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

	AeronScenePassHookFn    hook_fn[AERON_SCENE_HOOK_COUNT];
	void*                   hook_user[AERON_SCENE_HOOK_COUNT];
	AeronSceneAfterMeshesFn after_meshes_fn;
	void*                   after_meshes_user;

	AeronSceneMeshInstance     instances[AERON_SCENE_MAX_INSTANCES];
	AeronScenePreparedInstance prepared_instances[AERON_SCENE_MAX_INSTANCES];
	int                        instance_count;
	/* Camera-visible instances, nearest first (AeronSceneStorage_Prepare):
	 * opaque passes draw in this order so depth testing rejects what the
	 * nearer instances hide, and blending walks it backwards. */
	uint16_t camera_order[AERON_SCENE_MAX_INSTANCES];
	int      camera_order_count;
	int                        instances_dropped;
	AeronSceneLight            lights[AERON_SCENE_MAX_LIGHTS];
	int                        light_count;
	int                        lights_dropped;
	AeronSceneFrameUniform     frame_uniforms[AERON_SCENE_MAX_FRAME_UNIFORMS];
	int                        frame_uniform_count;
	/* Image-based lighting environment for this frame; textures borrowed. */
	AeronScenePbrEnvironment pbr_environment;
	int                      pbr_environment_set;
	/* The frame's probe sets, and the lighting block the colour passes
	 * bind, which probe draws push again with their set's fields. */
	AeronSceneProbeSet           probe_sets[AERON_SCENE_MAX_PROBE_SETS];
	int                          probe_set_count;
	/* AeronScene_SetProbeKeyShadow: the visibility pass's B. */
	int                          probe_key_shadow;
	/* AeronScene_SetProbeOcclusion, and its grids (probe_occlusion.c). */
	int                          probe_occlusion_enabled;
	AeronSceneProbeOcclusion     probe_occlusion;
	/* AeronScene_SetPlanarReflection, and the floor's mirrored view
	 * (planar_reflection.c). */
	int                          planar_reflection_enabled;
	AeronScenePlanarReflection   planar_reflection;
	/* pbr.c: the colour pass's t12 and t13 hold the occlusion grid and the
	 * floor's reflection, not the `_anim` shaders' effect textures. */
	int                          pbr_occlusion_in_effect_slot;
	AeronSceneLightingUniformGPU lighting_uniform;

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
	uint32_t                           receiver_local_shadow_size;  /* tile size */
	uint32_t                           receiver_local_shadow_tiles; /* in one row */
	/* The receiver-local map serves the USE_RECEIVER_LOCAL instances this
	 * frame (they leave the cascades' receivers); when it has no casters it
	 * is not rendered and they sample no shadow. */
	uint8_t                            receiver_local_owned;
	uint8_t                            receiver_local_empty;
	/* No cascade has a caster: the atlas is not rendered and sampling it
	 * binds the disabled uniform (an empty map is fully lit). */
	uint8_t                            shadow_atlas_empty;
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
	uint32_t                   mesh_table_values[AERON_SCENE_MESH_TABLE_HASH_CAP]; /* float4 base */
	uint32_t mesh_table_component_counts[AERON_SCENE_MESH_TABLE_HASH_CAP]; /* packed component counts */
	float (*mesh_table_staging)[4]; /* variable-length tables, float4 units */
	uint32_t                   mesh_table_count;
	uint32_t                   mesh_table_vec4_count;
	uint32_t                   mesh_table_staging_cap;
	AeronScenePointLightGPU*   point_light_staging;
	uint32_t                   point_light_count;
	/* Directional shadows ran without a normal render target (logged once). */
	int                        screen_shadow_warned;
	/* Spheres are packed first; point lights from here on are tubes. */
	uint32_t                   point_tube_start;
	uint32_t                   point_light_staging_cap;
	AeronSceneClusterLightGPU* cluster_light_staging;
	uint32_t                   cluster_light_staging_cap;
	AeronBuffer*               mesh_table_buffer;
	uint32_t                   mesh_table_buffer_cap;
	AeronBuffer*               point_light_buffer;
	uint32_t                   point_light_buffer_cap;
	AeronBuffer*               cluster_light_buffer;
	uint32_t                   cluster_light_buffer_cap;
	int                        storage_ready;
	int                        storage_error_logged;

	/* Clustered-forward point-light allocation. */
	AeronSceneClusteredLightDesc cluster_desc;
	AeronSceneClusterUniformGPU  cluster_uniform;
	AeronBuffer*                 cluster_data_buffer;
	uint32_t                     cluster_data_buffer_cap;
	AeronComputePipeline*        cluster_build_pipeline;
	float                        cluster_far_z;
	float                        cluster_near_z;
	uint32_t                     cluster_far_shrink_frames;
	uint32_t                     cluster_count;
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
	AeronShader*                 overlay_array_fs;
	AeronGraphicsPipeline*       overlay_pipes[3][3];
	AeronGraphicsPipeline*       overlay_array_pipes[3][3]; /* [blend][cull], 2D array textures */

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
	AeronShader*           ssao_receiver_local_ps; /* receiver-local shadow, G only */
	AeronShader*           ssao_blur_ps;
	AeronShader*           ssao_blur_lq_ps;
	AeronGraphicsPipeline* ssao_pipeline;
	AeronGraphicsPipeline* ssao_receiver_local_pipeline;
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
	/* Reductions remain valid while the retained motion field is unchanged. */
	int                mb_tile_valid;
	int                mb_neighbor_valid;
	AeronRenderTarget* mb_rt;
	AeronRenderTarget* mb_tile_rt;
	AeronRenderTarget* mb_neighbor_rt;
	int                mb_tile_w, mb_tile_h;
	int                mb_tile_size;

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

	/* Sky cube (AeronScene_SetSkyEnvironment). Cube pointer + basis + exposure
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
	/* Overlay cube of the sky's environment, drawn after the SKY
	 * billboards; per frame like sky_cube. Its shader and pipeline are
	 * built the first time an overlay is drawn. */
	AeronTexture*          sky_overlay; /* borrowed for the frame; NULL = no overlay */
	float                  sky_overlay_world_to_cube[9];
	int                    sky_overlay_tried;
	AeronShader*           sky_overlay_fs;
	AeronGraphicsPipeline* sky_overlay_pipe;

	/* Sky-sun discs (AeronScene_AddSkySun); reset by Begin. Sprite suns
	 * go through the SKY billboards instead. */
	AeronSceneSkySun       sky_suns[AERON_SCENE_MAX_SKY_SUNS];
	int                    sky_sun_count;
	int                    sky_sun_tried;
	AeronShader*           sky_sun_vs;
	AeronShader*           sky_sun_fs;
	AeronGraphicsPipeline* sky_sun_pipe;

	/* PBR material class (pbr.c), lazily initialized on the first frame that
	 * requires mesh or geometry-backed post-processing. */
	int pbr_tried;
	int pbr_debug_views;
	/* AeronScene_SetBackfaceFill; on by default. */
	int backface_fill;
	/* AeronScene_SetScreenSources; reset by Begin. The frame is uploaded
	 * with the per-frame storage and its buffer stands in for meshes
	 * without screen rectangles. */
	AeronTexture*         screen_graphics;
	AeronTexture*         screen_text;
	AeronSceneScreenFrame screen_frame;
	AeronBuffer*          screen_frame_buffer;
	uint32_t              screen_frame_buffer_cap;
	/* Debug material override (AeronScene_SetPbrDebugMaterial): enabled,
	 * metallic, roughness, 0. Reset by Begin. */
	float pbr_debug_material[4];
	/* Immutable world samplers for OFF/Native AA/Quality/Balanced/Performance.
	 * pbr_sampler and mesh_sampler select the active entries. The mesh
	 * (model atlas) samplers always exist: the game's mesh sampler, or the
	 * world sampler, with REPEAT addressing. */
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
	AeronShader*  pbr_blend_fs;
	AeronShader*  pbr_screen_fs;
	AeronShader*  pbr_mask_screen_fs;
	/* The colour shaders with image-based lighting, used while an
	 * environment is selected (pbr_environment_set). */
	AeronShader*  pbr_ibl_fs;
	AeronShader*  pbr_mask_ibl_fs;
	AeronShader*  pbr_blend_ibl_fs;
	AeronShader*  pbr_screen_ibl_fs;
	AeronShader*  pbr_mask_screen_ibl_fs;
#ifdef AERON_DEBUG_UI
	int          pbr_debug_tried;
	AeronShader* pbr_debug_fs;
	AeronShader* pbr_mask_debug_fs;
#endif
	AeronShader* pbr_prepass_fs;
	AeronShader* pbr_prepass_mask_fs;
	AeronShader* pbr_backface_vs;
	AeronShader* pbr_backface_fs;
	/* Pipeline matrix: kind x cull mode. Cull-NONE variants are created
	 * by Ensure (the long-standing behavior); other cull modes are
	 * created lazily on first use (per-instance cull_mode). */
	AeronShader*           pbr_prepass_stamp_fs;
	AeronShader*           pbr_prepass_stamp_screen_fs;
	AeronGraphicsPipeline* pbr_pipes[AERON_PBR_PIPE_KIND_COUNT][3];
	/* Colour kinds with image-based lighting, created on first use. */
	AeronGraphicsPipeline* pbr_ibl_pipes[AERON_PBR_PIPE_KIND_COUNT][3];
#ifdef AERON_DEBUG_UI
	AeronGraphicsPipeline* pbr_debug_pipes[AERON_PBR_PIPE_KIND_COUNT][3];
#endif
	/* Material animation (AeronScene_SetEffectAtlas): the atlas, the
	 * animation time, and the colour shaders with animation, loaded on
	 * first use, by [image-based lighting][plain, mask, blend, screen,
	 * mask screen], and
	 * their pipelines by [image-based lighting][kind][cull]. */
	AeronEffectAtlas*      effect_atlas;
	float                  animation_time;
	int                    pbr_anim_tried;
	AeronShader*           pbr_anim_vs;
	AeronShader*           pbr_anim_fs[2][5];
	AeronGraphicsPipeline* pbr_anim_pipes[2][AERON_PBR_PIPE_KIND_COUNT][3];
	/* Probe variants by [animation][plain, mask, blend, then the floor's
	 * mirrored view's plain and mask], loaded on first use, and their
	 * pipelines by [animation][kind][cull]. */
	int                    pbr_probe_tried;
	AeronShader*           pbr_probe_fs[2][5];
	AeronGraphicsPipeline* pbr_probe_pipes[2][AERON_PBR_PIPE_KIND_COUNT][3];
};

/* Shared small helpers (util.c). */
AeronShader* AeronSceneInternal_CompileShader(const char* name, AeronShaderStage stage, uint32_t samplers,
											  uint32_t ubs, uint32_t sbs);
AeronRenderTarget*         AeronSceneInternal_CreateColorRt(AeronTextureFormat fmt, int w, int h,
															const char* debug_name);
AeronBlendStateDesc        AeronSceneInternal_BlendOpaque(void);
AeronTexture*              AeronSceneInternal_WhiteTexture(void);
AeronTexture*              AeronSceneInternal_WhiteCubeTexture(void);
AeronTexture*              AeronSceneInternal_WhiteArrayTexture(void);
/* Process-shared split-sum BRDF table: 64x64 RG16F, u = N.V, v = perceptual
 * roughness, (A, B) with F = F0 * A + B. Generated once on the CPU. */
AeronTexture*              AeronSceneInternal_BrdfLutTexture(void);
const AeronSceneMeshTable* AeronScenePbr_IdentityTable(void);
int                        AeronSceneStorage_Prepare(struct AeronScene3D* s, AeronCommandBuffer* cmd);
void                       AeronSceneStorage_Release(struct AeronScene3D* s);
/* float4 base of a shadow caster's mesh table, and its packed component count. */
uint32_t AeronSceneStorage_ShadowTableBase(const struct AeronScene3D* s, uint16_t encoded_caster,
										   uint32_t* component_count);

/* clustered_lights.c */
int  AeronSceneClusteredLights_Ensure(struct AeronScene3D* s);
int  AeronSceneClusteredLights_Build(struct AeronScene3D* s, AeronCommandBuffer* cmd);
void AeronSceneClusteredLights_Bind(struct AeronScene3D* s, AeronRenderPass* pass);
void AeronSceneClusteredLights_Release(struct AeronScene3D* s);

/* Probe occlusion (probe_occlusion.c). Update rebuilds the grids where an
 * occluder changed, before the visibility pass and the colour passes read
 * them; it switches the feature off rather than fail the frame. */
void          AeronSceneProbeOcclusion_Update(struct AeronScene3D* s, AeronCommandBuffer* cmd);
void          AeronSceneProbeOcclusion_Release(struct AeronScene3D* s);
/* The occupancy (the visibility pass's cone) and the grid (the probe
 * shaders); a 1x1x1 placeholder while inactive. */
AeronTexture* AeronSceneProbeOcclusion_Occupancy(struct AeronScene3D* s);
AeronTexture* AeronSceneProbeOcclusion_Grid(struct AeronScene3D* s);
AeronSampler* AeronSceneProbeOcclusion_Sampler(struct AeronScene3D* s);
/* The lighting block's occlusion fields for probe set `set` (only the first
 * has a grid). */
void          AeronSceneProbeOcclusion_LightingFields(const struct AeronScene3D* s, int set, float origin[4],
													  float dims[4]);
/* World to occupancy cells (rows), the occupancy's cells along x, y, z
 * (w = 1 while active) and, for the visibility pass's cone, up to
 * AERON_SCENE_OCCLUSION_CONE_BOXES boxes in cells holding the occluders
 * grown by a coarse cell (lo, hi; lo.w = 1 when used). */
#define AERON_SCENE_OCCLUSION_CONE_BOXES 4
void          AeronSceneProbeOcclusion_OccupancyMapping(const struct AeronScene3D* s, float rows[3][4],
														float dims[4],
														float boxes[AERON_SCENE_OCCLUSION_CONE_BOXES * 2][4]);

/* The floor's reflection (planar_reflection.c). Render draws the mirrored
 * view and its mips, before the visibility pass; it switches the feature
 * off rather than fail the frame. */
void          AeronScenePlanarReflection_Render(struct AeronScene3D* s, AeronCommandBuffer* cmd);
void          AeronScenePlanarReflection_Release(struct AeronScene3D* s);
/* The reflection the floor reads (a placeholder while inactive), and the
 * lighting block's floor fields for probe set `set` drawing an instance
 * that is (`receiver`) or is not its owner. */
AeronTexture* AeronScenePlanarReflection_Image(struct AeronScene3D* s);
AeronSampler* AeronScenePlanarReflection_Sampler(struct AeronScene3D* s);
void          AeronScenePlanarReflection_LightingFields(const struct AeronScene3D* s, int set, int receiver,
														float rect[4], float params[4], float eye[4],
														float screen[4]);

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
/* Draws the culled faces of every single-sided instance's opaque range in
 * a dark color where no front face covers them (AeronScene_SetBackfaceFill).
 * Call after the opaque draws, in the same pass. */
int AeronScenePbr_DrawBackfaceFill(struct AeronScene3D* s, AeronRenderPass* pass);

/* directional_shadow.c */
int  AeronSceneDirectionalShadow_Prepare(struct AeronScene3D* scene);
int  AeronSceneDirectionalShadow_Render(struct AeronScene3D* scene, AeronCommandBuffer* command_buffer);
/* Draws the casters' depth (encoded as the shadow lists: an instance index,
 * or AERON_SCENE_MAX_INSTANCES plus a shadow-only index) with `view_proj`
 * into the current depth pass, both faces: what is nearest whichever way it
 * faces (probe_occlusion.c's views). 0 when the caster shaders do not load. */
int  AeronSceneDirectionalShadow_DrawDepth(struct AeronScene3D* scene, AeronRenderPass* pass,
										   const float view_proj[16], const uint16_t* casters, uint16_t caster_count);
void AeronSceneDirectionalShadow_Bind(struct AeronScene3D* scene, AeronRenderPass* render_pass);
void AeronSceneDirectionalShadow_BindForInstance(struct AeronScene3D* scene, AeronRenderPass* render_pass,
												 uint8_t shadow_flags);
void AeronSceneDirectionalShadow_BindScreen(struct AeronScene3D* scene, AeronRenderPass* render_pass);
/* How the screen pass shades receiver-local pixels: from the cascades
 * (the map is unused), from their own map (bound by
 * BindScreenReceiverLocal for a second pass), or unshadowed (the map is
 * owned but empty). */
enum {
	AERON_SHADOW_RECEIVER_LOCAL_CASCADES = 0,
	AERON_SHADOW_RECEIVER_LOCAL_MAP,
	AERON_SHADOW_RECEIVER_LOCAL_UNSHADOWED,
};
int  AeronSceneDirectionalShadow_ReceiverLocalMode(const struct AeronScene3D* scene);
void AeronSceneDirectionalShadow_BindScreenReceiverLocal(struct AeronScene3D* scene, AeronRenderPass* render_pass);
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
int  AeronSceneBb3d_Ensure(struct AeronScene3D* s);
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
/* The half-resolution visibility passes: AO when `ao_enabled` (else R is
 * 1) and the opaque receivers' directional shadow, then their blur. */
int  AeronScenePost_RunSsao(struct AeronScene3D* s, AeronCommandBuffer* cmd, int ao_enabled);
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

/* An instance's box along three orthonormal axes (world axes for the
 * camera, the light's for shadows): the union of its visible components' bounds,
 * each placed by its mesh-table row as the vertex shaders place its vertices
 * (a component past the table's count uses its last row). `center` is in world
 * space, `half` holds the half extents along the axes and `radius` the
 * sphere about `center` that holds the box. 0 without a mesh or a visible
 * component: the instance then draws nothing. */
typedef struct AeronSceneInstanceBox {
	float center[3];
	float half[3];
	float radius;
} AeronSceneInstanceBox;

int AeronSceneInternal_InstanceBox(const AeronSceneMeshInstance* instance, const float x_axis[3],
								   const float y_axis[3], const float z_axis[3], AeronSceneInstanceBox* out);
/* Binds the frame uniforms the host set (AeronScene_SetFrameUniformData). */
void AeronSceneInternal_BindFrameUniforms(struct AeronScene3D* s, AeronRenderPass* pass);
/* The probe set an instance's colour draws use; -1 for none. */
int32_t AeronSceneInternal_InstanceProbeSet(const struct AeronScene3D*    s,
											const AeronSceneMeshInstance* instance);
/* The culling planes (left, right, bottom, top, near; inward, normalized)
 * of a view-projection over a viewport, widened by the culling margin. */
void AeronSceneInternal_FrustumPlanes(const float view_proj[16], int width, int height, float planes[5][4]);

#endif /* AERON_SCENE_INTERNAL_H */
