#ifndef AERON_SCENE_ENVIRONMENT_H
#define AERON_SCENE_ENVIRONMENT_H

/*
 * Authored HDR environment packages.
 *
 * A package is a named definition in a `packages:` YAML map. It can have
 * these fields:
 *   - `radiance`: a visible sky (radiance cube). It requires
 *     `directional_lights` (use [] for a sky without suns). Each light is
 *     drawn as a sun using its `visual`.
 *   - `diffuse` and `specular`: image-based lighting cubes, each with a
 *     strength. `specular` requires `diffuse`.
 *   - `directional_lights`: up to four analytical lights that replace the
 *     game's lights. Without this field, the game keeps its own lights.
 *   - `ambient_fill`: radiance added from every direction (default 0).
 * Model probes are not part of packages. They are cooked into the model's
 * GLB (see aeron/scene/probe_set.h). Texture paths are stored as written.
 * The game decides which VFS root and prefix they are resolved against.
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
};

typedef enum AeronEnvironmentSunVisual {
	AERON_ENV_SUN_DISC,
	AERON_ENV_SUN_SPRITE,
	AERON_ENV_SUN_NONE, /* no `visual`; only allowed in a package without a sky */
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

typedef struct AeronEnvironmentDefinition {
	char                            name[AERON_ENV_NAME_MAX];
	char                            radiance_path[AERON_ENV_PATH_MAX]; /* empty: no sky */
	char                            diffuse_path[AERON_ENV_PATH_MAX];  /* empty: no diffuse cube */
	char                            specular_path[AERON_ENV_PATH_MAX]; /* empty: no reflections */
	float                           intensity; /* scales the sky, diffuse and specular together */
	float                           diffuse_strength;  /* 0 without a diffuse cube */
	float                           specular_strength; /* 0 without a specular cube */
	float                           ambient_fill[3];   /* linear radiance from every direction */
	float                           rotation_degrees[3]; /* yaw, pitch, roll; each in [0, 360) */
	AeronEnvironmentLightDefinition lights[AERON_ENV_MAX_LIGHTS];
	uint32_t                        light_count;
	int32_t                         primary_light_index; /* -1 when none */
	/* True when the package has `directional_lights`, which is always the
	 * case with a sky. Its lights replace the game's lights, even when the
	 * list is empty. Without the field, the game keeps its own lights. */
	bool                            has_directional_lights;
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
 * the list in error messages. A light's `visual` is optional here and
 * defaults to AERON_ENV_SUN_NONE. A package with a sky requires it. */
bool AeronEnvironment_ParseLights(const AeronConfigNode* lights, const char* owner,
								  AeronEnvironmentLightDefinition out[AERON_ENV_MAX_LIGHTS],
								  uint32_t* out_count, int32_t* out_primary, AeronConfigError* error);

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
	AeronTexture*              radiance; /* NULL without a sky */
	AeronTexture*              diffuse;  /* NULL without a diffuse cube */
	AeronTexture*              specular; /* NULL without a specular cube */
	uint32_t                   specular_mip_count;
	AeronSampler*              sampler;          /* the library's environment sampler */
	float                      world_to_cube[9]; /* row-major: Q^T * B */
	AeronEnvironmentLight      lights[AERON_ENV_MAX_LIGHTS];
	uint32_t                   light_count;
	int32_t                    primary_light_index; /* -1 when none */
} AeronEnvironmentPackage;

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
