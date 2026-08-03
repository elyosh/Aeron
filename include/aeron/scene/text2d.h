#ifndef AERON_SCENE_TEXT2D_H
#define AERON_SCENE_TEXT2D_H

/*
 * AeronText — minimal single-line text layout over AeronFontAtlas.
 *
 * Stateless CPU walk of the baked glyph metrics emitting sprite quads
 * into an AeronDrawList2D (the same technique game text renderers use;
 * this module exists so engine-level UI and future games don't rebuild
 * it). Measurement and wrapping are pure functions over the metrics
 * array — no GPU dependency, so they are headless-testable.
 *
 * Glyph model (TFNT): uniform cells top-aligned on the line, pen
 * advances by `advance`; no per-glyph bearings. Input is UTF-8;
 * codepoints outside the atlas range render the '?' fallback when the
 * atlas has one, else they are skipped.
 */

#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/font_atlas.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AeronTextAlign {
	AERON_TEXT_LEFT   = 0,
	AERON_TEXT_CENTER = 1,
	AERON_TEXT_RIGHT  = 2,
} AeronTextAlign;

/* `scale` maps atlas pixels to target pixels (wanted_px / bake_px).
 * `tint` and `shadow` are STRAIGHT-alpha colors — premultiplication for
 * the PMA glyph blend happens internally. shadow[3] <= 0 disables the
 * shadow pass. `tracking_px` is extra advance per glyph in target px. */
typedef struct AeronTextStyle {
	const AeronFontAtlas* font;
	float                 scale;
	float                 tint[4];
	float                 shadow[4];
	float                 tracking_px;
	AeronBlit2DFilter     filter;
} AeronTextStyle;

/* Width of one line in target pixels (no wrapping; '\n' measures as a
 * regular missing glyph). len < 0 = NUL-terminated. */
float AeronText_MeasureWidth(const AeronTextStyle* style, const char* utf8, int len);

/* Line height (font cell height) in target pixels. */
float AeronText_LineHeight(const AeronTextStyle* style);

/* Draws one line. `x` is the anchor for `align`; `y` is the TOP of the
 * line. Glyph positions snap to whole target pixels for crispness. When
 * the style has a shadow, a shadow pass is drawn first, offset down-right
 * by max(1, cell_h*scale/16) px. */
void AeronText_Draw(AeronDrawList2D* list, const AeronTextStyle* style, float x, float y,
					AeronTextAlign align, const char* utf8, int len, const AeronRectI* scissor);

/* One wrapped line: byte range into the source string + measured width. */
typedef struct AeronTextLine {
	int   start;
	int   length;
	float width_px;
} AeronTextLine;

/* Greedy word wrap at spaces ('\n' forces a break; overlong words hard-
 * break at glyph granularity). Returns the line count, capped at
 * max_lines with a once-per-call warning on overflow. */
int AeronText_Wrap(const AeronTextStyle* style, const char* utf8, int len, float max_width_px,
				   AeronTextLine* out_lines, int max_lines);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_TEXT2D_H */
