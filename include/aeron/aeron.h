#ifndef AERON_H
#define AERON_H

#include <stddef.h>
#include <stdint.h>

#include "aeron/audio.h"
#include "aeron/config_file.h"
#include "aeron/debug.h"
#include "aeron/dialog.h"
#include "aeron/input.h"
#include "aeron/log.h"
#include "aeron/numeric.h"
#include "aeron/paths.h"
#include "aeron/render.h"
#include "aeron/surface.h"
#include "aeron/sync.h"
#include "aeron/temporal.h"
#include "aeron/time.h"
#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Controls how Aeron maps the application's logical render area to the window. */
typedef enum AeronPresentationMode {
	AERON_PRESENTATION_STRETCH    = 0,
	AERON_PRESENTATION_ASPECT_FIT = 1
} AeronPresentationMode;

/* Startup configuration for the Aeron runtime. */
typedef struct AeronConfig {
	const char* org_name;
	const char* app_name;
	const char* asset_root;
	/* Optional explicit resource root. When unset, resource_path is resolved
	 * relative to the application directory. */
	const char* resource_root;
	const char* resource_path;
	/* Compiled-shader directory, relative to the application executable. */
	const char* shader_path;
	const char* window_title;
	/* Optional window/taskbar icon as an in-memory BMP passed to
	 * SDL_SetWindowIcon on Windows and Linux. Not applied on macOS, where
	 * the bundle icon is authoritative and SDL would instead replace the
	 * Dock icon with this lower-resolution image. */
	const void* window_icon_bmp;
	size_t      window_icon_bmp_size;
	/* Window size in logical points. <= 0 auto-sizes to ~90% of the
	 * primary display's usable area, locked to the logical aspect. */
	int                   window_width;
	int                   window_height;
	int                   logical_width;
	int                   logical_height;
	AeronPresentationMode presentation_mode;
	/* Optional swapchain background shown outside submitted layers. When
	 * disabled, Aeron's default background is retained. */
	int   clear_color_enabled;
	float clear_color_rgba[4];
} AeronConfig;

/* Initializes SDL, the Aeron window, render backend, text input, and VFS. */
int Aeron_Init(const AeronConfig* config);
/* Shuts down Aeron and releases all runtime-owned resources. */
void Aeron_Shutdown(void);

/* Starts a new frame, pumps platform events, updates input, and returns capped elapsed microseconds. */
int32_t Aeron_BeginFrame(void);
/* Presents all render submissions queued for the current frame. Returns zero
 * after an unexpected renderer failure. A successful no-op frame returns one. */
int Aeron_Present(void);

/* Requests that the application exit at the next convenient point. */
void Aeron_RequestQuit(void);
/* Returns nonzero after the user or application has requested exit. */
int Aeron_QuitRequested(void);
/* Reports one fatal application error, requests orderly shutdown, and latches a
 * nonzero process result. Later reports do not replace the first diagnostic. */
void Aeron_RequestFatalError(const char* title, const char* message);
int  Aeron_FatalErrorRequested(void);

/* Changes the application logical render size without touching the host window. */
int Aeron_SetLogicalSize(int width, int height);
/* Switches the host window between windowed and fullscreen modes. */
int Aeron_SetFullscreen(int fullscreen);
/* Returns nonzero while the host window is fullscreen. */
int Aeron_Fullscreen(void);
/* Asks the OS to raise the host window and give it input focus, e.g. after a
 * native dialog held focus. The request is asynchronous and the platform may
 * deny it under its focus-stealing policy. */
void Aeron_RaiseWindow(void);

/* Physical pixel dimensions of the aspect-fitted presentation area for the
 * current host frame. This is distinct from logical coordinates and window
 * points, particularly on high-density displays. */
int Aeron_GetPresentationPixelSize(int* width, int* height);

/* Returns the current immutable input snapshot for this frame. */
const AeronInputSnapshot* Aeron_InputSnapshot(void);
/* Returns the runtime VFS used by the application. */
AeronVfs* Aeron_GetVfs(void);
/* Shows or hides the host OS cursor; returns nonzero on success. */
int Aeron_SetHostCursorVisible(int visible);
/* Warps the host mouse to an application logical coordinate. */
int Aeron_WarpMouseLogical(int x, int y);
/* Enables or disables relative mouse mode for flight-style delta input. */
int Aeron_SetRelativeMouseMode(int enabled);
/* Nonzero while relative mouse mode is active, i.e. while the pointer is
 * captured and its deltas belong to the application rather than the OS. */
int Aeron_RelativeMouseMode(void);

#ifdef __cplusplus
}
#endif

#endif
