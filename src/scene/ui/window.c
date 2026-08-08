/*
 * AeronUi windows and static widgets (labels, headers, help text,
 * separators, buttons). Modals share the window body via
 * ui_begin_window_common; their scope/scrim handling lives in
 * containers.c.
 */

#include "internal.h"

#define UI_DEFAULT_WINDOW_W 720.0f

int ui_begin_window_common(AeronUiContext* ctx, const char* title, const AeronUiWindowDesc* desc,
						   AeronUiId id) {
	static const AeronUiWindowDesc k_default_desc = { 0.0f, 0.0f, 0.0f, 0.0f, 1, 0 };
	if (!desc) {
		desc = &k_default_desc;
	}
	const int   has_title = !(desc->flags & AERON_UI_WINDOW_NO_TITLE);
	const float pad       = ui_ref(ctx, ctx->theme.window_pad);
	const float title_h   = has_title ? ui_ref(ctx, ctx->theme.title_height) : 0.0f;
	const float width     = ui_ref(ctx, desc->width_ref > 0.0f ? desc->width_ref : UI_DEFAULT_WINDOW_W);

	/* Height: fixed, or measured content from the previous frame (one
	 * frame of settle on first open, masked by the open fade). */
	float        height;
	UiStateSlot* slot = ui_state(ctx, id);
	if (desc->height_ref > 0.0f) {
		height = ui_ref(ctx, desc->height_ref);
	} else {
		const float content_h = slot ? slot->v0 : 0.0f;
		height                = title_h + pad * 2.0f + content_h;
	}

	UiRect rect;
	rect.w = ui_snap(width);
	rect.h = ui_snap(height);
	if (desc->centered) {
		rect.x = ui_snap(((float)ctx->out_w - rect.w) * 0.5f);
		rect.y = ui_snap(((float)ctx->out_h - rect.h) * 0.5f);
	} else {
		rect.x = ui_snap(ui_ref(ctx, desc->x_ref));
		rect.y = ui_snap(ui_ref(ctx, desc->y_ref));
	}

	ctx->any_window = 1;

	ui_draw_shadow(ctx, &rect, NULL);
	ui_draw_surface(ctx, &rect, ctx->theme.surface, NULL, 0, ctx->theme.surface_border,
					ctx->theme.window_border_px, 0, NULL);
	if (has_title) {
		const UiRect title_rect = { rect.x, rect.y, rect.w, title_h };
		ui_draw_surface(ctx, &title_rect, ctx->theme.title_bar, ctx->theme.title_bar_low,
						ctx->theme.title_gradient, NULL, 0.0f, 1, NULL);
		const float title_px = ui_ref(ctx, ctx->theme.title_px);
		ui_draw_text(ctx, ui_font_title(ctx), rect.x + rect.w * 0.5f, rect.y + (title_h - title_px) * 0.5f,
					 AERON_TEXT_CENTER, title_px, ctx->theme.title_text, ui_label_text(title),
					 ui_label_text_len(title), NULL);
	}

	UiLayout layout = { 0 };
	layout.kind     = UI_LAYOUT_WINDOW;
	layout.x        = rect.x + pad;
	layout.w        = rect.w - pad * 2.0f;
	layout.cursor_y = rect.y + title_h + pad;
	layout.limit_y  = rect.y + rect.h - pad;
	layout.bounded  = desc->height_ref > 0.0f;
	layout.owner_id = id;
	layout.view_y   = layout.cursor_y;
	ui_layout_push(ctx, &layout);
	ctx->window_depth++;
	return 1;
}

void ui_end_window_common(AeronUiContext* ctx) {
	UiLayout* top = ui_layout_top(ctx);
	if (!top || top->kind != UI_LAYOUT_WINDOW || ctx->window_depth <= 0) {
		Aeron_LogWarn("aeron.scene", "unbalanced AeronUi_EndWindow");
		return;
	}
	if (top->columns > 0) {
		AeronUi_EndColumns(ctx);
	}
	/* Record the content height for next frame's auto-size, without the
	 * trailing item spacing. */
	UiStateSlot* slot = ui_state(ctx, top->owner_id);
	if (slot) {
		float content_h = top->cursor_y - top->view_y - ui_ref(ctx, ctx->theme.item_spacing);
		slot->v0        = fmaxf(content_h, 0.0f);
	}
	ui_layout_pop(ctx);
	ctx->window_depth--;
}

int AeronUi_BeginWindow(AeronUiContext* ctx, const char* title, const AeronUiWindowDesc* desc) {
	if (!ctx || !ctx->frame_active) {
		return 0;
	}
	return ui_begin_window_common(ctx, title, desc, ui_make_id(ctx, title));
}

void AeronUi_EndWindow(AeronUiContext* ctx) {
	if (!ctx || !ctx->frame_active) {
		return;
	}
	ui_end_window_common(ctx);
}

/* ============================ static widgets ============================ */

void AeronUi_Label(AeronUiContext* ctx, const char* text) {
	UiRect      rect;
	const float text_px = ctx ? ui_ref(ctx, ctx->theme.text_px) : 0.0f;
	if (!ctx || !ctx->frame_active ||
		!ui_layout_row(ctx, fmaxf(ui_ref(ctx, ctx->theme.row_height), text_px), &rect)) {
		return;
	}
	const UiLayout*   top  = ui_layout_top(ctx);
	const AeronRectI* clip = (top && top->clip.width > 0) ? &top->clip : NULL;
	ui_draw_text(ctx, ui_font_regular(ctx), rect.x, rect.y + (rect.h - text_px) * 0.5f, AERON_TEXT_LEFT,
				 text_px, ctx->theme.text, ui_label_text(text), ui_label_text_len(text), clip);
}

void AeronUi_Header(AeronUiContext* ctx, const char* text) {
	UiRect      rect;
	const float text_px = ctx ? ui_ref(ctx, ctx->theme.text_px) : 0.0f;
	if (!ctx || !ctx->frame_active ||
		!ui_layout_row(ctx, fmaxf(ui_ref(ctx, ctx->theme.row_height), text_px), &rect)) {
		return;
	}
	const UiLayout*   top  = ui_layout_top(ctx);
	const AeronRectI* clip = (top && top->clip.width > 0) ? &top->clip : NULL;
	ui_draw_text(ctx, ui_font_title(ctx), rect.x, rect.y + (rect.h - text_px) * 0.5f, AERON_TEXT_LEFT,
				 text_px, ctx->theme.accent, ui_label_text(text), ui_label_text_len(text), clip);
	/* Accent underline reinforces the section break. */
	const UiRect line = { rect.x, rect.y + rect.h - 1.0f, rect.w, 1.0f };
	ui_draw_fill(ctx, &line, ctx->theme.separator, clip);
}

static float ui_help_height_px(const AeronUiContext* ctx, const char* text, float width) {
	const AeronFontAtlas* font = ctx ? ui_font_regular(ctx) : NULL;
	if (!font || font->cell_h == 0 || width <= 0.0f) {
		return 0.0f;
	}
	const float    help_px = ui_ref(ctx, ctx->theme.help_px);
	AeronTextStyle style   = {
		.font        = font,
		.scale       = help_px / (float)font->cell_h,
		.tracking_px = ui_font_tracking_atlas(ctx, font) * help_px / (float)font->cell_h,
	};
	AeronTextLine lines[16];
	const int     count = AeronText_Wrap(&style, ui_label_text(text), ui_label_text_len(text), width, lines,
										 (int)(sizeof lines / sizeof lines[0]));
	return AeronText_LineHeight(&style) * (float)count;
}

float AeronUi_MeasureHelpHeight(AeronUiContext* ctx, const char* text, float width_ref) {
	if (!ctx || !ctx->frame_active || ctx->scale <= 0.0f) {
		return 0.0f;
	}
	float width;
	if (width_ref > 0.0f) {
		width = ui_ref(ctx, width_ref);
	} else {
		const UiLayout* layout = ui_layout_top(ctx);
		if (!layout)
			return 0.0f;
		width = layout->columns > 0 ? layout->col_w[layout->col_index] : layout->w;
	}
	return ui_help_height_px(ctx, text, width) / ctx->scale;
}

static void ui_help_colored(AeronUiContext* ctx, const char* text, const AeronUiColor color) {
	if (!ctx || !ctx->frame_active) {
		return;
	}
	UiLayout* top = ui_layout_top(ctx);
	if (!top) {
		return;
	}
	const float help_px = ui_ref(ctx, ctx->theme.help_px);
	const float width   = top->columns > 0 ? top->col_w[top->col_index] : top->w;

	AeronTextStyle style = { 0 };
	style.font           = ui_font_regular(ctx);
	if (!style.font || style.font->cell_h == 0) {
		return;
	}
	style.scale       = help_px / (float)style.font->cell_h;
	style.tracking_px = ui_font_tracking_atlas(ctx, style.font) * style.scale;

	AeronTextLine lines[16];
	const int     text_len = ui_label_text_len(text);
	const int     count    = AeronText_Wrap(&style, ui_label_text(text), text_len, width, lines, 16);
	const float   line_h   = AeronText_LineHeight(&style);

	UiRect rect;
	if (!ui_layout_row(ctx, line_h * (float)count, &rect)) {
		return;
	}
	const AeronRectI* clip = (top->clip.width > 0) ? &top->clip : NULL;
	for (int i = 0; i < count; i++) {
		ui_draw_text(ctx, style.font, rect.x, rect.y + line_h * (float)i, AERON_TEXT_LEFT, help_px, color,
					 ui_label_text(text) + lines[i].start, lines[i].length, clip);
	}
}

void AeronUi_Help(AeronUiContext* ctx, const char* text) {
	ui_help_colored(ctx, text, ctx ? ctx->theme.text_dim : NULL);
}

void AeronUi_Error(AeronUiContext* ctx, const char* text) {
	ui_help_colored(ctx, text, ctx ? ctx->theme.accent : NULL);
}

void AeronUi_Separator(AeronUiContext* ctx) {
	UiRect rect;
	if (!ctx || !ctx->frame_active || !ui_layout_row(ctx, ui_ref(ctx, 9.0f), &rect)) {
		return;
	}
	const UiLayout*   top  = ui_layout_top(ctx);
	const AeronRectI* clip = (top && top->clip.width > 0) ? &top->clip : NULL;
	const UiRect      line = { rect.x, rect.y + rect.h * 0.5f, rect.w, fmaxf(1.0f, ui_ref(ctx, 1.0f)) };
	ui_draw_fill(ctx, &line, ctx->theme.separator, clip);
}

void AeronUi_Spacer(AeronUiContext* ctx, float height_ref) {
	UiRect rect;
	if (!ctx || !ctx->frame_active || height_ref <= 0.0f) {
		return;
	}
	ui_layout_row(ctx, ui_ref(ctx, height_ref), &rect);
}

int AeronUi_ButtonEnabled(AeronUiContext* ctx, const char* label, int enabled) {
	UiRect rect;
	if (!ctx || !ctx->frame_active || !ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &rect)) {
		return 0;
	}
	const AeronUiId id        = ui_make_id(ctx, label);
	const int       activated = enabled && ui_widget_behavior(ctx, id, &rect, 1);
	if (!enabled) {
		ui_record_widget(ctx, id, &rect, 0);
	}
	if (activated) {
		ui_play_sound(ctx, AERON_UI_SOUND_ACCEPT);
	}
	const UiLayout*   top  = ui_layout_top(ctx);
	const AeronRectI* clip = (top && top->clip.width > 0) ? &top->clip : NULL;

	const int    hot     = enabled && (ui_is_focused(ctx, id) || ctx->hot_id == id);
	const int    pressed = enabled && ctx->active_id == id && ctx->hot_id == id;
	const float* bg      = !enabled  ? ctx->theme.widget_bg_low
						   : pressed ? ctx->theme.widget_bg_low
									 : (hot ? ctx->theme.widget_bg_hot : ctx->theme.widget_bg);
	ui_draw_surface(ctx, &rect, bg, ctx->theme.widget_bg_low,
					enabled && ctx->theme.widget_gradient && !pressed, ctx->theme.widget_border,
					ctx->theme.widget_border_px, !pressed, clip);
	if (enabled && ui_is_focused(ctx, id)) {
		ui_draw_focus_outline(ctx, &rect, clip);
	}

	const float text_px = ui_ref(ctx, ctx->theme.text_px);
	ui_draw_text(ctx, ui_font_regular(ctx), rect.x + rect.w * 0.5f, rect.y + (rect.h - text_px) * 0.5f,
				 AERON_TEXT_CENTER, text_px, enabled ? ctx->theme.text : ctx->theme.text_dim,
				 ui_label_text(label), ui_label_text_len(label), clip);
	return activated;
}

int AeronUi_Button(AeronUiContext* ctx, const char* label) { return AeronUi_ButtonEnabled(ctx, label, 1); }
