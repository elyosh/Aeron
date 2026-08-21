#ifndef AERON_DX5_INTERNAL_H
#define AERON_DX5_INTERNAL_H

/* Internal shared layout of the DirectDraw/Direct3D shim objects.
 *
 * ddraw.c owns the IDirectDraw / IDirectDrawSurface / IDirectDrawPalette
 * implementations; d3d.c owns the IDirect3D / IDirect3DDevice /
 * IDirect3DViewport / IDirect3DExecuteBuffer / IDirect3DTexture implementations.
 * They share the surface/device object layout because the recovered std3D code
 * QIs a DirectDraw surface for its Direct3D device, so the device shim must reach
 * that surface's render-target backing. This header is compat-internal only; the
 * recovered game code sees only the public COM vtables in ddraw.h / d3d.h. */

#include "aeron/dx5/d3d.h"
#include "aeron/dx5/ddraw.h"

#include "aeron/render.h"
#include "aeron/surface.h"

typedef struct D3DDeviceShim D3DDeviceShim;

typedef enum DDShimKind {
	DDSHIM_CPU,           /* offscreen-plain 2D surface backed by an AeronSurface */
	DDSHIM_PRIMARY,       /* the present sink (screen); no pixels of its own */
	DDSHIM_RENDER_TARGET, /* 3D render target: Aeron color+depth target backing */
} DDShimKind;

typedef struct DDrawPaletteShim {
	const IDirectDrawPaletteVtbl* lpVtbl;
	int refcount;
	AeronPaletteEntry entries[256];
	uint64_t revision;
} DDrawPaletteShim;

typedef struct DDrawSurfaceShim {
	const IDirectDrawSurfaceVtbl* lpVtbl;
	int refcount;
	DDShimKind kind;
	struct DDrawShim* owner;

	int width;
	int height;
	int bpp;
	AeronPixelFormat format;
	uint32_t caps; /* ddsCaps.dwCaps */

	/* Explicit DX pixel format for texture surfaces (DDSD_PIXELFORMAT). Used by
	 * IDirect3DTexture::Load to unpack the source pixels to RGBA8. has_pixel_format
	 * is 0 for plain surfaces whose format is derived from the display mode. */
	int has_pixel_format;
	DDPIXELFORMAT pixel_format;

	AeronSurface* cpu;                 /* CPU_LOCKABLE backing (NULL for PRIMARY) */
	struct DDrawSurfaceShim* attached; /* PRIMARY -> flip-chain back buffer */
	struct DDrawSurfaceShim* zbuffer;  /* render surface -> attached z-buffer surface */

	/* Render-target backing (DDSHIM_RENDER_TARGET, created lazily on the
	 * IDirect3DDevice QueryInterface). The color target is what 3D renders into;
	 * the CPU surface, when present, is the readback/writeback staging used by
	 * render-target Lock. cpu_dirty/gpu_dirty track which side holds the
	 * authoritative pixels.
	 *
	 * `rt` is the current work buffer (rendered into and composited into this
	 * frame); `rt_back` is the visible half of a DirectDraw flip chain. Flip
	 * swaps the two, while a primary Blt copies `rt` into `rt_back`; the host
	 * submits `rt_back` after the game tick. */
	AeronRenderTarget* rt;
	AeronRenderTarget* rt_back;
	AeronDepthTarget* depth;
	int cpu_dirty;
	int gpu_dirty;
	int pending_depth_clear;              /* DDBLT_DEPTHFILL -> next BeginScene */
	float pending_depth_clear_value;      /* normalized depth clear value */
	uint8_t* pending_depth_upload_data;   /* packed D16 CPU depth rectangle */
	uint32_t pending_depth_upload_capacity;
	AeronRectI pending_depth_upload_rect;
	int pending_depth_upload;
	struct DDrawSurfaceShim* attached_to; /* z-buffer -> its render-target surface */
	struct D3DDeviceShim* device;         /* device bound to this RT surface, if any */

	int has_colorkey;
	uint32_t colorkey;

	DDrawPaletteShim* palette;
	DDrawPaletteShim* applied_palette;
	uint64_t applied_palette_revision;
} DDrawSurfaceShim;

typedef struct DDrawShim {
	const IDirectDrawVtbl* lpVtbl;
	int refcount;
	int mode_w;
	int mode_h;
	int mode_bpp;
	AeronPixelFormat mode_format;
	DDrawSurfaceShim* primary;
} DDrawShim;

extern const IDirectDrawSurfaceVtbl g_ddSurfaceVtbl;
extern const IDirectDrawPaletteVtbl g_ddPaletteVtbl;
extern const IDirectDrawVtbl g_ddDeviceVtbl;

/* Uploads a render surface's CPU staging back into its color target when the
 * staging is authoritative (cpu_dirty), so a subsequent GPU pass or present sees
 * the CPU overlay content. Called by the D3D device before beginning a scene and
 * by the present path. No-op unless the surface is a lockable render target with
 * dirty staging. Implemented in ddraw.c. */
void DDShim_WritebackRenderTarget(DDrawSurfaceShim* s);

/* Flushes queued GPU draws on the device bound to a render-target surface, so a
 * DirectDraw operation observes completed pixels. Returns nonzero on success. */
int D3DCompat_FlushRenderTargetPass(DDrawSurfaceShim* s);

/* d3d.c entry points used by the DirectDraw QueryInterface hooks. */
IDirect3D* D3DCompat_CreateD3D(DDrawShim* dd);
IDirect3DDevice* D3DCompat_CreateDeviceForSurface(DDrawSurfaceShim* surface);
IDirect3DTexture* D3DCompat_CreateTexture(DDrawSurfaceShim* surface);

AeronRectI AeronDx5_PresentationRect(int surface_width, int surface_height);
void AeronDx5_NotifyPresent(int surface_width, int surface_height);
void D3DCompat_Shutdown(void);
void DDShim_ReleasePresentationResources(void);

#endif
