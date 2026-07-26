#include "internal.h"

static int Aeron_ClampInt(int value, int min_value, int max_value) {
	if (value < min_value) {
		return min_value;
	}

	if (value > max_value) {
		return max_value;
	}

	return value;
}

static int Aeron_ScaleAxisToLogical(int value, int source_size, int target_size) {
	if (source_size > 0 && source_size != target_size) {
		value = (int)(((int64_t)value * target_size) / source_size);
	}

	return Aeron_ClampInt(value, 0, target_size - 1);
}

static void Aeron_ClearMouseActivity(AeronInputSnapshot* input) {
	input->mouse.relative_x             = 0.0f;
	input->mouse.relative_y             = 0.0f;
	input->mouse.buttons                = 0;
	input->mouse.pressed_buttons        = 0;
	input->mouse.released_buttons       = 0;
	input->mouse.double_clicked_buttons = 0;
}

static int Aeron_UpdateRawMouseFromPlatform(AeronInputSnapshot* input) {
	float global_x;
	float global_y;
	int   window_x;
	int   window_y;

	SDL_GetGlobalMouseState(&global_x, &global_y);
	if (!SDL_GetWindowPosition(g_aeron.window, &window_x, &window_y)) {
		return 0;
	}

	input->mouse.raw_x = (int)(global_x - (float)window_x);
	input->mouse.raw_y = (int)(global_y - (float)window_y);
	return 1;
}

static int Aeron_RawMouseInsideWindow(const AeronInputSnapshot* input) {
	return input->window_width > 0 && input->window_height > 0 && input->mouse.raw_x >= 0 &&
		   input->mouse.raw_y >= 0 && input->mouse.raw_x < input->window_width &&
		   input->mouse.raw_y < input->window_height;
}

static void Aeron_UpdateLogicalMouse(AeronInputSnapshot* input, const SDL_Rect* content_rect,
									 int has_mouse_position) {
	int content_mouse_x;
	int content_mouse_y;

	if (g_aeron.relative_mouse_enabled) {
		input->mouse.inside_content = input->has_focus;
		if (!input->has_focus) {
			Aeron_ClearMouseActivity(input);
		}
		return;
	}

	if (!has_mouse_position) {
		input->mouse.inside_content = 0;
		Aeron_ClearMouseActivity(input);
		return;
	}

	input->mouse.inside_content =
		content_rect->w > 0 && content_rect->h > 0 && input->mouse.raw_x >= content_rect->x &&
		input->mouse.raw_y >= content_rect->y && input->mouse.raw_x < content_rect->x + content_rect->w &&
		input->mouse.raw_y < content_rect->y + content_rect->h;
	if (!input->mouse.inside_content) {
		Aeron_ClearMouseActivity(input);
		return;
	}

	content_mouse_x = input->mouse.raw_x - content_rect->x;
	content_mouse_y = input->mouse.raw_y - content_rect->y;
	input->mouse.x  = Aeron_ScaleAxisToLogical(content_mouse_x, content_rect->w, g_aeron.logical_width);
	input->mouse.y  = Aeron_ScaleAxisToLogical(content_mouse_y, content_rect->h, g_aeron.logical_height);
	/* relative_x/y stay the event-accumulated raw deltas in both pointer
	 * modes; consumers wanting logical-space deltas diff x/y instead. */
}

int32_t Aeron_BeginFrame(void) {
	SDL_Event event;
	SDL_Rect  content_rect;
	uint64_t  window_flags;
	int       has_platform_position;
	int       has_mouse_position;
	uint64_t  now_us;
	uint64_t  delta_us;

	if (!g_aeron.initialized) {
		return 0;
	}

	/* Throttle before sampling input so time spent waiting for FIFO
	 * presentation does not age the state used to build this frame. */
	if (!Aeron_WaitForPresentationSlot()) {
		Aeron_RequestFatalRendererError("waiting for the next presentation slot");
		return 0;
	}

	Aeron_ClearRenderSubmissions();
	SDL_zero(g_aeron.render_data_stats);
	Aeron_BeginInputFrame(&g_aeron.input);
	while (SDL_PollEvent(&event)) {
		if (Aeron_DebugUiHandleEvent(&event)) {
			continue;
		}
		Aeron_HandleEvent(&event);
	}
	/* Flip the swapchain composition here, between the event pump and any
	 * frame work, so it never runs while a pass or the overlay is recording. */
	Aeron_ApplyPendingOutputHdr();
	Aeron_UpdateGamepads(&g_aeron.input);
	Aeron_UpdatePresentationPixelSize();
	SDL_GetWindowSize(g_aeron.window, &g_aeron.input.window_width, &g_aeron.input.window_height);
	Aeron_ComputePresentationRect(g_aeron.input.window_width, g_aeron.input.window_height, &content_rect);
	window_flags            = SDL_GetWindowFlags(g_aeron.window);
	g_aeron.input.has_focus = (window_flags & SDL_WINDOW_INPUT_FOCUS) != 0;
	has_platform_position   = Aeron_UpdateRawMouseFromPlatform(&g_aeron.input);
	has_mouse_position      = has_platform_position && Aeron_RawMouseInsideWindow(&g_aeron.input);
	Aeron_UpdateLogicalMouse(&g_aeron.input, &content_rect, has_mouse_position);
	Aeron_DebugUiFilterInput(&g_aeron.input);

	now_us   = Aeron_NowUs();
	delta_us = now_us - g_aeron.last_frame_us;
	if (delta_us > AERON_MAX_DELTA_US) {
		delta_us = AERON_MAX_DELTA_US;
	}
	g_aeron.last_frame_us = now_us;

	return (int32_t)delta_us;
}
