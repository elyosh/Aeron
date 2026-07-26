#ifndef AERON_SCENE_FONT_ATLAS_H
#define AERON_SCENE_FONT_ATLAS_H

/*
 * AeronFontAtlas — baked bitmap-font atlas loader.
 *
 * Loads <basename>.png (RGBA, premultiplied-alpha glyph coverage) +
 * <basename>.fnt (TFNT v2: header + per-glyph records) into a sampled
 * GPU texture + a metrics array. The .fnt format is emitted by
 * tools/font_extract and cockpit_font_extract.
 *
 * Text LAYOUT stays game-side (escape opcodes, coordinate spaces,
 * shadows, backgrounds are game semantics) — games walk the glyph
 * metrics and emit sprite quads through AeronDrawList2D.
 */

#include <stdint.h>

#include "aeron/render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-glyph metrics, atlas pixel coords. Indexed by (ch - first_char). */
typedef struct AeronFontGlyph {
	uint16_t atlas_x, atlas_y; /* top-left in atlas pixels */
	uint16_t atlas_w, atlas_h; /* sub-rect dims in atlas pixels */
	uint16_t advance;          /* horizontal advance in atlas pixels */
} AeronFontGlyph;

typedef struct AeronFontAtlas {
	AeronTexture*   texture; /* owned; sampled RGBA8 */
	int             atlas_w, atlas_h;
	uint16_t        first_char;
	uint16_t        num_chars;
	uint16_t        cell_w, cell_h; /* uniform cell dims from the header */
	uint16_t        baseline;
	AeronFontGlyph* glyphs; /* owned, num_chars records */
	int             loaded;
} AeronFontAtlas;

/* Load <basename>.png + <basename>.fnt into `out` (zeroed first). The
 * texture uploads through `cmd`. */
int AeronFontAtlas_Load(AeronFontAtlas* out, AeronCommandBuffer* cmd, const char* basename);

/* Release the texture + metrics; `out` is zeroed. Safe on an unloaded
 * or NULL atlas. */
void AeronFontAtlas_Release(AeronFontAtlas* out);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_FONT_ATLAS_H */
