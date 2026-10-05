#ifndef AERON_SCENE_ENVIRONMENT_H
#define AERON_SCENE_ENVIRONMENT_H

/*
 * Authored HDR environment packages.
 *
 * A package is a named definition in a `packages:` YAML map. A `full`
 * package owns a visible sky (radiance cube), diffuse and specular
 * image-based lighting cubes, and up to four analytical directional lights,
 * each drawn as a visible sun. A `lighting` package only lights an enclosed
 * space with a diffuse cube. Texture paths are kept as written; the game
 * decides which VFS root and prefix they resolve against.
 *
 * Cube-local directions use the authored cubemap frame: +X right, +Y up,
 * -Z forward. `rotation_degrees` is (yaw, pitch, roll) in that frame,
 * composed as Q = Ry(yaw) * Rx(pitch) * Rz(roll).
 *
 * Definitions are immutable once parsed. Where the `packages:` node lives
 * and how packages are selected belong to the game.
 */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/config_file.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	AERON_ENV_MAX_LIGHTS   = 4,
	AERON_ENV_MAX_PACKAGES = 256,
	AERON_ENV_NAME_MAX     = 64, /* includes the terminator */
	AERON_ENV_PATH_MAX     = 256 /* includes the terminator */
};

typedef enum AeronEnvironmentKind {
	AERON_ENV_KIND_FULL,
	AERON_ENV_KIND_LIGHTING,
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

typedef struct AeronEnvironmentDefinition {
	char                            name[AERON_ENV_NAME_MAX];
	AeronEnvironmentKind            kind;
	char                            radiance_path[AERON_ENV_PATH_MAX]; /* empty for lighting packages */
	char                            diffuse_path[AERON_ENV_PATH_MAX];
	char                            specular_path[AERON_ENV_PATH_MAX]; /* empty for lighting packages */
	float                           sky_exposure;                      /* 0 for lighting packages */
	float                           diffuse_strength;
	float                           specular_strength; /* 0 for lighting packages */
	float                           ambient_floor_scale;
	float                           rotation_degrees[3]; /* yaw, pitch, roll; each in [0, 360) */
	AeronEnvironmentLightDefinition lights[AERON_ENV_MAX_LIGHTS];
	uint32_t                        light_count;
	int32_t                         primary_light_index; /* -1 when none */
} AeronEnvironmentDefinition;

/* Immutable set of parsed package definitions. */
typedef struct AeronEnvironmentDefinitionSet AeronEnvironmentDefinitionSet;

/* Parses a `packages:` map node. On failure returns false, leaves *out_set
 * NULL and fills `error` with the offending node's source path, line and
 * a message. An empty map yields an empty set. */
bool AeronEnvironment_ParsePackages(const AeronConfigNode* packages, AeronEnvironmentDefinitionSet** out_set,
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

#ifdef __cplusplus
}
#endif

#endif
