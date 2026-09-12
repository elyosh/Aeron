/* Physical keyboard chord capture; the application owns binding policy. */
#include "internal.h"

#include <stdio.h>

#define UI_KEYBOARD_CAPTURE_TIMEOUT_S 5.0f

void AeronUi_CancelKeyboardCapture(AeronUiContext* ctx) {
	if (ctx)
		memset(&ctx->keyboard_capture, 0, sizeof ctx->keyboard_capture);
}

int AeronUi_KeyboardCaptureActive(const AeronUiContext* ctx) {
	return ctx && (ctx->keyboard_capture.id || ctx->keyboard_capture_frame);
}

static int keyboard_all_released(const AeronInputSnapshot* input) {
	for (int key = 0; key < AERON_KEY_COUNT; ++key)
		if (input->key_down[key])
			return 0;
	return 1;
}

static AeronUiKeyboardCaptureResult keyboard_capture_update(AeronUiContext* ctx, AeronKeyChord* out) {
	UiKeyboardCaptureState*   state = &ctx->keyboard_capture;
	const AeronInputSnapshot* input = ctx->input;
	state->elapsed += ctx->dt;
	if (!input->has_focus || input->key_events_overflow || input->key_pressed[AERON_KEY_ESCAPE] ||
		state->elapsed >= UI_KEYBOARD_CAPTURE_TIMEOUT_S)
		return AERON_UI_KEYBOARD_CAPTURE_CANCELLED;
	if (state->activation_frame == input->frame_id)
		return AERON_UI_KEYBOARD_CAPTURE_NONE;
	if (!state->armed) {
		if (keyboard_all_released(input))
			state->armed = 1;
		return AERON_UI_KEYBOARD_CAPTURE_NONE;
	}
	for (uint16_t i = 0; i < input->key_event_count; ++i) {
		const AeronKeyEvent* event = &input->key_events[i];
		if (event->repeat)
			continue;
		const uint8_t modifier = AeronKey_Modifier((AeronKey)event->chord.key);
		if (event->down && !modifier) {
			*out = event->chord;
			return AERON_UI_KEYBOARD_CAPTURE_CAPTURED;
		}
		if (event->down) {
			if (state->candidate || (event->chord.modifiers & (uint8_t)~modifier))
				state->multiple_modifiers = 1;
			else
				state->candidate = event->chord.key;
		} else if (event->chord.key == state->candidate && !state->multiple_modifiers) {
			*out = (AeronKeyChord) { .key = state->candidate };
			return AERON_UI_KEYBOARD_CAPTURE_CAPTURED;
		}
	}
	if (keyboard_all_released(input)) {
		state->candidate          = 0;
		state->multiple_modifiers = 0;
	}
	return AERON_UI_KEYBOARD_CAPTURE_NONE;
}

AeronUiKeyboardCaptureResult AeronUi_KeyboardCapture(AeronUiContext* ctx, const char* label,
													 const char* display, AeronKeyChord* out) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !out || !ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row))
		return AERON_UI_KEYBOARD_CAPTURE_NONE;
	const AeronUiId              id     = ui_make_id(ctx, label);
	const UiLayout*              top    = ui_layout_top(ctx);
	const AeronRectI*            clip   = top && top->clip.width > 0 ? &top->clip : NULL;
	AeronUiKeyboardCaptureResult result = AERON_UI_KEYBOARD_CAPTURE_NONE;
	if (ctx->keyboard_capture.id == id) {
		ctx->keyboard_capture_frame = 1;
		result                      = keyboard_capture_update(ctx, out);
		if (result == AERON_UI_KEYBOARD_CAPTURE_CAPTURED || result == AERON_UI_KEYBOARD_CAPTURE_CANCELLED) {
			AeronUi_CancelKeyboardCapture(ctx);
			ui_play_sound(ctx, result == AERON_UI_KEYBOARD_CAPTURE_CAPTURED ? AERON_UI_SOUND_ACCEPT
																			: AERON_UI_SOUND_CANCEL);
		}
		memset(ctx->nav, 0, sizeof ctx->nav);
		ctx->nav_accept = ctx->nav_cancel = ctx->nav_tab_prev = ctx->nav_tab_next = 0;
		ctx->cancel_consumed                                                      = 1;
		ui_record_widget(ctx, id, &row, 1);
	} else if (!ui_capture_active(ctx) && ui_widget_behavior(ctx, id, &row, 1)) {
		AeronUi_CancelControllerCapture(ctx);
		ctx->keyboard_capture = (UiKeyboardCaptureState) {
			.id               = id,
			.activation_frame = ctx->input->frame_id,
			.armed            = keyboard_all_released(ctx->input),
		};
		ctx->text_edit_id = 0;
		memset(ctx->nav, 0, sizeof ctx->nav);
		ctx->keyboard_capture_frame = 1;
		ctx->nav_accept = ctx->nav_cancel = ctx->nav_tab_prev = ctx->nav_tab_next = 0;
		ctx->cancel_consumed                                                      = 1;
		result = AERON_UI_KEYBOARD_CAPTURE_STARTED;
		ui_play_sound(ctx, AERON_UI_SOUND_ACCEPT);
	} else {
		ui_record_widget(ctx, id, &row, 1);
	}
	const int active = ctx->keyboard_capture.id == id;
	if (ui_is_focused(ctx, id) && !active)
		ui_draw_row_focus_bg(ctx, &row, clip);
	const UiRect control = ui_form_row_split(ctx, label, &row, clip);
	const int    hot     = ctx->hot_id == id;
	ui_draw_surface(ctx, &control, active || hot ? ctx->theme.widget_bg_hot : ctx->theme.widget_bg,
					ctx->theme.widget_bg_low, ctx->theme.widget_gradient && !active,
					active ? ctx->theme.focus_outline : ctx->theme.widget_border, ctx->theme.widget_border_px,
					1, clip);
	char        prompt[64];
	const char* shown = display ? display : "-";
	if (active) {
		const float remaining = fmaxf(0.0f, UI_KEYBOARD_CAPTURE_TIMEOUT_S - ctx->keyboard_capture.elapsed);
		snprintf(prompt, sizeof prompt, "%s (%.0fs)",
				 ctx->keyboard_capture.armed ? "Press a key combination" : "Release keys", (double)remaining);
		shown = prompt;
	}
	const float text_px = ui_ref(ctx, ctx->theme.text_px);
	ui_draw_text(ctx, ui_font_regular(ctx), control.x + control.w * 0.5f, row.y + (row.h - text_px) * 0.5f,
				 AERON_TEXT_CENTER, text_px, active ? ctx->theme.accent : ctx->theme.text, shown, -1, clip);
	return result;
}
