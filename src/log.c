#include "internal.h"

#include <stdarg.h>

void Aeron_Log(const char* category, const char* fmt, ...) {
	char    message[1024];
	va_list args;

	va_start(args, fmt);
	SDL_vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	SDL_Log("%s: %s", category ? category : "aeron", message);
}

void Aeron_FatalError(const char* title, const char* message) {
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title ? title : "Aeron", message ? message : "Fatal error",
							 g_aeron.window);
}

void Aeron_RequestFatalError(const char* title, const char* message) {
	if (g_aeron.fatal_error_requested) {
		return;
	}
	g_aeron.fatal_error_requested = 1;
	g_aeron.quit_requested = 1;
	Aeron_Log("aeron", "fatal: %s", message ? message : "Fatal error");
	Aeron_FatalError(title, message);
}
