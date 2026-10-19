/* AeronFontFace: TrueType and OpenType fonts through stb_truetype. */

#include "aeron/scene/font_face.h"

#include "aeron/log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A private copy, so it doesn't clash with other copies of stb_truetype in
 * the same program. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define FONT_ATLAS_GAP 1 /* empty pixels around each glyph, so linear filtering doesn't bleed */
#define FONT_ATLAS_MAX_SIZE 4096

struct AeronFontFace {
	uint8_t* data;
	stbtt_fontinfo info;
	float em_scale; /* em per font unit */
	float ascent, descent;
};

static uint16_t face_u16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

AeronFontFace* AeronFontFace_LoadVfs(AeronVfs* vfs, AeronVfsRoot root, const char* path, size_t maximum_file_size) {
	uint8_t* data = NULL;
	size_t size = 0;
	if (!vfs || !path || !AeronVfs_ReadAll(vfs, root, path, maximum_file_size, &data, &size))
		return NULL;
	AeronFontFace* face = calloc(1, sizeof *face);
	const int offset = stbtt_GetFontOffsetForIndex(data, 0);
	if (!face || offset < 0 || !stbtt_InitFont(&face->info, data, offset)) {
		Aeron_LogError("aeron.scene", "font: '%s' isn't a TrueType or OpenType font", path);
		free(face);
		free(data);
		return NULL;
	}
	face->data = data;
	face->em_scale = stbtt_ScaleForMappingEmToPixels(&face->info, 1.0f);

	const stbtt_uint32 os2 = stbtt__find_table(data, (stbtt_uint32)offset, "OS/2");
	if (os2 && (size_t)os2 + 78 <= size) {
		face->ascent = (float)face_u16(data + os2 + 74) * face->em_scale;
		face->descent = (float)face_u16(data + os2 + 76) * face->em_scale;
	} else {
		int ascent = 0, descent = 0, line_gap = 0;
		stbtt_GetFontVMetrics(&face->info, &ascent, &descent, &line_gap);
		face->ascent = (float)ascent * face->em_scale;
		face->descent = (float)-descent * face->em_scale;
	}
	return face;
}

void AeronFontFace_Release(AeronFontFace* face) {
	if (!face)
		return;
	free(face->data);
	free(face);
}

void AeronFontFace_VMetrics(const AeronFontFace* face, float* ascent, float* descent) {
	if (ascent)
		*ascent = face ? face->ascent : 0.0f;
	if (descent)
		*descent = face ? face->descent : 0.0f;
}

int AeronFontFace_Advance(const AeronFontFace* face, uint32_t codepoint, float* advance) {
	if (!face || codepoint > 0x10FFFF)
		return 0;
	const int glyph = stbtt_FindGlyphIndex(&face->info, (int)codepoint);
	if (!glyph)
		return 0;
	int width = 0, bearing = 0;
	stbtt_GetGlyphHMetrics(&face->info, glyph, &width, &bearing);
	if (advance)
		*advance = (float)width * face->em_scale;
	return 1;
}

typedef struct FacePacked {
	int glyph; /* glyph index, 0 when the font doesn't have the char */
	int x0, y0, w, h;
} FacePacked;

typedef struct FaceOrder {
	int h, index;
} FaceOrder;

static int face_compare_height(const void* a, const void* b) {
	const FaceOrder* pa = (const FaceOrder*)a;
	const FaceOrder* pb = (const FaceOrder*)b;
	return pa->h != pb->h ? pb->h - pa->h : pa->index - pb->index;
}

/* Places the glyphs in rows of `width` pixels, tallest first. Returns the
 * height used, or 0 when it is over the maximum. */
static int face_pack(const FacePacked* glyphs, const FaceOrder* order, int count, int width, uint16_t* xs, uint16_t* ys) {
	int x = FONT_ATLAS_GAP, y = FONT_ATLAS_GAP, row = 0;
	for (int i = 0; i < count; i++) {
		const int index = order[i].index;
		const FacePacked* g = &glyphs[index];
		if (!g->w || !g->h) {
			xs[index] = ys[index] = 0;
			continue;
		}
		if (g->w + 2 * FONT_ATLAS_GAP > width)
			return 0;
		if (x + g->w + FONT_ATLAS_GAP > width) {
			x = FONT_ATLAS_GAP;
			y += row + FONT_ATLAS_GAP;
			row = 0;
		}
		xs[index] = (uint16_t)x;
		ys[index] = (uint16_t)y;
		x += g->w + FONT_ATLAS_GAP;
		if (g->h > row)
			row = g->h;
	}
	y += row + FONT_ATLAS_GAP;
	return y <= FONT_ATLAS_MAX_SIZE ? y : 0;
}

int AeronFontFace_BuildAtlas(const AeronFontFace* face, AeronCommandBuffer* cmd, const AeronFontFaceAtlasDesc* desc,
							 AeronFontAtlas* out) {
	if (!face || !cmd || !desc || !desc->codepoints || !desc->char_count || !out || !(desc->em_pixels > 0.0f) ||
		desc->em_pixels > 1024.0f || (uint32_t)desc->first_char + desc->char_count > UINT16_MAX + 1u)
		return 0;
	const int count = desc->char_count;
	const float scale = desc->em_pixels * face->em_scale;
	FacePacked* glyphs = calloc((size_t)count, sizeof *glyphs);
	FaceOrder* order = calloc((size_t)count, sizeof *order);
	uint16_t* xs = calloc((size_t)count, sizeof *xs);
	uint16_t* ys = calloc((size_t)count, sizeof *ys);
	AeronFontGlyph* metrics = calloc((size_t)count, sizeof *metrics);
	uint8_t *coverage = NULL, *pixels = NULL;
	int result = 0, width = 0, height = 0, max_advance = 1;

	if (!glyphs || !order || !xs || !ys || !metrics)
		goto done;
	for (int i = 0; i < count; i++) {
		FacePacked* g = &glyphs[i];
		const uint32_t cp = desc->codepoints[i];
		order[i].index = i;
		g->glyph = cp <= 0x10FFFF ? stbtt_FindGlyphIndex(&face->info, (int)cp) : 0;
		if (!g->glyph)
			continue;
		int advance = 0, bearing = 0, x1 = 0, y1 = 0;
		stbtt_GetGlyphHMetrics(&face->info, g->glyph, &advance, &bearing);
		stbtt_GetGlyphBitmapBox(&face->info, g->glyph, scale, scale, &g->x0, &g->y0, &x1, &y1);
		g->w = x1 - g->x0;
		g->h = y1 - g->y0;
		if (g->w <= 0 || g->h <= 0)
			g->w = g->h = 0;
		order[i].h = g->h;
		const long advance_px = lroundf((float)advance * scale);
		metrics[i].advance = (uint16_t)(advance_px < 0 ? 0 : advance_px > UINT16_MAX ? UINT16_MAX : advance_px);
		metrics[i].offset_x = (int16_t)g->x0;
		metrics[i].offset_y = (int16_t)g->y0;
		if (metrics[i].advance > max_advance)
			max_advance = metrics[i].advance;
	}
	qsort(order, (size_t)count, sizeof *order, face_compare_height);
	for (width = 256; width <= FONT_ATLAS_MAX_SIZE; width *= 2) {
		height = face_pack(glyphs, order, count, width, xs, ys);
		if (height && height <= width)
			break;
	}
	if (!height || width > FONT_ATLAS_MAX_SIZE) {
		Aeron_LogError("aeron.scene", "font: '%s' at %.1f pixels doesn't fit in an atlas",
					   desc->debug_name ? desc->debug_name : "?", desc->em_pixels);
		goto done;
	}
	coverage = calloc((size_t)width * (size_t)height, 1);
	pixels = calloc((size_t)width * (size_t)height, 4);
	if (!coverage || !pixels)
		goto done;
	for (int i = 0; i < count; i++) {
		const FacePacked* g = &glyphs[i];
		metrics[i].atlas_x = xs[i];
		metrics[i].atlas_y = ys[i];
		metrics[i].atlas_w = (uint16_t)g->w;
		metrics[i].atlas_h = (uint16_t)g->h;
		if (g->w && g->h)
			stbtt_MakeGlyphBitmap(&face->info, coverage + (size_t)ys[i] * (size_t)width + xs[i], g->w, g->h, width,
								  scale, scale, g->glyph);
	}
	for (size_t i = 0; i < (size_t)width * (size_t)height; i++)
		memset(pixels + i * 4, coverage[i], 4);

	const long ascent = lroundf(face->ascent * desc->em_pixels);
	const long line = lroundf((face->ascent + face->descent) * desc->em_pixels);
	result = AeronFontAtlas_InitRgba8(out, cmd,
									  &(AeronFontAtlasRgba8Desc) {
										  .pixels = pixels,
										  .width = width,
										  .height = height,
										  .pitch = (size_t)width * 4,
										  .first_char = desc->first_char,
										  .cell_w = (uint16_t)max_advance,
										  .cell_h = (uint16_t)(line > ascent && line > 0 ? line : ascent + 1),
										  .baseline = (uint16_t)(ascent > 0 ? ascent : 0),
										  .glyphs = metrics,
										  .glyph_count = desc->char_count,
										  .format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
										  .color_space = AERON_COLOR_SPACE_LINEAR_SRGB,
										  .alpha_mode = AERON_IMAGE_ALPHA_PREMULTIPLIED,
										  .generate_mips = false,
										  .debug_name = desc->debug_name,
									  });
done:
	free(pixels);
	free(coverage);
	free(metrics);
	free(ys);
	free(xs);
	free(order);
	free(glyphs);
	return result;
}
