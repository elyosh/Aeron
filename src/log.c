#include "internal.h"

#include <stdarg.h>

static SDL_LogPriority Aeron_LogSdlPriority(AeronLogLevel level) {
	switch (level) {
		case AERON_LOG_TRACE:
			return SDL_LOG_PRIORITY_TRACE;
		case AERON_LOG_VERBOSE:
			return SDL_LOG_PRIORITY_VERBOSE;
		case AERON_LOG_DEBUG:
			return SDL_LOG_PRIORITY_DEBUG;
		case AERON_LOG_INFO:
			return SDL_LOG_PRIORITY_INFO;
		case AERON_LOG_WARN:
			return SDL_LOG_PRIORITY_WARN;
		case AERON_LOG_ERROR:
			return SDL_LOG_PRIORITY_ERROR;
		case AERON_LOG_CRITICAL:
			return SDL_LOG_PRIORITY_CRITICAL;
		default:
			return SDL_LOG_PRIORITY_INFO;
	}
}

void Aeron_LogMessageV(AeronLogLevel level, const char* category, const char* fmt, va_list args) {
	char            message[1024];
	SDL_LogPriority priority;

	priority = Aeron_LogSdlPriority(level);
	SDL_vsnprintf(message, sizeof(message), fmt ? fmt : "", args);
	SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "%s: %s", category ? category : "aeron", message);
}

void Aeron_LogMessage(AeronLogLevel level, const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(level, category, fmt, args);
	va_end(args);
}

#if !defined(NDEBUG)
void Aeron_LogTrace(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_TRACE, category, fmt, args);
	va_end(args);
}
#endif

void Aeron_LogVerbose(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_VERBOSE, category, fmt, args);
	va_end(args);
}

#if !defined(NDEBUG)
void Aeron_LogDebug(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_DEBUG, category, fmt, args);
	va_end(args);
}
#endif

void Aeron_LogInfo(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_INFO, category, fmt, args);
	va_end(args);
}

void Aeron_LogWarn(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_WARN, category, fmt, args);
	va_end(args);
}

void Aeron_LogError(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_ERROR, category, fmt, args);
	va_end(args);
}

void Aeron_LogCritical(const char* category, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	Aeron_LogMessageV(AERON_LOG_CRITICAL, category, fmt, args);
	va_end(args);
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
	g_aeron.quit_requested        = 1;
	Aeron_LogCritical("aeron", "fatal: %s", message ? message : "Fatal error");
	Aeron_FatalError(title, message);
}
