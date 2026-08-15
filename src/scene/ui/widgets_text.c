/* AeronUi single-line UTF-8 text editor. */

#include "internal.h"

#include <SDL3/SDL.h>

#include <stdlib.h>

static const AeronRectI* text_current_clip(AeronUiContext* ctx) {
	const UiLayout* top = ui_layout_top(ctx);
	return top && top->clip.width > 0 ? &top->clip : NULL;
}

static size_t text_bounded_length(const char* text, size_t capacity) {
	size_t length = 0;
	while (length < capacity && text[length]) {
		length++;
	}
	return length;
}

static size_t text_cp_next(const char* text, size_t length, size_t offset) {
	if (offset >= length) {
		return length;
	}
	offset++;
	while (offset < length && ((unsigned char)text[offset] & 0xc0u) == 0x80u) {
		offset++;
	}
	return offset;
}

static size_t text_cp_prev(const char* text, size_t offset) {
	if (offset == 0) {
		return 0;
	}
	offset--;
	while (offset > 0 && ((unsigned char)text[offset] & 0xc0u) == 0x80u) {
		offset--;
	}
	return offset;
}

static int text_utf8_valid(const char* text, size_t length) {
	for (size_t i = 0; i < length;) {
		const unsigned char first = (unsigned char)text[i++];
		int                 extra;
		uint32_t            cp;
		if (first < 0x80u) {
			continue;
		}
		if ((first & 0xe0u) == 0xc0u) {
			extra = 1;
			cp    = first & 0x1fu;
		} else if ((first & 0xf0u) == 0xe0u) {
			extra = 2;
			cp    = first & 0x0fu;
		} else if ((first & 0xf8u) == 0xf0u) {
			extra = 3;
			cp    = first & 0x07u;
		} else {
			return 0;
		}
		if (i + (size_t)extra > length) {
			return 0;
		}
		for (int n = 0; n < extra; n++) {
			const unsigned char next = (unsigned char)text[i++];
			if ((next & 0xc0u) != 0x80u) {
				return 0;
			}
			cp = (cp << 6) | (next & 0x3fu);
		}
		if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu) || (extra == 1 && cp < 0x80u) ||
			(extra == 2 && cp < 0x800u) || (extra == 3 && cp < 0x10000u)) {
			return 0;
		}
	}
	return 1;
}

static void text_selection(const AeronUiContext* ctx, size_t* begin, size_t* end) {
	if (ctx->text_cursor < ctx->text_anchor) {
		*begin = ctx->text_cursor;
		*end   = ctx->text_anchor;
	} else {
		*begin = ctx->text_anchor;
		*end   = ctx->text_cursor;
	}
}

static int text_delete_range(char* value, size_t* length, size_t begin, size_t end) {
	if (begin >= end || end > *length) {
		return 0;
	}
	memmove(value + begin, value + end, *length - end + 1);
	*length -= end - begin;
	return 1;
}

static int text_insert(char* value, size_t capacity, size_t* length, size_t begin, size_t end,
					   const char* inserted, size_t inserted_length, size_t* cursor) {
	if (!text_utf8_valid(inserted, inserted_length) || begin > end || end > *length) {
		return 0;
	}
	for (size_t i = 0; i < inserted_length; i++) {
		const unsigned char byte = (unsigned char)inserted[i];
		if (byte < 0x20u || byte == 0x7fu)
			return 0;
	}
	const size_t kept = *length - (end - begin);
	if (inserted_length > capacity - 1 - kept) {
		return 0;
	}
	memmove(value + begin + inserted_length, value + end, *length - end + 1);
	memcpy(value + begin, inserted, inserted_length);
	*length = kept + inserted_length;
	*cursor = begin + inserted_length;
	return 1;
}

static float text_prefix_width(AeronUiContext* ctx, const char* text, size_t bytes, float text_px) {
	return ui_text_width(ctx, ui_font_regular(ctx), text_px, text, (int)bytes);
}

static size_t text_hit_offset(AeronUiContext* ctx, const char* value, size_t length, float text_px,
							  float local_x) {
	size_t offset   = 0;
	float  previous = 0.0f;
	while (offset < length) {
		const size_t next  = text_cp_next(value, length, offset);
		const float  width = text_prefix_width(ctx, value, next, text_px);
		if (local_x < (previous + width) * 0.5f) {
			return offset;
		}
		previous = width;
		offset   = next;
	}
	return length;
}

static AeronRectI text_clip_rect(const UiRect* rect, const AeronRectI* parent) {
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

static int text_platform_shortcut(const AeronInputSnapshot* input) {
#if defined(__APPLE__)
	return input->key_down[AERON_KEY_LGUI] || input->key_down[AERON_KEY_RGUI];
#else
	return input->key_down[AERON_KEY_LCTRL] || input->key_down[AERON_KEY_RCTRL];
#endif
}

static uint32_t input_text(AeronUiContext* ctx, const char* label, char* value, size_t capacity,
						   uint32_t flags, const char* action_label) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !value || capacity == 0 ||
		!ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row)) {
		return 0;
	}
	const size_t length_checked = text_bounded_length(value, capacity);
	if (length_checked == capacity || !text_utf8_valid(value, length_checked)) {
		return 0;
	}
	size_t            length      = length_checked;
	const AeronUiId   id          = ui_make_id(ctx, label);
	const AeronRectI* parent_clip = text_current_clip(ctx);
	UiRect            control     = ui_form_row_split(ctx, label, &row, parent_clip);
	UiRect            input_row   = row;
	UiRect            action      = { 0 };
	const float       text_px     = ui_ref(ctx, ctx->theme.text_px);
	const int         has_action  = action_label && ui_label_text_len(action_label) > 0;
	if (has_action) {
		const float gap         = ui_ref(ctx, ctx->theme.item_spacing);
		const float label_width = ui_text_width(ctx, ui_font_regular(ctx), text_px,
												ui_label_text(action_label), ui_label_text_len(action_label));
		const float desired_width   = fmaxf(control.h, ui_snap(label_width + 2.0f * ui_ref(ctx, 12.0f)));
		const float available_width = fmaxf(0.0f, control.w - gap);
		action.w                    = fminf(desired_width, available_width);
		action.h                    = control.h;
		action.x                    = control.x + control.w - action.w;
		action.y                    = control.y;
		control.w                   = fmaxf(0.0f, action.x - gap - control.x);
		input_row.w                 = fmaxf(0.0f, control.x + control.w - row.x);
	}
	const int read_only   = (flags & AERON_UI_INPUT_TEXT_READ_ONLY) != 0;
	const int was_editing = ctx->text_edit_id == id;
	if (was_editing) {
		if (ctx->text_cursor > length)
			ctx->text_cursor = length;
		if (ctx->text_anchor > length)
			ctx->text_anchor = length;
		while (ctx->text_cursor > 0 && ctx->text_cursor < length &&
			   ((unsigned char)value[ctx->text_cursor] & 0xc0u) == 0x80u)
			ctx->text_cursor--;
		while (ctx->text_anchor > 0 && ctx->text_anchor < length &&
			   ((unsigned char)value[ctx->text_anchor] & 0xc0u) == 0x80u)
			ctx->text_anchor--;
	}
	const int submit_before =
		was_editing && ui_is_focused(ctx, id) &&
		(ctx->input->key_pressed[AERON_KEY_RETURN] || ctx->input->key_pressed[AERON_KEY_KP_ENTER]);
	if (was_editing && ctx->input->key_pressed[AERON_KEY_SPACE] && ctx->nav_accept > 0) {
		ctx->nav_accept--;
	}
	const int    activated = ui_widget_behavior(ctx, id, &input_row, 1);
	const float  inset     = ui_ref(ctx, 8.0f);
	const UiRect text_rect = { control.x + inset, control.y, fmaxf(0.0f, control.w - inset * 2.0f),
							   control.h };
	int          started   = 0;
	int          changed   = 0;

	if (!read_only && !was_editing && activated) {
		ctx->text_edit_id = id;
		ctx->text_cursor  = length;
		ctx->text_anchor  = length;
		ctx->text_scroll  = 0.0f;
		started           = 1;
	} else if (was_editing && activated && submit_before) {
		ctx->text_edit_id = 0;
	}

	if (!read_only && (ctx->input->mouse.pressed_buttons & AERON_MOUSE_BUTTON_LEFT) &&
		ui_mouse_in(ctx, &control, parent_clip)) {
		ctx->text_edit_id = id;
		const float local = ctx->mouse_x - text_rect.x + ctx->text_scroll;
		ctx->text_cursor  = text_hit_offset(ctx, value, length, text_px, local);
		if (!(ctx->input->key_down[AERON_KEY_LSHIFT] || ctx->input->key_down[AERON_KEY_RSHIFT])) {
			ctx->text_anchor = ctx->text_cursor;
		}
		started = !was_editing;
	}
	if (!read_only && ctx->text_edit_id == id && ctx->active_id == id &&
		(ctx->input->mouse.buttons & AERON_MOUSE_BUTTON_LEFT) && ctx->mouse_present) {
		const float local = ctx->mouse_x - text_rect.x + ctx->text_scroll;
		ctx->text_cursor  = text_hit_offset(ctx, value, length, text_px, local);
	}

	if (ctx->text_edit_id == id && !started) {
		const int shift    = ctx->input->key_down[AERON_KEY_LSHIFT] || ctx->input->key_down[AERON_KEY_RSHIFT];
		const int shortcut = text_platform_shortcut(ctx->input);
		size_t    begin, end;
		text_selection(ctx, &begin, &end);

		if (ctx->nav_cancel > 0) {
			ctx->nav_cancel--;
			ctx->cancel_consumed = 1;
			ctx->text_edit_id    = 0;
			ui_play_sound(ctx, AERON_UI_SOUND_CANCEL);
		}
		if (ctx->input->key_pressed[AERON_KEY_HOME]) {
			ctx->text_cursor = 0;
			if (!shift)
				ctx->text_anchor = 0;
		}
		if (ctx->input->key_pressed[AERON_KEY_END]) {
			ctx->text_cursor = length;
			if (!shift)
				ctx->text_anchor = length;
		}
		for (int n = 0; n < ctx->nav[UI_DIR_LEFT]; n++) {
			ctx->text_cursor = text_cp_prev(value, ctx->text_cursor);
			if (!shift)
				ctx->text_anchor = ctx->text_cursor;
		}
		for (int n = 0; n < ctx->nav[UI_DIR_RIGHT]; n++) {
			ctx->text_cursor = text_cp_next(value, length, ctx->text_cursor);
			if (!shift)
				ctx->text_anchor = ctx->text_cursor;
		}
		ctx->nav[UI_DIR_LEFT]  = 0;
		ctx->nav[UI_DIR_RIGHT] = 0;
		ctx->nav[UI_DIR_UP]    = 0;
		ctx->nav[UI_DIR_DOWN]  = 0;

		const int key_a = AERON_KEY_A;
		const int key_c = AERON_KEY_A + ('c' - 'a');
		const int key_v = AERON_KEY_A + ('v' - 'a');
		const int key_x = AERON_KEY_A + ('x' - 'a');
		if (shortcut && ctx->input->key_pressed[key_a]) {
			ctx->text_anchor = 0;
			ctx->text_cursor = length;
		}
		text_selection(ctx, &begin, &end);
		if (shortcut && (ctx->input->key_pressed[key_c] || ctx->input->key_pressed[key_x]) && begin < end) {
			char* copied = (char*)SDL_malloc(end - begin + 1);
			if (copied) {
				memcpy(copied, value + begin, end - begin);
				copied[end - begin] = '\0';
				SDL_SetClipboardText(copied);
				SDL_free(copied);
			}
			if (ctx->input->key_pressed[key_x] && text_delete_range(value, &length, begin, end)) {
				ctx->text_cursor = ctx->text_anchor = begin;
				changed                             = 1;
			}
		}
		if (shortcut && ctx->input->key_pressed[key_v]) {
			char* pasted = SDL_GetClipboardText();
			if (pasted) {
				text_selection(ctx, &begin, &end);
				if (text_insert(value, capacity, &length, begin, end, pasted, strlen(pasted),
								&ctx->text_cursor)) {
					ctx->text_anchor = ctx->text_cursor;
					changed          = 1;
				}
				SDL_free(pasted);
			}
		}
		text_selection(ctx, &begin, &end);
		for (int repeat = 0; repeat < ctx->input->key_typed[AERON_KEY_BACKSPACE]; repeat++) {
			text_selection(ctx, &begin, &end);
			if (begin == end)
				begin = text_cp_prev(value, begin);
			if (!text_delete_range(value, &length, begin, end))
				break;
			ctx->text_cursor = ctx->text_anchor = begin;
			changed                             = 1;
		}
		for (int repeat = 0; repeat < ctx->input->key_typed[AERON_KEY_DELETE]; repeat++) {
			text_selection(ctx, &begin, &end);
			if (begin == end)
				end = text_cp_next(value, length, end);
			if (!text_delete_range(value, &length, begin, end))
				break;
			ctx->text_cursor = ctx->text_anchor = begin;
			changed                             = 1;
		}
		if (!shortcut && ctx->input->text_length > 0) {
			text_selection(ctx, &begin, &end);
			if (text_insert(value, capacity, &length, begin, end, ctx->input->text, ctx->input->text_length,
							&ctx->text_cursor)) {
				ctx->text_anchor = ctx->text_cursor;
				changed          = 1;
			}
		}
	}

	if (ui_is_focused(ctx, id))
		ui_draw_row_focus_bg(ctx, &input_row, parent_clip);
	ui_draw_surface(ctx, &control, ctx->hot_id == id ? ctx->theme.widget_bg_hot : ctx->theme.widget_bg,
					ctx->theme.widget_bg_low, ctx->theme.widget_gradient, ctx->theme.widget_border,
					ctx->theme.widget_border_px, 1, parent_clip);
	const AeronRectI field_clip = text_clip_rect(&text_rect, parent_clip);
	if (ctx->text_edit_id == id) {
		const float cursor_x = text_prefix_width(ctx, value, ctx->text_cursor, text_px);
		if (cursor_x - ctx->text_scroll > text_rect.w - 2.0f) {
			ctx->text_scroll = cursor_x - text_rect.w + 2.0f;
		} else if (cursor_x < ctx->text_scroll) {
			ctx->text_scroll = cursor_x;
		}
		size_t begin, end;
		text_selection(ctx, &begin, &end);
		if (begin < end) {
			const float  x0        = text_prefix_width(ctx, value, begin, text_px) - ctx->text_scroll;
			const float  x1        = text_prefix_width(ctx, value, end, text_px) - ctx->text_scroll;
			const UiRect selection = { text_rect.x + x0, text_rect.y + 2.0f, x1 - x0,
									   fmaxf(0.0f, text_rect.h - 4.0f) };
			ui_draw_fill(ctx, &selection, ctx->theme.row_highlight, &field_clip);
		}
	}
	ui_draw_text(ctx, ui_font_regular(ctx), text_rect.x - ctx->text_scroll,
				 text_rect.y + (text_rect.h - text_px) * 0.5f, AERON_TEXT_LEFT, text_px,
				 read_only ? ctx->theme.text_dim : ctx->theme.text, value, (int)length, &field_clip);
	if (ctx->text_edit_id == id) {
		const float  cursor_x = text_prefix_width(ctx, value, ctx->text_cursor, text_px);
		const UiRect caret    = { text_rect.x + cursor_x - ctx->text_scroll,
								  text_rect.y + (text_rect.h - text_px) * 0.5f, 1.0f, text_px };
		ui_draw_fill(ctx, &caret, ctx->theme.focus_outline, &field_clip);
	}
	if (ui_is_focused(ctx, id))
		ui_draw_row_focus_ring(ctx, &input_row, parent_clip);
	const int action_activated = has_action && ui_button_at(ctx, ui_make_child_id(id, action_label),
															action_label, &action, 1, parent_clip);
	if (changed)
		ui_play_sound(ctx, AERON_UI_SOUND_ADJUST);
	return (changed ? AERON_UI_INPUT_TEXT_CHANGED : 0u) |
		   (action_activated ? AERON_UI_INPUT_TEXT_ACTION_ACTIVATED : 0u);
}

int AeronUi_InputText(AeronUiContext* ctx, const char* label, char* value, size_t capacity, uint32_t flags) {
	return (input_text(ctx, label, value, capacity, flags, NULL) & AERON_UI_INPUT_TEXT_CHANGED) != 0;
}

uint32_t AeronUi_InputTextWithAction(AeronUiContext* ctx, const char* label, char* value, size_t capacity,
									 uint32_t flags, const char* action_label) {
	return input_text(ctx, label, value, capacity, flags, action_label);
}
