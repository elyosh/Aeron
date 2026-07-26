#include "aeron/surface.h"

#include <SDL3/SDL.h>

#include <stddef.h>
#include <string.h>

struct AeronSurface {
	SDL_Surface*      surface;
	AeronPixelFormat  format;
	uint32_t          flags;
	uint32_t          generation;
	int               locked;
	int               color_key_enabled;
	uint32_t          color_key;
	AeronPaletteEntry palette[256];
	int               palette_set;
};

static SDL_PixelFormat AeronSurface_ToSdlFormat(AeronPixelFormat format) {
	switch (format) {
		case AERON_PIXEL_FORMAT_INDEX8:
			return SDL_PIXELFORMAT_INDEX8;
		case AERON_PIXEL_FORMAT_RGB555:
			return SDL_PIXELFORMAT_XRGB1555;
		case AERON_PIXEL_FORMAT_RGB565:
			return SDL_PIXELFORMAT_RGB565;
		case AERON_PIXEL_FORMAT_XRGB8888:
			return SDL_PIXELFORMAT_XRGB32;
		case AERON_PIXEL_FORMAT_ARGB8888:
			return SDL_PIXELFORMAT_ARGB32;
		case AERON_PIXEL_FORMAT_RGBA8888:
			return SDL_PIXELFORMAT_RGBA32;
		case AERON_PIXEL_FORMAT_BGRA8888:
			return SDL_PIXELFORMAT_BGRA32;
		default:
			return SDL_PIXELFORMAT_UNKNOWN;
	}
}

static int AeronSurface_BppForFormat(AeronPixelFormat format) {
	switch (format) {
		case AERON_PIXEL_FORMAT_INDEX8:
			return 8;
		case AERON_PIXEL_FORMAT_RGB555:
		case AERON_PIXEL_FORMAT_RGB565:
			return 16;
		case AERON_PIXEL_FORMAT_XRGB8888:
		case AERON_PIXEL_FORMAT_ARGB8888:
		case AERON_PIXEL_FORMAT_RGBA8888:
		case AERON_PIXEL_FORMAT_BGRA8888:
			return 32;
		default:
			return 0;
	}
}

static SDL_Rect AeronSurface_ToSdlRect(const AeronSurfaceRect* rect) {
	SDL_Rect sdl_rect;

	sdl_rect.x = rect->x;
	sdl_rect.y = rect->y;
	sdl_rect.w = rect->w;
	sdl_rect.h = rect->h;
	return sdl_rect;
}

static int AeronSurface_SetSdlColorKey(AeronSurface* surface, int enabled, uint32_t pixel_value) {
	if (!surface) {
		return 0;
	}

	if (!SDL_SetSurfaceColorKey(surface->surface, enabled != 0, pixel_value)) {
		return 0;
	}

	return 1;
}

int AeronSurface_Create(int width, int height, AeronPixelFormat format, uint32_t flags,
						AeronSurface** out_surface) {
	AeronSurface*   surface;
	SDL_Surface*    sdl_surface;
	SDL_PixelFormat sdl_format;

	if (!out_surface) {
		return 0;
	}

	*out_surface = NULL;
	sdl_format   = AeronSurface_ToSdlFormat(format);
	if (width <= 0 || height <= 0 || sdl_format == SDL_PIXELFORMAT_UNKNOWN) {
		return 0;
	}

	sdl_surface = SDL_CreateSurface(width, height, sdl_format);
	if (!sdl_surface) {
		return 0;
	}

	surface = (AeronSurface*)SDL_calloc(1, sizeof(*surface));
	if (!surface) {
		SDL_DestroySurface(sdl_surface);
		return 0;
	}

	surface->surface           = sdl_surface;
	surface->format            = format;
	surface->flags             = flags;
	surface->generation        = 1;
	surface->color_key_enabled = (flags & AERON_SURFACE_COLOR_KEY) != 0;
	if (surface->color_key_enabled && !AeronSurface_SetSdlColorKey(surface, 1, surface->color_key)) {
		SDL_DestroySurface(sdl_surface);
		SDL_free(surface);
		return 0;
	}

	*out_surface = surface;
	return 1;
}

void AeronSurface_Destroy(AeronSurface* surface) {
	if (!surface) {
		return;
	}

	SDL_DestroySurface(surface->surface);
	SDL_free(surface);
}

void* AeronSurface_Lock(AeronSurface* surface, int* out_pitch) {
	if (!surface || !(surface->flags & AERON_SURFACE_CPU_LOCKABLE)) {
		return NULL;
	}

	if (!surface->locked && !SDL_LockSurface(surface->surface)) {
		return NULL;
	}

	surface->locked = 1;
	if (out_pitch) {
		*out_pitch = surface->surface->pitch;
	}

	return surface->surface->pixels;
}

void AeronSurface_Unlock(AeronSurface* surface) {
	if (!surface || !surface->locked) {
		return;
	}

	SDL_UnlockSurface(surface->surface);
	surface->locked = 0;
	++surface->generation;
}

int AeronSurface_IsLocked(const AeronSurface* surface) { return surface ? surface->locked : 0; }

int AeronSurface_Clear(AeronSurface* surface, uint32_t pixel_value) {
	if (!surface) {
		return 0;
	}

	if (!SDL_FillSurfaceRect(surface->surface, NULL, pixel_value)) {
		return 0;
	}

	++surface->generation;
	return 1;
}

int AeronSurface_ClearRect(AeronSurface* surface, const AeronSurfaceRect* rect, uint32_t pixel_value) {
	SDL_Rect sdl_rect;

	if (!surface) {
		return 0;
	}

	if (rect) {
		sdl_rect = AeronSurface_ToSdlRect(rect);
	}
	if (!SDL_FillSurfaceRect(surface->surface, rect ? &sdl_rect : NULL, pixel_value)) {
		return 0;
	}

	++surface->generation;
	return 1;
}

int AeronSurface_Blit(AeronSurface* dst, int dst_x, int dst_y, const AeronSurface* src,
					  const AeronSurfaceRect* src_rect, uint32_t flags) {
	SDL_Rect sdl_src_rect;
	SDL_Rect sdl_dst_rect;
	int      use_color_key;
	int      restore_color_key;

	if (!dst || !src) {
		return 0;
	}

	use_color_key     = (flags & AERON_SURFACE_BLIT_COLOR_KEY) != 0 && src->color_key_enabled;
	restore_color_key = src->color_key_enabled != use_color_key;
	if (restore_color_key &&
		!AeronSurface_SetSdlColorKey((AeronSurface*)src, use_color_key, src->color_key)) {
		return 0;
	}

	if (src_rect) {
		sdl_src_rect = AeronSurface_ToSdlRect(src_rect);
	}

	sdl_dst_rect.x = dst_x;
	sdl_dst_rect.y = dst_y;
	sdl_dst_rect.w = 0;
	sdl_dst_rect.h = 0;
	if (!SDL_BlitSurface((SDL_Surface*)src->surface, src_rect ? &sdl_src_rect : NULL, dst->surface,
						 &sdl_dst_rect)) {
		if (restore_color_key) {
			AeronSurface_SetSdlColorKey((AeronSurface*)src, src->color_key_enabled, src->color_key);
		}
		return 0;
	}

	if (restore_color_key &&
		!AeronSurface_SetSdlColorKey((AeronSurface*)src, src->color_key_enabled, src->color_key)) {
		return 0;
	}

	++dst->generation;
	return 1;
}

int AeronSurface_SetColorKey(AeronSurface* surface, int enabled, uint32_t pixel_value) {
	if (!surface) {
		return 0;
	}

	if (!AeronSurface_SetSdlColorKey(surface, enabled, pixel_value)) {
		return 0;
	}

	surface->color_key_enabled = enabled != 0;
	surface->color_key         = pixel_value;
	if (enabled) {
		surface->flags |= AERON_SURFACE_COLOR_KEY;
	} else {
		surface->flags &= ~((uint32_t)AERON_SURFACE_COLOR_KEY);
	}

	return 1;
}

int AeronSurface_HasColorKey(const AeronSurface* surface) { return surface ? surface->color_key_enabled : 0; }

uint32_t AeronSurface_GetColorKey(const AeronSurface* surface) { return surface ? surface->color_key : 0; }

int AeronSurface_SetPalette(AeronSurface* surface, const AeronPaletteEntry* palette, uint32_t count) {
	SDL_Color colors[256];
	uint32_t  i;

	if (!surface || !palette || count > 256u || surface->format != AERON_PIXEL_FORMAT_INDEX8) {
		return 0;
	}

	for (i = 0; i < count; ++i) {
		colors[i].r = palette[i].r;
		colors[i].g = palette[i].g;
		colors[i].b = palette[i].b;
		colors[i].a = palette[i].a;
	}

	if (!SDL_GetSurfacePalette(surface->surface) && !SDL_CreateSurfacePalette(surface->surface)) {
		return 0;
	}

	if (!SDL_SetPaletteColors(SDL_GetSurfacePalette(surface->surface), colors, 0, (int)count)) {
		return 0;
	}

	memcpy(surface->palette, palette, (size_t)count * sizeof(surface->palette[0]));
	if (count < 256u) {
		memset(&surface->palette[count], 0, (size_t)(256u - count) * sizeof(surface->palette[0]));
	}
	surface->palette_set = 1;
	++surface->generation;
	return 1;
}

int AeronSurface_GetFrameView(const AeronSurface* surface, AeronPixelFrameView* out_view) {
	if (!surface || !out_view) {
		return 0;
	}

	out_view->pixels      = surface->surface->pixels;
	out_view->width       = surface->surface->w;
	out_view->height      = surface->surface->h;
	out_view->pitch       = surface->surface->pitch;
	out_view->bpp         = AeronSurface_BppForFormat(surface->format);
	out_view->format      = surface->format;
	out_view->color_space = AERON_COLOR_SPACE_SRGB;
	out_view->palette     = surface->palette_set ? surface->palette : NULL;
	out_view->generation  = surface->generation;
	return 1;
}

int AeronSurface_GetWidth(const AeronSurface* surface) { return surface ? surface->surface->w : 0; }

int AeronSurface_GetHeight(const AeronSurface* surface) { return surface ? surface->surface->h : 0; }

int AeronSurface_GetPitch(const AeronSurface* surface) { return surface ? surface->surface->pitch : 0; }

int AeronSurface_GetBpp(const AeronSurface* surface) {
	return surface ? AeronSurface_BppForFormat(surface->format) : 0;
}

AeronPixelFormat AeronSurface_GetFormat(const AeronSurface* surface) {
	return surface ? surface->format : AERON_PIXEL_FORMAT_UNKNOWN;
}

uint32_t AeronSurface_GetFlags(const AeronSurface* surface) { return surface ? surface->flags : 0; }

uint32_t AeronSurface_GetGeneration(const AeronSurface* surface) { return surface ? surface->generation : 0; }
