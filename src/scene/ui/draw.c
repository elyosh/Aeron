/*
 * AeronUi drawing helpers — theme colors to PMA draw-list records.
 * Everything renders through rounded-rect instances and AeronText glyph
 * sprites in content-pixel space.
 */

#include "internal.h"

void ui_pma(const AeronUiContext* ctx, const AeronUiColor color, float out[4]) {
	const float a = color[3] * ctx->fade;
	out[0]        = color[0] * a;
	out[1]        = color[1] * a;
	out[2]        = color[2] * a;
	out[3]        = a;
}

void ui_draw_surface(AeronUiContext* ctx, const UiRect* rect, const AeronUiColor bg,
					 const AeronUiColor bg_low, int gradient, const AeronUiColor border_color,
					 float border_px, int bevel, const AeronRectI* clip) {
	AeronDrawList2DRRect r = { 0 };

	r.dst_x     = ui_snap(rect->x);
	r.dst_y     = ui_snap(rect->y);
	r.dst_w     = ui_snap(rect->w);
	r.dst_h     = ui_snap(rect->h);
	r.radius_px = ui_ref(ctx, ctx->theme.corner_radius);
	r.soft_px   = 1.0f;
	ui_pma(ctx, bg, r.fill_top);
	if (gradient && bg_low) {
		ui_pma(ctx, bg_low, r.fill_bottom);
	} else {
		memcpy(r.fill_bottom, r.fill_top, sizeof r.fill_bottom);
	}
	if (border_color && border_px > 0.0f) {
		r.border_px = fmaxf(1.0f, ui_snap(ui_ref(ctx, border_px)));
		ui_pma(ctx, border_color, r.border);
	}
	if (bevel && ctx->theme.bevel_px > 0.0f) {
		r.bevel_px = fmaxf(1.0f, ui_snap(ui_ref(ctx, ctx->theme.bevel_px)));
		ui_pma(ctx, ctx->theme.bevel_hi, r.bevel_hi);
		ui_pma(ctx, ctx->theme.bevel_lo, r.bevel_lo);
	}
	if (clip) {
		r.scissor = *clip;
	}
	AeronDrawList_AddRRect(ctx->list, &r);
}

void ui_draw_fill(AeronUiContext* ctx, const UiRect* rect, const AeronUiColor color, const AeronRectI* clip) {
	float pma[4];
	/* AddFill lazily creates the shared white GPU texture — never reach
	 * it in offline (headless) mode. */
	if (ctx->offline) {
		return;
	}
	ui_pma(ctx, color, pma);
	AeronDrawList_AddFill(ctx->list, ui_snap(rect->x), ui_snap(rect->y), ui_snap(rect->w), ui_snap(rect->h),
						  pma, AERON_BLIT2D_BLEND_PMA, clip);
}

void ui_draw_focus_outline(AeronUiContext* ctx, const UiRect* rect, const AeronRectI* clip) {
	AeronDrawList2DRRect r      = { 0 };
	const float          expand = fmaxf(1.0f, ui_snap(ui_ref(ctx, ctx->theme.focus_px)));

	r.dst_x     = ui_snap(rect->x) - expand;
	r.dst_y     = ui_snap(rect->y) - expand;
	r.dst_w     = ui_snap(rect->w) + 2.0f * expand;
	r.dst_h     = ui_snap(rect->h) + 2.0f * expand;
	r.radius_px = ui_ref(ctx, ctx->theme.corner_radius) + expand;
	r.border_px = expand;
	r.soft_px   = 1.0f;
	ui_pma(ctx, ctx->theme.focus_outline, r.border);
	if (clip) {
		r.scissor = *clip;
	}
	AeronDrawList_AddRRect(ctx->list, &r);
}

/* Focused form-row treatment: a subtle highlight bar behind the whole
 * row (drawn before the row content) plus a focus ring drawn after the
 * content so it cleanly surrounds full-height controls instead of
 * disappearing behind them. The rect matches the row bounds exactly —
 * every control right-aligns to the row edge, so the ring meets the
 * selector/rebind body edge with no gap. */
static AeronDrawList2DRRect ui_row_focus_rect(AeronUiContext* ctx, const UiRect* row,
											  const AeronRectI* clip) {
	AeronDrawList2DRRect r = { 0 };

	r.dst_x     = ui_snap(row->x);
	r.dst_y     = ui_snap(row->y);
	r.dst_w     = ui_snap(row->w);
	r.dst_h     = ui_snap(row->h);
	r.radius_px = ui_ref(ctx, ctx->theme.corner_radius);
	r.soft_px   = 1.0f;
	if (clip) {
		r.scissor = *clip;
	}
	return r;
}

void ui_draw_row_focus_bg(AeronUiContext* ctx, const UiRect* row, const AeronRectI* clip) {
	AeronDrawList2DRRect r = ui_row_focus_rect(ctx, row, clip);
	ui_pma(ctx, ctx->theme.row_highlight, r.fill_top);
	memcpy(r.fill_bottom, r.fill_top, sizeof r.fill_bottom);
	AeronDrawList_AddRRect(ctx->list, &r);
}

void ui_draw_row_focus_ring(AeronUiContext* ctx, const UiRect* row, const AeronRectI* clip) {
	AeronDrawList2DRRect r = ui_row_focus_rect(ctx, row, clip);

	/* The border band renders inward from the row bounds — it only ever
	 * overlaps the highlight bar's own margin, because form-row content
	 * is inset from the row edges (ui_form_row_split). */
	r.border_px = fmaxf(1.0f, ui_snap(ui_ref(ctx, ctx->theme.focus_px)));
	ui_pma(ctx, ctx->theme.focus_outline, r.border);
	AeronDrawList_AddRRect(ctx->list, &r);
}

void ui_draw_shadow(AeronUiContext* ctx, const UiRect* rect, const AeronRectI* clip) {
	AeronDrawList2DRRect r    = { 0 };
	const float          soft = ui_ref(ctx, ctx->theme.shadow_soft_px);

	if (soft <= 0.0f) {
		return;
	}
	r.dst_x     = ui_snap(rect->x);
	r.dst_y     = ui_snap(rect->y) + soft * 0.25f;
	r.dst_w     = ui_snap(rect->w);
	r.dst_h     = ui_snap(rect->h);
	r.radius_px = ui_ref(ctx, ctx->theme.corner_radius) + soft * 0.25f;
	r.soft_px   = soft;
	ui_pma(ctx, ctx->theme.window_shadow, r.fill_top);
	memcpy(r.fill_bottom, r.fill_top, sizeof r.fill_bottom);
	if (clip) {
		r.scissor = *clip;
	}
	AeronDrawList_AddRRect(ctx->list, &r);
}

/* Style for one themed text draw. Glyph scale maps the atlas cell
 * height onto the wanted pixel size. */
static AeronTextStyle ui_text_style(const AeronUiContext* ctx, const AeronFontAtlas* font, float size_px,
									const AeronUiColor color) {
	AeronTextStyle style = { 0 };
	if (!font || font->cell_h == 0) {
		return style;
	}
	style.font        = font;
	style.scale       = size_px / (float)font->cell_h;
	style.tracking_px = ui_font_tracking_atlas(ctx, font) * style.scale;
	style.filter      = AERON_BLIT2D_FILTER_LINEAR;
	style.tint[0] = color[0];
	style.tint[1] = color[1];
	style.tint[2] = color[2];
	style.tint[3] = color[3] * ctx->fade;
	return style;
}

void ui_draw_text(AeronUiContext* ctx, const AeronFontAtlas* font, float x, float y, AeronTextAlign align,
				  float size_px, const AeronUiColor color, const char* text, int len,
				  const AeronRectI* clip) {
	const AeronTextStyle style = ui_text_style(ctx, font, size_px, color);
	if (!style.font) {
		return;
	}
	AeronText_Draw(ctx->list, &style, x, y, align, text, len, clip);
}

float ui_text_width(const AeronUiContext* ctx, const AeronFontAtlas* font, float size_px, const char* text,
					int len) {
	const AeronTextStyle style = ui_text_style(ctx, font, size_px, (const float*)ctx->theme.text);
	if (!style.font) {
		return 0.0f;
	}
	return AeronText_MeasureWidth(&style, text, len);
}
