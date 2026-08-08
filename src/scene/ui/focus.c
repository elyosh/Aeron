/*
 * AeronUi input intents, focus navigation, and shared widget behavior.
 *
 * Keyboard arrows use the snapshot's typed counts (OS typematic repeat);
 * gamepad d-pad and left stick feed dt-based repeat accumulators — no
 * polling threads, pure frame math. Navigation leftovers resolve at
 * EndFrame against the completed widget index of the current frame.
 */

#include "internal.h"

#include <stdlib.h> /* abs */

#define UI_STICK_ENTER 13107 /* 40% of 32767 */
#define UI_STICK_EXIT 8192   /* 25% — hysteresis */

/* Letter scancodes are ordered from AERON_KEY_A. */
#define UI_KEY_Q (AERON_KEY_A + ('q' - 'a'))
#define UI_KEY_E (AERON_KEY_A + ('e' - 'a'))

/* Aggregated held state of one navigation direction across gamepads. */
static int ui_pad_dir_held(const AeronUiContext* ctx, int dir) {
	static const AeronGamepadButton dpad[UI_DIR_COUNT] = {
		AERON_GAMEPAD_BUTTON_DPAD_UP,
		AERON_GAMEPAD_BUTTON_DPAD_DOWN,
		AERON_GAMEPAD_BUTTON_DPAD_LEFT,
		AERON_GAMEPAD_BUTTON_DPAD_RIGHT,
	};
	for (int i = 0; i < AERON_CONTROLLER_MAX; i++) {
		const AeronControllerSnapshot* pad = &ctx->input->controllers[i];
		if (!pad->connected) {
			continue;
		}
		if (pad->gamepad_buttons & (1u << dpad[dir])) {
			return 1;
		}
	}
	if (dir == UI_DIR_UP) {
		return ctx->stick_latch_y < 0;
	}
	if (dir == UI_DIR_DOWN) {
		return ctx->stick_latch_y > 0;
	}
	if (dir == UI_DIR_LEFT) {
		return ctx->stick_latch_x < 0;
	}
	return ctx->stick_latch_x > 0;
}

/* Aggregated pressed-edge test across connected gamepads. */
static int ui_pad_pressed(const AeronUiContext* ctx, AeronGamepadButton button) {
	int pressed = 0;
	for (int i = 0; i < AERON_CONTROLLER_MAX; i++) {
		const AeronControllerSnapshot* pad = &ctx->input->controllers[i];
		if (pad->connected && (pad->gamepad_pressed_buttons & (1u << button))) {
			pressed++;
		}
	}
	return pressed;
}

/* Latches one stick axis into -1/0/+1 with hysteresis. */
static void ui_stick_latch(int16_t value, int* latch) {
	if (*latch == 0) {
		if (value <= -UI_STICK_ENTER) {
			*latch = -1;
		} else if (value >= UI_STICK_ENTER) {
			*latch = 1;
		}
	} else if ((*latch < 0 && value > -UI_STICK_EXIT) || (*latch > 0 && value < UI_STICK_EXIT)) {
		*latch = 0;
	}
}

void ui_collect_input(AeronUiContext* ctx) {
	const AeronInputSnapshot* in = ctx->input;

	memset(ctx->nav, 0, sizeof ctx->nav);
	ctx->nav_accept           = 0;
	ctx->nav_accept_down      = 0;
	ctx->nav_cancel           = 0;
	ctx->nav_tab_next         = 0;
	ctx->nav_tab_prev         = 0;
	ctx->focus_at_frame_start = ctx->focus_id;

	/* Left-stick hysteresis across connected gamepads (strongest axis). */
	int16_t stick_x = 0;
	int16_t stick_y = 0;
	for (int i = 0; i < AERON_CONTROLLER_MAX; i++) {
		const AeronControllerSnapshot* pad = &ctx->input->controllers[i];
		if (!pad->connected) {
			continue;
		}
		const int16_t x = pad->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX];
		const int16_t y = pad->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTY];
		if (abs(x) > abs(stick_x)) {
			stick_x = x;
		}
		if (abs(y) > abs(stick_y)) {
			stick_y = y;
		}
	}
	ui_stick_latch(stick_x, &ctx->stick_latch_x);
	ui_stick_latch(stick_y, &ctx->stick_latch_y);

	/* Keyboard: typed counts carry OS typematic repeats. */
	ctx->nav[UI_DIR_UP] += in->key_typed[AERON_KEY_UP];
	ctx->nav[UI_DIR_DOWN] += in->key_typed[AERON_KEY_DOWN];
	ctx->nav[UI_DIR_LEFT] += in->key_typed[AERON_KEY_LEFT];
	ctx->nav[UI_DIR_RIGHT] += in->key_typed[AERON_KEY_RIGHT];
	ctx->nav_accept += in->key_pressed[AERON_KEY_RETURN] + in->key_pressed[AERON_KEY_KP_ENTER] +
					   in->key_pressed[AERON_KEY_SPACE];
	ctx->nav_accept_down =
		in->key_down[AERON_KEY_RETURN] || in->key_down[AERON_KEY_KP_ENTER] || in->key_down[AERON_KEY_SPACE];
	ctx->nav_cancel += in->key_pressed[AERON_KEY_ESCAPE];
	ctx->nav_tab_prev += in->key_pressed[UI_KEY_Q];
	ctx->nav_tab_next += in->key_pressed[UI_KEY_E];

	/* Gamepad edges. */
	ctx->nav_accept += ui_pad_pressed(ctx, AERON_GAMEPAD_BUTTON_SOUTH);
	ctx->nav_cancel += ui_pad_pressed(ctx, AERON_GAMEPAD_BUTTON_EAST);
	ctx->nav_tab_prev += ui_pad_pressed(ctx, AERON_GAMEPAD_BUTTON_LEFT_SHOULDER);
	ctx->nav_tab_next += ui_pad_pressed(ctx, AERON_GAMEPAD_BUTTON_RIGHT_SHOULDER);

	/* Gamepad directions: edge + dt repeat (delay, then interval). */
	for (int dir = 0; dir < UI_DIR_COUNT; dir++) {
		const int held = ui_pad_dir_held(ctx, dir);
		if (held && !ctx->held_prev[dir]) {
			ctx->nav[dir]++;
			ctx->repeat_t[dir] = 0.0f;
		} else if (held) {
			ctx->repeat_t[dir] += ctx->dt;
			const float delay    = ctx->theme.repeat_delay_s;
			const float interval = fmaxf(ctx->theme.repeat_interval_s, 1.0f / 120.0f);
			while (ctx->repeat_t[dir] >= delay + interval) {
				ctx->nav[dir]++;
				ctx->repeat_t[dir] -= interval;
			}
		} else {
			ctx->repeat_t[dir] = 0.0f;
		}
		ctx->held_prev[dir] = held;
	}
}

int ui_mouse_in(const AeronUiContext* ctx, const UiRect* rect, const AeronRectI* clip) {
	if (!ctx->mouse_present) {
		return 0;
	}
	const float x = ctx->mouse_x;
	const float y = ctx->mouse_y;
	if (x < rect->x || y < rect->y || x >= rect->x + rect->w || y >= rect->y + rect->h) {
		return 0;
	}
	if (clip && clip->width > 0 && clip->height > 0 &&
		(x < (float)clip->x || y < (float)clip->y || x >= (float)(clip->x + clip->width) ||
		 y >= (float)(clip->y + clip->height))) {
		return 0;
	}
	return 1;
}

int ui_is_focused(const AeronUiContext* ctx, AeronUiId id) { return ctx->focus_id == id; }

int ui_consume_adjust(AeronUiContext* ctx, AeronUiId id) {
	if (!ui_is_focused(ctx, id)) {
		return 0;
	}
	const int delta        = ctx->nav[UI_DIR_RIGHT] - ctx->nav[UI_DIR_LEFT];
	ctx->nav[UI_DIR_LEFT]  = 0;
	ctx->nav[UI_DIR_RIGHT] = 0;
	return delta;
}

int ui_widget_behavior(AeronUiContext* ctx, AeronUiId id, const UiRect* rect, int focusable) {
	const UiLayout*   top       = ui_layout_top(ctx);
	const AeronRectI* clip      = (top && top->clip.width > 0) ? &top->clip : NULL;
	int               activated = 0;

	ui_record_widget(ctx, id, rect, focusable);

	/* Widgets below an open modal are inert. Modal state lags one frame
	 * by design (declaration order); steady state is exact. */
	if (ctx->scope_depth < ctx->top_scope_prev) {
		return 0;
	}

	const int hovered = ui_mouse_in(ctx, rect, clip);
	if (hovered) {
		ctx->hot_id = id;
		if (focusable && ctx->mouse_moved && ctx->focus_id != id) {
			ctx->focus_id = id;
		}
	}

	if (hovered && (ctx->input->mouse.pressed_buttons & AERON_MOUSE_BUTTON_LEFT)) {
		ctx->active_id = id;
		if (focusable) {
			ctx->focus_id = id;
		}
	}
	if (ctx->active_id == id && (ctx->input->mouse.released_buttons & AERON_MOUSE_BUTTON_LEFT)) {
		if (hovered) {
			activated = 1;
		}
		ctx->active_id = 0;
	}

	if (focusable && ui_is_focused(ctx, id) && ctx->nav_accept > 0) {
		ctx->nav_accept--;
		activated = 1;
	}
	return activated;
}

/* Declaration-order neighbor search among focusable widgets of the top
 * scope. `step` is +1 (down) or -1 (up); wraps. Returns -1 when none. */
static int ui_next_focusable(const AeronUiContext* ctx, int from, int step, int top_scope) {
	const int count = ctx->widget_count;
	if (count <= 0) {
		return -1;
	}
	int index = from;
	for (int walked = 0; walked < count; walked++) {
		index += step;
		if (index < 0) {
			index = count - 1;
		} else if (index >= count) {
			index = 0;
		}
		const UiWidgetRec* w = &ctx->widgets[index];
		if (w->focusable && w->scope == top_scope) {
			return index;
		}
	}
	return -1;
}

static int ui_find_widget(const AeronUiContext* ctx, AeronUiId id) {
	for (int i = 0; i < ctx->widget_count; i++) {
		if (ctx->widgets[i].id == id) {
			return i;
		}
	}
	return -1;
}

/* Geometric column hop: nearest focusable in an adjacent column of the
 * same columns run, preferring vertical overlap. */
static int ui_column_hop(const AeronUiContext* ctx, int from, int direction, int top_scope) {
	const UiWidgetRec* cur = &ctx->widgets[from];
	if (cur->col_run < 0) {
		return -1;
	}
	const float cy        = cur->rect.y + cur->rect.h * 0.5f;
	int         best      = -1;
	float       best_cost = 0.0f;
	for (int i = 0; i < ctx->widget_count; i++) {
		const UiWidgetRec* w = &ctx->widgets[i];
		if (!w->focusable || w->scope != top_scope || w->col_run != cur->col_run ||
			w->column != cur->column + direction) {
			continue;
		}
		const float wy   = w->rect.y + w->rect.h * 0.5f;
		const float cost = fabsf(wy - cy);
		if (best < 0 || cost < best_cost) {
			best      = i;
			best_cost = cost;
		}
	}
	return best;
}

/* Scrolls the focused widget's region so the widget lands in view. */
static void ui_scroll_focus_into_view(AeronUiContext* ctx, const UiWidgetRec* focused) {
	if (!focused->scroll_id) {
		return;
	}
	const UiScrollRec* scroll = NULL;
	for (int i = 0; i < ctx->scroll_count; i++) {
		if (ctx->scrolls[i].id == focused->scroll_id) {
			scroll = &ctx->scrolls[i];
			break;
		}
	}
	UiStateSlot* slot = scroll ? ui_state(ctx, scroll->id) : NULL;
	if (!slot) {
		return;
	}
	const float view_top    = scroll->view_y;
	const float view_bottom = scroll->view_y + scroll->view_h;
	float       offset      = slot->v0;
	if (focused->rect.y < view_top) {
		offset -= view_top - focused->rect.y;
	} else if (focused->rect.y + focused->rect.h > view_bottom) {
		offset += (focused->rect.y + focused->rect.h) - view_bottom;
	}
	const float max_offset = fmaxf(0.0f, scroll->content_h - scroll->view_h);
	slot->v0               = fminf(fmaxf(offset, 0.0f), max_offset);
}

void ui_resolve_navigation(AeronUiContext* ctx) {
	int top_scope = 0;
	for (int i = 0; i < ctx->widget_count; i++) {
		if (ctx->widgets[i].scope > top_scope) {
			top_scope = ctx->widgets[i].scope;
		}
	}
	ctx->top_scope_prev = ctx->modal_open_id ? top_scope : 0;

	if (!ctx->any_window || ctx->widget_count == 0) {
		ctx->focus_id         = 0;
		ctx->rebind_id        = 0;
		ctx->rebind_capturing = 0;
		ctx->text_edit_id     = 0;
		return;
	}

	/* A capture whose widget vanished (screen change) must not keep
	 * capture_all latched. */
	if (ctx->rebind_id && ui_find_widget(ctx, ctx->rebind_id) < 0) {
		ctx->rebind_id        = 0;
		ctx->rebind_capturing = 0;
	}
	if (ctx->text_edit_id && ui_find_widget(ctx, ctx->text_edit_id) < 0) {
		ctx->text_edit_id = 0;
	}

	/* Focus validity: the focused widget must exist, be focusable, and
	 * live in the top scope; otherwise focus the first eligible one. */
	int focus_index = ui_find_widget(ctx, ctx->focus_id);
	if (focus_index >= 0 &&
		(!ctx->widgets[focus_index].focusable || ctx->widgets[focus_index].scope != top_scope)) {
		focus_index = -1;
	}
	if (focus_index < 0) {
		focus_index   = ui_next_focusable(ctx, -1, 1, top_scope);
		ctx->focus_id = focus_index >= 0 ? ctx->widgets[focus_index].id : 0;
	}
	if (focus_index < 0) {
		return;
	}

	/* Leftover vertical navigation: declaration order (columns declare
	 * column-major, so this walks down a column then into the next). */
	int moves = ctx->nav[UI_DIR_DOWN] - ctx->nav[UI_DIR_UP];
	while (moves != 0) {
		const int next = ui_next_focusable(ctx, focus_index, moves > 0 ? 1 : -1, top_scope);
		if (next < 0) {
			break;
		}
		focus_index = next;
		moves += moves > 0 ? -1 : 1;
	}

	/* Leftover horizontal navigation: column hop. */
	int hops = ctx->nav[UI_DIR_RIGHT] - ctx->nav[UI_DIR_LEFT];
	while (hops != 0) {
		const int next = ui_column_hop(ctx, focus_index, hops > 0 ? 1 : -1, top_scope);
		if (next < 0) {
			break;
		}
		focus_index = next;
		hops += hops > 0 ? -1 : 1;
	}

	ctx->focus_id = ctx->widgets[focus_index].id;
	if (ctx->text_edit_id && ctx->text_edit_id != ctx->focus_id) {
		ctx->text_edit_id = 0;
	}
	ui_scroll_focus_into_view(ctx, &ctx->widgets[focus_index]);

	if (ctx->focus_id != ctx->focus_at_frame_start) {
		ui_play_sound(ctx, AERON_UI_SOUND_FOCUS);
	}
}
