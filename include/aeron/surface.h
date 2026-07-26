#ifndef AERON_SURFACE_H
#define AERON_SURFACE_H

#include <stdint.h>

#include "aeron/render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque CPU surface with explicit pixel format, pitch, palette, and color-key state. */
typedef struct AeronSurface AeronSurface;

/* Surface flags; CPU_LOCKABLE and COLOR_KEY affect current behavior, others are stored usage hints. */
typedef enum AeronSurfaceFlags {
	AERON_SURFACE_CPU_LOCKABLE = 1u << 0,
	AERON_SURFACE_PRESENTABLE  = 1u << 1,
	AERON_SURFACE_OFFSCREEN    = 1u << 2,
	AERON_SURFACE_COLOR_KEY    = 1u << 3
} AeronSurfaceFlags;

/* Optional behavior flags for AeronSurface_Blit. */
typedef enum AeronSurfaceBlitFlags {
	AERON_SURFACE_BLIT_NONE      = 0,
	AERON_SURFACE_BLIT_COLOR_KEY = 1u << 0
} AeronSurfaceBlitFlags;

/* Integer rectangle in surface pixel coordinates. */
typedef struct AeronSurfaceRect {
	int x;
	int y;
	int w;
	int h;
} AeronSurfaceRect;

/* Creates a surface and writes it to out_surface; returns nonzero on success. */
int AeronSurface_Create(int width, int height, AeronPixelFormat format, uint32_t flags,
						AeronSurface** out_surface);
/* Destroys a surface created by AeronSurface_Create. */
void AeronSurface_Destroy(AeronSurface* surface);

/* Locks a CPU-lockable surface and returns writable pixel memory plus optional pitch. */
void* AeronSurface_Lock(AeronSurface* surface, int* out_pitch);
/* Unlocks a previously locked surface and advances its generation counter. */
void AeronSurface_Unlock(AeronSurface* surface);
/* Returns nonzero if the surface is currently locked. */
int AeronSurface_IsLocked(const AeronSurface* surface);

/* Fills the entire surface with the raw pixel value. */
int AeronSurface_Clear(AeronSurface* surface, uint32_t pixel_value);
/* Fills a clipped rectangle with the raw pixel value. */
int AeronSurface_ClearRect(AeronSurface* surface, const AeronSurfaceRect* rect, uint32_t pixel_value);
/* Blits pixels from src to dst with clipping and optional source color-key testing. */
int AeronSurface_Blit(AeronSurface* dst, int dst_x, int dst_y, const AeronSurface* src,
					  const AeronSurfaceRect* src_rect, uint32_t flags);

/* Enables or disables the surface color key and stores the raw key pixel value. */
int AeronSurface_SetColorKey(AeronSurface* surface, int enabled, uint32_t pixel_value);
/* Returns nonzero if the surface has color-key testing enabled. */
int AeronSurface_HasColorKey(const AeronSurface* surface);
/* Returns the raw color-key pixel value stored on the surface. */
uint32_t AeronSurface_GetColorKey(const AeronSurface* surface);

/* Copies count palette entries into an INDEX8 surface; count must be at most 256. */
int AeronSurface_SetPalette(AeronSurface* surface, const AeronPaletteEntry* palette, uint32_t count);
/* Builds a CPU frame view suitable for Aeron_SubmitPixelLayer. */
int AeronSurface_GetFrameView(const AeronSurface* surface, AeronPixelFrameView* out_view);

/* Returns the surface width in pixels. */
int AeronSurface_GetWidth(const AeronSurface* surface);
/* Returns the surface height in pixels. */
int AeronSurface_GetHeight(const AeronSurface* surface);
/* Returns the surface row pitch in bytes. */
int AeronSurface_GetPitch(const AeronSurface* surface);
/* Returns the surface bits per pixel. */
int AeronSurface_GetBpp(const AeronSurface* surface);
/* Returns the surface pixel format. */
AeronPixelFormat AeronSurface_GetFormat(const AeronSurface* surface);
/* Returns the creation flags stored on the surface. */
uint32_t AeronSurface_GetFlags(const AeronSurface* surface);
/* Returns the surface generation counter, incremented by mutating surface operations. */
uint32_t AeronSurface_GetGeneration(const AeronSurface* surface);

#ifdef __cplusplus
}
#endif

#endif
