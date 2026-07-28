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
		Aeron_LogError("aeron", "could not resolve application-relative %s path: %s", description, SDL_GetError());
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

	memset(&g_aeron, 0, sizeof(g_aeron));
	g_aeron.clear_color_rgba[0] = 0.035f;
	g_aeron.clear_color_rgba[1] = 0.047f;
	g_aeron.clear_color_rgba[2] = 0.071f;
	g_aeron.clear_color_rgba[3] = 1.0f;
	if (config && config->clear_color_enabled) {
		memcpy(g_aeron.clear_color_rgba, config->clear_color_rgba, sizeof g_aeron.clear_color_rgba);
	}

	Aeron_CopyString(g_aeron.app_name, sizeof(g_aeron.app_name),
					 config && config->app_name ? config->app_name : "aeron");
	SDL_SetAppMetadata(g_aeron.app_name, NULL, NULL);

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
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

	if (!Aeron_WindowInit(config)) {
		SDL_Quit();
		return 0;
	}

	if (!Aeron_RenderBackendInit()) {
		Aeron_WindowShutdown();
		SDL_Quit();
		return 0;
	}

	if (!Aeron_AudioInit()) {
		/* Audio is non-fatal: continue without sound rather than failing init. */
		Aeron_LogWarn("aeron", "audio subsystem unavailable; continuing without sound");
	}

	Aeron_GamepadsInit();
	SDL_StartTextInput(g_aeron.window);
	Aeron_InitVfs(config);
	Aeron_DebugUiInitInternal();
	g_aeron.input.has_focus = 1;
	g_aeron.last_frame_us   = Aeron_NowUs();
	g_aeron.initialized     = 1;
	Aeron_SetPresentationVsyncDivisor(1);

	return 1;
}

void Aeron_Shutdown(void) {
	if (!g_aeron.initialized) {
		return;
	}

	Aeron_DebugUiShutdownInternal();
	Aeron_AudioShutdown();
	SDL_StopTextInput(g_aeron.window);
	Aeron_GamepadsShutdown();
	Aeron_RenderBackendShutdown();
	Aeron_WindowShutdown();
	AeronVfs_DeinitInternal(&g_aeron.vfs);
	SDL_Quit();
	memset(&g_aeron, 0, sizeof(g_aeron));
}

void Aeron_RequestQuit(void) { g_aeron.quit_requested = 1; }

int Aeron_QuitRequested(void) { return g_aeron.quit_requested; }

int Aeron_FatalErrorRequested(void) { return g_aeron.fatal_error_requested; }

AeronVfs* Aeron_GetVfs(void) { return &g_aeron.vfs; }
