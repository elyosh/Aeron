#ifndef AERON_DX5_COMPAT_H
#define AERON_DX5_COMPAT_H

#include "aeron/dx5/ddraw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronDx5Rect {
	int x;
	int y;
	int width;
	int height;
} AeronDx5Rect;

typedef struct AeronDx5Config {
	void* context;
	AeronDx5Rect (*presentation_rect)(void* context, int surface_width, int surface_height);
	void (*presented)(void* context, int surface_width, int surface_height);
} AeronDx5Config;

/* The configuration is copied. Call before DirectDrawCreate. A null
 * configuration restores direct surface-sized presentation with no callback. */
void AeronDx5_Configure(const AeronDx5Config* config);

/* Compatibility factory used by ports that must avoid binding the platform's
 * DirectDrawCreate symbol. */
HRESULT DirectDrawCreate_Compat(const DxGuid* driver, IDirectDraw** out, void* outer);

/* Re-submits the retained classic frame when the recovered game did not reach
 * a DirectDraw presentation boundary during the current host frame. */
void AeronDx5_ResubmitIfIdle(void);
void AeronDx5_ResetPresentationState(void);

/* Modern flight presentation policy. Suppression applies only to render-target
 * surfaces; CPU DirectDraw surfaces used by frontends continue normally. */
void AeronDx5_SetClassicFlightRenderingSuppressed(int suppressed);
int AeronDx5_IsClassicFlightRenderingSuppressed(void);

/* Submits the last complete classic frame without creating a recovered present
 * event or advancing the DirectDraw flip chain. */
void AeronDx5_SubmitLastPresented(void);

/* Monotonic count of completed, non-suppressed classic presentations. */
uint64_t AeronDx5_GetClassicFlightFrameSerial(void);

/* Publishes a CPU-written rectangle from an attached 16-bit DirectDraw depth
 * surface to the render target used by the compatibility device. The pixels
 * are consumed before the next Direct3D scene segment. */
void AeronDx5_CommitDepthSurfaceRect(IDirectDrawSurface* surface, const AeronDx5Rect* rect);

/* Composes a CPU DirectDraw surface over a render-target surface with an
 * explicit per-pixel coverage plane (one byte per source pixel, 0 = fully
 * transparent, 255 = opaque; coverage_pitch 0 = tightly packed rows). The
 * GPU frame is never read back, so hosts can layer CPU overlays over
 * Direct3D output without the lock's readback stall and 16bpp rounding. */
int AeronDx5_ComposeSurfaceOverRenderTarget(IDirectDrawSurface* dst, int dst_x, int dst_y,
											 IDirectDrawSurface* src, const uint8_t* coverage,
											 int coverage_pitch);

/* Releases compatibility-owned GPU caches after the recovered game has
 * released every DirectDraw and Direct3D interface. */
void AeronDx5_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
