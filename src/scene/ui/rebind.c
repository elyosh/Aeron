/* Selected-controller capture widget. */

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>

#define UI_CONTROLLER_CAPTURE_TIMEOUT_S 5.0f
#define UI_CONTROLLER_CAPTURE_AXIS_DELTA 16384
#define UI_CONTROLLER_CAPTURE_AXIS_HALF 16384

static const AeronControllerSnapshot* capture_find_controller(const AeronUiContext* ctx,
															  uint32_t              instance_id) {
	for (int slot = 0; slot < AERON_CONTROLLER_MAX; ++slot) {
		const AeronControllerSnapshot* controller = &ctx->input->controllers[slot];
		if (controller->connected && controller->instance_id == instance_id)
			return controller;
	}
	return NULL;
}

static int capture_axis_count(const AeronControllerSnapshot* controller) {
	return controller->kind == AERON_CONTROLLER_KIND_GAMEPAD    ? AERON_GAMEPAD_AXIS_COUNT
		   : controller->axis_count < AERON_CONTROLLER_AXIS_MAX ? controller->axis_count
																: AERON_CONTROLLER_AXIS_MAX;
}

static int capture_axis_available(const AeronControllerSnapshot* controller, int axis) {
	return controller->kind != AERON_CONTROLLER_KIND_GAMEPAD ||
		   (controller->gamepad_available_axes & (1u << axis)) != 0;
}

static int16_t capture_axis_value(const AeronControllerSnapshot* controller, int axis) {
	return controller->kind == AERON_CONTROLLER_KIND_GAMEPAD ? controller->gamepad_axes[axis]
															 : controller->raw_axes[axis];
}

static int capture_start(AeronUiContext* ctx, AeronUiId id, const AeronUiControllerCaptureDesc* desc) {
	const AeronControllerSnapshot* controller = capture_find_controller(ctx, desc->instance_id);
	if (!controller || controller->kind == AERON_CONTROLLER_KIND_NONE)
		return 0;
	UiControllerCaptureState* state = &ctx->controller_capture;
	memset(state, 0, sizeof *state);
	state->id              = id;
	state->instance_id     = controller->instance_id;
	state->controller_kind = controller->kind;
	state->mode            = desc->mode;
	const int axis_count   = capture_axis_count(controller);
	for (int axis = 0; axis < axis_count; ++axis)
		state->axis_baseline[axis] = capture_axis_value(controller, axis);
	if (controller->kind == AERON_CONTROLLER_KIND_JOYSTICK) {
		const int hat_count = controller->hat_count < AERON_CONTROLLER_HAT_MAX ? controller->hat_count
																			   : AERON_CONTROLLER_HAT_MAX;
		for (int hat = 0; hat < hat_count; ++hat)
			state->hat_armed[hat] = controller->raw_hats[hat] == AERON_CONTROLLER_HAT_CENTERED;
	}
	return 1;
}

static int capture_analog_axis(const AeronControllerSnapshot*  controller,
							   const UiControllerCaptureState* state, AeronUiControllerInput* out) {
	int       best_axis  = -1;
	int       best_delta = 0;
	const int axis_count = capture_axis_count(controller);
	for (int axis = 0; axis < axis_count; ++axis) {
		if (!capture_axis_available(controller, axis))
			continue;
		const int delta = abs((int)capture_axis_value(controller, axis) - (int)state->axis_baseline[axis]);
		if (delta > best_delta) {
			best_delta = delta;
			best_axis  = axis;
		}
	}
	if (best_axis < 0 || best_delta < UI_CONTROLLER_CAPTURE_AXIS_DELTA)
		return 0;
	out->controller_kind = controller->kind;
	out->value.axis      = best_axis;
	return 1;
}

static int capture_button(const AeronControllerSnapshot* controller, AeronUiControllerInput* out) {
	if (controller->kind == AERON_CONTROLLER_KIND_GAMEPAD) {
		const uint32_t pressed = controller->gamepad_pressed_buttons & controller->gamepad_available_buttons;
		for (int button = 0; button < AERON_GAMEPAD_BUTTON_COUNT; ++button) {
			if (!(pressed & (1u << button)))
				continue;
			out->controller_kind = controller->kind;
			out->value.digital =
				(AeronControllerDigitalSource) { AERON_CONTROLLER_DIGITAL_BUTTON, (uint8_t)button, 0, 0.5f };
			return 1;
		}
	} else {
		const int count = controller->button_count < AERON_CONTROLLER_BUTTON_MAX
							  ? controller->button_count
							  : AERON_CONTROLLER_BUTTON_MAX;
		for (int button = 0; button < count; ++button) {
			if (!(controller->raw_pressed_buttons & (UINT64_C(1) << button)))
				continue;
			out->controller_kind = controller->kind;
			out->value.digital =
				(AeronControllerDigitalSource) { AERON_CONTROLLER_DIGITAL_BUTTON, (uint8_t)button, 0, 0.5f };
			return 1;
		}
	}
	return 0;
}

static int capture_digital_axis(const AeronControllerSnapshot*  controller,
								const UiControllerCaptureState* state, AeronUiControllerInput* out) {
	int       best_axis  = -1;
	int       best_delta = 0;
	int       best_value = 0;
	const int axis_count = capture_axis_count(controller);
	for (int axis = 0; axis < axis_count; ++axis) {
		if (!capture_axis_available(controller, axis))
			continue;
		const int value = capture_axis_value(controller, axis);
		const int delta = abs(value - (int)state->axis_baseline[axis]);
		if (abs(value) >= UI_CONTROLLER_CAPTURE_AXIS_HALF && delta > best_delta) {
			best_axis  = axis;
			best_delta = delta;
			best_value = value;
		}
	}
	if (best_axis < 0 || best_delta < UI_CONTROLLER_CAPTURE_AXIS_DELTA)
		return 0;
	out->controller_kind = controller->kind;
	out->value.digital =
		(AeronControllerDigitalSource) { best_value < 0 ? AERON_CONTROLLER_DIGITAL_AXIS_NEGATIVE
														: AERON_CONTROLLER_DIGITAL_AXIS_POSITIVE,
										 (uint8_t)best_axis, 0, 0.5f };
	return 1;
}

static int capture_hat(const AeronControllerSnapshot* controller, UiControllerCaptureState* state,
					   AeronUiControllerInput* out) {
	if (controller->kind != AERON_CONTROLLER_KIND_JOYSTICK)
		return 0;
	static const uint8_t directions[] = { AERON_CONTROLLER_HAT_UP, AERON_CONTROLLER_HAT_RIGHT,
										  AERON_CONTROLLER_HAT_DOWN, AERON_CONTROLLER_HAT_LEFT };
	const int            count =
		controller->hat_count < AERON_CONTROLLER_HAT_MAX ? controller->hat_count : AERON_CONTROLLER_HAT_MAX;
	for (int hat = 0; hat < count; ++hat) {
		const uint8_t value = controller->raw_hats[hat];
		if (value == AERON_CONTROLLER_HAT_CENTERED) {
			state->hat_armed[hat] = 1;
			continue;
		}
		if (!state->hat_armed[hat])
			continue;
		for (size_t direction = 0; direction < sizeof directions / sizeof directions[0]; ++direction) {
			if (!(value & directions[direction]))
				continue;
			out->controller_kind = controller->kind;
			out->value.digital = (AeronControllerDigitalSource) { AERON_CONTROLLER_DIGITAL_HAT, (uint8_t)hat,
																  directions[direction], 0.5f };
			return 1;
		}
	}
	return 0;
}

void AeronUi_CancelControllerCapture(AeronUiContext* ctx) {
	if (ctx)
		memset(&ctx->controller_capture, 0, sizeof ctx->controller_capture);
}

AeronUiControllerCaptureResult AeronUi_ControllerCapture(AeronUiContext* ctx, const char* label,
														 const char*                         display,
														 const AeronUiControllerCaptureDesc* desc,
														 AeronUiControllerInput*             out) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !desc || !out ||
		!ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row))
		return AERON_UI_CONTROLLER_CAPTURE_NONE;
	const AeronUiId           id     = ui_make_id(ctx, label);
	const UiLayout*           top    = ui_layout_top(ctx);
	const AeronRectI*         clip   = top && top->clip.width > 0 ? &top->clip : NULL;
	UiControllerCaptureState* state  = &ctx->controller_capture;
	const int                 active = state->id == id;
	const int enabled = desc->instance_id != 0 && capture_find_controller(ctx, desc->instance_id) != NULL;
	AeronUiControllerCaptureResult result = AERON_UI_CONTROLLER_CAPTURE_NONE;

	if (active) {
		ctx->controller_capture_frame             = 1;
		const AeronControllerSnapshot* controller = capture_find_controller(ctx, state->instance_id);
		state->elapsed += ctx->dt;
		if (desc->instance_id != state->instance_id || desc->mode != state->mode || !controller ||
			controller->kind != state->controller_kind || ctx->input->key_pressed[AERON_KEY_ESCAPE] ||
			state->elapsed >= UI_CONTROLLER_CAPTURE_TIMEOUT_S) {
			AeronUi_CancelControllerCapture(ctx);
			result = AERON_UI_CONTROLLER_CAPTURE_CANCELLED;
			ui_play_sound(ctx, AERON_UI_SOUND_CANCEL);
		} else {
			const int captured = state->mode == AERON_UI_CONTROLLER_CAPTURE_ANALOG_AXIS
									 ? capture_analog_axis(controller, state, out)
									 : capture_button(controller, out) ||
										   capture_digital_axis(controller, state, out) ||
										   capture_hat(controller, state, out);
			if (captured) {
				AeronUi_CancelControllerCapture(ctx);
				result = AERON_UI_CONTROLLER_CAPTURE_CAPTURED;
				ui_play_sound(ctx, AERON_UI_SOUND_ACCEPT);
			}
		}
		memset(ctx->nav, 0, sizeof ctx->nav);
		ctx->nav_accept      = 0;
		ctx->nav_cancel      = 0;
		ctx->cancel_consumed = 1;
		ui_record_widget(ctx, id, &row, 1);
	} else if (enabled && ui_widget_behavior(ctx, id, &row, 1)) {
		if (capture_start(ctx, id, desc)) {
			result                        = AERON_UI_CONTROLLER_CAPTURE_STARTED;
			ctx->controller_capture_frame = 1;
			ui_play_sound(ctx, AERON_UI_SOUND_ACCEPT);
		}
	} else if (!enabled) {
		ui_record_widget(ctx, id, &row, 0);
	}

	const int active_now = ctx->controller_capture.id == id;
	if (ui_is_focused(ctx, id) && !active_now)
		ui_draw_row_focus_bg(ctx, &row, clip);
	const UiRect control = ui_form_row_split(ctx, label, &row, clip);
	const int    hot     = enabled && ctx->hot_id == id;
	ui_draw_surface(ctx, &control, active_now || hot ? ctx->theme.widget_bg_hot : ctx->theme.widget_bg,
					ctx->theme.widget_bg_low, ctx->theme.widget_gradient && !active_now,
					active_now ? ctx->theme.focus_outline : ctx->theme.widget_border,
					ctx->theme.widget_border_px, 1, clip);
	char        prompt[64];
	const char* shown = display ? display : "-";
	if (active_now) {
		const float remaining =
			fmaxf(0.0f, UI_CONTROLLER_CAPTURE_TIMEOUT_S - ctx->controller_capture.elapsed);
		snprintf(prompt, sizeof prompt, "%s (%.0fs)",
				 desc->mode == AERON_UI_CONTROLLER_CAPTURE_ANALOG_AXIS ? "Move an axis" : "Press a control",
				 (double)remaining);
		shown = prompt;
	}
	const float text_px = ui_ref(ctx, ctx->theme.text_px);
	ui_draw_text(ctx, ui_font_regular(ctx), control.x + control.w * 0.5f, row.y + (row.h - text_px) * 0.5f,
				 AERON_TEXT_CENTER, text_px,
				 active_now ? ctx->theme.accent
				 : enabled  ? ctx->theme.text
							: ctx->theme.text_dim,
				 shown, -1, clip);
	if (ui_is_focused(ctx, id) && !active_now)
		ui_draw_row_focus_ring(ctx, &row, clip);
	return result;
}
