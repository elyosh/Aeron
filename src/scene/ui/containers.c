/*
 * AeronUi containers — tab bar, scroll region, modal dialog.
 */

#include "internal.h"

/* ============================== tab bar ============================== */

int AeronUi_BeginTabBar(AeronUiContext* ctx, const char* id, const char* const* titles, int count,
						int* active) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !titles || !active || count <= 0 ||
		!ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row)) {
		return 0;
	}
	(void)id;
	const AeronRectI* clip = NULL;
	{
		const UiLayout* top = ui_layout_top(ctx);
		clip                = (top && top->clip.width > 0) ? &top->clip : NULL;
	}

	int page = *active < 0 ? 0 : (*active >= count ? count - 1 : *active);

	/* Q/E and shoulder buttons cycle pages with wrap; the first tab bar
	 * of the frame consumes the intent. */
	const int cycle   = ctx->nav_tab_next - ctx->nav_tab_prev;
	ctx->nav_tab_next = 0;
	ctx->nav_tab_prev = 0;
	if (cycle != 0) {
		page = (page + cycle) % count;
		if (page < 0) {
			page += count;
		}
		ui_play_sound(ctx, AERON_UI_SOUND_FOCUS);
	}

	const int   interactive = ctx->scope_depth >= ctx->top_scope_prev;
	const float tab_w       = row.w / (float)count;
	const float text_px     = ui_ref(ctx, ctx->theme.text_px);

	for (int i = 0; i < count; i++) {
		const UiRect tab     = { ui_snap(row.x + tab_w * (float)i), row.y, ui_snap(tab_w), row.h };
		const int    hovered = interactive && ui_mouse_in(ctx, &tab, clip);
		if (hovered && (ctx->input->mouse.pressed_buttons & AERON_MOUSE_BUTTON_LEFT) && page != i) {
			page = i;
			ui_play_sound(ctx, AERON_UI_SOUND_FOCUS);
		}
		const int selected = page == i;
		ui_draw_surface(ctx, &tab, selected ? ctx->theme.widget_bg_hot : ctx->theme.widget_bg,
						ctx->theme.widget_bg_low, ctx->theme.widget_gradient && !selected,
						ctx->theme.widget_border, ctx->theme.widget_border_px, selected, clip);
		if (selected) {
			/* Accent strip marks the active page. */
			const UiRect strip = { tab.x, tab.y + tab.h - 3.0f, tab.w, 3.0f };
			ui_draw_fill(ctx, &strip, ctx->theme.accent, clip);
		}
		ui_draw_text(ctx, ui_font_regular(ctx), tab.x + tab.w * 0.5f, tab.y + (tab.h - text_px) * 0.5f,
					 AERON_TEXT_CENTER, text_px, selected ? ctx->theme.text : ctx->theme.text_dim,
					 ui_label_text(titles[i]), ui_label_text_len(titles[i]), clip);
	}

	*active = page;
	return 1;
}

void AeronUi_EndTabBar(AeronUiContext* ctx) {
	if (ctx && ctx->frame_active) {
		AeronUi_Spacer(ctx, 4.0f);
	}
}

/* ============================ scroll region ============================ */

#define UI_WHEEL_STEP_REF 60.0f

int AeronUi_BeginScroll(AeronUiContext* ctx, const char* id, float height_ref) {
	UiRect view;
	if (!ctx || !ctx->frame_active || height_ref <= 0.0f ||
		!ui_layout_row(ctx, ui_ref(ctx, height_ref), &view)) {
		return 0;
	}
	const AeronUiId scroll_id = ui_make_id(ctx, id);
	UiStateSlot*    slot      = ui_state(ctx, scroll_id);
	float           offset    = slot ? slot->v0 : 0.0f;
	const float     content_h = slot ? slot->v1 : 0.0f; /* previous frame */

	/* Wheel scrolls while hovering the view. */
	const UiLayout*   parent      = ui_layout_top(ctx);
	const AeronRectI* parent_clip = (parent && parent->clip.width > 0) ? &parent->clip : NULL;
	if (ctx->scope_depth >= ctx->top_scope_prev) {
		offset -= (float)ctx->input->mouse.wheel_y * ui_ref(ctx, UI_WHEEL_STEP_REF) *
				  (ui_mouse_in(ctx, &view, parent_clip) ? 1.0f : 0.0f);
	}
	offset = fminf(fmaxf(offset, 0.0f), fmaxf(0.0f, content_h - view.h));
	if (slot) {
		slot->v0 = offset;
	}

	if (ctx->scroll_count < AERON_UI_MAX_SCROLLS) {
		ctx->scrolls[ctx->scroll_count++] = (UiScrollRec) { scroll_id, view.y, view.h, content_h };
	} else {
		Aeron_LogWarn("aeron.scene", "ui scroll cap (%d) hit", AERON_UI_MAX_SCROLLS);
	}

	/* Clip = view rect intersected with the parent clip. */
	AeronRectI clip = { (int)view.x, (int)view.y, (int)view.w, (int)view.h };
	if (parent_clip) {
		const int x0   = clip.x > parent_clip->x ? clip.x : parent_clip->x;
		const int y0   = clip.y > parent_clip->y ? clip.y : parent_clip->y;
		const int x1_a = clip.x + clip.width;
		const int x1_b = parent_clip->x + parent_clip->width;
		const int y1_a = clip.y + clip.height;
		const int y1_b = parent_clip->y + parent_clip->height;
		clip.x         = x0;
		clip.y         = y0;
		clip.width     = (x1_a < x1_b ? x1_a : x1_b) - x0;
		clip.height    = (y1_a < y1_b ? y1_a : y1_b) - y0;
	}

	UiLayout layout = { 0 };
	layout.kind     = UI_LAYOUT_SCROLL;
	layout.x        = view.x;
	layout.w        = view.w - ui_ref(ctx, ctx->theme.scrollbar_px) - ui_ref(ctx, 6.0f);
	layout.cursor_y = view.y - offset;
	layout.clip     = clip;
	layout.owner_id = scroll_id;
	layout.view_y   = view.y;
	layout.view_h   = view.h;
	layout.offset   = offset;
	ui_layout_push(ctx, &layout);
	return 1;
}

void AeronUi_EndScroll(AeronUiContext* ctx) {
	UiLayout* top = ctx && ctx->frame_active ? ui_layout_top(ctx) : NULL;
	if (!top || top->kind != UI_LAYOUT_SCROLL) {
		Aeron_LogWarn("aeron.scene", "unbalanced AeronUi_EndScroll");
		return;
	}
	if (top->columns > 0) {
		AeronUi_EndColumns(ctx);
	}
	const float content_h = top->cursor_y + top->offset - top->view_y - ui_ref(ctx, ctx->theme.item_spacing);
	const AeronUiId scroll_id = top->owner_id;
	const float     view_y    = top->view_y;
	const float     view_h    = top->view_h;
	const float     view_x    = top->x;
	const float     view_w    = top->w + ui_ref(ctx, ctx->theme.scrollbar_px) + ui_ref(ctx, 6.0f);
	ui_layout_pop(ctx);

	UiStateSlot* slot = ui_state(ctx, scroll_id);
	if (slot) {
		slot->v1 = fmaxf(content_h, 0.0f);
	}
	for (int i = 0; i < ctx->scroll_count; i++) {
		if (ctx->scrolls[i].id == scroll_id) {
			ctx->scrolls[i].content_h = fmaxf(content_h, 0.0f);
		}
	}
	if (content_h <= view_h || !slot) {
		return;
	}

	/* Scrollbar: proportional thumb, draggable. */
	const AeronRectI* clip = NULL;
	{
		const UiLayout* parent = ui_layout_top(ctx);
		clip                   = (parent && parent->clip.width > 0) ? &parent->clip : NULL;
	}
	const float  bar_w   = fmaxf(2.0f, ui_ref(ctx, ctx->theme.scrollbar_px));
	const float  max_off = content_h - view_h;
	const float  thumb_h = fmaxf(view_h * view_h / content_h, ui_ref(ctx, 24.0f));
	const float  travel  = view_h - thumb_h;
	const UiRect track   = { view_x + view_w - bar_w, view_y, bar_w, view_h };
	const UiRect thumb   = { track.x, view_y + travel * (slot->v0 / max_off), bar_w, thumb_h };

	/* Thumb drag: press anywhere on the track captures. */
	const AeronUiId thumb_id = scroll_id ^ 0x5C6011BAu;
	if (ctx->scope_depth >= ctx->top_scope_prev && ui_mouse_in(ctx, &track, clip) &&
		(ctx->input->mouse.pressed_buttons & AERON_MOUSE_BUTTON_LEFT)) {
		ctx->active_id = thumb_id;
	}
	if (ctx->active_id == thumb_id) {
		if (ctx->input->mouse.released_buttons & AERON_MOUSE_BUTTON_LEFT) {
			ctx->active_id = 0;
		} else if (travel > 0.0f) {
			slot->v0 =
				fminf(fmaxf((ctx->mouse_y - view_y - thumb_h * 0.5f) / travel * max_off, 0.0f), max_off);
		}
	}

	ui_draw_fill(ctx, &track, ctx->theme.scrollbar, clip);
	AeronDrawList2DRRect r = { 0 };
	r.dst_x                = ui_snap(thumb.x);
	r.dst_y                = ui_snap(thumb.y);
	r.dst_w                = ui_snap(thumb.w);
	r.dst_h                = ui_snap(thumb.h);
	r.radius_px            = bar_w * 0.5f;
	r.soft_px              = 1.0f;
	ui_pma(ctx, ctx->theme.scrollbar_thumb, r.fill_top);
	memcpy(r.fill_bottom, r.fill_top, sizeof r.fill_bottom);
	if (clip) {
		r.scissor = *clip;
	}
	AeronDrawList_AddRRect(ctx->list, &r);
}

/* =============================== modal =============================== */

int AeronUi_BeginModal(AeronUiContext* ctx, const char* title, int* open, const AeronUiWindowDesc* desc) {
	if (!ctx || !ctx->frame_active || !open) {
		return 0;
	}
	const AeronUiId id   = ui_make_id(ctx, title);
	UiStateSlot*    slot = ui_state(ctx, id);

	if (!*open) {
		/* Close transition: restore the focus saved at open. */
		if (slot && slot->v0 != 0.0f) {
			slot->v0 = 0.0f;
			if (ctx->scope_depth + 1 < AERON_UI_MAX_SCOPES) {
				ctx->focus_id = ctx->scope_saved_focus[ctx->scope_depth + 1];
			}
		}
		return 0;
	}
	/* An active text editor consumes cancel inside its widget first. */
	if (ctx->nav_cancel > 0 && ctx->text_edit_id == 0) {
		ctx->nav_cancel--;
		ctx->cancel_consumed = 1;
		*open                = 0;
		ui_play_sound(ctx, AERON_UI_SOUND_CANCEL);
		if (slot && slot->v0 != 0.0f) {
			slot->v0 = 0.0f;
			if (ctx->scope_depth + 1 < AERON_UI_MAX_SCOPES) {
				ctx->focus_id = ctx->scope_saved_focus[ctx->scope_depth + 1];
			}
		}
		return 0;
	}
	if (ctx->scope_depth + 1 >= AERON_UI_MAX_SCOPES) {
		Aeron_LogWarn("aeron.scene", "ui modal scope cap (%d) hit", AERON_UI_MAX_SCOPES);
		return 0;
	}

	ctx->scope_depth++;
	if (slot && slot->v0 == 0.0f) {
		/* Open transition: save focus, start fresh in the modal. */
		slot->v0                                 = 1.0f;
		ctx->scope_saved_focus[ctx->scope_depth] = ctx->focus_id;
		ctx->focus_id                            = 0;
	}
	ctx->modal_open_id = id;

	/* Scrim over everything declared so far. */
	const UiRect full = { 0.0f, 0.0f, (float)ctx->out_w, (float)ctx->out_h };
	ui_draw_fill(ctx, &full, ctx->theme.scrim, NULL);

	return ui_begin_window_common(ctx, title, desc, id);
}

void AeronUi_EndModal(AeronUiContext* ctx) {
	if (!ctx || !ctx->frame_active || ctx->scope_depth <= 0) {
		Aeron_LogWarn("aeron.scene", "unbalanced AeronUi_EndModal");
		return;
	}
	ui_end_window_common(ctx);
	ctx->scope_depth--;
}
