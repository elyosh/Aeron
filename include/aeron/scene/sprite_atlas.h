#ifndef AERON_SCENE_SPRITE_ATLAS_H
#define AERON_SCENE_SPRITE_ATLAS_H

/* Sprite-atlas layout consumer.
 *
 * A packed sprite atlas pairs one image (PNG/KTX2) with a YAML layout
 * file giving each frame's pixel rect within the atlas. Game tooling
 * (TIE's `filmextract --atlas`) produces matched (.png, .yaml) pairs.
 *
 * Schema (mirrors the writer):
 *
 *     atlas:
 *       w: <atlas pixel width>
 *       h: <atlas pixel height>
 *     frames:
 *       - { x: 2, y: 2, w: 25, h: 64 }
 *       - { x: 29, y: 2, w: 32, h: 64 }
 *       - ...
 *
 * Frame index semantics belong to the game (TIE: ANIM state index).
 *
 * The YAML may also carry `origin_x` / `origin_y` per frame (the
 * source frame's bbox top-left in the game's classic coords). Runtime
 * draw paths don't read them — the convention is that the emit side
 * bakes any origin offset into the draw position. The parser keeps
 * them in parallel arrays purely so round-tripping (load → mutate →
 * save) preserves the values.
 *
 * Both inline-flow `{ ... }` and block style for frame entries are
 * accepted — libyaml handles the variants transparently.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Float pixel rect. Also aliased as the TIE shell's RemasterRect. */
typedef struct AeronSpriteRect { float x, y, w, h; } AeronSpriteRect;

typedef struct AeronSpriteAtlas {
	AeronSpriteRect *frames; /* owned; one entry per frame index */
	/* Parallel arrays carrying the per-frame `origin_x`/`origin_y`
	 * keys from the YAML. Allocated alongside `frames` and the same
	 * length (`frame_count`). Defaulted to 0 when the YAML omits the
	 * keys. Not consumed by draw paths; preserved purely so
	 * Aeron_SpriteAtlasSave round-trips the on-disk file. */
	int16_t *origin_x;
	int16_t *origin_y;
	/* Optional per-frame `id` keys: sparse game-side identities (XWA
	 * DAT sprite ids are structured offsets like 4002, not dense
	 * indices). NULL when the YAML carries no `id` keys — frames are
	 * then addressed positionally, and Save emits no ids (round-trip
	 * parity for id-less corpora). Look up with
	 * Aeron_SpriteAtlasFindById. */
	int32_t *ids;
	/* Optional per-frame `page` keys: multi-image atlases for content
	 * that exceeds one texture (page 0 is the base image; the naming
	 * convention for further pages belongs to the asset producer,
	 * e.g. "<base>_p<N>.ktx2"). NULL when single-page (all frames on
	 * page 0); Save then emits no `page` keys. */
	int16_t *pages;
	/* Optional per-frame `classic_w`/`classic_h` keys: the frame's
	 * original classic-resolution pixel dims, emitted by the producer
	 * so consumers reproducing classic-geometry draws never derive a
	 * scale factor. NULL when the YAML carries none. */
	int16_t *classic_w;
	int16_t *classic_h;
	int      page_count; /* 1 when pages == NULL */
	int      frame_count;
	int      atlas_w; /* full-image dims (upscaled when extracted with --scale) */
	int      atlas_h;
	/* Original classic-resolution atlas dims, emitted by the extractor
	 * when scaling was used. Lets consumers recover the scale factor
	 * (atlas_w / classic_atlas_w) without hard-coding mode constants.
	 * Both 0 when the atlas wasn't upscaled (atlas_w/h IS classic). */
	int classic_atlas_w;
	int classic_atlas_h;
} AeronSpriteAtlas;

/* Parse `yaml_path`. On success populates *out and returns true; on
 * failure leaves *out zeroed, logs a one-line warning via Aeron_Log,
 * and returns false (callers treat false as "skip this sprite"). */
bool Aeron_SpriteAtlasLoad(AeronSpriteAtlas *out, const char *yaml_path);

/* Write `a` back to `yaml_path` in the same `atlas: { w, h, classic_w?,
 * classic_h? } / frames: [ {...} ]` shape the extractor emits. Atomic:
 * writes to `<yaml_path>.tmp` then renames over the target so a crash
 * mid-write can't truncate the source-of-truth.
 *
 * Per-frame x/y/w/h come from `frames[i]` rounded to nearest int (the
 * runtime stores them as floats; the on-disk format is integer pixel
 * coords). origin_x/origin_y are emitted from the parallel arrays.
 * `classic_w`/`classic_h` are emitted only when non-zero.
 *
 * On failure returns false and writes a NUL-terminated reason into
 * `err` (untouched on success). `err` may be NULL. */
bool Aeron_SpriteAtlasSave(const AeronSpriteAtlas *a, const char *yaml_path,
						   char *err, size_t errsz);

void Aeron_SpriteAtlasFree(AeronSpriteAtlas *a);

/* Frame index carrying per-frame id `id`, or -1 (also -1 when the
 * atlas has no `id` keys — positional atlases don't alias ids). */
int Aeron_SpriteAtlasFindById(const AeronSpriteAtlas *a, int32_t id);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_SPRITE_ATLAS_H */
