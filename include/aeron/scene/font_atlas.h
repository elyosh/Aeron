#ifndef AERON_SCENE_FONT_ATLAS_H
#define AERON_SCENE_FONT_ATLAS_H

/*
 * AeronFontAtlas — owned bitmap-font metrics and texture.
 *
 * Loads TFNT v2 metrics with PNG or KTX2 pixels, or initializes the same
 * resource from runtime-decoded RGBA8. The .fnt format is emitted by
 * tools/font_extract and cockpit_font_extract.
 *
 * Text LAYOUT stays game-side (escape opcodes, coordinate spaces,
 * shadows, backgrounds are game semantics) — games walk the glyph
 * metrics and emit sprite quads through AeronDrawList2D.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aeron/image.h"
#include "aeron/render.h"
#include "aeron/vfs.h"

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
	AeronTexture*   texture; /* owned sampled texture */
	int             atlas_w, atlas_h;
	uint16_t        first_char;
	uint16_t        num_chars;
	uint16_t        cell_w, cell_h; /* uniform cell dims from the header */
	uint16_t        baseline;
	AeronFontGlyph* glyphs; /* owned, num_chars records */
	int             loaded;
} AeronFontAtlas;

typedef struct AeronFontAtlasRgba8Desc {
	const uint8_t *pixels;
	int width, height;
	size_t pitch;
	uint16_t first_char;
	uint16_t cell_w, cell_h;
	uint16_t baseline;
	const AeronFontGlyph *glyphs;
	uint16_t glyph_count;
	AeronTextureFormat format;
	AeronColorSpace color_space;
	AeronImageAlphaMode alpha_mode;
	bool generate_mips;
	const char *debug_name;
} AeronFontAtlasRgba8Desc;

int AeronFontAtlas_InitRgba8(AeronFontAtlas *out,
							 AeronCommandBuffer *cmd,
							 const AeronFontAtlasRgba8Desc *desc);

/* Load <basename>.png + <basename>.fnt into `out` (zeroed first). The
 * texture uploads through `cmd`. */
int AeronFontAtlas_Load(AeronFontAtlas* out, AeronCommandBuffer* cmd, const char* basename);
int AeronFontAtlas_LoadVfs(AeronFontAtlas *out, AeronCommandBuffer *cmd,
		AeronVfs *vfs, AeronVfsRoot root, const char *basename,
		size_t maximum_file_size);

/* Load <basename>.ktx2 + <basename>.fnt. The compressed texture and copied
 * metrics are owned by `out` and released together. */
int AeronFontAtlas_LoadKtx2(AeronFontAtlas *out, AeronCommandBuffer *cmd,
		const char *basename);
int AeronFontAtlas_LoadKtx2Vfs(AeronFontAtlas *out,
		AeronCommandBuffer *cmd, AeronVfs *vfs, AeronVfsRoot root,
		const char *basename, size_t maximum_file_size);

/* Release the texture + metrics; `out` is zeroed. Safe on an unloaded
 * or NULL atlas. */
void AeronFontAtlas_Release(AeronFontAtlas* out);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_FONT_ATLAS_H */
