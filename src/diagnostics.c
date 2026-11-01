#include "internal.h"

#include <stdlib.h>

/* Crash dumps kept in the diagnostics directory; each can be tens of megabytes. */
#define AERON_DIAGNOSTICS_DUMP_HISTORY 4

static int SDLCALL Aeron_DiagnosticsCompareNames(const void* left, const void* right) {
	return SDL_strcmp(*(char* const*)left, *(char* const*)right);
}

void Aeron_DiagnosticsPrune(const char* directory, const char* pattern, int keep) {
	char   path[AERON_MAX_PATH];
	char** names;
	int    count;

	names = SDL_GlobDirectory(directory, pattern, 0, &count);
	if (!names) {
		return;
	}
	SDL_qsort(names, (size_t)count, sizeof *names, Aeron_DiagnosticsCompareNames);
	for (int index = 0; index < count - keep; ++index) {
		if (SDL_snprintf(path, sizeof path, "%s" AERON_PATH_SEPARATOR "%s", directory, names[index]) <
			(int)sizeof path) {
			/* Fails harmlessly on Windows while another instance still has the file open. */
			SDL_RemovePath(path);
		}
	}
	SDL_free(names);
}

#ifdef AERON_HAS_CRASH_HANDLER
/* Never equal to a real depth; keeps the compiler from proving the recursion endless. */
static volatile int s_crash_test_depth_limit = -1;

static int Aeron_CrashTestRecurse(volatile char* caller_frame, int depth) {
	volatile char frame[4096];
	int           result;

	frame[0] = caller_frame ? (char)(caller_frame[0] + 1) : 0;
	if (depth == s_crash_test_depth_limit) {
		return frame[0];
	}
	/* Passing the frame down and reading it afterwards keeps every frame live. */
	result = Aeron_CrashTestRecurse(frame, depth + 1);
	return result + frame[0];
}

/* AERON_TEST_CRASH=access|abort|stack faults deliberately once the handler is installed,
 * to check crash reporting on a player's machine. */
static void Aeron_CrashSelfTest(void) {
	const char* mode = SDL_getenv("AERON_TEST_CRASH");

	if (!mode) {
		return;
	}
	Aeron_LogWarn("aeron.crash", "AERON_TEST_CRASH=%s: crashing deliberately", mode);
	if (SDL_strcmp(mode, "abort") == 0) {
		abort();
	} else if (SDL_strcmp(mode, "stack") == 0) {
		Aeron_CrashTestRecurse(NULL, 0);
	} else {
		int* volatile target = (int*)(uintptr_t)0x10;
		*target              = 0;
	}
}
#endif

void Aeron_DiagnosticsInit(const AeronConfig* config) {
	static int  initialized;
	const char* org_name;
	const char* app_name;
	const char* app_version;
	char*       pref_path;
	char        directory[AERON_MAX_PATH];
	char        pattern[128];
	int         sdl_version;

	if (initialized) {
		return;
	}
	initialized = 1;
	/* Matches the VFS user root. Logs and crash dumps share one directory so a
	 * player can send everything by zipping a single folder. */
	org_name    = config && config->org_name ? config->org_name : "aeron";
	app_name    = config && config->app_name ? config->app_name : "aeron";
	app_version = config && config->app_version ? config->app_version : "unknown";
	pref_path   = SDL_GetPrefPath(org_name, app_name);
	if (!pref_path) {
		Aeron_LogWarn("aeron", "no user directory for logs and crash reports: %s", SDL_GetError());
		return;
	}
	if (SDL_snprintf(directory, sizeof directory, "%sdiagnostics", pref_path) >= (int)sizeof directory) {
		SDL_free(pref_path);
		return;
	}
	Aeron_LogFileInit(directory, app_name);
	sdl_version = SDL_GetVersion();
	Aeron_LogInfo("aeron", "%s %s on %s, SDL %d.%d.%d", app_name, app_version, SDL_GetPlatform(),
				  SDL_VERSIONNUM_MAJOR(sdl_version), SDL_VERSIONNUM_MINOR(sdl_version),
				  SDL_VERSIONNUM_MICRO(sdl_version));
	if (SDL_snprintf(pattern, sizeof pattern, "%s-*.dmp", app_name) < (int)sizeof pattern) {
		Aeron_DiagnosticsPrune(directory, pattern, AERON_DIAGNOSTICS_DUMP_HISTORY);
	}
#ifdef AERON_HAS_CRASH_HANDLER
	Aeron_CrashHandlerInit(directory, app_name, app_version);
	Aeron_CrashSelfTest();
#endif
	SDL_free(pref_path);
}
