/* AeronUi virtualized selectable list. */

#include "internal.h"

#include <stdint.h>

static const AeronRectI* list_current_clip(AeronUiContext* ctx) {
	const UiLayout* top = ui_layout_top(ctx);
	return top && top->clip.width > 0 ? &top->clip : NULL;
}

static size_t list_find_enabled(const AeronUiListItem* items, size_t count, size_t start, int step) {
	if (!items || count == 0)
		return SIZE_MAX;
	size_t index = start;
	for (size_t walked = 0; walked < count; walked++) {
		if (step > 0) {
			if (index >= count)
				return SIZE_MAX;
		} else if (index >= count) {
			return SIZE_MAX;
		}
		if (!(items[index].flags & AERON_UI_LIST_ITEM_DISABLED))
			return index;
		if (step > 0) {
			index++;
		} else {
			if (index == 0)
				return SIZE_MAX;
			index--;
		}
	}
	return SIZE_MAX;
}

static size_t list_move_count(const AeronUiListItem* items, size_t count, size_t selected, int delta,
							  int* consumed) {
	if (consumed)
		*consumed = 0;
	if (!items || count == 0 || delta == 0)
		return selected;
	size_t    result    = selected;
	const int step      = delta > 0 ? 1 : -1;
	int       remaining = delta > 0 ? delta : -delta;
	while (remaining-- > 0) {
		const bool   valid      = result < count && !(items[result].flags & AERON_UI_LIST_ITEM_DISABLED);
		const size_t next_start = !valid     ? (step > 0 ? 0 : count - 1)
								  : step > 0 ? result + 1
											 : (result == 0 ? SIZE_MAX : result - 1);
		const size_t next       = list_find_enabled(items, count, next_start, step);
		if (next == SIZE_MAX)
			break;
		result = next;
		if (consumed)
			++*consumed;
	}
	return result;
}

static size_t list_move(const AeronUiListItem* items, size_t count, size_t selected, int delta) {
	return list_move_count(items, count, selected, delta, NULL);
}

static AeronRectI list_clip(const UiRect* rect, const AeronRectI* parent) {
	AeronRectI clip = { (int)rect->x, (int)rect->y, (int)rect->w, (int)rect->h };
	if (parent) {
		const int x0 = clip.x > parent->x ? clip.x : parent->x;
		const int y0 = clip.y > parent->y ? clip.y : parent->y;
		const int x1 =
			clip.x + clip.width < parent->x + parent->width ? clip.x + clip.width : parent->x + parent->width;
		const int y1 = clip.y + clip.height < parent->y + parent->height ? clip.y + clip.height
																		 : parent->y + parent->height;
		clip         = (AeronRectI) { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
	}
	return clip;
}

uint32_t AeronUi_ListBox(AeronUiContext* ctx, const char* label, const AeronUiListItem* items,
						 size_t item_count, size_t* selected, float height_ref) {
	UiRect rect;
	if (!ctx || !ctx->frame_active || !selected || height_ref <= 0.0f || (item_count > 0 && !items) ||
		!ui_layout_row(ctx, ui_ref(ctx, height_ref), &rect)) {
		return AERON_UI_LIST_NONE;
	}
	const AeronUiId   id          = ui_make_id(ctx, label);
	const AeronRectI* parent_clip = list_current_clip(ctx);
	UiStateSlot*      state       = ui_state(ctx, id);
	const float       row_h       = fmaxf(ui_ref(ctx, ctx->theme.row_height), 1.0f);
	const float       bar_w       = fmaxf(ui_ref(ctx, ctx->theme.scrollbar_px), 8.0f);
	const float       content_w   = fmaxf(0.0f, rect.w - bar_w - ui_ref(ctx, 4.0f));
	const float       max_scroll  = fmaxf(0.0f, (float)item_count * row_h - rect.h);
	float             scroll      = state ? fminf(fmaxf(state->v0, 0.0f), max_scroll) : 0.0f;
	const size_t      initial     = *selected;
	if (*selected >= item_count ||
		(*selected != SIZE_MAX && (items[*selected].flags & AERON_UI_LIST_ITEM_DISABLED))) {
		*selected = SIZE_MAX;
	}

	const int accept_before = ui_is_focused(ctx, id) && ctx->nav_accept > 0;
	ui_widget_behavior(ctx, id, &rect, 1);
	uint32_t result = AERON_UI_LIST_NONE;

	if (ui_is_focused(ctx, id)) {
		const int moves = ctx->nav[UI_DIR_DOWN] - ctx->nav[UI_DIR_UP];
		if (moves) {
			int consumed        = 0;
			*selected           = list_move_count(items, item_count, *selected, moves, &consumed);
			ctx->nav[UI_DIR_UP] = ctx->nav[UI_DIR_DOWN] = 0;
			const int remaining                         = (moves < 0 ? -moves : moves) - consumed;
			if (remaining > 0)
				ctx->nav[moves > 0 ? UI_DIR_DOWN : UI_DIR_UP] = remaining;
		}
		const int visible = (int)fmaxf(1.0f, floorf(rect.h / row_h));
		if (ctx->input->key_pressed[AERON_KEY_PAGEUP])
			*selected = list_move(items, item_count, *selected, -visible);
		if (ctx->input->key_pressed[AERON_KEY_PAGEDOWN])
			*selected = list_move(items, item_count, *selected, visible);
		if (ctx->input->key_pressed[AERON_KEY_HOME])
			*selected = list_find_enabled(items, item_count, 0, 1);
		if (ctx->input->key_pressed[AERON_KEY_END] && item_count)
			*selected = list_find_enabled(items, item_count, item_count - 1, -1);
	}

	if (ui_mouse_in(ctx, &rect, parent_clip) && ctx->input->mouse.wheel_y) {
		scroll -= (float)ctx->input->mouse.wheel_y * row_h * 3.0f;
	}
	scroll = fminf(fmaxf(scroll, 0.0f), max_scroll);

	if (ctx->mouse_present && ctx->mouse_x < rect.x + content_w && ui_mouse_in(ctx, &rect, parent_clip) &&
		(ctx->input->mouse.pressed_buttons & AERON_MOUSE_BUTTON_LEFT)) {
		const size_t hit = (size_t)fmaxf(0.0f, floorf((ctx->mouse_y - rect.y + scroll) / row_h));
		if (hit < item_count && !(items[hit].flags & AERON_UI_LIST_ITEM_DISABLED)) {
			*selected = hit;
			if (ctx->input->mouse.double_clicked_buttons & AERON_MOUSE_BUTTON_LEFT)
				result |= AERON_UI_LIST_ACTIVATED;
		}
	}
	if (accept_before && *selected != SIZE_MAX)
		result |= AERON_UI_LIST_ACTIVATED;
	if (*selected != initial) {
		result |= AERON_UI_LIST_CHANGED;
		ui_play_sound(ctx, AERON_UI_SOUND_FOCUS);
	}

	if (*selected != SIZE_MAX) {
		const float top    = (float)*selected * row_h;
		const float bottom = top + row_h;
		if (top < scroll)
			scroll = top;
		if (bottom > scroll + rect.h)
			scroll = bottom - rect.h;
	}
	scroll = fminf(fmaxf(scroll, 0.0f), max_scroll);
	if (state)
		state->v0 = scroll;

	ui_draw_surface(ctx, &rect, ctx->theme.widget_bg, ctx->theme.widget_bg_low, ctx->theme.widget_gradient,
					ctx->theme.widget_border, ctx->theme.widget_border_px, 0, parent_clip);
	const AeronRectI clip          = list_clip(&rect, parent_clip);
	const size_t     first         = (size_t)floorf(scroll / row_h);
	const size_t     visible_count = (size_t)ceilf(rect.h / row_h) + 1u;
	const size_t     last          = first + visible_count < item_count ? first + visible_count : item_count;
	const float      text_px       = ui_ref(ctx, ctx->theme.text_px);
	for (size_t i = first; i < last; i++) {
		const UiRect row = { rect.x, rect.y + (float)i * row_h - scroll, content_w, row_h };
		if (i == *selected) {
			ui_draw_fill(ctx, &row, ctx->theme.widget_bg_hot, &clip);
			if (ui_is_focused(ctx, id))
				ui_draw_fill(ctx, &row, ctx->theme.row_highlight, &clip);
		}
		const int   disabled = (items[i].flags & AERON_UI_LIST_ITEM_DISABLED) != 0;
		const char* prefix   = (items[i].flags & AERON_UI_LIST_ITEM_DIRECTORY) ? "> " : "  ";
		const float x        = row.x + ui_ref(ctx, 8.0f);
		ui_draw_text(ctx, ui_font_regular(ctx), x, row.y + (row.h - text_px) * 0.5f, AERON_TEXT_LEFT, text_px,
					 disabled ? ctx->theme.text_dim : ctx->theme.text, prefix, -1, &clip);
		const float prefix_w = ui_text_width(ctx, ui_font_regular(ctx), text_px, prefix, -1);
		ui_draw_text(ctx, ui_font_regular(ctx), x + prefix_w, row.y + (row.h - text_px) * 0.5f,
					 AERON_TEXT_LEFT, text_px, disabled ? ctx->theme.text_dim : ctx->theme.text,
					 items[i].label ? items[i].label : "", -1, &clip);
		if (items[i].detail && items[i].detail[0]) {
			ui_draw_text(ctx, ui_font_regular(ctx), row.x + row.w - ui_ref(ctx, 8.0f),
						 row.y + (row.h - text_px) * 0.5f, AERON_TEXT_RIGHT, text_px, ctx->theme.text_dim,
						 items[i].detail, -1, &clip);
		}
	}

	if (max_scroll > 0.0f) {
		const UiRect track   = { rect.x + rect.w - bar_w, rect.y, bar_w, rect.h };
		const float  thumb_h = fmaxf(row_h, rect.h * rect.h / ((float)item_count * row_h));
		const float  travel  = rect.h - thumb_h;
		const UiRect thumb   = { track.x, track.y + (max_scroll > 0.0f ? scroll / max_scroll * travel : 0.0f),
								 track.w, thumb_h };
		const AeronUiId thumb_id = id ^ 0x9e3779b9u;
		if (ui_mouse_in(ctx, &track, parent_clip) &&
			(ctx->input->mouse.pressed_buttons & AERON_MOUSE_BUTTON_LEFT))
			ctx->active_id = thumb_id;
		if (ctx->active_id == thumb_id) {
			if (ctx->input->mouse.released_buttons & AERON_MOUSE_BUTTON_LEFT) {
				ctx->active_id = 0;
			} else if (travel > 0.0f) {
				scroll =
					fminf(fmaxf((ctx->mouse_y - track.y - thumb_h * 0.5f) / travel, 0.0f), 1.0f) * max_scroll;
				if (state)
					state->v0 = scroll;
			}
		}
		ui_draw_fill(ctx, &track, ctx->theme.scrollbar, &clip);
		ui_draw_fill(ctx, &thumb, ctx->theme.scrollbar_thumb, &clip);
	}
	if (ui_is_focused(ctx, id))
		ui_draw_focus_outline(ctx, &rect, parent_clip);
	if (result & AERON_UI_LIST_ACTIVATED)
		ui_play_sound(ctx, AERON_UI_SOUND_ACCEPT);
	return result;
}
