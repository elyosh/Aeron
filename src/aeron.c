#include "internal.h"

#include <string.h>

AeronRuntime g_aeron;

void Aeron_CopyString(char* dst, size_t dst_size, const char* src) {
	if (!dst || !dst_size) {
		return;
	}

	if (!src) {
		dst[0] = '\0';
		return;
	}

	SDL_snprintf(dst, dst_size, "%s", src);
}

static int Aeron_ResolveApplicationRoot(char* out, size_t capacity, const char* relative_path,
										const char* description) {
	if (!relative_path || !relative_path[0]) {
		Aeron_LogError("aeron", "application-relative %s path is not configured", description);
		return 0;
	}
	if (!Aeron_ApplicationPath(relative_path, out, capacity)) {
		Aeron_LogError("aeron", "could not resolve application-relative %s path: %s", description,
					   SDL_GetError());
		return 0;
	}
	return 1;
}

static int Aeron_ResolveShaderRoot(const AeronConfig* config) {
	const char* shader_path = config ? config->shader_path : NULL;
	return Aeron_ResolveApplicationRoot(g_aeron.shader_root, sizeof g_aeron.shader_root, shader_path,
										"shader");
}

int Aeron_Init(const AeronConfig* config) {
	AeronConfig resolved_config;
	char        application_resource_root[AERON_MAX_PATH];
	if (g_aeron.initialized) {
		return 1;
	}
	/* Before the log file, which can fail, so thread sinks always work. */
	Aeron_LogInstallOutput();
	Aeron_DiagnosticsInit(config);

	memset(&g_aeron, 0, sizeof(g_aeron));
	g_aeron.clear_color_rgba[0] = 0.035f;
	g_aeron.clear_color_rgba[1] = 0.047f;
	g_aeron.clear_color_rgba[2] = 0.071f;
	g_aeron.clear_color_rgba[3] = 1.0f;
	if (config && config->clear_color_enabled) {
		memcpy(g_aeron.clear_color_rgba, config->clear_color_rgba, sizeof g_aeron.clear_color_rgba);
	}

	Aeron_CopyString(g_aeron.org_name, sizeof(g_aeron.org_name),
					 config && config->org_name ? config->org_name : "aeron");
	Aeron_CopyString(g_aeron.app_name, sizeof(g_aeron.app_name),
					 config && config->app_name ? config->app_name : "aeron");
	SDL_SetAppMetadata(g_aeron.app_name, NULL, NULL);

#if defined(__APPLE__)
	if (!SDL_SetHint(SDL_HINT_MAC_OPTION_AS_ALT, "both")) {
		Aeron_LogWarn("aeron.input", "could not configure macOS Option keys as Alt");
	}
#endif

	/* The application also has the focus while a headset has it, and the
	 * desktop window may not. Joysticks and gamepads must still be read then. */
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
		Aeron_LogError("aeron", "SDL_Init failed: %s", SDL_GetError());
		return 0;
	}
	if (!Aeron_ResolveShaderRoot(config)) {
		SDL_Quit();
		return 0;
	}
	if (config) {
		resolved_config = *config;
		if ((!resolved_config.resource_root || !resolved_config.resource_root[0]) &&
			resolved_config.resource_path && resolved_config.resource_path[0]) {
			if (!Aeron_ResolveApplicationRoot(application_resource_root, sizeof application_resource_root,
											  resolved_config.resource_path, "resource")) {
				SDL_Quit();
				return 0;
			}
			resolved_config.resource_root = application_resource_root;
		}
		config = &resolved_config;
	}

	/* The VFS comes before the window and the GPU device, so the
	 * application can read its configuration for the startup options. */
	Aeron_InitVfs(config);
	if (config && config->startup_options) {
		AeronStartupOptions startup = { .xr_mode = AERON_XR_OFF };
		config->startup_options(&g_aeron.vfs, config->startup_userdata, &startup);
		g_aeron.xr_mode    = startup.xr_mode;
		g_aeron.xr_options = startup;
	}

	if (!Aeron_WindowInit(config)) {
		AeronVfs_DeinitInternal(&g_aeron.vfs);
		SDL_Quit();
		return 0;
	}
	/* Keep the native window responsive while the remaining synchronous
	 * backends initialize. Events stay queued until the runtime is ready to
	 * process them through Aeron_PumpEvents. */
	SDL_PumpEvents();

	if (!Aeron_RenderBackendInit()) {
		Aeron_WindowShutdown();
		AeronVfs_DeinitInternal(&g_aeron.vfs);
		SDL_Quit();
		return 0;
	}
	SDL_PumpEvents();

	if (!Aeron_AudioInit()) {
		/* Audio is non-fatal: continue without sound rather than failing init. */
		Aeron_LogWarn("aeron", "audio subsystem unavailable; continuing without sound");
	}
	SDL_PumpEvents();

	Aeron_ControllersInit();
	SDL_PumpEvents();
	SDL_StartTextInput(g_aeron.window);
	Aeron_DebugUiInitInternal();
	g_aeron.input.has_focus = 1;
	g_aeron.last_frame_us   = Aeron_NowUs();
	g_aeron.initialized     = 1;
	Aeron_SetPresentationVsyncDivisor(1);
	Aeron_SetLatencyPacing(1);

	return 1;
}

void Aeron_Shutdown(void) {
	if (!g_aeron.initialized) {
		return;
	}

	Aeron_DebugUiShutdownInternal();
	Aeron_AudioShutdown();
	SDL_StopTextInput(g_aeron.window);
	Aeron_ControllersShutdown();
	/* While the window exists: the TrackIR interface unregisters it. */
	AeronHeadTracker_Shutdown();
	Aeron_RenderBackendShutdown();
	Aeron_RelaunchBeforeWindowClose();
	Aeron_WindowShutdown();
	AeronVfs_DeinitInternal(&g_aeron.vfs);
	/* After the window and the files are closed, so the new process doesn't
	   share them. */
	Aeron_RelaunchRun();
	SDL_Quit();
	memset(&g_aeron, 0, sizeof(g_aeron));
}

void Aeron_RequestQuit(void) { g_aeron.quit_requested = 1; }

int Aeron_QuitRequested(void) { return g_aeron.quit_requested; }

void Aeron_ClearQuitRequest(void) {
	g_aeron.quit_requested = 0;
	Aeron_RelaunchCancel();
}

int Aeron_FatalErrorRequested(void) { return g_aeron.fatal_error_requested; }

AeronVfs* Aeron_GetVfs(void) { return &g_aeron.vfs; }
