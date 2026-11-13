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
#include "aeron/scene/mesh.h"
#include "aeron/temporal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronScene3D AeronScene3D;

enum {
	AERON_SCENE_POINT_LIGHT_CAPACITY = 256,
	AERON_SCENE_MAX_PROBE_SETS       = 8,
};

/* Per-row payload packed into the scene's per-frame storage buffer: per-row
 * 3x4 affine + packed visibility / highlight / markings / emissive lanes. A
 * model's rows are its components, then its controls. Instances without one
 * get the identity default.
 *
 * Capacity is AERON_MAX_MESH_COMPONENTS, but only rows 0..component_count-1
 * are read: writers fill those (normally the model's AeronSceneMesh.row_count)
 * and leave the rest untouched, and the scene uploads the table packed at
 * component_count (mesh_table_layout.hlsli). Zero means the full capacity. */
typedef struct AeronSceneMeshTable {
	uint32_t component_count;
	float    rows[AERON_MAX_MESH_COMPONENTS][3][4];
	float    visibility_packed[AERON_MESH_PACKED_LANES][4];
	float    highlight_packed[AERON_MESH_PACKED_LANES][4];
	float    markings_packed[AERON_MESH_PACKED_LANES][4];
	float    emissive_packed[AERON_MESH_PACKED_LANES][4];
} AeronSceneMeshTable;

/* One physical point light for this frame: a sphere of `source_radius`
 * (world units, > 0) at `pos`, giving color / max(d, source_radius)^2 with
 * Lambert diffuse, so `color` is the linear-HDR intensity (the illuminance
 * at one world unit); its specular highlight widens with the sphere's
 * apparent size. It fades to zero at `radius` (world units). A non-zero
 * `half_axis` (world units) makes it a tube: the segment pos +- half_axis,
 * `source_radius` thick, emitting the same total intensity (a point light
 * of `color` from afar), with `radius` measured from the segment. A
 * non-zero `direction` makes the light one-sided, a Lambertian emitter
 * such as a ceiling panel: it gives `color * cos(angle from direction)` in
 * front and nothing behind, so the light straight ahead is unchanged. A
 * tube's direction is made perpendicular to its axis (a flat strip); one
 * along the axis leaves the tube emitting all around.
 *
 * A non-zero `spot_outer_angle` makes a sphere with a direction a spot
 * light, as KHR_lights_punctual's: instead of the cosine, it gives the
 * full `color` within `spot_inner_angle` of `direction` and nothing past
 * `spot_outer_angle` (half angles in radians, the outer one at most pi/2),
 * with a smooth ramp between them. A tube ignores the angles, and a spot
 * light without a direction is drawn as a sphere, with a warning. */
typedef struct AeronSceneLight {
	float pos[3];
	float radius;
	float color[3];
	float source_radius;
	float half_axis[3];
	float direction[3];
	float spot_inner_angle;
	float spot_outer_angle;
} AeronSceneLight;

typedef struct AeronSceneClusteredLightDesc {
	uint32_t depth_slices;
	/* Cluster occupancy view, drawn by the debug shader variant
	 * (AERON_DEBUG_UI builds only). */
	int debug_view;
} AeronSceneClusteredLightDesc;

typedef struct AeronSceneClusteredLightStats {
	uint32_t submitted_light_count;
	uint32_t dropped_light_count;
	uint32_t grid_x;
	uint32_t grid_y;
	uint32_t grid_z;
	uint32_t effective_tile_size;
	uint32_t cluster_count;
	uint32_t clustered_active;
	uint64_t allocated_buffer_bytes;
} AeronSceneClusteredLightStats;

/* Directional-shadow flags (AeronSceneMeshInstance.shadow_flags).
 * USE_RECEIVER_LOCAL_SHADOW receivers sample a dedicated map fitted around
 * them, half the cascade atlas size (1024..4096); its texel grid turns and
 * moves with the first such receiver. CAST_RECEIVER_LOCAL_ONLY casts into
 * that map only, not the cascades: an instance shadowing itself without
 * repeating what another caster already casts on the rest of the scene.
 * SHADOW_DOUBLE_SIDED casts with both faces whatever the instance's cull
 * mode, so single-sided geometry (inward-facing walls) still blocks light
 * reaching it from behind. */
enum {
	AERON_SCENE_INSTANCE_NO_CAST_SHADOW                     = 1u << 0,
	AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW                  = 1u << 1,
	AERON_SCENE_INSTANCE_EXCLUDE_FROM_RECEIVER_LOCAL_SHADOW = 1u << 2,
	AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW          = 1u << 3,
	AERON_SCENE_INSTANCE_CAST_RECEIVER_LOCAL_ONLY           = 1u << 4,
	AERON_SCENE_INSTANCE_SHADOW_DOUBLE_SIDED                = 1u << 5,
};

/* An instance's material animation state (gltf_mesh.h, "Material
 * animation"). */
typedef struct AeronSceneMaterialAnim {
	uint32_t state_mask;   /* player states, bit per AERON_ANIM_STATE_* (0-30) */
	uint32_t seed;         /* stable per object: its damage choice and placement */
	uint32_t damage_stage; /* 0 none, 1-3: the 75, 50, 25 % hull stages */
	uint32_t energy;       /* 0 none, 1 shields, 2 tractor, 3 jamming */
	float    energy_start; /* animation seconds when `energy` began */
	float    onset[32];    /* animation seconds when each state last became set */
} AeronSceneMaterialAnim;

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
	const AeronSceneMeshTable* mesh_table;
	/* Optional previous-frame articulation used only for velocity. NULL
	 * reuses mesh_table, which is correct for rigid or unchanged meshes. */
	const AeronSceneMeshTable* prev_mesh_table;
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
	/* Draw only the material's emission and alpha, with no lighting.
	 * OpenXWA sets this for projectiles, because the original game draws
	 * them at full brightness whatever their materials. The instance is
	 * drawn whole, opaque faces included, in the transparent pass, with a
	 * shader that skips lighting; it keeps its depth prepass.
	 * AeronScene_SetEmissionMinWidth and AeronScene_SetEmissionFade apply
	 * to it. It has no effect on a mesh with alpha-mask faces, screens or
	 * material animation, on an instance with a probe set or a
	 * receiver-local shadow, and in debug views: those draw as usual. */
	uint8_t emission_only;
	/* Stamp the screen primitives of the alpha-BLEND range (holograms,
	 * transparent screens) into the velocity buffer where their content is
	 * visible, so motion blur treats them as part of this instance (a
	 * cockpit) instead of blurring them with what lies behind. */
	uint8_t velocity_stamp_screens;
	/* Draw the screen primitives of the alpha-BLEND range into the screen
	 * layer (AeronScene_ScreenLayer) instead of the scene. The game puts
	 * the layer over the scene after its own post effects, so they don't
	 * move or blur the screens. */
	uint8_t screens_to_layer;
	/* Ordinary mesh instances cast and receive directional shadows by
	 * default. A receiver-local shadow map can preserve environment
	 * shadows while excluding selected casters from selected receivers. */
	uint8_t shadow_flags;
	/* Material animation state, borrowed for the frame; NULL for none
	 * (AeronScene_SetEffectAtlas). */
	const AeronSceneMaterialAnim* anim;
	/* The probe set this instance owns (AeronScene_AddProbeSet), as its
	 * index + 1; 0 lets the scene choose the first set whose extent its
	 * bounds overlap, if any. */
	uint8_t probe_set_owner;
	/* The mesh's tag groups (AeronSceneMesh.tag_groups) this instance hides:
	 * bit g hides the materials and lights tagged with group g. The
	 * materials' surfaces are not drawn in any pass. */
	uint32_t hidden_tag_groups;
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
	/* Shadows are filtered with contact-hardening PCSS: a 16-tap blocker
	 * search, then 16 comparison taps over a penumbra sized by the light's
	 * angular radius, between the minimum and maximum radius. */
	float light_angular_radius_degrees; /* source half-angle */
	float max_filter_radius;            /* atlas texels */
	float pcss_min_filter_radius;       /* atlas texels */
	float normal_bias_texels;           /* lateral receiver offset at grazing incidence */
	float depth_bias_texels;            /* receiver comparison offset along the light */
	float transition_fraction;
	float distance_fade_fraction;
	int   debug_cascades;
	/* Camera distance (world units) that the receiver-local map serves with
	 * an extra, finer tile, where its texels are largest on screen; 0 = one
	 * tile for the whole receivers. */
	float receiver_local_near_distance;
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
 * SSAO and the half-resolution directional shadow consume (without it,
 * shadowed surfaces take a single comparison). MSAA also allocates it for
 * the single-sample prepass used by depth-aware post effects. */
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
 * near plane, and optional NDC X/Y offsets included in the projection
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
/* Precreate mesh/billboard pipelines and enabled post resources after Begin,
 * outside render passes. Intended for loading screens; draws no geometry. */
int AeronScene_PrepareResources(AeronScene3D* scene, AeronCullMode mesh_cull);
/* Creates the colour pipelines that `mesh` draws with in `scene`, for
 * instances with cull mode `cull`, so that the first frame that shows the
 * mesh doesn't wait for them. It creates them for each of the mesh's ranges,
 * in both colour passes, with and without image-based lighting. They use the
 * mesh's shaders, with material animation or detail layers when the mesh has
 * them and the scene has an effect atlas. Pipelines belong to the scene and
 * its meshes share them, so a new scene needs its meshes prepared again.
 * Call it outside render passes. Returns zero when a pipeline can't be
 * created. */
int AeronScene_PrepareMesh(AeronScene3D* scene, const AeronSceneMesh* mesh, AeronCullMode cull);

/* Per-frame submissions (reset by AeronScene_Begin). Instances and
 * lights are copied; referenced meshes/tables are borrowed until
 * Render() completes. Over-capacity submissions are dropped with a
 * once-per-frame log. */
void AeronScene_AddMeshInstance(AeronScene3D* scene, const AeronSceneMeshInstance* instance);
/* Returns one when accepted. Capacity failures are counted and logged once
 * per frame rather than silently dropping a light. */
int AeronScene_AddLight(AeronScene3D* scene, const AeronSceneLight* light);

/* The sphere that holds what `light` can light (world units): a spot
 * light's cone, else the sphere of its range around its centre, widened by
 * a tube's half length. */
void AeronScene_LightBounds(const AeronSceneLight* light, float center[3], float* radius);

/* The scene's luminous efficacy: scene values are W/m^2 at 683 lm/W, so
 * 1 / AERON_SCENE_LUMENS_PER_WATT is the `intensity_scale` that turns candela
 * into the scene's units. */
#define AERON_SCENE_LUMENS_PER_WATT 683.0f
/* Lights end where their illuminance times the exposure falls under this.
 * There, a light adds at most a few levels to a dark pixel, and the window
 * has already faded it to almost nothing. */
#define AERON_SCENE_LIGHT_CUTOFF 0.004f

/* World-space light for the mesh's model light `index` (AeronFlightLight),
 * as placed by an instance: `transform` is the instance's row-major
 * model->world matrix, scaled by `units_per_meter` world units per metre,
 * and `table` (optional) articulates the light with its component.
 * `intensity_scale` converts the light's candela to the scene's units (the
 * scene illuminance per lux); intensity also converts from metres, so the
 * light gives the same illuminance 1 m away. A light without a range ends
 * where its illuminance times `exposure` falls under AERON_SCENE_LIGHT_CUTOFF.
 * A point is a sphere and a tube a tube; a panel is a one-sided tube along
 * its length when it is at least three times longer than wide, else a
 * one-sided sphere of its area. `variant` is the instance's variant index
 * (AeronSceneMeshInstance.variant): a light that names variants shines only
 * with them. `hidden_tag_groups` are the instance's hidden tag groups
 * (AeronSceneMeshInstance.hidden_tag_groups). Returns zero for an invalid
 * index or exposure, a light that emits nothing, a light on a component the
 * table hides, a light that doesn't shine with the variant, or a light in a
 * hidden tag group. */
int AeronScene_MeshLight(const AeronSceneMesh* mesh, uint32_t index, const float transform[16],
						 float units_per_meter, const AeronSceneMeshTable* table, uint32_t variant,
						 uint32_t hidden_tag_groups, float intensity_scale, float exposure,
						 AeronSceneLight* out);
/* World-space light for a light that no mesh holds, such as a craft's
 * headlight, converted as AeronScene_MeshLight converts a model light:
 * `transform` places it, in world units per metre. */
int AeronScene_FlightLight(const AeronFlightLight* light, const float transform[16], float units_per_meter,
						   float intensity_scale, float exposure, AeronSceneLight* out);

/* Persistent clustered-forward point-light configuration. The tile size is
 * selected automatically per frame. */
int  AeronScene_SetClusteredLights(AeronScene3D* scene, const AeronSceneClusteredLightDesc* desc);
void AeronScene_GetClusteredLightStats(const AeronScene3D* scene, AeronSceneClusteredLightStats* out);

/* Configure the key directional shadow for this frame. Visible ordinary
 * instances cast automatically. AddShadowCaster queues hidden ordinary
 * geometry (for example the player exterior in an internal view). */
void AeronScene_SetDirectionalShadow(AeronScene3D* scene, const AeronSceneDirectionalShadowDesc* shadow);
void AeronScene_AddShadowCaster(AeronScene3D* scene, const AeronSceneMeshInstance* instance);
void AeronScene_GetDirectionalShadowStats(const AeronScene3D* scene, AeronSceneDirectionalShadowStats* out);

/* Clear color of the scene pass (linear HDR). Deep-space default. */
void AeronScene_SetClearColor(AeronScene3D* scene, const float rgba[4]);

struct AeronEnvironmentLight;
struct AeronPreparedEnvironment;

/* Draws the sky of a prepared environment this frame: its radiance cube,
 * rotated by its world_to_cube and scaled by its intensity. Begin resets
 * it, so without a call there is no sky. An environment without a sky
 * (no radiance) draws nothing. The sky is the first draw of the color
 * pass, under the BEFORE_OPAQUE hook, SKY billboards and all meshes, at
 * the reversed-Z far plane (depth GE test, no write). The environment's
 * overlay, when it has one, is drawn over the SKY billboards and under the
 * meshes. The environment must stay prepared until Render() completes.
 * Its suns are not drawn; queue them with AeronScene_AddSkySun. */
void AeronScene_SetSkyEnvironment(AeronScene3D* scene, const struct AeronPreparedEnvironment* environment);

/* Queue one visible sun for this frame (reset by Begin), drawn on the sky
 * at infinity along the light's world direction, after the sky cube and
 * behind all geometry, additively:
 *   - disc: a limb-darkened disc of the light's angular radius whose mean
 *     radiance is its linear colour over the disc's solid angle, times
 *     radiance_scale, plus the optional glow (which carries
 *     glow_strength times the disc's irradiance out to its radius). A disc
 *     below a pixel is widened with its flux kept; then the light's
 *     display_max_radiance caps the drawn radiance after the scene's
 *     exposure (keeping the size), and anything still beyond a half-float
 *     target is widened with its flux kept. At most four discs per frame.
 *   - sprite: the light's sprite, angular_size_degrees wide, times
 *     sprite_strength, as an additive SKY billboard. A sun follows its own
 *     light only, not the environment's intensity.
 * A light without a visual draws nothing. The sprite texture must stay
 * valid until Render. */
void AeronScene_AddSkySun(AeronScene3D* scene, const struct AeronEnvironmentLight* light);

void AeronScene_SetPassHook(AeronScene3D* scene, AeronScenePassSlot slot, AeronScenePassHookFn fn,
							void* user);

/* Optional in-pass callback after opaque, transparent, and articulated mesh
 * geometry, but before scene billboards. Begin clears it. */
void AeronScene_SetAfterMeshes(AeronScene3D* scene, AeronSceneAfterMeshesFn fn, void* user);

/* Sampler used for the pbr channel-atlas binds. NULL restores the scene's
 * default (linear). Games swap in their own to carry anisotropy settings
 * or a point-sampled retro style (TIE: XvT). Aeron copies the immutable state
 * into mode-specific variants with AMD's recommended temporal mip bias and
 * REPEAT addressing, which dedicated atlas layers require. The
 * source sampler remains owned by the caller and must stay alive while selected. */
/* Selects a caller-owned base sampler and creates mode-specific variants.
 * Returns zero without changing the current sampler set on failure. */
int AeronScene_SetMeshSampler(AeronScene3D* scene, AeronSampler* sampler);

/* Select the diagnostic PBR shader variant for this frame. Begin resets
 * the selection; non-debug builds retain the call as a no-op so game code
 * does not need a build-mode conditional. */
void AeronScene_SetPbrDebugViews(AeronScene3D* scene, int enabled);

/* Debug material override for this frame (Begin resets it): every PBR
 * material is shaded with the given metallic and perceptual roughness, for
 * checking lighting and reflections without authored PBR assets. Enabling
 * it selects the diagnostic shader variant; a no-op in builds without the
 * debug UI. */
void AeronScene_SetPbrDebugMaterial(AeronScene3D* scene, int enabled, float metallic, float roughness);

/* Process-wide scale of emitted light (default 1): material emission, base
 * colour emission, screens and holograms in the PBR shader, and additive
 * and premultiplied billboards (glows, explosions, lens flares). With a low
 * bloom threshold, emitters much brighter than lit surfaces glow through
 * bloom as a lens scatters light. Clamped to >= 0. */
float AeronScene_EmissiveScale(void);
void  AeronScene_SetEmissiveScale(float scale);

/* Back-face fill (on by default; persists across frames): after the opaque
 * meshes, the faces that back- or front-face-culled instances cull are
 * drawn in a dark color, writing depth, wherever no front face covers them.
 * Hairline cracks between authored surfaces, and openings into shells, then
 * show a dark interior instead of the background, and later draws behind
 * the model (engine glows, billboards, transparent meshes) stay hidden. Costs one extra
 * position-only draw per such instance; almost every fragment fails the
 * depth test. */
void AeronScene_SetBackfaceFill(AeronScene3D* scene, int enabled);

/* Leaves out the instances that are small on screen, by the larger side of
 * their bound's projection, in render pixels; 0 turns a threshold off.
 * Instances under `prepass_min_px` aren't drawn in the depth prepass and
 * don't cast the cascades' shadows. Instances under `draw_min_px` aren't
 * drawn at all, except meshes with only blended faces, such as laser bolts.
 * It applies only with MSAA on and FSR off: otherwise the colour pass needs
 * the prepass's depth of every instance. */
void AeronScene_SetSmallInstanceCull(AeronScene3D* scene, float prepass_min_px, float draw_min_px);

/* The minimum width on screen, in render pixels, of emission_only
 * instances, such as laser bolts; 0 turns it off. The width is measured
 * across the bounds' two shorter axes, keeping the one that looks wider. A
 * thinner instance is drawn this wide across that axis, and its emission
 * is multiplied by its real width / drawn width, so it keeps the same
 * light. It then shows as a steady strip instead of broken pixels that
 * flicker as it moves. Wider instances don't change. */
void AeronScene_SetEmissionMinWidth(AeronScene3D* scene, float min_width_px);

/* Dims emission_only instances as they get thinner on screen: their
 * emission is multiplied by (width / full_width_px)^power, capped at 1,
 * with the width in render pixels measured as for
 * AeronScene_SetEmissionMinWidth. Instances at least full_width_px wide
 * don't change. Unlike the minimum width, this doesn't keep their light:
 * it's a way to make far laser bolts dimmer. A full_width_px or power of
 * 0 turns it off. */
void AeronScene_SetEmissionFade(AeronScene3D* scene, float full_width_px, float power);

/* Sets the brightness of emission_only instances, as multiples of their
 * material's emission. They are drawn at `scale`, and the bloom gets
 * `bloom` from them: when `bloom` is higher than `scale`,
 * AeronScene_DrawEmissionBloom adds the difference to the bloom only. A
 * low `scale` keeps the colours of bright laser bolts from clipping in the
 * tone mapping, and `bloom` keeps their glow. Both default to 1. */
void AeronScene_SetEmissionLight(AeronScene3D* scene, float scale, float bloom);

/* Draws the emission_only instances of the last AeronScene_Render into
 * `pass`, adding (bloom - scale) times their emission
 * (AeronScene_SetEmissionLight). `pass` is the extra-light pass of
 * AeronSceneBloom_Apply: a target of target_w x target_h pixels that
 * covers the scene's output. Pixels hidden by the scene's depth add
 * nothing. Call it after AeronScene_Render and before the next
 * AeronScene_Begin. Returns 0 on failure. Returns 1 without drawing when
 * `bloom` isn't above `scale` or nothing was rendered. */
int AeronScene_DrawEmissionBloom(AeronScene3D* scene, AeronRenderPass* pass, int target_w, int target_h);

/* Screen surfaces (gltf_mesh.h, "Screen surfaces"): the per-frame inputs of
 * every screen material. 16-byte rows only, so the C layout matches the
 * shader's structured buffer on every backend. */
#define AERON_SCENE_SCREEN_HIDE_TRANSPARENT 0x1u /* drop transparent, not always-visible */
#define AERON_SCENE_SCREEN_BAR_VISIBLE 0x2u      /* draw fill bars */

typedef struct AeronSceneScreenFrame {
	uint32_t valid[2];                                 /* row 0: bit per source with a rectangle below */
	uint32_t color_state;                              /*        background: 0 bg, 1 highlight, 2 lock */
	uint32_t flags;                                    /*        AERON_SCENE_SCREEN_* */
	float    display_strength;                         /* row 1: emissive multiplier of screens */
	float    bar_level;                                /*        fill-bar level, 0..1 */
	float    holo_time;                                /*        hologram animation time, seconds */
	float    holo_fade;                                /*        hologram visibility, 0..1 */
	float    holo_color[4];                            /* row 2: linear hologram RGB, emissive multiplier */
	float    source[AERON_GLTF_SCREEN_MAX_SOURCES][4]; /* texture UV x0, y0, x1, y1 */
} AeronSceneScreenFrame;

/* Sets the screen inputs for the current frame: the captured `graphics`
 * and `text` textures (premultiplied, sampled bilinearly without mips) and
 * `frame`. NULL textures or frame turn screens off: every source is
 * invalid and holograms are hidden. Reset by Begin. */
void AeronScene_SetScreenSources(AeronScene3D* scene, AeronTexture* graphics, AeronTexture* text,
								 const AeronSceneScreenFrame* frame);

/* Material animation (gltf_mesh.h, "Material animation"): meshes created
 * with `atlas` (AeronScene_MeshCreateEx) draw their flipbooks from it at
 * the animation time, in seconds. Without an atlas, every mesh draws its
 * static materials. Both persist across frames; the atlas is borrowed. */
void AeronScene_SetEffectAtlas(AeronScene3D* scene, AeronEffectAtlas* atlas);
void AeronScene_SetAnimationTime(AeronScene3D* scene, double seconds);

/* The camera exposure the scene's image is shown at
 * (AeronScenePresent_ExposureFromEv100). The temporal upscaler uses it to
 * judge how bright pixels look. Persists across frames; 1 by default. While
 * the scene meters its exposure (AeronScene_SetAutoExposure), the metered
 * value replaces it. */
void AeronScene_SetExposure(AeronScene3D* scene, float exposure);

/* What the image meter measures in a pixel. */
typedef enum AeronSceneExposureChannel {
	AERON_SCENE_EXPOSURE_LUMINANCE = 0, /* Rec.709 luminance */
	AERON_SCENE_EXPOSURE_MAX       = 1, /* the largest of r, g and b: what clips first */
} AeronSceneExposureChannel;

/* The image meter's settings. EVs are EV100. */
typedef struct AeronSceneAutoExposure {
	float ev100;        /* the anchor of partial metering */
	float compensation; /* EV added to brightness */
	float metering;     /* 0 = ev100, 1 = the metered EV */
	float min_ev100;    /* limits of the target */
	float max_ev100;
	float brighten_speed; /* adaptation rate per second when the scene gets darker */
	float darken_speed;   /* adaptation rate per second when the scene gets brighter */
	float low_percent;    /* the darkest share of the weighted pixels left out, 0 to 100 */
	float high_percent;   /* the brightest share starts here, above low_percent */
	float sky_weight;     /* weight of pixels where no geometry wrote depth */
	float near_weight;    /* weight of geometry nearer than near_distance, such as a cockpit */
	float near_distance;  /* in metres (AeronScene3DDesc.view_space_to_meters); 0 = none */
	float edge_weight;    /* weight at the screen's corners; 1 at the centre */
	AeronSceneExposureChannel channel;
	int                       freeze; /* keep the current EV */
} AeronSceneAutoExposure;

/* Meters the scene's exposure from its image, on the GPU, and adapts it
 * over real time. Each Render measures the scene's colour after the 3D
 * draws and before FSR: a histogram of the pixels' EV100 (reflected-light
 * meter, K = 12.5), weighted by `edge_weight`, `sky_weight` and
 * `near_weight`, averaged
 * between the two percentiles. The target is
 * lerp(ev100, metered, metering) - compensation, within the limits, and the
 * current EV closes the gap to it as 1 - e^(-speed * t). The frame's image
 * uses the exposure metered from that frame (AeronScene_ExposureTexture).
 * NULL goes back to the fixed exposure of AeronScene_SetExposure. Persists
 * across frames. The first metered frame, and the first after the settings
 * change, take their target at once. */
void AeronScene_SetAutoExposure(AeronScene3D* scene, const AeronSceneAutoExposure* settings);

/* The next metered frame takes its target at once: a camera cut, a load. */
void AeronScene_SnapExposure(AeronScene3D* scene);

/* The 1x1 R32G32_FLOAT texture with the scene's exposure in R and its
 * EV100 in G, written by each Render: by the meter, or from
 * AeronScene_SetExposure. FSR reads it, and so can
 * AeronScenePresentChain_Draw. NULL before the first Render. */
AeronTexture* AeronScene_ExposureTexture(AeronScene3D* scene);

/* The meter's histogram: half-EV bins from EV100 -8. Darker pixels are not
 * counted; brighter ones go in the last bin. */
#define AERON_SCENE_EXPOSURE_BINS 64
#define AERON_SCENE_EXPOSURE_FIRST_EV100 (-8.0f)
#define AERON_SCENE_EXPOSURE_BIN_EV 0.5f

typedef struct AeronSceneExposureReadout {
	float exposure; /* the exposure k */
	float ev100;    /* the current EV100 */
	float target_ev100;
	float metered_ev100; /* the last metered value */
	float low_ev100;     /* the range the average was taken over */
	float high_ev100;
	float geometry_share;                       /* geometry's share of the histogram's weight */
	int   metered;                              /* 0 when no pixel was in the histogram's range */
	float histogram[AERON_SCENE_EXPOSURE_BINS]; /* weights; they add up to 1 */
} AeronSceneExposureReadout;

/* The newest metering the GPU has finished, a few frames late. Returns 0
 * before the first one and while the exposure is fixed. */
int AeronScene_GetExposureReadout(const AeronScene3D* scene, AeronSceneExposureReadout* out);

/* Fills the scene's depth and its meter's weights into `out`, a debug
 * view of AeronScenePresentChain_SetDebug, without changing its view.
 * Returns 0 while the exposure is fixed. */
struct AeronScenePresentDebug;
int AeronScene_GetMeterDebug(AeronScene3D* scene, struct AeronScenePresentDebug* out);

/* The radiance in W/m^2/sr of a texel of 1 in an image the scene shows
 * unlit: cockpit screens and holograms, the unlit part of base colour
 * flipbook frames, and the billboards outside the SKY stage that emit light
 * (additive and premultiplied). These images have no unit of their own, so
 * the caller decides how bright they are. Persists across frames; 1 by
 * default. */
void AeronScene_SetImageRadiance(AeronScene3D* scene, float radiance);

/* Image-based lighting environment for the PBR shader, for the current
 * frame. `diffuse` is a Lambert irradiance / pi cube; its sample along the
 * shading normal, times `diffuse_strength`, adds to the diffuse ambient.
 * `specular` (GGX roughness mips) and its strength and mip count are
 * carried for specular image-based lighting. `world_to_local` (row-major)
 * maps world directions into the cubes' frame for both lookups. */
typedef struct AeronScenePbrEnvironment {
	AeronTexture* diffuse;  /* NULL = none */
	AeronTexture* specular; /* NULL = none */
	AeronSampler* sampler;
	uint32_t      specular_mip_count;
	float         diffuse_strength;
	float         specular_strength;
	float         world_to_local[9];
} AeronScenePbrEnvironment;

struct AeronPreparedEnvironment;

/* Selects the environment for the current frame (copied; textures borrowed
 * through Render). NULL, a missing diffuse cube or sampler, or a zero
 * diffuse strength clears it. Begin clears the selection. The raw form
 * serves producers that are not authored environments, such as a procedural
 * tunnel. */
void AeronScene_SetPbrEnvironment(AeronScene3D* scene, const AeronScenePbrEnvironment* environment);
/* Selects a prepared environment's diffuse cube, diffuse strength and
 * world-to-cube matrix. NULL clears the selection. */
void AeronScene_SetPbrPreparedEnvironment(AeronScene3D*                          scene,
										  const struct AeronPreparedEnvironment* environment);

/* Places a model's probe set (AeronSceneMesh.probes) for this frame.
 * `model_to_world` is the transform of the instance that owns the set, a
 * row-major 3x4 matrix from model metres to scene units. Instances that use
 * the set are lit by its probes and grid, blended with the environment
 * where those do not apply. Directional lights do not light anything inside
 * the grid, because a set is treated as sealed. Screens never use probes.
 * Returns the set's index, or -1 when AERON_SCENE_MAX_PROBE_SETS sets are
 * already placed. Begin clears the sets. The set must stay valid until
 * Render. */
int AeronScene_AddProbeSet(AeronScene3D* scene, const struct AeronProbeSet* probes,
						   const float model_to_world[12]);

/* The key light is in the probe sets' bake (a hangar's ceiling light), and
 * the probe shaders take its shadow from every side (PbrLightFS
 * key_shadow_facing 0): the half-resolution visibility pass then also reads
 * that shadow 1 m out along the normals of surfaces in shadow turning away
 * from the light, so that a hull's own curve does not shadow it. Reset by
 * Begin. */
void AeronScene_SetProbeKeyShadow(AeronScene3D* scene, int enabled);

/* The instances inside the first probe set's grid box that cast shadows,
 * other than the set's owner (whose model the probes were baked in), take
 * its light from what they cover: the probe shaders' diffuse light and
 * reflections lose what they hide (a hangar's ceiling light under a parked
 * ship). Their occupancy is rebuilt where one moved. Replaces the key light
 * shadow's cut of the probe light. Reset by Begin. */
void AeronScene_SetProbeOcclusion(AeronScene3D* scene, int enabled);

/* When the first probe set has a reflection plane (a hangar's floor), the
 * instances inside its grid other than its owner are drawn mirrored across
 * it, and the owner's surfaces on the plane reflect them in place of the
 * probes' reflection. Reset by Begin. */
void AeronScene_SetPlanarReflection(AeronScene3D* scene, int enabled);

/* Queue a per-frame uniform blob (copied; reset by Begin) that the
 * scene binds to (stage, slot) in the color pass AFTER the
 * BEFORE_OPAQUE hook and before the sky billboards / instance walk —
 * the declarative form of the games' pass-level shading environment.
 * PBR fragment slots: b0 is scene-owned directional shadow data, b1 the
 * game's lighting block (PbrLightFS), b2 the scene lighting block
 * (clustered lights and environment); b3 is free.
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
	/* Blur limits and velocity tile coverage scale with output height from
	 * a 2160p reference. Sample ceilings are 8 (low) and 16 (high). */
	int   mb_quality; /* 0=off, 1=low (own velocity), 2=high (TileMax/NeighborMax dilation) */
	float mb_shutter;
	int   mb_camera_blur;
	int   mb_velocity_viz;
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
 * `prev_ori` is the previous camera orientation. The camera velocity fill
 * runs when the camera turned since it. NULL uses the orientation of the
 * scene's last render.
 * `velocity_regen` 0 holds the previous velocity buffer (the sim did
 * not advance this host frame); 1 regenerates it. */
void AeronScene_SetMotionContext(AeronScene3D* scene, const float prev_view_proj[16], const float prev_ori[4],
								 int velocity_regen);

/* After Render(): the output-resolution RT holding the finished scene. */
AeronRenderTarget* AeronScene_SceneRt(AeronScene3D* scene);
/* After Render(): the screens of the `screens_to_layer` instances, at render
 * resolution, with premultiplied colour and their coverage in alpha, or
 * NULL when no instance drew into it this frame. Draw it over the scene with
 * `layer.rgb + scene * (1 - layer.a)`. The layer is made the first time an
 * instance uses it and kept until the scene's targets are released. */
AeronTexture* AeronScene_ScreenLayer(AeronScene3D* scene);

/* Record the frame's passes into `cmd` (no pass may be open on it). Returns
 * zero after recording a sticky command-buffer failure.
 * Opens the scene pass with shared color and reversed-Z depth clears,
 * runs the in-pass hooks, then closes it and runs BEFORE_POST. */
int AeronScene_Render(AeronScene3D* scene, AeronCommandBuffer* cmd);

/* Borrowed views for layer submission / game-side hook passes. */
AeronTexture*      AeronScene_ColorTexture(AeronScene3D* scene);
AeronRenderTarget* AeronScene_ColorRt(AeronScene3D* scene);
AeronDepthTarget*  AeronScene_DepthRt(AeronScene3D* scene);
/* The value that AeronScene_DepthRt holds at view distance `distance`, in
 * scene units, for the last rendered frame. A larger value is nearer. */
float              AeronScene_DepthAtDistance(const AeronScene3D* scene, float distance);
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
