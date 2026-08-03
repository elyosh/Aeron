/*
 * AeronText — single-line text layout over AeronFontAtlas.
 * See aeron/scene/text2d.h for the glyph model and contracts.
 */

#include "aeron/scene/text2d.h"

#include "aeron/log.h"

#include <math.h>
#include <string.h>

/* Decodes the next UTF-8 codepoint at *i, advancing it. Malformed bytes
 * decode as one replacement codepoint each so the walk always makes
 * progress. */
static uint32_t text2d_next_cp(const char* s, int len, int* i) {
	const uint8_t* u     = (const uint8_t*)s;
	uint8_t        first = u[*i];
	int            extra;
	uint32_t       cp;

	(*i)++;
	if (first < 0x80u) {
		return first;
	}
	if ((first & 0xE0u) == 0xC0u) {
		extra = 1;
		cp    = first & 0x1Fu;
	} else if ((first & 0xF0u) == 0xE0u) {
		extra = 2;
		cp    = first & 0x0Fu;
	} else if ((first & 0xF8u) == 0xF0u) {
		extra = 3;
		cp    = first & 0x07u;
	} else {
		return 0xFFFDu;
	}
	for (int k = 0; k < extra; k++) {
		if (*i >= len || (u[*i] & 0xC0u) != 0x80u) {
			return 0xFFFDu;
		}
		cp = (cp << 6) | (u[*i] & 0x3Fu);
		(*i)++;
	}
	return cp;
}

/* Glyph for a codepoint, or the '?' fallback, or NULL (skip). */
static const AeronFontGlyph* text2d_glyph(const AeronFontAtlas* font, uint32_t cp) {
	uint32_t index;

	if (cp >= font->first_char && cp < (uint32_t)font->first_char + font->num_chars) {
		return &font->glyphs[cp - font->first_char];
	}
	index = (uint32_t)'?';
	if (index >= font->first_char && index < (uint32_t)font->first_char + font->num_chars) {
		return &font->glyphs[index - font->first_char];
	}
	return NULL;
}

static int text2d_style_valid(const AeronTextStyle* style) {
	return style && style->font && style->font->glyphs && style->font->num_chars > 0 && style->scale > 0.0f;
}

static int text2d_len(const char* utf8, int len) {
	if (!utf8) {
		return 0;
	}
	return len < 0 ? (int)strlen(utf8) : len;
}

float AeronText_MeasureWidth(const AeronTextStyle* style, const char* utf8, int len) {
	float width = 0.0f;

	if (!text2d_style_valid(style)) {
		return 0.0f;
	}
	len = text2d_len(utf8, len);
	for (int i = 0; i < len;) {
		const AeronFontGlyph* g = text2d_glyph(style->font, text2d_next_cp(utf8, len, &i));
		if (!g || g->advance == 0) {
			continue;
		}
		width += (float)g->advance * style->scale + style->tracking_px;
	}
	return width;
}

float AeronText_LineHeight(const AeronTextStyle* style) {
	if (!text2d_style_valid(style)) {
		return 0.0f;
	}
	return (float)style->font->cell_h * style->scale;
}

/* One layout pass: emits glyph sprites tinted with the given PMA color
 * at (x, y) + offset. */
static void text2d_emit(AeronDrawList2D* list, const AeronTextStyle* style, float x, float y,
						const float tint_pma[4], const char* utf8, int len, const AeronRectI* scissor) {
	const AeronFontAtlas* font  = style->font;
	const float           inv_w = 1.0f / (float)(font->atlas_w > 0 ? font->atlas_w : 1);
	const float           inv_h = 1.0f / (float)(font->atlas_h > 0 ? font->atlas_h : 1);
	float                 pen   = x;

	for (int i = 0; i < len;) {
		const AeronFontGlyph* g = text2d_glyph(font, text2d_next_cp(utf8, len, &i));
		if (!g || g->advance == 0) {
			continue;
		}
		if (g->atlas_w > 0 && g->atlas_h > 0) {
			AeronDrawList2DSprite s = { 0 };
			s.texture               = font->texture;
			s.src_u0                = (float)g->atlas_x * inv_w;
			s.src_v0                = (float)g->atlas_y * inv_h;
			s.src_u1                = (float)(g->atlas_x + g->atlas_w) * inv_w;
			s.src_v1                = (float)(g->atlas_y + g->atlas_h) * inv_h;
			/* Whole-pixel snap keeps 1:1-ish downscales crisp. */
			s.dst_x = floorf(pen + 0.5f);
			s.dst_y = floorf(y + 0.5f);
			s.dst_w = (float)g->atlas_w * style->scale;
			s.dst_h = (float)g->atlas_h * style->scale;
			memcpy(s.tint, tint_pma, sizeof s.tint);
			s.blend  = AERON_BLIT2D_BLEND_PMA;
			s.filter = style->filter;
			if (scissor) {
				s.scissor = *scissor;
			}
			AeronDrawList_AddSprite(list, &s);
		}
		pen += (float)g->advance * style->scale + style->tracking_px;
	}
}

void AeronText_Draw(AeronDrawList2D* list, const AeronTextStyle* style, float x, float y,
					AeronTextAlign align, const char* utf8, int len, const AeronRectI* scissor) {
	float pma[4];

	if (!list || !text2d_style_valid(style) || !style->font->texture) {
		return;
	}
	len = text2d_len(utf8, len);
	if (len <= 0) {
		return;
	}
	if (align != AERON_TEXT_LEFT) {
		const float width = AeronText_MeasureWidth(style, utf8, len);
		x -= (align == AERON_TEXT_CENTER) ? width * 0.5f : width;
	}
	if (style->shadow[3] > 0.0f) {
		const float offset = fmaxf(1.0f, floorf(AeronText_LineHeight(style) / 16.0f + 0.5f));
		pma[0]             = style->shadow[0] * style->shadow[3];
		pma[1]             = style->shadow[1] * style->shadow[3];
		pma[2]             = style->shadow[2] * style->shadow[3];
		pma[3]             = style->shadow[3];
		text2d_emit(list, style, x + offset, y + offset, pma, utf8, len, scissor);
	}
	pma[0] = style->tint[0] * style->tint[3];
	pma[1] = style->tint[1] * style->tint[3];
	pma[2] = style->tint[2] * style->tint[3];
	pma[3] = style->tint[3];
	text2d_emit(list, style, x, y, pma, utf8, len, scissor);
}

/* Advance width of the single codepoint starting at byte `i`. */
static float text2d_cp_advance(const AeronTextStyle* style, const char* utf8, int len, int* i) {
	const AeronFontGlyph* g = text2d_glyph(style->font, text2d_next_cp(utf8, len, i));
	if (!g || g->advance == 0) {
		return 0.0f;
	}
	return (float)g->advance * style->scale + style->tracking_px;
}

int AeronText_Wrap(const AeronTextStyle* style, const char* utf8, int len, float max_width_px,
				   AeronTextLine* out_lines, int max_lines) {
	int   count      = 0;
	int   line_start = 0;
	float line_width = 0.0f;
	/* End/width of the line's last word — flushes trim trailing spaces. */
	int   content_end   = 0;
	float content_width = 0.0f;
	int   overflowed    = 0;

	if (!text2d_style_valid(style) || !out_lines || max_lines <= 0) {
		return 0;
	}
	len = text2d_len(utf8, len);

	for (int i = 0; i < len;) {
		if (utf8[i] == '\n') {
			if (count < max_lines) {
				out_lines[count++] = (AeronTextLine) { line_start, content_end - line_start, content_width };
			} else {
				overflowed = 1;
			}
			i++;
			line_start    = i;
			line_width    = 0.0f;
			content_end   = i;
			content_width = 0.0f;
			continue;
		}

		/* Measure the next token: a run of spaces, or one word. */
		int   token_start = i;
		float token_width = 0.0f;
		if (utf8[i] == ' ') {
			while (i < len && utf8[i] == ' ') {
				int j = i;
				token_width += text2d_cp_advance(style, utf8, len, &j);
				i = j;
			}
		} else {
			while (i < len && utf8[i] != ' ' && utf8[i] != '\n') {
				int   j = i;
				float a = text2d_cp_advance(style, utf8, len, &j);
				/* Hard-break an overlong word at glyph granularity,
				 * keeping at least one glyph per line. */
				if (i > token_start && line_width + token_width + a > max_width_px &&
					token_start == line_start) {
					break;
				}
				token_width += a;
				i = j;
			}
		}

		if (line_width + token_width > max_width_px && line_start < token_start) {
			/* Token doesn't fit: close the current line before it,
			 * trimmed of trailing spaces. */
			if (count < max_lines) {
				out_lines[count++] = (AeronTextLine) { line_start, content_end - line_start, content_width };
			} else {
				overflowed = 1;
			}
			if (utf8[token_start] == ' ') {
				/* A wrapped space run is consumed by the break itself. */
				line_start = i;
			} else {
				/* Re-scan the word at line start so the hard-break rule
				 * for overlong words applies to it. */
				i          = token_start;
				line_start = token_start;
			}
			line_width    = 0.0f;
			content_end   = line_start;
			content_width = 0.0f;
			continue;
		}
		line_width += token_width;
		if (utf8[token_start] != ' ') {
			content_end   = i;
			content_width = line_width;
		}
	}
	if (content_end > line_start || count == 0) {
		if (count < max_lines) {
			out_lines[count++] = (AeronTextLine) { line_start, content_end - line_start, content_width };
		} else {
			overflowed = 1;
		}
	}
	if (overflowed) {
		Aeron_LogWarn("aeron.scene", "text wrap line cap (%d) hit; dropping", max_lines);
	}
	return count;
}
