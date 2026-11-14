#include "internal.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* Kept outside g_aeron, which Aeron_Shutdown clears before the process
 * exits. */
static struct {
	int    requested;
	char** args;
	int    count;
} s_relaunch;

static void free_args(void) {
	for (int i = 0; i < s_relaunch.count; ++i) {
		SDL_free(s_relaunch.args[i]);
	}
	SDL_free(s_relaunch.args);
	s_relaunch.args  = NULL;
	s_relaunch.count = 0;
}

#ifdef __APPLE__
/* The bundle that holds the executable, or 0 when it doesn't run from one.
 * SDL's base path is <bundle>/Contents/Resources/ in a bundle. */
static int bundle_path(char* out, size_t capacity) {
	static const char k_suffix[] = ".app/Contents/Resources/";
	const char*       base       = SDL_GetBasePath();
	size_t            length;

	if (!base) {
		return 0;
	}
	length = SDL_strlen(base);
	if (length < sizeof k_suffix - 1 || SDL_strcmp(base + length - (sizeof k_suffix - 1), k_suffix) != 0) {
		return 0;
	}
	/* Keeps ".app". */
	length -= sizeof "/Contents/Resources/" - 1;
	if (length >= capacity) {
		return 0;
	}
	SDL_memcpy(out, base, length);
	out[length] = '\0';
	return 1;
}
#endif

static int request_relaunch(const char* const* args, int count) {
	char path[AERON_MAX_PATH];

	if (count < 0 || (count > 0 && !args)) {
		SDL_SetError("invalid relaunch arguments");
		return 0;
	}
	if (!Aeron_ExecutablePath(path, sizeof path)) {
		return 0;
	}
	if (!SDL_GetPathInfo(path, NULL)) {
		return 0;
	}
	Aeron_RelaunchCancel();
	if (count > 0) {
		s_relaunch.args = (char**)SDL_calloc((size_t)count, sizeof *s_relaunch.args);
		if (!s_relaunch.args) {
			return 0;
		}
		for (int i = 0; i < count; ++i) {
			s_relaunch.args[i] = SDL_strdup(args[i] ? args[i] : "");
			if (!s_relaunch.args[i]) {
				s_relaunch.count = i;
				free_args();
				return 0;
			}
		}
	}
	s_relaunch.count     = count;
	s_relaunch.requested = 1;
	return 1;
}

int Aeron_RequestRelaunch(const char* const* args, int count) {
	if (!request_relaunch(args, count)) {
		Aeron_LogError("aeron", "cannot start %s again: %s", g_aeron.app_name, SDL_GetError());
		return 0;
	}
	Aeron_RequestQuit();
	return 1;
}

void Aeron_RelaunchCancel(void) {
	free_args();
	s_relaunch.requested = 0;
}

void Aeron_RelaunchBeforeWindowClose(void) {
#ifdef _WIN32
	/* Windows lets a new window come to the front only if the process that
	   started it may do so. This process loses that right when its window
	   closes, so it passes it on while the window is still in front. */
	if (s_relaunch.requested) {
		AllowSetForegroundWindow(ASFW_ANY);
	}
#endif
}

/* Starts `argv` and returns its process. A background process doesn't belong
   to this one: its input and output go to the null device, and on Linux and
   macOS it runs in a session of its own, so closing the terminal doesn't stop
   it. */
static SDL_Process* start_process(const char* const* argv, int background) {
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_Process*     process;

	if (!props) {
		return NULL;
	}
	SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)argv);
	SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, background != 0);
	process = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);
	return process;
}

static int relaunch(void) {
	char         path[AERON_MAX_PATH];
	const char** argv;
	int          argc = 0;
	int          ok   = 0;

	/* The program, `open` and its options on macOS, the arguments and the
	   final NULL. */
	argv = (const char**)SDL_calloc((size_t)s_relaunch.count + 5, sizeof *argv);
	if (!argv) {
		return 0;
	}
#ifdef __APPLE__
	/* LaunchServices starts a new copy of the bundle (-n), which then comes
	   to the front like an app started from the Finder. It starts in "/". */
	if (bundle_path(path, sizeof path)) {
		SDL_Process* process;
		uint64_t     deadline_ms = SDL_GetTicks() + 5000;
		int          exit_code   = -1;
		int          exited      = 0;

		argv[argc++] = "/usr/bin/open";
		argv[argc++] = "-n";
		argv[argc++] = path;
		argv[argc++] = "--args";
		for (int i = 0; i < s_relaunch.count; ++i) {
			argv[argc++] = s_relaunch.args[i];
		}
		process = start_process(argv, 0);
		if (process) {
			/* open returns once the new copy has started. */
			while (!(exited = SDL_WaitProcess(process, false, &exit_code)) && SDL_GetTicks() < deadline_ms) {
				SDL_Delay(10);
			}
			if (!exited) {
				/* The new copy is most likely on its way. Saying that it
				   failed would make the player start a second one. */
				Aeron_LogWarn("aeron", "open is still running after 5 seconds");
				ok = 1;
			} else if (exit_code == 0) {
				ok = 1;
			} else {
				SDL_SetError("open failed with code %d", exit_code);
			}
			SDL_DestroyProcess(process);
		}
		SDL_free(argv);
		return ok;
	}
#endif
	if (Aeron_ExecutablePath(path, sizeof path)) {
		SDL_Process* process;

		argv[argc++] = path;
		for (int i = 0; i < s_relaunch.count; ++i) {
			argv[argc++] = s_relaunch.args[i];
		}
		process = start_process(argv, 1);
		if (process) {
			ok = 1;
			/* Doesn't stop the process. */
			SDL_DestroyProcess(process);
		}
	}
	SDL_free(argv);
	return ok;
}

void Aeron_RelaunchRun(void) {
	if (!s_relaunch.requested) {
		return;
	}
	if (relaunch()) {
		Aeron_LogInfo("aeron", "started a new copy of %s", g_aeron.app_name);
	} else {
		char message[512];

		Aeron_LogError("aeron", "cannot start %s again: %s", g_aeron.app_name, SDL_GetError());
		SDL_snprintf(message, sizeof message, "%s couldn't start again. Start it again yourself.",
					 g_aeron.app_name);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, g_aeron.app_name, message, NULL);
	}
	Aeron_RelaunchCancel();
}
