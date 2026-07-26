#include "internal.h"

int Aeron_WindowInit(const AeronConfig* config) {
	const char* title;
	int         width;
	int         height;

	title                     = config && config->window_title ? config->window_title : "Aeron";
	width                     = config && config->window_width > 0 ? config->window_width : 0;
	height                    = config && config->window_height > 0 ? config->window_height : 0;
	g_aeron.presentation_mode = config ? config->presentation_mode : AERON_PRESENTATION_STRETCH;
	if (g_aeron.presentation_mode != AERON_PRESENTATION_ASPECT_FIT) {
		g_aeron.presentation_mode = AERON_PRESENTATION_STRETCH;
	}

	/* Auto-size when the config gives no explicit window size: ~90% of
	 * the primary display's usable area (excludes menu bar / taskbar),
	 * locked to the logical aspect ratio. Falls back to 640x480. */
	if (width <= 0 || height <= 0) {
		SDL_Rect      bounds;
		SDL_DisplayID display   = SDL_GetPrimaryDisplay();
		int           logical_w = config && config->logical_width > 0 ? config->logical_width : 640;
		int           logical_h = config && config->logical_height > 0 ? config->logical_height : 480;

		if (display != 0 && SDL_GetDisplayUsableBounds(display, &bounds) && bounds.w > 0 && bounds.h > 0) {
			int target_h = (bounds.h * 9) / 10;
			int target_w = (int)(((int64_t)target_h * logical_w) / logical_h);
			int max_w    = (bounds.w * 9) / 10;
			if (target_w > max_w) {
				target_w = max_w;
				target_h = (int)(((int64_t)target_w * logical_h) / logical_w);
			}
			width  = target_w & ~1;
			height = target_h & ~1;
		} else {
			width  = 640;
			height = 480;
		}
	}

	g_aeron.logical_width  = config && config->logical_width > 0 ? config->logical_width : width;
	g_aeron.logical_height = config && config->logical_height > 0 ? config->logical_height : height;

	/* HIGH_PIXEL_DENSITY: on Retina / HiDPI displays the window size is
	 * interpreted as logical points while the GPU swapchain allocates at
	 * physical pixels (e.g. 2x the point size). Without it the swapchain
	 * is point-sized and the OS upscales — visibly soft output. All
	 * Aeron mouse/window math stays in points; only Aeron_Present's
	 * swapchain-measured presentation rect sees pixels. */
	g_aeron.window =
		SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!g_aeron.window) {
		Aeron_Log("aeron", "SDL_CreateWindow failed: %s", SDL_GetError());
		return 0;
	}

	{
		int pixel_w = 0;
		int pixel_h = 0;
		SDL_GetWindowSizeInPixels(g_aeron.window, &pixel_w, &pixel_h);
		Aeron_UpdatePresentationPixelSize();
		Aeron_Log("aeron", "window %dx%d pt, %dx%d px (logical %dx%d)", width, height, pixel_w, pixel_h,
				  g_aeron.logical_width, g_aeron.logical_height);
	}

	if (g_aeron.presentation_mode == AERON_PRESENTATION_ASPECT_FIT) {
		const float aspect = (float)g_aeron.logical_width / (float)g_aeron.logical_height;
		if (!SDL_SetWindowAspectRatio(g_aeron.window, aspect, aspect)) {
			Aeron_Log("aeron", "SDL_SetWindowAspectRatio failed: %s", SDL_GetError());
		}
	}

	return 1;
}

void Aeron_WindowShutdown(void) {
	if (g_aeron.window) {
		SDL_DestroyWindow(g_aeron.window);
		g_aeron.window = NULL;
	}
}

int Aeron_SetLogicalSize(int width, int height) {
	if (width <= 0 || height <= 0) {
		return 0;
	}

	g_aeron.logical_width  = width;
	g_aeron.logical_height = height;
	Aeron_UpdatePresentationPixelSize();
	return 1;
}

int Aeron_SetFullscreen(int fullscreen) {
	int current;

	if (!g_aeron.window) {
		return 0;
	}
	fullscreen = fullscreen != 0;
	current    = (SDL_GetWindowFlags(g_aeron.window) & SDL_WINDOW_FULLSCREEN) != 0;
	if (current == fullscreen) {
		return 1;
	}
	if (!SDL_SetWindowFullscreen(g_aeron.window, fullscreen)) {
		Aeron_Log("aeron", "SDL_SetWindowFullscreen failed: %s", SDL_GetError());
		return 0;
	}

	Aeron_UpdatePresentationPixelSize();
	Aeron_RefreshPresentationTiming();
	return 1;
}

int Aeron_Fullscreen(void) {
	return g_aeron.window && (SDL_GetWindowFlags(g_aeron.window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void Aeron_UpdatePresentationPixelSize(void) {
	SDL_Rect content_rect = { 0 };
	int      pixel_width  = 0;
	int      pixel_height = 0;

	if (g_aeron.window && SDL_GetWindowSizeInPixels(g_aeron.window, &pixel_width, &pixel_height)) {
		Aeron_ComputePresentationRect(pixel_width, pixel_height, &content_rect);
	}
	g_aeron.presentation_pixel_width  = content_rect.w;
	g_aeron.presentation_pixel_height = content_rect.h;
}

int Aeron_GetPresentationPixelSize(int* width, int* height) {
	if (width) {
		*width = g_aeron.presentation_pixel_width;
	}
	if (height) {
		*height = g_aeron.presentation_pixel_height;
	}
	return g_aeron.presentation_pixel_width > 0 && g_aeron.presentation_pixel_height > 0;
}

int Aeron_SetHostCursorVisible(int visible) {
	if (visible) {
		return SDL_ShowCursor() ? 1 : 0;
	}

	return SDL_HideCursor() ? 1 : 0;
}

int Aeron_WarpMouseLogical(int x, int y) {
	SDL_Rect content_rect;
	int      window_width;
	int      window_height;
	int      raw_x;
	int      raw_y;

	if (!g_aeron.window || g_aeron.logical_width <= 0 || g_aeron.logical_height <= 0) {
		return 0;
	}

	SDL_GetWindowSize(g_aeron.window, &window_width, &window_height);
	Aeron_ComputePresentationRect(window_width, window_height, &content_rect);
	if (content_rect.w <= 0 || content_rect.h <= 0) {
		return 0;
	}

	raw_x = content_rect.x + (int)(((int64_t)x * content_rect.w) / g_aeron.logical_width);
	raw_y = content_rect.y + (int)(((int64_t)y * content_rect.h) / g_aeron.logical_height);
	SDL_WarpMouseInWindow(g_aeron.window, (float)raw_x, (float)raw_y);

	g_aeron.input.window_width         = window_width;
	g_aeron.input.window_height        = window_height;
	g_aeron.input.mouse.raw_x          = raw_x;
	g_aeron.input.mouse.raw_y          = raw_y;
	g_aeron.input.mouse.x              = x;
	g_aeron.input.mouse.y              = y;
	g_aeron.input.mouse.inside_content = 1;
	g_aeron.input.mouse.relative_x     = 0;
	g_aeron.input.mouse.relative_y     = 0;
	return 1;
}

int Aeron_SetRelativeMouseMode(int enabled) {
	enabled = enabled != 0;
	if (g_aeron.relative_mouse_enabled == enabled) {
		return 1;
	}

	if (!g_aeron.window) {
		return 0;
	}

	if (!SDL_SetWindowRelativeMouseMode(g_aeron.window, enabled)) {
		Aeron_Log("aeron", "SDL_SetWindowRelativeMouseMode failed: %s", SDL_GetError());
		return 0;
	}

	g_aeron.relative_mouse_enabled = enabled;
	Aeron_SetHostCursorVisible(0);
	return 1;
}

int Aeron_RelativeMouseMode(void) { return g_aeron.relative_mouse_enabled; }

void Aeron_ComputePresentationRect(int container_width, int container_height, SDL_Rect* rect) {
	int64_t content_width;
	int64_t content_height;

	if (!rect) {
		return;
	}

	if (container_width <= 0 || container_height <= 0) {
		rect->x = 0;
		rect->y = 0;
		rect->w = 0;
		rect->h = 0;
		return;
	}

	content_width  = container_width;
	content_height = container_height;
	if (g_aeron.presentation_mode == AERON_PRESENTATION_ASPECT_FIT) {
		if (content_width * g_aeron.logical_height > content_height * g_aeron.logical_width) {
			content_width = (content_height * g_aeron.logical_width) / g_aeron.logical_height;
		} else {
			content_height = (content_width * g_aeron.logical_height) / g_aeron.logical_width;
		}
	}

	if (content_width < 1) {
		content_width = 1;
	}
	if (content_height < 1) {
		content_height = 1;
	}

	rect->w = (int)content_width;
	rect->h = (int)content_height;
	rect->x = (container_width - rect->w) / 2;
	rect->y = (container_height - rect->h) / 2;
}
