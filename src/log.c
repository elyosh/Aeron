#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

/* Previous session logs (<app>-<date>-<time>.log) kept beside the current one. */
#define AERON_LOG_FILE_HISTORY 20
/* Distinct SDL warnings logged per session. Later ones are dropped. */
#define AERON_LOG_SDL_WARNING_LIMIT 128

static struct {
	SDL_IOStream*         stream;
	SDL_LogOutputFunction forward;
	void*                 forward_userdata;
	/* Performance counter, unlike SDL ticks, keeps running across SDL_Quit. */
	Uint64 start_counter;
	/* The folder of the log file, which also holds the crash reports. */
	char directory[AERON_MAX_PATH];
	/* Hashes of the SDL warnings already logged. */
	Uint32 sdl_warnings[AERON_LOG_SDL_WARNING_LIMIT];
	int    sdl_warning_count;
} s_log_file;

static const char* const k_log_priority_names[SDL_LOG_PRIORITY_COUNT] = {
	"", "TRACE", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL",
};

/* Aeron's messages use the application category and name their own. SDL's
 * other categories get these names. */
static const char* const k_log_sdl_category_names[] = {
	NULL,        "sdl.error",  "sdl.assert", "sdl.system", "sdl.audio",
	"sdl.video", "sdl.render", "sdl.input",  "sdl.test",   "sdl.gpu",
};

/* Returns whether an SDL warning was logged before, and remembers it. Some
 * are sent on every call, such as one for each texture upload in GPU debug
 * mode on Direct3D 12. */
static int Aeron_LogSdlWarningSeen(const char* message) {
	const Uint32 hash = SDL_murmur3_32(message, SDL_strlen(message), 0);

	for (int index = 0; index < s_log_file.sdl_warning_count; ++index) {
		if (s_log_file.sdl_warnings[index] == hash) {
			return 1;
		}
	}
	if (s_log_file.sdl_warning_count == AERON_LOG_SDL_WARNING_LIMIT) {
		return 1;
	}
	s_log_file.sdl_warnings[s_log_file.sdl_warning_count++] = hash;
	return 0;
}

/* Runs under SDL's log output lock, so writes from different threads do not interleave. */
static void SDLCALL Aeron_LogFileOutput(void* userdata, int category, SDL_LogPriority priority,
										const char* message) {
	char   prefix[64];
	Uint64 elapsed_ms;
	int    length;

	(void)userdata;
	/* SDL's warnings are logged once each. Its errors are all logged. */
	if (category != SDL_LOG_CATEGORY_APPLICATION && priority <= SDL_LOG_PRIORITY_WARN && message &&
		Aeron_LogSdlWarningSeen(message)) {
		return;
	}
	if (s_log_file.forward) {
		s_log_file.forward(s_log_file.forward_userdata, category, priority, message);
	}
	if (!s_log_file.stream) {
		return;
	}
	elapsed_ms =
		(SDL_GetPerformanceCounter() - s_log_file.start_counter) * 1000 / SDL_GetPerformanceFrequency();
	length = SDL_snprintf(prefix, sizeof prefix, "[%6llu.%03u] %08llx %-8s ",
						  (unsigned long long)(elapsed_ms / 1000), (unsigned)(elapsed_ms % 1000),
						  (unsigned long long)SDL_GetCurrentThreadID(),
						  priority > SDL_LOG_PRIORITY_INVALID && priority < SDL_LOG_PRIORITY_COUNT
							  ? k_log_priority_names[priority]
							  : "?");
	if (length > 0) {
		SDL_WriteIO(s_log_file.stream, prefix, (size_t)SDL_min(length, (int)sizeof prefix - 1));
	}
	if (category > SDL_LOG_CATEGORY_APPLICATION && category < (int)SDL_arraysize(k_log_sdl_category_names)) {
		SDL_WriteIO(s_log_file.stream, k_log_sdl_category_names[category],
					SDL_strlen(k_log_sdl_category_names[category]));
		SDL_WriteIO(s_log_file.stream, ": ", 2);
	}
	SDL_WriteIO(s_log_file.stream, message ? message : "", message ? SDL_strlen(message) : 0);
	SDL_WriteIO(s_log_file.stream, "\n", 1);
}

void Aeron_LogFileInit(const char* directory, const char* app_name) {
	char         stamp[128];
	char         path[AERON_MAX_PATH];
	SDL_Time     now;
	SDL_DateTime date;
	int          length;

	if (s_log_file.stream || !directory || !app_name) {
		return;
	}
	if (!SDL_CreateDirectory(directory)) {
		Aeron_LogWarn("aeron.log", "cannot create log directory %s: %s", directory, SDL_GetError());
		return;
	}
	if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &date, true)) {
		Aeron_LogWarn("aeron.log", "cannot read the clock to name the log file: %s", SDL_GetError());
		return;
	}
	length = SDL_snprintf(stamp, sizeof stamp, "%s-%04d%02d%02d-%02d%02d%02d", app_name, date.year,
						  date.month, date.day, date.hour, date.minute, date.second);
	if (length < 0 || length >= (int)sizeof stamp) {
		return;
	}
	if (SDL_snprintf(path, sizeof path, "%s-*.log", app_name) < (int)sizeof path) {
		Aeron_DiagnosticsPrune(directory, path, AERON_LOG_FILE_HISTORY);
	}
	/* Instances started within the same second each get their own file; the
	 * "_N" suffix sorts after the unsuffixed name, keeping pruning order. */
	for (int attempt = 1; attempt <= 9 && !s_log_file.stream; ++attempt) {
		length = attempt == 1
					 ? SDL_snprintf(path, sizeof path, "%s" AERON_PATH_SEPARATOR "%s.log", directory, stamp)
					 : SDL_snprintf(path, sizeof path, "%s" AERON_PATH_SEPARATOR "%s_%d.log", directory,
									stamp, attempt);
		if (length < 0 || length >= (int)sizeof path) {
			return;
		}
		/* Exclusive create, so a concurrent instance never truncates this file. */
		s_log_file.stream = SDL_IOFromFile(path, "wx");
	}
	if (!s_log_file.stream) {
		Aeron_LogWarn("aeron.log", "cannot open log file %s: %s", path, SDL_GetError());
		return;
	}
	/* Unbuffered, so a crash keeps every line. SDL streams have no write buffer
	 * on Windows. SDL_FlushIO is not used because it waits for the drive. */
	FILE* file = (FILE*)SDL_GetPointerProperty(SDL_GetIOProperties(s_log_file.stream),
											   SDL_PROP_IOSTREAM_STDIO_FILE_POINTER, NULL);
	if (file) {
		setvbuf(file, NULL, _IONBF, 0);
	}
	SDL_strlcpy(s_log_file.directory, directory, sizeof s_log_file.directory);
	s_log_file.start_counter = SDL_GetPerformanceCounter();
	SDL_GetLogOutputFunction(&s_log_file.forward, &s_log_file.forward_userdata);
	SDL_SetLogOutputFunction(Aeron_LogFileOutput, NULL);
	Aeron_LogInfo("aeron.log", "session started %04d-%02d-%02d %02d:%02d:%02d, logging to %s", date.year,
				  date.month, date.day, date.hour, date.minute, date.second, path);
}

void* Aeron_LogFileNativeHandle(void) {
#ifdef _WIN32
	if (s_log_file.stream) {
		return SDL_GetPointerProperty(SDL_GetIOProperties(s_log_file.stream),
									  SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER, NULL);
	}
#endif
	return NULL;
}

int Aeron_LogFileDescriptor(void) {
#ifndef _WIN32
	if (s_log_file.stream) {
		return (int)SDL_GetNumberProperty(SDL_GetIOProperties(s_log_file.stream),
										  SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1);
	}
#endif
	return -1;
}

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
	enum { k_open_folder = 1, k_close = 2 };

	static const AeronMessageBoxButton buttons[] = {
		{ k_open_folder, "Open Folder", 1, 0 },
		{ k_close, "Close", 0, 1 },
	};
	char text[4096];
	int  selected = k_close;

	title   = title ? title : "Aeron";
	message = message ? message : "Fatal error";
	/* Without a log file there is nothing to send. */
	if (!s_log_file.stream) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, g_aeron.window);
		return;
	}
	SDL_snprintf(text, sizeof text,
				 "%s\n\nThe log files are in:\n%s\n\n"
				 "Please zip and send the whole \"diagnostics\" folder when reporting the problem.",
				 message, s_log_file.directory);
	const AeronMessageBoxOptions options = {
		.kind         = AERON_MESSAGE_BOX_ERROR,
		.title        = title,
		.message      = text,
		.buttons      = buttons,
		.button_count = SDL_arraysize(buttons),
	};
	if (!Aeron_ShowMessageBox(&options, &selected)) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, g_aeron.window);
		return;
	}
	if (selected == k_open_folder) {
		Aeron_OpenFolder(s_log_file.directory);
	}
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
