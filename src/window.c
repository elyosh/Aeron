#include "internal.h"

#include <limits.h>
#include <math.h>

static int Aeron_CalculateAspectWidth(int height, int aspect_width, int aspect_height, int* out_width) {
	int64_t width;
	if (!out_width || height <= 0 || aspect_width <= 0 || aspect_height <= 0) {
		return 0;
	}
	width = ((int64_t)height * aspect_width + aspect_height / 2) / aspect_height;
	if (width <= 0 || width > INT_MAX) {
		return 0;
	}
	*out_width = (int)width;
	return 1;
}

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
	SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (config && config->window_hidden) {
		window_flags |= SDL_WINDOW_HIDDEN;
	}
	g_aeron.window = SDL_CreateWindow(title, width, height, window_flags);
	if (!g_aeron.window) {
		Aeron_LogError("aeron", "SDL_CreateWindow failed: %s", SDL_GetError());
		return 0;
	}

	/* Application-supplied window/taskbar icon on Windows and Linux. Not
	 * applied on macOS: SDL implements it as [NSApp setApplicationIconImage:],
	 * which would replace the bundle icon in the Dock and app switcher with
	 * this much lower-resolution image. */
#ifndef SDL_PLATFORM_MACOS
	if (config && config->window_icon_bmp && config->window_icon_bmp_size > 0) {
		SDL_Surface* icon =
			SDL_LoadBMP_IO(SDL_IOFromConstMem(config->window_icon_bmp, config->window_icon_bmp_size), true);
		if (icon) {
			if (!SDL_SetWindowIcon(g_aeron.window, icon)) {
				Aeron_LogWarn("aeron", "SDL_SetWindowIcon failed: %s", SDL_GetError());
			}
			SDL_DestroySurface(icon);
		} else {
			Aeron_LogWarn("aeron", "window icon decode failed: %s", SDL_GetError());
		}
	}
#endif

	{
		int pixel_w = 0;
		int pixel_h = 0;
		SDL_GetWindowSizeInPixels(g_aeron.window, &pixel_w, &pixel_h);
		Aeron_UpdatePresentationPixelSize();
		const char* video_driver = SDL_GetCurrentVideoDriver();
		Aeron_LogInfo("aeron", "window %dx%d pt, %dx%d px (logical %dx%d), video driver %s", width, height,
					  pixel_w, pixel_h, g_aeron.logical_width, g_aeron.logical_height,
					  video_driver ? video_driver : "unknown");
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

int Aeron_GetLogicalSize(int* width, int* height) {
	if (width) {
		*width = g_aeron.logical_width;
	}
	if (height) {
		*height = g_aeron.logical_height;
	}
	return g_aeron.logical_width > 0 && g_aeron.logical_height > 0;
}

int Aeron_GetWindowSize(int* width, int* height) {
	int window_width  = 0;
	int window_height = 0;

	if (!g_aeron.window || !SDL_GetWindowSize(g_aeron.window, &window_width, &window_height)) {
		return 0;
	}
	if (width) {
		*width = window_width;
	}
	if (height) {
		*height = window_height;
	}
	return window_width > 0 && window_height > 0;
}

static int Aeron_ApplyWindowAspectRatio(void) {
	Uint64 flags;
	float  ratio;

	if (!g_aeron.window || g_aeron.window_aspect_width <= 0 || g_aeron.window_aspect_height <= 0) {
		return 0;
	}
	flags = SDL_GetWindowFlags(g_aeron.window);
	if ((flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) || g_aeron.window_spanned) {
		g_aeron.window_aspect_pending = 1;
		return 1;
	}

	ratio                         = (float)g_aeron.window_aspect_width / (float)g_aeron.window_aspect_height;
	g_aeron.window_aspect_pending = 0;
	if (!SDL_SetWindowAspectRatio(g_aeron.window, ratio, ratio)) {
		Aeron_LogWarn("aeron", "SDL_SetWindowAspectRatio failed: %s", SDL_GetError());
		return 0;
	}
	return 1;
}

int Aeron_SetWindowAspectRatio(int aspect_width, int aspect_height) {
	if (!g_aeron.window || aspect_width <= 0 || aspect_height <= 0) {
		return 0;
	}
	g_aeron.window_aspect_width   = aspect_width;
	g_aeron.window_aspect_height  = aspect_height;
	g_aeron.window_aspect_pending = 1;
	return Aeron_ApplyWindowAspectRatio();
}

void Aeron_ApplyPendingWindowAspectRatio(void) {
	if (g_aeron.window_aspect_pending) {
		Aeron_ApplyWindowAspectRatio();
	}
}

int Aeron_ResizeWindowToAspect(int aspect_width, int aspect_height) {
	SDL_Rect      bounds;
	SDL_DisplayID display;
	Uint64        flags;
	int           current_width;
	int           current_height;
	int           target_width;
	int           target_height;

	if (!g_aeron.window || aspect_width <= 0 || aspect_height <= 0) {
		return 0;
	}
	flags = SDL_GetWindowFlags(g_aeron.window);
	if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) {
		return 1;
	}
	if (!SDL_GetWindowSize(g_aeron.window, &current_width, &current_height) || current_width <= 0 ||
		current_height <= 0) {
		Aeron_LogWarn("aeron", "SDL_GetWindowSize failed while applying window aspect: %s", SDL_GetError());
		return 0;
	}

	target_height = current_height;
	if (!Aeron_CalculateAspectWidth(target_height, aspect_width, aspect_height, &target_width)) {
		return 0;
	}
	display = SDL_GetDisplayForWindow(g_aeron.window);
	if (display != 0 && SDL_GetDisplayUsableBounds(display, &bounds) && bounds.w > 0 && bounds.h > 0 &&
		(target_width > bounds.w || target_height > bounds.h)) {
		target_height = bounds.h;
		if (!Aeron_CalculateAspectWidth(target_height, aspect_width, aspect_height, &target_width)) {
			return 0;
		}
		if (target_width > bounds.w) {
			target_width  = bounds.w;
			target_height = (int)(((int64_t)target_width * aspect_height + aspect_width / 2) / aspect_width);
		}
	}
	if (target_width <= 0 || target_height <= 0) {
		return 0;
	}
	if ((target_width != current_width || target_height != current_height) &&
		!SDL_SetWindowSize(g_aeron.window, target_width, target_height)) {
		Aeron_LogWarn("aeron", "SDL_SetWindowSize failed while applying window aspect: %s", SDL_GetError());
		return 0;
	}
	if (!SDL_SyncWindow(g_aeron.window)) {
		Aeron_LogWarn("aeron", "SDL_SyncWindow failed while applying window aspect: %s", SDL_GetError());
		return 0;
	}
	SDL_GetWindowSize(g_aeron.window, &g_aeron.input.window_width, &g_aeron.input.window_height);
	Aeron_UpdatePresentationPixelSize();
	return 1;
}

/* Finds the row of displays that `display` is in: the displays next to each
 * other that have the same top and height. Writes the rectangle that covers
 * them. Returns how many displays the row has, or zero when the displays
 * don't all have the same pixel density. */
static int Aeron_FindDisplayRow(SDL_DisplayID display, SDL_Rect* out) {
	SDL_Rect row;
	if (!SDL_GetDisplayBounds(display, &row)) {
		return 0;
	}
	int            display_count = 0;
	SDL_DisplayID* displays      = SDL_GetDisplays(&display_count);
	if (!displays) {
		return 0;
	}
	const SDL_DisplayMode* mode    = SDL_GetDesktopDisplayMode(display);
	const float            density = mode ? mode->pixel_density : 1.0f;
	int                    count   = 1;
	int                    same    = 1;
	/* Each pass adds the displays that touch the row's left or right edge. */
	for (int grown = 1; grown;) {
		grown = 0;
		for (int i = 0; i < display_count; ++i) {
			SDL_Rect bounds;
			if (!SDL_GetDisplayBounds(displays[i], &bounds) || bounds.y != row.y || bounds.h != row.h) {
				continue;
			}
			if (bounds.x + bounds.w != row.x && bounds.x != row.x + row.w) {
				continue;
			}
			const SDL_DisplayMode* other = SDL_GetDesktopDisplayMode(displays[i]);
			if ((other ? other->pixel_density : 1.0f) != density) {
				same = 0;
			}
			if (bounds.x < row.x) {
				row.x = bounds.x;
			}
			row.w += bounds.w;
			count++;
			grown = 1;
		}
	}
	SDL_free(displays);
	*out = row;
	return same ? count : 0;
}

static void Aeron_LeaveSpan(void) {
	if (!g_aeron.window_spanned) {
		return;
	}
	g_aeron.window_spanned = 0;
	const SDL_Rect* rect   = &g_aeron.window_rect_before_span;
	SDL_SetWindowBordered(g_aeron.window, true);
	SDL_SetWindowResizable(g_aeron.window, true);
	SDL_SetWindowSize(g_aeron.window, rect->w, rect->h);
	SDL_SetWindowPosition(g_aeron.window, rect->x, rect->y);
	SDL_SyncWindow(g_aeron.window);
	Aeron_ApplyPendingWindowAspectRatio();
}

/* Returns zero when the window doesn't span its row of displays afterwards. */
static int Aeron_EnterSpan(void) {
	SDL_Rect            row;
	const SDL_DisplayID display = SDL_GetDisplayForWindow(g_aeron.window);
	const int           count   = display ? Aeron_FindDisplayRow(display, &row) : 0;
	if (count == 0) {
		Aeron_LogWarn("aeron", "the window can't span the displays: they differ in pixel density");
		return 0;
	}
	if (count == 1) {
		Aeron_LogInfo("aeron", "the window can't span the displays: there is no display next to this one");
		return 0;
	}
	if (SDL_GetWindowFlags(g_aeron.window) & SDL_WINDOW_FULLSCREEN) {
		SDL_SetWindowFullscreen(g_aeron.window, false);
		SDL_SyncWindow(g_aeron.window);
	}
	if (!g_aeron.window_spanned) {
		SDL_Rect* before = &g_aeron.window_rect_before_span;
		SDL_GetWindowPosition(g_aeron.window, &before->x, &before->y);
		SDL_GetWindowSize(g_aeron.window, &before->w, &before->h);
		g_aeron.window_spanned = 1;
	}
	if (g_aeron.window_aspect_width > 0) {
		/* The aspect constraint would stop the window from taking the row's
		 * size. It is set again when the window leaves the span. */
		SDL_SetWindowAspectRatio(g_aeron.window, 0.0f, 0.0f);
		g_aeron.window_aspect_pending = 1;
	}
	SDL_SetWindowBordered(g_aeron.window, false);
	SDL_SetWindowResizable(g_aeron.window, false);
	SDL_SetWindowPosition(g_aeron.window, row.x, row.y);
	SDL_SetWindowSize(g_aeron.window, row.w, row.h);
	SDL_SyncWindow(g_aeron.window);
	SDL_Rect placed = { 0 };
	SDL_GetWindowPosition(g_aeron.window, &placed.x, &placed.y);
	SDL_GetWindowSize(g_aeron.window, &placed.w, &placed.h);
	if (placed.x != row.x || placed.y != row.y || placed.w != row.w || placed.h != row.h) {
		Aeron_LogWarn("aeron",
					  "the window can't span the displays: asked for %dx%d at %d,%d and got %dx%d at %d,%d",
					  row.w, row.h, row.x, row.y, placed.w, placed.h, placed.x, placed.y);
		Aeron_LeaveSpan();
		return 0;
	}
	Aeron_LogInfo("aeron", "the window spans %d displays: %dx%d at %d,%d", count, row.w, row.h, row.x, row.y);
	return 1;
}

int Aeron_SetWindowMode(AeronWindowMode mode) {
	if (!g_aeron.window) {
		return 0;
	}
	g_aeron.window_mode = (int)mode;
	if (mode == AERON_WINDOW_SPAN) {
		if (Aeron_EnterSpan()) {
			Aeron_UpdatePresentationPixelSize();
			Aeron_RefreshPresentationTiming();
			return 1;
		}
		mode = AERON_WINDOW_FULLSCREEN;
	}
	Aeron_LeaveSpan();
	const int fullscreen = mode == AERON_WINDOW_FULLSCREEN;
	const int current    = (SDL_GetWindowFlags(g_aeron.window) & SDL_WINDOW_FULLSCREEN) != 0;
	if (current != fullscreen && !SDL_SetWindowFullscreen(g_aeron.window, fullscreen)) {
		Aeron_LogWarn("aeron", "SDL_SetWindowFullscreen failed: %s", SDL_GetError());
		return 0;
	}
	Aeron_UpdatePresentationPixelSize();
	Aeron_RefreshPresentationTiming();
	return 1;
}

AeronWindowMode Aeron_WindowMode(void) {
	if (!g_aeron.window) {
		return AERON_WINDOW_WINDOWED;
	}
	if (SDL_GetWindowFlags(g_aeron.window) & SDL_WINDOW_FULLSCREEN) {
		return AERON_WINDOW_FULLSCREEN;
	}
	return g_aeron.window_spanned ? AERON_WINDOW_SPAN : AERON_WINDOW_WINDOWED;
}

void Aeron_OnDisplaysChanged(void) {
	if (g_aeron.window && g_aeron.window_mode == AERON_WINDOW_SPAN) {
		Aeron_SetWindowMode(AERON_WINDOW_SPAN);
	}
}

int Aeron_GetWindowDisplayRects(AeronRectI* out, int capacity) {
	if (!g_aeron.window || !out || capacity <= 0) {
		return 0;
	}
	SDL_Rect window;
	int      pixel_width  = 0;
	int      pixel_height = 0;
	SDL_GetWindowPosition(g_aeron.window, &window.x, &window.y);
	SDL_GetWindowSize(g_aeron.window, &window.w, &window.h);
	if (!SDL_GetWindowSizeInPixels(g_aeron.window, &pixel_width, &pixel_height) || window.w <= 0 ||
		window.h <= 0) {
		return 0;
	}
	int            display_count = 0;
	SDL_DisplayID* displays      = SDL_GetDisplays(&display_count);
	if (!displays) {
		return 0;
	}
	const double scale_x = (double)pixel_width / window.w;
	const double scale_y = (double)pixel_height / window.h;
	int          count   = 0;
	for (int i = 0; i < display_count && count < capacity; ++i) {
		SDL_Rect bounds;
		SDL_Rect shown;
		if (!SDL_GetDisplayBounds(displays[i], &bounds) ||
			!SDL_GetRectIntersection(&bounds, &window, &shown)) {
			continue;
		}
		const int left   = (int)lround((shown.x - window.x) * scale_x);
		const int top    = (int)lround((shown.y - window.y) * scale_y);
		const int right  = (int)lround((shown.x + shown.w - window.x) * scale_x);
		const int bottom = (int)lround((shown.y + shown.h - window.y) * scale_y);
		/* Keep the rectangles sorted by their left edge. */
		int at = count++;
		for (; at > 0 && out[at - 1].x > left; --at) {
			out[at] = out[at - 1];
		}
		out[at] = (AeronRectI) { left, top, right - left, bottom - top };
	}
	SDL_free(displays);
	return count;
}

int Aeron_SetFullscreen(int fullscreen) {
	return Aeron_SetWindowMode(fullscreen ? AERON_WINDOW_FULLSCREEN : AERON_WINDOW_WINDOWED);
}

int Aeron_Fullscreen(void) {
	return g_aeron.window && (SDL_GetWindowFlags(g_aeron.window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void Aeron_RaiseWindow(void) {
	if (g_aeron.window && !SDL_RaiseWindow(g_aeron.window)) {
		Aeron_LogWarn("aeron", "SDL_RaiseWindow failed: %s", SDL_GetError());
	}
}

void Aeron_UpdatePresentationPixelSize(void) {
	SDL_Rect content_rect = { 0 };
	int      pixel_width  = 0;
	int      pixel_height = 0;

	if (g_aeron.xr_screen_texture) {
		/* With VR, the layers are drawn into the virtual screen. */
		content_rect.w = g_aeron.xr_screen_width;
		content_rect.h = g_aeron.xr_screen_height;
	} else if (g_aeron.window && SDL_GetWindowSizeInPixels(g_aeron.window, &pixel_width, &pixel_height)) {
		Aeron_ComputePresentationRect(pixel_width, pixel_height, &content_rect);
	}
	if (content_rect.w != g_aeron.presentation_pixel_width ||
		content_rect.h != g_aeron.presentation_pixel_height) {
		Aeron_ResetPacing();
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
		Aeron_LogWarn("aeron", "SDL_SetWindowRelativeMouseMode failed: %s", SDL_GetError());
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
		/* An application-logical size quantizes the container aspect, so a
		 * matching-aspect container can still fit a pixel or two short and
		 * produce hairline bars — and lose exact coverage, which direct
		 * swapchain rendering requires. Snap sub-1/256 shortfalls to full
		 * coverage */
		if ((container_width - content_width) * 256 <= container_width) {
			content_width = container_width;
		}
		if ((container_height - content_height) * 256 <= container_height) {
			content_height = container_height;
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
