#ifndef AERON_SCENE_SPRITE_ATLAS_H
#define AERON_SCENE_SPRITE_ATLAS_H

/* Sprite atlases.
 *
 * A packed sprite atlas pairs one or more KTX2 pages with a YAML file that
 * gives each frame's pixel rect in its page:
 *
 *     units: game                # or design, the default
 *     atlas: {w: 512, h: 256}    # the size of page 0
 *     frames:
 *       - {id: 4002, page: 1, x: 2, y: 2, w: 25, h: 64, size: [6, 14], offset: [-3, -7]}
 *       - ...
 *
 * `size` is the frame's draw size and `offset` the position of its anchor,
 * in the atlas's units: the game's own units with `units: game`, else design
 * units, which the game converts. `id`, `page`, `size`, `offset` and
 * `light: [r, g, b]` are optional. The game gives the frame indices and the
 * ids their meaning. Other keys of the file belong to the game, and this
 * reader ignores them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Float pixel rect. Also aliased as the TIE shell's RemasterRect. */
typedef struct AeronSpriteRect {
	float x, y, w, h;
} AeronSpriteRect;

typedef struct AeronSpriteAtlas {
	AeronSpriteRect* frames; /* owned; one entry per frame index */
	/* Optional per-frame `id` keys: identities that the game gives the
	 * frames, such as XWA's sprite ids (4002). NULL when no frame has one.
	 * Aeron_SpriteAtlasFindById looks them up. */
	int32_t* ids;
	/* Optional per-frame `page` keys. Page 0 is the base image, and the
	 * game names the other pages, such as "<base>_p<N>.ktx2". NULL when
	 * every frame is on page 0. */
	int16_t* pages;
	/* Optional per-frame `size: [w, h]` and `offset: [x, y]`, two values
	 * per frame. A frame without the key has 0, 0. NULL when no frame has
	 * the key. */
	float* size;
	float* offset;
	/* Optional per-frame `light: [r, g, b]` keys: a colour that the game
	 * gives the frame, such as a sun's. Three floats per frame; a frame
	 * without the key has -1 in each. NULL when no frame has it. */
	float* light;
	/* `units: game`: the sizes and offsets are in the game's own units. */
	bool     game_units;
	int      page_count; /* 1 when pages == NULL */
	int      frame_count;
	int      atlas_w; /* the size of page 0 */
	int      atlas_h;
} AeronSpriteAtlas;

/* Parse `yaml_path`. On success populates *out and returns true; on
 * failure leaves *out zeroed, logs a one-line warning via Aeron_Log,
 * and returns false (callers treat false as "skip this sprite"). */
bool Aeron_SpriteAtlasLoad(AeronSpriteAtlas* out, const char* yaml_path);
bool Aeron_SpriteAtlasLoadVfs(AeronSpriteAtlas* out, AeronVfs* vfs, AeronVfsRoot root, const char* yaml_path);

/* Writes `a` to `yaml_path` in the form that the loaders read. The write
 * is atomic: it writes `<yaml_path>.tmp` and renames it over the target.
 * The rects are rounded to whole pixels. On failure returns false and
 * writes a reason into `err`, which may be NULL. */
bool Aeron_SpriteAtlasSave(const AeronSpriteAtlas* a, const char* yaml_path, char* err, size_t errsz);

void Aeron_SpriteAtlasFree(AeronSpriteAtlas* a);

/* Frame index carrying per-frame id `id`, or -1 (also -1 when the
 * atlas has no `id` keys — positional atlases don't alias ids). */
int Aeron_SpriteAtlasFindById(const AeronSpriteAtlas* a, int32_t id);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_SPRITE_ATLAS_H */
