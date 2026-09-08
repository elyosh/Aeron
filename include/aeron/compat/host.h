#ifndef AERON_COMPAT_HOST_H
#define AERON_COMPAT_HOST_H

#include "aeron/compat/ddraw.h"
#include "aeron/vfs.h"

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

/* Completes one host frame by submitting the final retained classic image when
 * normal classic presentation is enabled. Call exactly once after the recovered
 * game tick and before submitting modern overlay layers. */
void AeronDx5_EndFrame(void);
void AeronDx5_ResetPresentationState(void);

/* Modern flight presentation policy. Suppression applies only to render-target
 * surfaces; CPU DirectDraw surfaces used by frontends continue normally. */
void AeronDx5_SetClassicFlightRenderingSuppressed(int suppressed);
int  AeronDx5_IsClassicFlightRenderingSuppressed(void);

/* Forces submission of the final retained classic frame, including while normal
 * classic presentation is suppressed. Does not advance the flip chain. */
void AeronDx5_ForceSubmitRetainedFrame(void);

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

/* --- WinMM CD audio ------------------------------------------------------ */

typedef struct AeronWinmmCdAudioDesc {
	AeronVfs*    vfs;
	AeronVfsRoot root;
	const char*  directory;
} AeronWinmmCdAudioDesc;

int  AeronWinmm_ConfigureCdAudio(const AeronWinmmCdAudioDesc* desc);
void AeronWinmm_Shutdown(void);

/* --- WinMM joystick source ------------------------------------------------
 *
 * Physical-to-logical controller mapping (device selection, axis assignment,
 * per-game options) stays game-owned. The game registers one source; the
 * joyGet* shim consumes it. */

typedef struct AeronWinmmJoystickState {
	uint32_t    axes[4]; /* X, Y, Z, R positions in 0..65535. */
	uint32_t    buttons; /* Bit i = button i+1 down. */
	uint32_t    button_count;
	int         pov_direction; /* -1 centered, otherwise up/right/down/left = 0..3. */
	int         has_pov;
	const char* name; /* Device name for JOYCAPSA.szPname; may be NULL. */
} AeronWinmmJoystickState;

/* Returns 0 when no controller is selected or connected. */
typedef int (*AeronWinmmJoystickSource)(AeronWinmmJoystickState* out, void* user);

/* Registers the game's controller source. joyGetNumDevs reports one device
 * while a source is registered; NULL unregisters. */
void AeronCompat_SetJoystickSource(AeronWinmmJoystickSource fn, void* user);

/* --- DirectInput rumble provider ------------------------------------------
 *
 * The DirectInput force-feedback surface routes to the game's controller
 * rumble through this provider; per-game selection and mapping stay outside
 * Aeron. Without a registered provider the FF device reports no rumble
 * support. */

typedef struct AeronCompatRumbleProvider {
	/* Non-zero when the selected controller can rumble. */
	int (*has_rumble)(void* user);
	/* Stable id of the selected controller, or 0 when none. */
	uint32_t (*controller_instance_id)(void* user);
	/* Drives the two rumble motors; (0, 0, 0) stops. */
	int (*rumble)(uint16_t low, uint16_t high, uint32_t duration_ms, void* user);
	void* user;
} AeronCompatRumbleProvider;

/* The provider is copied; NULL unregisters. */
void AeronCompat_SetRumbleProvider(const AeronCompatRumbleProvider* provider);

/* --- Per-frame input capture ----------------------------------------------
 *
 * Captures the current Aeron input frame into the DirectInput keyboard and
 * mouse devices. The host shell calls this once per frame so buffered key
 * edges accumulate independently of the game's fixed-step polling; a
 * suppressed frame captures keyboard state without forwarding key presses
 * and leaves the mouse untouched. No-op until DirectInput devices exist. */
void AeronCompat_Update(int input_suppressed);
/* Host-thread per-key filter, using AeronKey values. Enabling also removes
 * queued events for this key from every live keyboard device. */
void AeronCompat_SetKeySuppressed(int key, int suppressed);
int  AeronCompat_IsKeySuppressed(int key);

#ifdef __cplusplus
}
#endif

#endif
