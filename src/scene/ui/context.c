/*
 * AeronUi context lifecycle, identity, transient state, and the
 * per-frame begin/end pair. See internal.h for the architecture.
 */

#include "internal.h"

#include <stdlib.h>

/* FNV-1a over the label, seeded by the id stack so identical labels in
 * different scopes stay distinct. */
static uint32_t ui_fnv1a(uint32_t seed, const void* data, size_t size) {
	const uint8_t* bytes = (const uint8_t*)data;
	uint32_t       hash  = seed;
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

AeronUiId ui_make_id(AeronUiContext* ctx, const char* label) {
	const uint32_t seed = ctx->id_stack_depth > 0 ? ctx->id_stack[ctx->id_stack_depth - 1] : 2166136261u;
	if (!label) {
		label = "";
	}
	AeronUiId id = ui_fnv1a(seed, label, strlen(label));
	return id ? id : 1u; /* 0 is the empty-slot sentinel */
}

const char* ui_label_text(const char* label) { return label ? label : ""; }

int ui_label_text_len(const char* label) {
	if (!label) {
		return 0;
	}
	const char* cut = strstr(label, "##");
	return cut ? (int)(cut - label) : (int)strlen(label);
}

UiStateSlot* ui_state(AeronUiContext* ctx, AeronUiId id) {
	const uint32_t start = id % AERON_UI_STATE_SLOTS;
	UiStateSlot*   best  = NULL;

	for (uint32_t probe = 0; probe < 8; probe++) {
		UiStateSlot* slot = &ctx->state[(start + probe) % AERON_UI_STATE_SLOTS];
		if (slot->id == id) {
			slot->frame = ctx->frame_id;
			return slot;
		}
		if (slot->id == 0) {
			best = slot;
			break;
		}
		/* Evict the least recently touched slot in the probe window,
		 * but never one in use this frame. */
		if (slot->frame != ctx->frame_id && (!best || slot->frame < best->frame)) {
			best = slot;
		}
	}
	if (!best) {
		return NULL;
	}
	best->id    = id;
	best->v0    = 0.0f;
	best->v1    = 0.0f;
	best->frame = ctx->frame_id;
	return best;
}

void ui_play_sound(AeronUiContext* ctx, AeronUiSound sound) {
	if (ctx->sound_fn) {
		ctx->sound_fn(sound, ctx->sound_user);
	}
}

const AeronFontAtlas* ui_font_regular(const AeronUiContext* ctx) { return ctx->fonts.regular; }

const AeronFontAtlas* ui_font_title(const AeronUiContext* ctx) {
	return ctx->fonts.title ? ctx->fonts.title : ctx->fonts.regular;
}

float ui_font_tracking_atlas(const AeronUiContext* ctx, const AeronFontAtlas* font) {
	if (ctx->fonts.title && font == ctx->fonts.title) {
		return ctx->fonts.title_tracking_atlas;
	}
	return ctx->fonts.regular_tracking_atlas;
}

AeronUiContext* AeronUi_Create(const AeronUiDesc* desc) {
	AeronUiContext* ctx = (AeronUiContext*)calloc(1, sizeof *ctx);
	if (!ctx) {
		return NULL;
	}
	const int max_widgets = desc && desc->max_widgets > 0 ? desc->max_widgets : AERON_UI_DEFAULT_WIDGETS;
	const int record_cap =
		desc && desc->draw_record_cap > 0 ? desc->draw_record_cap : AERON_UI_DEFAULT_RECORDS;

	ctx->max_widgets  = max_widgets;
	ctx->sound_fn     = desc ? desc->sound_fn : NULL;
	ctx->sound_user   = desc ? desc->sound_user : NULL;
	ctx->widgets      = (UiWidgetRec*)calloc((size_t)max_widgets, sizeof *ctx->widgets);
	ctx->prev_widgets = (UiWidgetRec*)calloc((size_t)max_widgets, sizeof *ctx->prev_widgets);
	ctx->list         = AeronDrawList_Create(record_cap);
	if (!ctx->widgets || !ctx->prev_widgets || !ctx->list) {
		AeronUi_Destroy(ctx);
		return NULL;
	}
	ctx->theme    = *AeronUi_DefaultTheme();
	ctx->frame_id = 1;
	return ctx;
}

void AeronUi_Destroy(AeronUiContext* ctx) {
	if (!ctx) {
		return;
	}
	if (ctx->fallback_rt) {
		Aeron_DestroyRenderTarget(ctx->fallback_rt);
	}
	AeronDrawList_Destroy(ctx->list);
	free(ctx->prev_widgets);
	free(ctx->widgets);
	free(ctx);
}

void AeronUi_SetTheme(AeronUiContext* ctx, const AeronUiTheme* theme) {
	if (ctx && theme) {
		ctx->theme = *theme;
	}
}

void AeronUi_SetFonts(AeronUiContext* ctx, const AeronUiFontSet* fonts) {
	if (ctx && fonts) {
		ctx->fonts = *fonts;
	}
}

/* Maps the snapshot's logical mouse position into content pixels. When
 * the engine has no logical size (headless tests), coordinates pass
 * through unmapped. */
static void ui_update_mouse(AeronUiContext* ctx) {
	const AeronMouseSnapshot* mouse = &ctx->input->mouse;
	int                       logical_w, logical_h;
	float                     x = (float)mouse->x;
	float                     y = (float)mouse->y;

	if (Aeron_GetLogicalSize(&logical_w, &logical_h)) {
		x = x * (float)ctx->out_w / (float)logical_w;
		y = y * (float)ctx->out_h / (float)logical_h;
	}
	ctx->mouse_present = mouse->inside_content;
	ctx->mouse_moved   = fabsf(x - ctx->last_mouse_x) > 0.5f || fabsf(y - ctx->last_mouse_y) > 0.5f;
	ctx->last_mouse_x  = x;
	ctx->last_mouse_y  = y;
	ctx->mouse_x       = x;
	ctx->mouse_y       = y;
}

void AeronUi_BeginFrame(AeronUiContext* ctx, const AeronUiFrameDesc* frame) {
	if (!ctx || !frame || !frame->input || ctx->frame_active) {
		if (ctx && ctx->frame_active) {
			Aeron_LogWarn("aeron.scene", "AeronUi_BeginFrame without EndFrame");
		}
		return;
	}
	ctx->frame_active = 1;
	ctx->input        = frame->input;
	ctx->dt           = frame->dt_seconds > 0.0f ? frame->dt_seconds : 0.0f;

	/* Output space: explicit (offline tests) or the engine content rect. */
	if (frame->output_w > 0 && frame->output_h > 0) {
		ctx->out_w        = frame->output_w;
		ctx->out_h        = frame->output_h;
		ctx->content_rect = (AeronRectI) { 0, 0, ctx->out_w, ctx->out_h };
		ctx->direct_path  = 0;
		ctx->offline      = 1;
	} else {
		if (!Aeron_GetContentPixelRect(&ctx->content_rect)) {
			ctx->content_rect = (AeronRectI) { 0, 0, 1920, 1080 };
		}
		ctx->out_w       = ctx->content_rect.width;
		ctx->out_h       = ctx->content_rect.height;
		ctx->direct_path = Aeron_CanRenderDirectToSwapchain(ctx->out_w, ctx->out_h);
		ctx->offline     = 0;
	}
	ctx->scale = (float)ctx->out_h / AERON_UI_REFERENCE_H;
	if (ctx->scale <= 0.0f) {
		ctx->scale = 1.0f;
	}

	/* Swap the widget index double buffer. */
	UiWidgetRec* swap      = ctx->prev_widgets;
	ctx->prev_widgets      = ctx->widgets;
	ctx->prev_widget_count = ctx->widget_count;
	ctx->widgets           = swap;
	ctx->widget_count      = 0;
	ctx->widget_dropped    = 0;

	/* Per-frame declaration state. */
	ctx->id_stack_depth  = 0;
	ctx->layout_depth    = 0;
	ctx->window_depth    = 0;
	ctx->next_col_run    = 0;
	ctx->scope_depth     = 0;
	ctx->modal_open_id   = 0;
	ctx->hot_id          = 0;
	ctx->any_window      = 0;
	ctx->cancel_consumed = 0;
	ctx->value_changed   = 0;
	ctx->scroll_count    = 0;

	/* Open fade: restart when the UI was absent last frame. */
	if (ctx->theme.open_fade_s > 0.0f) {
		const float step = ctx->dt / ctx->theme.open_fade_s;
		ctx->fade = ctx->any_window_prev ? fminf(1.0f, ctx->fade + step) : fminf(1.0f, fmaxf(step, 0.10f));
	} else {
		ctx->fade = 1.0f;
	}

	ui_update_mouse(ctx);
	ui_collect_input(ctx);

	/* The draw list records in content-pixel space. The direct path
	 * replays into the borrowed swapchain pass (no target); the fallback
	 * path latches its content-rect render target here. */
	AeronRenderTarget* target = NULL;
	if (!ctx->offline && !ctx->direct_path) {
		target = ui_submit_ensure_fallback_rt(ctx);
	}
	AeronDrawList_Begin(ctx->list, target, ctx->out_w, ctx->out_h, AERON_DRAWLIST2D_CLEAR, NULL);
}

AeronUiOutput AeronUi_EndFrame(AeronUiContext* ctx) {
	AeronUiOutput out = { 0 };

	if (!ctx || !ctx->frame_active) {
		return out;
	}
	if (ctx->layout_depth != 0 || ctx->window_depth != 0) {
		Aeron_LogWarn("aeron.scene", "AeronUi_EndFrame with unbalanced containers (%d/%d)", ctx->layout_depth,
					  ctx->window_depth);
		ctx->layout_depth = 0;
		ctx->window_depth = 0;
	}
	if (ctx->id_stack_depth != 0) {
		Aeron_LogWarn("aeron.scene", "AeronUi_EndFrame with unbalanced PushId (%d)", ctx->id_stack_depth);
		ctx->id_stack_depth = 0;
	}

	ui_resolve_navigation(ctx);

	out.wants_keyboard = ctx->any_window;
	out.wants_mouse    = ctx->any_window;
	out.wants_gamepad  = ctx->any_window;
	out.capture_all    = ctx->rebind_capturing;
	if (ctx->any_window && ctx->nav_cancel > 0 && !ctx->cancel_consumed) {
		out.cancel_pressed = 1;
		ui_play_sound(ctx, AERON_UI_SOUND_CANCEL);
	}

	ctx->any_window_prev = ctx->any_window;
	ctx->frame_active    = 0;
	ctx->frame_id++;
	return out;
}

void AeronUi_PushId(AeronUiContext* ctx, int id) {
	if (!ctx || ctx->id_stack_depth >= AERON_UI_ID_STACK_CAP) {
		if (ctx) {
			Aeron_LogWarn("aeron.scene", "ui id stack cap (%d) hit", AERON_UI_ID_STACK_CAP);
		}
		return;
	}
	const uint32_t seed = ctx->id_stack_depth > 0 ? ctx->id_stack[ctx->id_stack_depth - 1] : 2166136261u;
	ctx->id_stack[ctx->id_stack_depth++] = ui_fnv1a(seed, &id, sizeof id);
}

void AeronUi_PopId(AeronUiContext* ctx) {
	if (ctx && ctx->id_stack_depth > 0) {
		ctx->id_stack_depth--;
	}
}
