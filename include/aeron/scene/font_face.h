#ifndef AERON_SCENE_FONT_FACE_H
#define AERON_SCENE_FONT_FACE_H

/*
 * AeronFontFace: a TrueType or OpenType font read through the VFS.
 *
 * It gives the font's metrics in em units and builds an AeronFontAtlas at a
 * given pixel size. A font collection uses its first font.
 */

#include <stddef.h>
#include <stdint.h>

#include "aeron/render.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronFontFace AeronFontFace;

/* Returns NULL when the file can't be read or isn't a font. */
AeronFontFace* AeronFontFace_LoadVfs(AeronVfs* vfs, AeronVfsRoot root, const char* path, size_t maximum_file_size);
void AeronFontFace_Release(AeronFontFace* face);

/* Ascent and descent in em, both positive. They come from the OS/2 table's
 * usWinAscent and usWinDescent, which Windows' GDI uses, or from the hhea
 * table when the font has no OS/2 table. */
void AeronFontFace_VMetrics(const AeronFontFace* face, float* ascent, float* descent);
/* Sets *advance to the advance width of `codepoint` in em. Returns 0, and
 * leaves *advance unchanged, when the font has no glyph for it. */
int AeronFontFace_Advance(const AeronFontFace* face, uint32_t codepoint, float* advance);

typedef struct AeronFontFaceAtlasDesc {
	float em_pixels; /* the em size in pixels */
	uint16_t first_char;
	uint16_t char_count;
	const uint32_t* codepoints; /* the code point of each char, char_count entries */
	const char* debug_name;
} AeronFontFaceAtlasDesc;

/* Rasterizes the chars and uploads them through `cmd` into `out`, which
 * must be empty. The texture holds premultiplied white. Each glyph's advance
 * is in atlas pixels, and offset_x and offset_y place its rectangle from the
 * pen position on the baseline. A char the font doesn't have gets an empty
 * rectangle. The atlas's baseline is the ascent in pixels, and its cell is
 * the line height. */
int AeronFontFace_BuildAtlas(const AeronFontFace* face, AeronCommandBuffer* cmd, const AeronFontFaceAtlasDesc* desc,
							 AeronFontAtlas* out);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_FONT_FACE_H */
