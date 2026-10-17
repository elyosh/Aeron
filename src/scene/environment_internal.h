#ifndef AERON_SCENE_ENVIRONMENT_INTERNAL_H
#define AERON_SCENE_ENVIRONMENT_INTERNAL_H

/* Package library internals shared with the Environment debug tool. */

#include "aeron/scene/environment.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum EnvTextureKind {
	ENV_TEXTURE_2D,   /* sun sprites */
	ENV_TEXTURE_CUBE, /* sky and lighting cubes */
} EnvTextureKind;

/* One texture held by the published set, keyed by package path. */
typedef struct EnvTexture {
	char               path[AERON_ENV_PATH_MAX];
	AeronTexture*      texture; /* NULL when loading failed */
	AeronTextureFormat format;
	int                size; /* level-0 width */
	int                level_count;
	EnvTextureKind     kind;
	uint64_t           bytes; /* uploaded payload */
	char               failure[128];
} EnvTexture;

/* One prepared reference. */
typedef struct EnvSlot {
	AeronEnvironmentReference reference;
	AeronEnvironmentPackage   package;
	bool                      available;
	bool                      overridden; /* package.settings edited in the debug tool */
	char                      failure[160];
} EnvSlot;

struct AeronEnvironmentLibrary {
	const AeronEnvironmentDefinitionSet* definitions;
	AeronVfs*                            vfs;
	AeronVfsRoot                         root;
	char                                 path_prefix[AERON_ENV_PATH_MAX];
	float                                base[9];
	float                                sun_max_radiance;
	AeronSampler*                        sampler;
	EnvTexture*                          textures;
	uint32_t                             texture_count;
	EnvSlot*                             slots;
	uint32_t                             slot_count;
	bool                                 reload_requested;
	bool                                 bc6h_reported;
	int32_t                              active;
	AeronEnvironmentDebugRowsFn          debug_rows;
	void*                                debug_rows_user;
};

/* Recomputes a slot's matrix and world-space lights from package.settings. */
void env_library_apply(AeronEnvironmentLibrary* library, EnvSlot* slot);
/* Drops a slot's debug overrides. */
void env_library_reset(AeronEnvironmentLibrary* library, EnvSlot* slot);

#ifdef __cplusplus
}
#endif

#endif
