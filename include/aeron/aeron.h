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
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Controls how Aeron maps the application's logical render area to the window. */
typedef enum AeronPresentationMode {
	AERON_PRESENTATION_STRETCH    = 0,
	AERON_PRESENTATION_ASPECT_FIT = 1
} AeronPresentationMode;

/* Options that Aeron needs before it creates the window and the GPU device.
 * They can't change while the program runs. */
typedef struct AeronStartupOptions {
	/* Whether to start with VR. The default is AERON_XR_OFF. */
	AeronXrMode xr_mode;
} AeronStartupOptions;

/* Fills the startup options, for example from the user's configuration file.
 * `vfs` is ready to use, and `options` holds the defaults. */
typedef void (*AeronStartupOptionsFn)(AeronVfs* vfs, void* userdata, AeronStartupOptions* options);

/* Startup configuration for the Aeron runtime. */
typedef struct AeronConfig {
	const char* org_name;
	const char* app_name;
	/* Optional version string recorded in the session log and crash reports. */
	const char* app_version;
	/* Optional name for the session log and crash dump files, which the crash
	 * message also shows. app_name is used when it isn't set. Programs that
	 * share an app_name, and so a user folder, set it to keep their files
	 * apart. */
	const char* diagnostics_name;
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
	int window_width;
	int window_height;
	/* Create the window hidden: for offscreen tools that never present (the
	 * GPU device still needs a window to claim). */
	int                   window_hidden;
	int                   logical_width;
	int                   logical_height;
	AeronPresentationMode presentation_mode;
	/* Optional swapchain background shown outside submitted layers. When
	 * disabled, Aeron's default background is retained. */
	int   clear_color_enabled;
	float clear_color_rgba[4];
	/* Optional. Aeron_Init calls it once, after the VFS is ready and before
	 * it creates the window and the GPU device. */
	AeronStartupOptionsFn startup_options;
	void*                 startup_userdata;
} AeronConfig;

/* Initializes SDL, the VFS, the Aeron window, render backend and text input.
 * Also starts the per-session log file under the user root's diagnostics/
 * directory and, on Windows, a crash handler that writes minidumps there. */
int Aeron_Init(const AeronConfig* config);
/* Shuts down Aeron and releases all runtime-owned resources. */
void Aeron_Shutdown(void);

/* Starts a new frame, pumps platform events, updates input, and returns capped elapsed microseconds. */
int32_t Aeron_BeginFrame(void);
/* Services queued platform events without starting a frame. Intended for
 * synchronous startup or other work performed outside the normal frame loop.
 * Persistent input state and lifecycle requests are updated immediately;
 * transient input edges are cleared by the next Aeron_BeginFrame. */
void Aeron_PumpEvents(void);
/* Presents all render submissions queued for the current frame. Returns zero
 * after an unexpected renderer failure. A successful no-op frame returns one. */
int Aeron_Present(void);

/* Requests that the application exit at the next convenient point. */
void Aeron_RequestQuit(void);
/* Returns nonzero after the user or application has requested exit. */
int Aeron_QuitRequested(void);
/* Clears a quit request, for an application that asks before it quits. It
 * also cancels a relaunch. */
void Aeron_ClearQuitRequest(void);
/* Requests a quit, then a new start of the application with arguments `args`
 * (without the program). Aeron_Shutdown starts the new process after it closes
 * the window, and doesn't wait for it. Returns 0, logs why and requests
 * nothing when the executable can't be found. */
int Aeron_RequestRelaunch(const char* const* args, int count);
/* Reports one fatal application error, requests orderly shutdown, and latches a
 * nonzero process result. Later reports do not replace the first diagnostic. */
void Aeron_RequestFatalError(const char* title, const char* message);
int  Aeron_FatalErrorRequested(void);

/* Changes the application logical render size without touching the host window. */
int Aeron_SetLogicalSize(int width, int height);
/* Returns the application logical render size configured at init or by
 * Aeron_SetLogicalSize. Returns nonzero when a logical size is set. */
int Aeron_GetLogicalSize(int* width, int* height);
/* Returns the current window client size in logical points. */
int Aeron_GetWindowSize(int* width, int* height);
/* Constrains ordinary window resizing to one width:height ratio. Fixed-size
 * states such as fullscreen defer the constraint until they are left. */
int Aeron_SetWindowAspectRatio(int aspect_width, int aspect_height);
/* Adjusts an ordinary window to the requested aspect while keeping it inside
 * the current display's usable bounds. Fixed-size states are left unchanged. */
int Aeron_ResizeWindowToAspect(int aspect_width, int aspect_height);

typedef enum AeronWindowMode {
	AERON_WINDOW_WINDOWED = 0,
	AERON_WINDOW_FULLSCREEN,
	/* A borderless window over every display of the row that the window's
	 * display is in: the displays next to each other with the same top and
	 * height. */
	AERON_WINDOW_SPAN,
} AeronWindowMode;

/* Changes the window mode. Span falls back to fullscreen, with a log
 * message, when the row has one display, when its displays differ in pixel
 * density, or when the system doesn't place the window as asked. Returns
 * zero when the mode couldn't be applied at all. */
int Aeron_SetWindowMode(AeronWindowMode mode);
/* Returns the mode the window is in now. This can differ from the mode last
 * asked for: span can fall back, and the user can enter or leave fullscreen
 * with the system's own controls. */
AeronWindowMode Aeron_WindowMode(void);
/* Writes the parts of the window that each display shows, in window pixels,
 * from left to right. Returns how many it wrote, at most `capacity`. */
int Aeron_GetWindowDisplayRects(AeronRectI* out, int capacity);
/* Same as Aeron_SetWindowMode with fullscreen or windowed. */
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
