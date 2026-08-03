/*
 * AeronUi rebind capture widget. Activation arms a capture; the next
 * discrete input (key, mouse button, pad button, or pad axis push) is
 * reported back raw. While capturing, EndFrame reports capture_all so
 * the game suppresses every input path.
 *
 * Transient state rides one UiStateSlot: v0 = capture time elapsed,
 * v1 = float-encoded bitmask of "armed" pad axes (an axis must be seen
 * near rest once during the capture before it can trigger, so a drifted
 * or held stick cannot instantly self-capture). 4 pads x 6 axes = 24
 * bits — exact in a float.
 */

#include "internal.h"

#include <stdlib.h> /* abs */

#define UI_REBIND_TIMEOUT_S 5.0f
#define UI_AXIS_CAPTURE 0x6000 /* 75% of 32767 */
#define UI_AXIS_REST 0x2000    /* 25% */

/* Scans for the first captured input. Returns nonzero and fills `out`. */
static int ui_rebind_scan(AeronUiContext* ctx, uint32_t* armed_mask, AeronUiCapturedInput* out) {
	const AeronInputSnapshot* in = ctx->input;

	for (int key = 0; key < AERON_KEY_COUNT; key++) {
		if (key == AERON_KEY_ESCAPE || !in->key_pressed[key]) {
			continue;
		}
		out->kind = AERON_UI_CAPTURE_KEY;
		out->code = key;
		return 1;
	}
	for (int bit = 0; bit < 5; bit++) {
		if (in->mouse.pressed_buttons & (1u << bit)) {
			out->kind = AERON_UI_CAPTURE_MOUSE_BUTTON;
			out->code = bit;
			return 1;
		}
	}
	for (int pad = 0; pad < AERON_CONTROLLER_MAX; pad++) {
		const AeronControllerSnapshot* controller = &in->controllers[pad];
		if (!controller->connected) {
			continue;
		}
		for (int button = 0; button < 32; button++) {
			if (controller->gamepad_pressed_buttons & (1u << button)) {
				out->kind       = AERON_UI_CAPTURE_PAD_BUTTON;
				out->code       = button;
				out->controller = pad;
				return 1;
			}
		}
		for (int axis = 0; axis < AERON_GAMEPAD_AXIS_COUNT; axis++) {
			const int     bit   = pad * AERON_GAMEPAD_AXIS_COUNT + axis;
			const int16_t value = controller->gamepad_axes[axis];
			if (abs(value) < UI_AXIS_REST) {
				*armed_mask |= 1u << bit;
				continue;
			}
			if ((*armed_mask & (1u << bit)) && abs(value) >= UI_AXIS_CAPTURE) {
				out->kind           = AERON_UI_CAPTURE_PAD_AXIS;
				out->code           = axis;
				out->controller     = pad;
				out->axis_direction = value < 0 ? -1 : 1;
				return 1;
			}
		}
	}
	return 0;
}

int AeronUi_Rebind(AeronUiContext* ctx, const char* label, const char* display, AeronUiCapturedInput* out) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !out || !ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row)) {
		return AERON_UI_REBIND_NONE;
	}
	const AeronUiId   id        = ui_make_id(ctx, label);
	const UiLayout*   top       = ui_layout_top(ctx);
	const AeronRectI* clip      = (top && top->clip.width > 0) ? &top->clip : NULL;
	UiStateSlot*      slot      = ui_state(ctx, id);
	const int         capturing = ctx->rebind_id == id;
	int               result    = AERON_UI_REBIND_NONE;

	if (capturing && slot) {
		/* Same-frame grace: the activating input must not self-capture. */
		slot->v0 += ctx->dt;
		uint32_t armed = (uint32_t)slot->v1;
		if (ctx->input->key_pressed[AERON_KEY_ESCAPE] || slot->v0 >= UI_REBIND_TIMEOUT_S) {
			ctx->rebind_id = 0;
			result         = AERON_UI_REBIND_CANCELLED;
			ui_play_sound(ctx, AERON_UI_SOUND_CANCEL);
		} else if (slot->v0 > 0.0f && ui_rebind_scan(ctx, &armed, out)) {
			ctx->rebind_id = 0;
			result         = AERON_UI_REBIND_CAPTURED;
			ui_play_sound(ctx, AERON_UI_SOUND_ACCEPT);
		}
		slot->v1 = (float)armed;
		/* Swallow this frame's navigation while capturing. */
		memset(ctx->nav, 0, sizeof ctx->nav);
		ctx->nav_accept = 0;
		ctx->nav_cancel = 0;
		ui_record_widget(ctx, id, &row, 1);
	} else if (ui_widget_behavior(ctx, id, &row, 1)) {
		ctx->rebind_id = id;
		if (slot) {
			slot->v0 = 0.0f;
			slot->v1 = 0.0f;
		}
		result = AERON_UI_REBIND_STARTED;
		ui_play_sound(ctx, AERON_UI_SOUND_ACCEPT);
	}
	ctx->rebind_capturing = ctx->rebind_id != 0;

	/* Draw: form row with the binding (or the capture prompt). The
	 * row-focus bar carries the focused look; capture mode brightens
	 * the control and swaps its border to the focus color. */
	const int active_now = ctx->rebind_id == id;
	if (ui_is_focused(ctx, id) && !active_now) {
		ui_draw_row_focus_bg(ctx, &row, clip);
	}
	const UiRect control = ui_form_row_split(ctx, label, &row, clip);
	const int    hot     = ctx->hot_id == id;
	ui_draw_surface(ctx, &control,
					active_now || hot ? ctx->theme.widget_bg_hot : ctx->theme.widget_bg,
					ctx->theme.widget_bg_low, ctx->theme.widget_gradient && !active_now,
					active_now ? ctx->theme.focus_outline : ctx->theme.widget_border,
					ctx->theme.widget_border_px, 1, clip);
	const float text_px = ui_ref(ctx, ctx->theme.text_px);
	ui_draw_text(ctx, ui_font_regular(ctx), control.x + control.w * 0.5f, row.y + (row.h - text_px) * 0.5f,
				 AERON_TEXT_CENTER, text_px, active_now ? ctx->theme.accent : ctx->theme.text,
				 active_now ? "press an input..." : (display ? display : "-"), -1, clip);
	if (ui_is_focused(ctx, id) && !active_now) {
		ui_draw_row_focus_ring(ctx, &row, clip);
	}
	return result;
}
