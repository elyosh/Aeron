/*
 * AeronUi value widgets — toggle, sliders, selector. Each renders as a
 * form row: label in the left portion (theme.label_fraction), control
 * in the remainder. All values are game-owned; calls return nonzero the
 * frame the value changes.
 */

#include "internal.h"

#include <stdio.h>

UiRect ui_form_row_split(AeronUiContext* ctx, const char* label, const UiRect* row, const AeronRectI* clip) {
	/* Content is inset from the row bounds on every side so the focus
	 * bar/ring (drawn at the row bounds) keeps a uniform margin around
	 * labels and controls. Constant — layout never shifts with focus. */
	const float  pad_x   = ui_snap(ui_ref(ctx, 8.0f));
	const float  pad_y   = ui_snap(ui_ref(ctx, 3.0f));
	const UiRect content = { row->x + pad_x, row->y + pad_y, row->w - 2.0f * pad_x, row->h - 2.0f * pad_y };

	const float fraction = fminf(fmaxf(ctx->theme.label_fraction, 0.1f), 0.9f);
	const float label_w  = ui_snap(content.w * fraction);
	const float text_px  = ui_ref(ctx, ctx->theme.text_px);

	ui_draw_text(ctx, ui_font_regular(ctx), content.x, content.y + (content.h - text_px) * 0.5f,
				 AERON_TEXT_LEFT, text_px, ctx->theme.text, ui_label_text(label), ui_label_text_len(label),
				 clip);

	UiRect control = { content.x + label_w, content.y, content.w - label_w, content.h };
	return control;
}

static const AeronRectI* ui_current_clip(AeronUiContext* ctx) {
	const UiLayout* top = ui_layout_top(ctx);
	return (top && top->clip.width > 0) ? &top->clip : NULL;
}

/* Wheel steps while the mouse hovers the row (natural wheel direction:
 * up = increase). */
static int ui_wheel_steps(AeronUiContext* ctx, const UiRect* rect, const AeronRectI* clip) {
	if (!ui_mouse_in(ctx, rect, clip) || ctx->scope_depth < ctx->top_scope_prev) {
		return 0;
	}
	return ctx->input->mouse.wheel_y;
}

int AeronUi_Toggle(AeronUiContext* ctx, const char* label, int* value) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !value ||
		!ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row)) {
		return 0;
	}
	const AeronUiId   id      = ui_make_id(ctx, label);
	const AeronRectI* clip    = ui_current_clip(ctx);
	const int         initial = *value != 0;
	int               state   = initial;

	if (ui_widget_behavior(ctx, id, &row, 1)) {
		state = !state;
	}
	const int adjust = ui_consume_adjust(ctx, id);
	if (adjust < 0) {
		state = 0;
	} else if (adjust > 0) {
		state = 1;
	}

	if (ui_is_focused(ctx, id)) {
		ui_draw_row_focus_bg(ctx, &row, clip);
	}
	const UiRect control = ui_form_row_split(ctx, label, &row, clip);

	/* Switch: pill track + sliding knob, right-aligned in the control. */
	const float  track_h = ui_snap(fminf(row.h * 0.62f, ui_ref(ctx, 22.0f)));
	const float  track_w = ui_snap(track_h * 1.9f);
	const float  pad     = ui_snap((track_h - fmaxf(track_h - ui_ref(ctx, 6.0f), 4.0f)) * 0.5f);
	const float  knob    = track_h - pad * 2.0f;
	const UiRect track   = { control.x + control.w - track_w, row.y + (row.h - track_h) * 0.5f, track_w,
							 track_h };

	AeronDrawList2DRRect r = { 0 };
	r.dst_x                = track.x;
	r.dst_y                = track.y;
	r.dst_w                = track.w;
	r.dst_h                = track.h;
	r.radius_px            = track_h * 0.5f;
	r.border_px            = fmaxf(1.0f, ui_ref(ctx, ctx->theme.widget_border_px));
	r.soft_px              = 1.0f;
	ui_pma(ctx, state ? ctx->theme.accent : ctx->theme.widget_bg, r.fill_top);
	memcpy(r.fill_bottom, r.fill_top, sizeof r.fill_bottom);
	ui_pma(ctx, ctx->theme.widget_border, r.border);
	if (clip) {
		r.scissor = *clip;
	}
	AeronDrawList_AddRRect(ctx->list, &r);

	AeronDrawList2DRRect k = { 0 };
	k.dst_x                = state ? track.x + track.w - pad - knob : track.x + pad;
	k.dst_y                = track.y + pad;
	k.dst_w                = knob;
	k.dst_h                = knob;
	k.radius_px            = knob * 0.5f;
	k.soft_px              = 1.0f;
	ui_pma(ctx, ctx->theme.text, k.fill_top);
	memcpy(k.fill_bottom, k.fill_top, sizeof k.fill_bottom);
	if (clip) {
		k.scissor = *clip;
	}
	AeronDrawList_AddRRect(ctx->list, &k);

	/* State text keeps the switch readable at a glance. */
	const float text_px = ui_ref(ctx, ctx->theme.text_px);
	ui_draw_text(ctx, ui_font_regular(ctx), track.x - ui_ref(ctx, 10.0f), row.y + (row.h - text_px) * 0.5f,
				 AERON_TEXT_RIGHT, text_px, state ? ctx->theme.text : ctx->theme.text_dim,
				 state ? "On" : "Off", -1, clip);

	if (ui_is_focused(ctx, id)) {
		ui_draw_row_focus_ring(ctx, &row, clip);
	}
	if (state != initial) {
		*value = state;
		ui_play_sound(ctx, AERON_UI_SOUND_ADJUST);
		return 1;
	}
	return 0;
}

/* Shared slider core over a normalized 0..1 position. Returns the new
 * position after keyboard steps / drag / wheel, and renders. */
static float ui_slider_core(AeronUiContext* ctx, AeronUiId id, const char* label, const UiRect* row,
							const AeronRectI* clip, float t, float step_t, const char* value_text) {
	ui_widget_behavior(ctx, id, row, 1);

	if (ui_is_focused(ctx, id)) {
		ui_draw_row_focus_bg(ctx, row, clip);
	}
	const UiRect control = ui_form_row_split(ctx, label, row, clip);
	const float  value_w = ui_snap(fminf(control.w * 0.35f, ui_ref(ctx, 110.0f)));
	const UiRect zone    = { control.x, control.y, control.w - value_w - ui_ref(ctx, 10.0f), control.h };

	/* Interactions. */
	const int adjust = ui_consume_adjust(ctx, id);
	t += (float)adjust * step_t;
	if (ui_is_focused(ctx, id)) {
		if (ctx->input->key_pressed[AERON_KEY_HOME]) {
			t = 0.0f;
		}
		if (ctx->input->key_pressed[AERON_KEY_END]) {
			t = 1.0f;
		}
	}
	t += (float)ui_wheel_steps(ctx, row, clip) * step_t;
	if (ctx->active_id == id && ctx->mouse_present && zone.w > 4.0f) {
		t = (ctx->mouse_x - zone.x) / zone.w;
	}
	t = fminf(fmaxf(t, 0.0f), 1.0f);

	/* Track, fill, knob. */
	const float  track_px = fmaxf(2.0f, ui_ref(ctx, ctx->theme.slider_track_px));
	const float  knob_w   = ui_snap(ui_ref(ctx, 10.0f));
	const float  knob_h   = ui_snap(fminf(row->h * 0.62f, ui_ref(ctx, 22.0f)));
	const float  usable   = zone.w - knob_w;
	const float  knob_x   = zone.x + usable * t;
	const UiRect track    = { zone.x, zone.y + (zone.h - track_px) * 0.5f, zone.w, track_px };
	const UiRect fill     = { track.x, track.y, knob_x - zone.x + knob_w * 0.5f, track_px };
	ui_draw_fill(ctx, &track, ctx->theme.slider_track, clip);
	ui_draw_fill(ctx, &fill, ctx->theme.accent, clip);

	AeronDrawList2DRRect k = { 0 };
	k.dst_x                = ui_snap(knob_x);
	k.dst_y                = ui_snap(zone.y + (zone.h - knob_h) * 0.5f);
	k.dst_w                = knob_w;
	k.dst_h                = knob_h;
	k.radius_px            = ui_ref(ctx, ctx->theme.corner_radius);
	k.border_px            = fmaxf(1.0f, ui_ref(ctx, ctx->theme.widget_border_px));
	k.soft_px              = 1.0f;
	ui_pma(ctx, ctx->theme.text, k.fill_top);
	memcpy(k.fill_bottom, k.fill_top, sizeof k.fill_bottom);
	ui_pma(ctx, ctx->theme.widget_border, k.border);
	if (clip) {
		k.scissor = *clip;
	}
	AeronDrawList_AddRRect(ctx->list, &k);

	const float text_px = ui_ref(ctx, ctx->theme.text_px);
	ui_draw_text(ctx, ui_font_regular(ctx), control.x + control.w, row->y + (row->h - text_px) * 0.5f,
				 AERON_TEXT_RIGHT, text_px, ctx->theme.accent, value_text, -1, clip);
	if (ui_is_focused(ctx, id)) {
		ui_draw_row_focus_ring(ctx, row, clip);
	}
	return t;
}

int AeronUi_SliderInt(AeronUiContext* ctx, const char* label, int* value, int min, int max, int step,
					  const char* fmt) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !value || max <= min ||
		!ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row)) {
		return 0;
	}
	if (step <= 0) {
		step = 1;
	}
	const AeronUiId   id      = ui_make_id(ctx, label);
	const AeronRectI* clip    = ui_current_clip(ctx);
	const int         initial = *value;
	char              text[48];
	snprintf(text, sizeof text, fmt ? fmt : "%d", initial);

	const float range  = (float)(max - min);
	const float t      = ((float)initial - (float)min) / range;
	const float step_t = (float)step / range;
	const float new_t  = ui_slider_core(ctx, id, label, &row, clip, t, step_t, text);

	/* Snap to the step grid. */
	int result = min + (int)((new_t * range) / (float)step + 0.5f) * step;
	result     = result < min ? min : (result > max ? max : result);
	if (result != initial) {
		*value = result;
		ui_play_sound(ctx, AERON_UI_SOUND_ADJUST);
		return 1;
	}
	return 0;
}

int AeronUi_SliderFloat(AeronUiContext* ctx, const char* label, float* value, float min, float max,
						float step, const char* fmt) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !value || max <= min ||
		!ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row)) {
		return 0;
	}
	if (step <= 0.0f) {
		step = (max - min) / 100.0f;
	}
	const AeronUiId   id      = ui_make_id(ctx, label);
	const AeronRectI* clip    = ui_current_clip(ctx);
	const float       initial = *value;
	char              text[48];
	snprintf(text, sizeof text, fmt ? fmt : "%.2f", (double)initial);

	const float range  = max - min;
	const float t      = (initial - min) / range;
	const float step_t = step / range;
	const float new_t  = ui_slider_core(ctx, id, label, &row, clip, t, step_t, text);

	float result = min + floorf((new_t * range) / step + 0.5f) * step;
	result       = fminf(fmaxf(result, min), max);
	if (fabsf(result - initial) > step * 0.25f) {
		*value = result;
		ui_play_sound(ctx, AERON_UI_SOUND_ADJUST);
		return 1;
	}
	return 0;
}

int AeronUi_Selector(AeronUiContext* ctx, const char* label, int* index, const char* const* options,
					 int count) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !index || !options || count <= 0 ||
		!ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row)) {
		return 0;
	}
	const AeronUiId   id      = ui_make_id(ctx, label);
	const AeronRectI* clip    = ui_current_clip(ctx);
	const int         initial = *index < 0 ? 0 : (*index >= count ? count - 1 : *index);
	int               result  = initial;

	const int activated = ui_widget_behavior(ctx, id, &row, 1);

	if (ui_is_focused(ctx, id)) {
		ui_draw_row_focus_bg(ctx, &row, clip);
	}
	const UiRect control = ui_form_row_split(ctx, label, &row, clip);
	const float  arrow_w = ui_snap(fminf(control.w * 0.15f, row.h));

	int steps = ui_consume_adjust(ctx, id);
	steps -= ui_wheel_steps(ctx, &row, clip); /* wheel down = next */
	if (activated) {
		/* Click: the left arrow zone cycles back, anywhere else forward.
		 * Keyboard accept cycles forward. */
		const int in_left = ctx->mouse_present && ctx->hot_id == id && ctx->mouse_x < control.x + arrow_w;
		steps += in_left ? -1 : 1;
	}
	if (steps != 0) {
		result = (initial + steps) % count;
		if (result < 0) {
			result += count;
		}
	}

	/* Body + arrows + centered value. The row-focus bar carries the
	 * focused look; the body only brightens on mouse hover. */
	const int hot = ctx->hot_id == id;
	ui_draw_surface(ctx, &control, hot ? ctx->theme.widget_bg_hot : ctx->theme.widget_bg,
					ctx->theme.widget_bg_low, ctx->theme.widget_gradient, ctx->theme.widget_border,
					ctx->theme.widget_border_px, 1, clip);

	const float text_px = ui_ref(ctx, ctx->theme.text_px);
	const float text_y  = row.y + (row.h - text_px) * 0.5f;
	ui_draw_text(ctx, ui_font_regular(ctx), control.x + arrow_w * 0.5f, text_y, AERON_TEXT_CENTER, text_px,
				 ctx->theme.text_dim, "<", -1, clip);
	ui_draw_text(ctx, ui_font_regular(ctx), control.x + control.w - arrow_w * 0.5f, text_y, AERON_TEXT_CENTER,
				 text_px, ctx->theme.text_dim, ">", -1, clip);
	ui_draw_text(ctx, ui_font_regular(ctx), control.x + control.w * 0.5f, text_y, AERON_TEXT_CENTER, text_px,
				 ctx->theme.text, options[result], -1, clip);

	if (ui_is_focused(ctx, id)) {
		ui_draw_row_focus_ring(ctx, &row, clip);
	}
	if (result != initial) {
		*index = result;
		ui_play_sound(ctx, AERON_UI_SOUND_ADJUST);
		return 1;
	}
	return 0;
}

int AeronUi_SegmentedSelector(AeronUiContext* ctx, const char* id_label, int* index,
							  const char* const* options, int count) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !index || !options || count <= 0 ||
		!ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row)) {
		return 0;
	}
	const AeronUiId   id        = ui_make_id(ctx, id_label);
	const AeronRectI* clip      = ui_current_clip(ctx);
	const int         initial   = *index < 0 ? 0 : (*index >= count ? count - 1 : *index);
	int               result    = initial;
	const int         activated = ui_widget_behavior(ctx, id, &row, 1);
	const int         adjust    = ui_consume_adjust(ctx, id);

	if (adjust < 0)
		result = initial > 0 ? initial - 1 : 0;
	else if (adjust > 0)
		result = initial + 1 < count ? initial + 1 : count - 1;
	if (activated) {
		if (ctx->mouse_present && ctx->hot_id == id) {
			const float segment_w = row.w / (float)count;
			result                = (int)((ctx->mouse_x - row.x) / segment_w);
			result                = result < 0 ? 0 : (result >= count ? count - 1 : result);
		} else {
			result = (initial + 1) % count;
		}
	}

	if (ui_is_focused(ctx, id))
		ui_draw_row_focus_bg(ctx, &row, clip);
	const float segment_w = row.w / (float)count;
	const float text_px   = ui_ref(ctx, ctx->theme.text_px);
	for (int option = 0; option < count; ++option) {
		const UiRect segment  = { ui_snap(row.x + segment_w * (float)option), row.y, ui_snap(segment_w),
								  row.h };
		const int    selected = option == result;
		const int    hovered  = ctx->hot_id == id && ui_mouse_in(ctx, &segment, clip);
		ui_draw_surface(ctx, &segment, selected || hovered ? ctx->theme.widget_bg_hot : ctx->theme.widget_bg,
						ctx->theme.widget_bg_low, ctx->theme.widget_gradient && !selected,
						ctx->theme.widget_border, ctx->theme.widget_border_px, selected, clip);
		if (selected) {
			const UiRect accent = { segment.x, segment.y + segment.h - ui_ref(ctx, 3.0f), segment.w,
									ui_ref(ctx, 3.0f) };
			ui_draw_fill(ctx, &accent, ctx->theme.accent, clip);
		}
		ui_draw_text(ctx, ui_font_regular(ctx), segment.x + segment.w * 0.5f,
					 segment.y + (segment.h - text_px) * 0.5f, AERON_TEXT_CENTER, text_px,
					 selected ? ctx->theme.text : ctx->theme.text_dim, ui_label_text(options[option]),
					 ui_label_text_len(options[option]), clip);
	}
	if (ui_is_focused(ctx, id))
		ui_draw_row_focus_ring(ctx, &row, clip);
	if (result != initial) {
		*index = result;
		ui_play_sound(ctx, AERON_UI_SOUND_ADJUST);
		return 1;
	}
	return 0;
}

void AeronUi_ControllerAxisMeter(AeronUiContext* ctx, const char* label, float value, float deadzone) {
	UiRect row;
	if (!ctx || !ctx->frame_active || !ui_layout_row(ctx, ui_ref(ctx, ctx->theme.row_height), &row))
		return;
	if (!isfinite(value))
		value = 0.0f;
	if (!isfinite(deadzone))
		deadzone = 0.0f;
	value                     = fminf(fmaxf(value, -1.0f), 1.0f);
	deadzone                  = fminf(fmaxf(deadzone, 0.0f), 1.0f);
	const AeronRectI* clip    = ui_current_clip(ctx);
	const UiRect      control = ui_form_row_split(ctx, label, &row, clip);
	const float       value_w = ui_snap(fminf(control.w * 0.28f, ui_ref(ctx, 90.0f)));
	const UiRect      zone    = { control.x, control.y, control.w - value_w - ui_ref(ctx, 10.0f), control.h };
	const float       track_h = ui_snap(fminf(zone.h * 0.5f, ui_ref(ctx, 14.0f)));
	const UiRect      track   = { zone.x, zone.y + (zone.h - track_h) * 0.5f, zone.w, track_h };
	ui_draw_fill(ctx, &track, ctx->theme.slider_track, clip);
	const float  center = track.x + track.w * 0.5f;
	const UiRect dead   = { center - track.w * 0.5f * deadzone, track.y, track.w * deadzone, track.h };
	ui_draw_fill(ctx, &dead, ctx->theme.widget_bg_hot, clip);
	const UiRect center_line = { ui_snap(center), track.y, fmaxf(1.0f, ui_ref(ctx, 1.0f)), track.h };
	ui_draw_fill(ctx, &center_line, ctx->theme.separator, clip);
	const float  marker_x = center + value * track.w * 0.5f;
	const UiRect marker   = { ui_snap(marker_x - ui_ref(ctx, 2.0f)), track.y - ui_ref(ctx, 3.0f),
							  fmaxf(3.0f, ui_ref(ctx, 4.0f)), track.h + ui_ref(ctx, 6.0f) };
	ui_draw_fill(ctx, &marker, ctx->theme.accent, clip);
	char text[32];
	snprintf(text, sizeof text, "%+.2f", (double)value);
	const float text_px = ui_ref(ctx, ctx->theme.text_px);
	ui_draw_text(ctx, ui_font_regular(ctx), control.x + control.w, row.y + (row.h - text_px) * 0.5f,
				 AERON_TEXT_RIGHT, text_px, ctx->theme.accent, text, -1, clip);
}
