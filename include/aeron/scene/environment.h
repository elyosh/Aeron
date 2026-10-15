#ifndef AERON_SCENE_ENVIRONMENT_H
#define AERON_SCENE_ENVIRONMENT_H

/*
 * Authored HDR environment packages.
 *
 * A package is a named definition in a `packages:` YAML map. A `full`
 * package owns a visible sky (radiance cube), diffuse and specular
 * image-based lighting cubes, and up to four analytical directional lights,
 * each drawn as a visible sun. A `lighting` package only lights an enclosed
 * space with a diffuse cube. A `probes` package belongs to a model: its
 * reflection probes (a cube array) and its irradiance grid, placed in the
 * model's frame, blended with the scene's environment where they do not
 * apply. Texture paths are kept as written; the game decides which VFS
 * root and prefix they resolve against.
 *
 * Cube-local directions use the authored cubemap frame: +X right, +Y up,
 * -Z forward. `rotation_degrees` is (yaw, pitch, roll) in that frame,
 * composed as Q = Ry(yaw) * Rx(pitch) * Rz(roll).
 *
 * Definitions are immutable once parsed. Where the `packages:` node lives
 * and how packages are selected belong to the game.
 *
 * The package library turns the definitions a game currently needs into
 * GPU packages: it loads and shares textures by path, prepares each
 * reference's world-to-cube matrix and world-space lights, publishes a new
 * set only once it is built, and retires textures the new set no longer
 * carries. It opens files only in Prepare and ServiceReload, which are meant
 * for loading screens. All calls are render-thread only.
 */

#include <stdbool.h>
#include <stdint.h>

#include <stddef.h>

#include "aeron/config_file.h"
#include "aeron/render.h"
#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	AERON_ENV_MAX_LIGHTS   = 4,
	AERON_ENV_MAX_PACKAGES = 256,
	AERON_ENV_NAME_MAX     = 64,  /* includes the terminator */
	AERON_ENV_PATH_MAX     = 256, /* includes the terminator */
	/* A probes package's cube array: 6 layers per probe, within the 256
	 * array layers every Vulkan device has. */
	AERON_ENV_MAX_PROBES = 42,
	/* Irradiance grid points per axis. */
	AERON_ENV_PROBE_GRID_MIN = 2,
	AERON_ENV_PROBE_GRID_MAX = 64
};

typedef enum AeronEnvironmentKind {
	AERON_ENV_KIND_FULL,
	AERON_ENV_KIND_LIGHTING,
	AERON_ENV_KIND_PROBES,
} AeronEnvironmentKind;

typedef enum AeronEnvironmentSunVisual {
	AERON_ENV_SUN_DISC,
	AERON_ENV_SUN_SPRITE,
} AeronEnvironmentSunVisual;

typedef struct AeronEnvironmentLightDefinition {
	char                      name[AERON_ENV_NAME_MAX];
	float                     direction_cube[3]; /* normalized surface-to-light direction */
	float                     color_linear[3];
	float                     intensity;
	float                     angular_radius_degrees;
	bool                      primary;
	bool                      casts_shadows;
	bool                      lens_flare;
	AeronEnvironmentSunVisual visual;
	float                     radiance_scale;                  /* disc */
	float                     glow_radius_degrees;             /* disc; 0 when absent */
	float                     glow_strength;                   /* disc; 0 when absent */
	char                      sprite_path[AERON_ENV_PATH_MAX]; /* sprite */
	float                     angular_size_degrees;            /* sprite */
	float                     sprite_strength;                 /* sprite */
} AeronEnvironmentLightDefinition;

typedef enum AeronEnvironmentProbeShape {
	AERON_ENV_PROBE_BOX,
	AERON_ENV_PROBE_ELLIPSOID,
} AeronEnvironmentProbeShape;

/* A reflection probe, in the model frame (aeron axes), metres. `center` is
 * where its cube was captured and the centre of its influence; `size` the
 * influence's full extent (an ellipsoid's axes). Its weight falls from 1 to
 * 0 over the outer `falloff` fraction of the influence. Reflections are
 * projected onto the parallax volume, around `parallax_center` (by default
 * `center`: several probes in a room can share the room's box). */
typedef struct AeronEnvironmentProbeDefinition {
	float                      center[3];
	float                      size[3];
	AeronEnvironmentProbeShape shape;
	float                      falloff; /* (0, 1] */
	float                      parallax_center[3];
	float                      parallax_size[3];
	AeronEnvironmentProbeShape parallax_shape;
} AeronEnvironmentProbeDefinition;

typedef struct AeronEnvironmentDefinition {
	char                            name[AERON_ENV_NAME_MAX];
	AeronEnvironmentKind            kind;
	char                            radiance_path[AERON_ENV_PATH_MAX]; /* empty for lighting packages */
	char                            diffuse_path[AERON_ENV_PATH_MAX];
	char                            specular_path[AERON_ENV_PATH_MAX]; /* empty: no reflections (lighting) */
	float                           intensity; /* scales the sky, diffuse and specular together */
	float                           diffuse_strength;
	float                           specular_strength; /* 0 without a specular cube */
	float                           ambient_fill[3];   /* linear radiance from every direction */
	float                           rotation_degrees[3]; /* yaw, pitch, roll; each in [0, 360) */
	AeronEnvironmentLightDefinition lights[AERON_ENV_MAX_LIGHTS];
	uint32_t                        light_count;
	int32_t                         primary_light_index; /* -1 when none */
	/* Probes packages: layer k of the cube array is probe k; the grid is
	 * an axis-aligned box of resolution points, sampled as Blender's
	 * volume probe (point i at (i + 1) / (resolution + 1) of the box). */
	char                            cubes_path[AERON_ENV_PATH_MAX];
	char                            grid_path[AERON_ENV_PATH_MAX];
	float                           grid_center[3];
	float                           grid_size[3];
	uint32_t                        grid_resolution[3];
	AeronEnvironmentProbeDefinition probes[AERON_ENV_MAX_PROBES];
	uint32_t                        probe_count; /* 1 to AERON_ENV_MAX_PROBES */
	/* When the scene's key light is in the bake (a hangar's ceiling light),
	 * the part of the probe light its shadow removes, 0-1 (default 0.7). */
	float                           key_light_shadow;
	/* The floor that reflects what stands on it (a hangar's): a horizontal
	 * rectangle facing up, its centre and size along x and y; the owner's
	 * surfaces within `distance` of it take its reflection. */
	bool                            has_reflection_plane;
	float                           reflection_plane_center[3];
	float                           reflection_plane_size[2];
	float                           reflection_plane_distance;
} AeronEnvironmentDefinition;

/* A set of parsed package definitions, fixed once in use. */
typedef struct AeronEnvironmentDefinitionSet AeronEnvironmentDefinitionSet;

/* Parses a `packages:` map node. On failure returns false, leaves *out_set
 * NULL and fills `error` with the offending node's source path, line and
 * a message. An empty map yields an empty set. */
bool AeronEnvironment_ParsePackages(const AeronConfigNode* packages, AeronEnvironmentDefinitionSet** out_set,
									AeronConfigError* error);
/* Adds the packages of another `packages:` map to `set`, for packages kept
 * in their own files. Names must stay unique across the set. Call it before
 * the set is used: it may move the definitions. On failure the set is
 * unchanged and `error` is filled as above. */
bool AeronEnvironment_AddPackages(const AeronConfigNode* packages, AeronEnvironmentDefinitionSet* set,
								  AeronConfigError* error);
void AeronEnvironmentDefinitionSet_Destroy(AeronEnvironmentDefinitionSet* set);

uint32_t AeronEnvironmentDefinitionSet_Count(const AeronEnvironmentDefinitionSet* set);
const AeronEnvironmentDefinition* AeronEnvironmentDefinitionSet_Get(const AeronEnvironmentDefinitionSet* set,
																	uint32_t index);
/* Returns the definition named `name`, or NULL. */
const AeronEnvironmentDefinition* AeronEnvironmentDefinitionSet_Find(const AeronEnvironmentDefinitionSet* set,
																	 const char* name);

/* Parses a `directional_lights` sequence with the package light schema
 * (also used by tools that take a light list on its own). `owner` names
 * the list in error messages. */
bool AeronEnvironment_ParseLights(const AeronConfigNode* lights, const char* owner,
								  AeronEnvironmentLightDefinition out[AERON_ENV_MAX_LIGHTS],
								  uint32_t* out_count, int32_t* out_primary, AeronConfigError* error);

/* Reads the `grid_box`, `grid_resolution`, `probes`, `key_light_shadow` and
 * `reflection_plane` fields of a map with the probes package schema into `def`, leaving its
 * other fields (also used by tools whose map holds other fields; the caller
 * checks those). `what` names the map in error messages. */
bool AeronEnvironment_ParseProbeSet(const AeronConfigNode* map, const char* what,
									AeronEnvironmentDefinition* def, AeronConfigError* error);

/* Package and light names: 1 to 63 characters of lower-case ASCII letters,
 * digits, '.' and '_'. */
bool AeronEnvironment_IsValidName(const char* name);

/* A game's use of a package: its name and an optional rotation that
 * replaces the package's own `rotation_degrees`, so one package can appear
 * with different headings in different places while sharing textures. */
typedef struct AeronEnvironmentReference {
	char  package[AERON_ENV_NAME_MAX];
	bool  has_rotation;
	float rotation_degrees[3]; /* yaw, pitch, roll; each in [0, 360) */
} AeronEnvironmentReference;

/* Normalizes degrees into [0, 360). */
float AeronEnvironment_NormalizeDegrees(float degrees);

/* Formats `definition` as a `packages:` document holding its one entry.
 * Returns the full length (excluding the terminator) like snprintf; the
 * output is truncated to `size`. */
size_t AeronEnvironment_FormatPackageYaml(const AeronEnvironmentDefinition* definition, char* out,
										  size_t size);

/* ---- probe cells ------------------------------------------------------ */

enum {
	AERON_ENV_PROBE_CELL_PROBES   = 4,  /* probes kept per cell */
	AERON_ENV_PROBE_CELL_MAX_AXIS = 64, /* cells per axis */
};

#define AERON_ENV_PROBE_CELL_MIN_SIZE 2.0f /* metres */

/* Which probes a point may use, precomputed so a pixel tests at most
 * AERON_ENV_PROBE_CELL_PROBES whatever the probe count. The cells cover
 * the union of the probes' influence boxes (an ellipsoid counts as its
 * box) with cubes of max(AERON_ENV_PROBE_CELL_MIN_SIZE, largest extent /
 * AERON_ENV_PROBE_CELL_MAX_AXIS). A cell lists the probes whose influence
 * box overlaps it, smallest volume first, at most four, in one word: byte k
 * holds the k-th probe's index + 1, 0 when none. */
typedef struct AeronEnvironmentProbeCells {
	float     min[3];    /* model frame, metres */
	float     cell_size; /* metres */
	uint32_t  dims[3];
	uint32_t* words;          /* dims[0] * dims[1] * dims[2]; x fastest, then y, then z */
	uint32_t  most_probes;    /* most influence boxes overlapping one cell */
	uint32_t  overflow_cells; /* cells overlapped by more than four (the four smallest kept) */
} AeronEnvironmentProbeCells;

/* Builds the cells of a probes package; false when it has no probes or
 * memory runs out. Free with AeronEnvironment_FreeProbeCells. */
bool AeronEnvironment_BuildProbeCells(const AeronEnvironmentDefinition* definition,
									  AeronEnvironmentProbeCells*       out);
void AeronEnvironment_FreeProbeCells(AeronEnvironmentProbeCells* cells);

/* ---- package library ------------------------------------------------- */

/* One prepared light, in world space. */
typedef struct AeronEnvironmentLight {
	char                      name[AERON_ENV_NAME_MAX];
	float                     direction_world[3]; /* surface-to-light, after rotation */
	float                     color_linear[3];    /* color_linear * intensity */
	float                     angular_radius_degrees;
	bool                      primary;
	bool                      casts_shadows;
	bool                      lens_flare;
	AeronEnvironmentSunVisual visual;
	float                     radiance_scale;       /* disc */
	float                     glow_radius_degrees;  /* disc */
	float                     glow_strength;        /* disc */
	float                     display_max_radiance; /* disc: largest drawn radiance; 0 = no cap */
	AeronTexture*             sprite;               /* sprite; borrowed from the library */
	float                     angular_size_degrees; /* sprite */
	float                     sprite_strength;      /* sprite */
} AeronEnvironmentLight;

/* One prepared reference. Textures are borrowed from the library and stay
 * valid until a later Prepare or ServiceReload publishes a set without them. */
typedef struct AeronEnvironmentPackage {
	const AeronEnvironmentDefinition* definition; /* as parsed */
	/* Effective values: the definition, the reference's rotation and any
	 * debug overrides. Texture paths are those of the definition. */
	AeronEnvironmentDefinition settings;
	AeronTexture*              radiance; /* NULL for lighting packages */
	AeronTexture*              diffuse;
	AeronTexture*              specular; /* NULL for lighting packages */
	uint32_t                   specular_mip_count;
	AeronSampler*              sampler;          /* the library's environment sampler */
	float                      world_to_cube[9]; /* row-major: Q^T * B */
	AeronEnvironmentLight      lights[AERON_ENV_MAX_LIGHTS];
	uint32_t                   light_count;
	int32_t                    primary_light_index; /* -1 when none */
	/* Probes packages (NULL otherwise): the probes' cube array (probe k in
	 * layer k), the irradiance grid as a 3D RGBA16F texture of rx x ry x
	 * 6 rz texels (slab f, z from f rz, holds the irradiance / pi toward
	 * +x, -x, +y, -y, +z, -z for f = 0 to 5 in RGB, and the luminance of
	 * the mean radiance in A), and the set buffer below. */
	AeronTexture* probe_cubes;
	AeronTexture* probe_grid;
	AeronBuffer*  probe_buffer;
	uint32_t      probe_mip_count;
	/* The set's extent in the model frame, metres: its probes' influence
	 * boxes and its grid box with the grid's fade. */
	float probe_bounds_min[3];
	float probe_bounds_max[3];
} AeronEnvironmentPackage;

/* The set buffer of a probes package, uint4 elements (floats as bits):
 *   [0]            cells min xyz (model frame, metres), 1 / cell size
 *   [1]            cell dims xyz, probe count (uints)
 *   [2 + 6k]       probe k: centre xyz, 1 / falloff
 *   [2 + 6k + 1]   probe k: 1 / half size xyz, cube array layer (uint)
 *   [2 + 6k + 2]   probe k: parallax half size xyz, shapes (uint: bit 0
 *                  influence ellipsoid, bit 1 parallax ellipsoid)
 *   [2 + 6k + 3]   probe k: parallax centre xyz, 0
 *   [2 + 6k + 4]   probe k: the grid's light at its centre, as luminance:
 *                  toward +x, -x, +y, -y
 *   [2 + 6k + 5]   probe k: toward +z, -z, the mean, 0 (all 0 when the
 *                  centre is outside the grid)
 *   [2 + 6N ...]   the cell words (AeronEnvironmentProbeCells), four per
 *                  element, x fastest, then y, then z.
 * The grid weighs 1 inside its box and falls to 0 one point spacing
 * outside it, so the set's extent includes that margin. */
enum { AERON_ENV_PROBE_BUFFER_HEADER = 2, AERON_ENV_PROBE_BUFFER_RECORD = 6 };

/* Prepared probes packages (textures and buffers) above this size are
 * logged as a warning: probes cost about 450 KB each, so many probe-owning
 * models add up. */
#define AERON_ENV_WARN_BYTES ((uint64_t)256 << 20)

typedef struct AeronEnvironmentLibrary AeronEnvironmentLibrary;

typedef struct AeronEnvironmentLibraryDesc {
	/* Borrowed; must outlive the library, or AeronEnvironmentLibrary_SetDefinitions's
	 * next preparation. */
	const AeronEnvironmentDefinitionSet* definitions;
	/* Package texture paths resolve to `path_prefix` + path under `root`. */
	AeronVfs*    vfs;
	AeronVfsRoot root;
	const char*  path_prefix; /* may be NULL */
	/* Row-major world-to-cube matrix B for a zero package rotation. */
	float base_world_to_cube[9];
	/* Largest radiance a sun disc is drawn with (its lighting is not
	 * capped); 0 selects AERON_ENV_DEFAULT_SUN_MAX_RADIANCE. A physical
	 * disc is tens of thousands of times brighter than the sky, which
	 * screen-space bloom cannot resolve cleanly; the halo comes from the
	 * authored glow instead. */
	float sun_max_radiance;
} AeronEnvironmentLibraryDesc;

#define AERON_ENV_DEFAULT_SUN_MAX_RADIANCE 100.0f

AeronEnvironmentLibrary* AeronEnvironmentLibrary_Create(const AeronEnvironmentLibraryDesc* desc);
/* Destroys the library and every texture it holds. */
void AeronEnvironmentLibrary_Destroy(AeronEnvironmentLibrary* library);

/* Loads and uploads, with `cmd`, every texture the references need, reusing
 * textures the current set holds, then publishes the new set (index i is
 * references[i]) and releases the textures it no longer carries. A reference
 * whose package is unknown, or whose textures fail to load, upload or are
 * unsupported by the device, is unavailable; the others are usable. Debug
 * overrides carry over to an identical reference whose definition has not
 * changed. Returns false only when
 * the set could not be built at all; the previous set then stays published. */
bool AeronEnvironmentLibrary_Prepare(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd,
									 const AeronEnvironmentReference* references, uint32_t count);

uint32_t AeronEnvironmentLibrary_ReferenceCount(const AeronEnvironmentLibrary* library);
/* The prepared package for reference `index`, or NULL when unavailable. */
const AeronEnvironmentPackage* AeronEnvironmentLibrary_Find(const AeronEnvironmentLibrary* library,
															uint32_t                       index);
/* Why reference `index` is unavailable, or NULL when it is available. */
const char* AeronEnvironmentLibrary_FailureReason(const AeronEnvironmentLibrary* library, uint32_t index);
/* Clamp-to-edge trilinear sampler for every environment texture. */
AeronSampler* AeronEnvironmentLibrary_Sampler(const AeronEnvironmentLibrary* library);

/* Debug reload: RequestReload marks the current references for reloading
 * from disk; the game calls ServiceReload with a command buffer at its next
 * preparation point. The current set stays published until the reloaded one
 * replaces it. ServiceReload returns true when it ran. */
void AeronEnvironmentLibrary_RequestReload(AeronEnvironmentLibrary* library);
/* Replaces the borrowed definitions, for a game that reads them again on a
 * reload. The next Prepare or ServiceReload resolves references against
 * `definitions`; keep the previous set until that call returns, since the
 * published packages point into it until then. */
void AeronEnvironmentLibrary_SetDefinitions(AeronEnvironmentLibrary*             library,
											const AeronEnvironmentDefinitionSet* definitions);
bool AeronEnvironmentLibrary_ReloadRequested(const AeronEnvironmentLibrary* library);
bool AeronEnvironmentLibrary_ServiceReload(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd);

/* The reference the game currently renders with, or -1; shown by the debug
 * tool. */
void AeronEnvironmentLibrary_SetActive(AeronEnvironmentLibrary* library, int32_t index);

/* Registers the `Environment` debug tool for this library when the debug UI
 * is compiled in (otherwise a no-op). `rows`, when set, emits the game's own
 * rows inside the tool window. One library is shown at a time; destroying it
 * detaches the tool. */
typedef void (*AeronEnvironmentDebugRowsFn)(void* user);
void AeronEnvironmentLibrary_RegisterDebugTool(AeronEnvironmentLibrary*    library,
											   AeronEnvironmentDebugRowsFn rows, void* user);

#ifdef __cplusplus
}
#endif

#endif
