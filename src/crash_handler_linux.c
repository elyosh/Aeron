#define _GNU_SOURCE

#include "internal.h"

#include <elf.h>
#include <execinfo.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

/*
 * Linux crash reporting. Fatal signals append the signal, faulting address and
 * a module+offset backtrace to the session log, then re-raise with the default
 * action so a configured core dump handler (systemd-coredump) still runs.
 * Only the main thread gets an alternate signal stack; a stack overflow on
 * another thread terminates without a report.
 */

#define AERON_CRASH_STACK_FRAMES 256
#define AERON_CRASH_ALT_STACK_SIZE (64 * 1024)

static const int k_crash_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };

static struct {
	SDL_AtomicInt crashing;
	pthread_t     crashing_thread;
	char          app_name[64];
	char          app_version[64];
	char          build_id[64];
} s_crash;

static char s_crash_alt_stack[AERON_CRASH_ALT_STACK_SIZE];

static void Aeron_CrashWriteAll(int fd, const char* data, size_t length) {
	while (length > 0) {
		ssize_t written = write(fd, data, length);
		if (written <= 0) {
			return;
		}
		data += written;
		length -= (size_t)written;
	}
}

/* Writes to the session log and stderr. SDL_vsnprintf does not allocate for these formats. */
static void Aeron_CrashLog(const char* fmt, ...) {
	char    line[512];
	va_list args;
	int     length;
	int     log_fd = Aeron_LogFileDescriptor();

	va_start(args, fmt);
	length = SDL_vsnprintf(line, sizeof line - 1, fmt, args);
	va_end(args);
	if (length < 0) {
		return;
	}
	if (length > (int)sizeof line - 2) {
		length = (int)sizeof line - 2;
	}
	line[length++] = '\n';
	if (log_fd >= 0) {
		Aeron_CrashWriteAll(log_fd, line, (size_t)length);
	}
	Aeron_CrashWriteAll(STDERR_FILENO, line, (size_t)length);
}

static const char* Aeron_CrashSignalName(int signal_number) {
	switch (signal_number) {
		case SIGSEGV:
			return "SIGSEGV (segmentation fault)";
		case SIGBUS:
			return "SIGBUS (bus error)";
		case SIGILL:
			return "SIGILL (illegal instruction)";
		case SIGFPE:
			return "SIGFPE (arithmetic exception)";
		case SIGABRT:
			return "SIGABRT (abort)";
		default:
			return "fatal signal";
	}
}

static void* Aeron_CrashProgramCounter(const void* context) {
	const ucontext_t* user_context = (const ucontext_t*)context;
#if defined(__x86_64__)
	return (void*)(uintptr_t)user_context->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
	return (void*)(uintptr_t)user_context->uc_mcontext.pc;
#else
	(void)user_context;
	return NULL;
#endif
}

/* Prints one module+offset line per frame, folding runs of one frame (deep recursion). */
static void Aeron_CrashWriteFrames(int fd, void* const* frames, int count) {
	char line[64];
	int  length;

	for (int index = 0; index < count;) {
		int run = 1;
		while (index + run < count && frames[index + run] == frames[index]) {
			++run;
		}
		backtrace_symbols_fd((void**)&frames[index], 1, fd);
		if (run > 1) {
			length = SDL_snprintf(line, sizeof line, "  (repeated %d more times)\n", run - 1);
			if (length > 0 && length < (int)sizeof line) {
				Aeron_CrashWriteAll(fd, line, (size_t)length);
			}
		}
		index += run;
	}
}

static void Aeron_CrashSignalHandler(int signal_number, siginfo_t* info, void* context) {
	static void* frames[AERON_CRASH_STACK_FRAMES];
	int          count;
	int          log_fd;

	if (!SDL_CompareAndSwapAtomicInt(&s_crash.crashing, 0, 1)) {
		/* The report itself faulted: die now. Any other thread waits for the
		 * reporting thread, whose re-raised signal ends the process. */
		if (pthread_equal(pthread_self(), s_crash.crashing_thread)) {
			signal(signal_number, SIG_DFL);
			raise(signal_number);
			return;
		}
		for (;;) {
			pause();
		}
	}
	s_crash.crashing_thread = pthread_self();
	Aeron_CrashLog("crash: %s %s: %s (code %d) at pc %p on thread %08llx", s_crash.app_name,
				   s_crash.app_version, Aeron_CrashSignalName(signal_number), info->si_code,
				   Aeron_CrashProgramCounter(context), (unsigned long long)SDL_GetCurrentThreadID());
	if (signal_number != SIGABRT) {
		Aeron_CrashLog("crash: fault address %p", info->si_addr);
	}
	Aeron_CrashLog("crash: build id %s", s_crash.build_id);
	Aeron_CrashLog("crash: call stack:");
	/* Frame 0 is this handler; the signal trampoline and the faulting frame follow. */
	count  = backtrace(frames, AERON_CRASH_STACK_FRAMES);
	log_fd = Aeron_LogFileDescriptor();
	if (log_fd >= 0) {
		Aeron_CrashWriteFrames(log_fd, frames + 1, count - 1);
	}
	Aeron_CrashWriteFrames(STDERR_FILENO, frames + 1, count - 1);

	/* Blocked until this handler returns, then delivered with the default action. */
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

/* Records the executable's GNU build ID, which identifies the matching symbols archive. */
static int Aeron_CrashReadBuildId(struct dl_phdr_info* info, size_t size, void* userdata) {
	(void)size;
	(void)userdata;
	for (int header = 0; header < info->dlpi_phnum; ++header) {
		const ElfW(Phdr)* segment = &info->dlpi_phdr[header];
		const unsigned char* note;
		const unsigned char* end;

		if (segment->p_type != PT_NOTE) {
			continue;
		}
		note = (const unsigned char*)(uintptr_t)(info->dlpi_addr + segment->p_vaddr);
		end  = note + segment->p_memsz;
		while (note + sizeof(ElfW(Nhdr)) <= end) {
			const ElfW(Nhdr)* note_header = (const ElfW(Nhdr)*)(const void*)note;
			const unsigned char* name     = note + sizeof *note_header;
			const unsigned char* desc     = name + ((note_header->n_namesz + 3u) & ~3u);

			if (note_header->n_type == NT_GNU_BUILD_ID && note_header->n_namesz == 4 &&
				memcmp(name, "GNU", 4) == 0) {
				for (ElfW(Word) index = 0;
					 index < note_header->n_descsz && index * 2 + 2 < sizeof s_crash.build_id; ++index) {
					SDL_snprintf(s_crash.build_id + index * 2, 3, "%02x", desc[index]);
				}
				return 1;
			}
			note = desc + ((note_header->n_descsz + 3u) & ~3u);
		}
	}
	/* The first object reported is the executable itself; stop there. */
	return 1;
}

void Aeron_CrashHandlerInit(const char* directory, const char* app_name, const char* app_version) {
	static int       installed;
	stack_t          alt_stack;
	struct sigaction action;
	void*            warm_frame;

	(void)directory;
	if (installed) {
		return;
	}
	installed = 1;
	Aeron_CopyString(s_crash.app_name, sizeof s_crash.app_name, app_name);
	Aeron_CopyString(s_crash.app_version, sizeof s_crash.app_version, app_version);
	Aeron_CopyString(s_crash.build_id, sizeof s_crash.build_id, "unknown");
	dl_iterate_phdr(Aeron_CrashReadBuildId, NULL);
	/* The first backtrace() call loads libgcc_s, which allocates; do it now, not in the handler. */
	backtrace(&warm_frame, 1);

	alt_stack.ss_sp    = s_crash_alt_stack;
	alt_stack.ss_size  = sizeof s_crash_alt_stack;
	alt_stack.ss_flags = 0;
	if (sigaltstack(&alt_stack, NULL) != 0) {
		Aeron_LogWarn("aeron.crash", "no alternate signal stack; stack overflows will not be reported");
	}
	memset(&action, 0, sizeof action);
	action.sa_sigaction = Aeron_CrashSignalHandler;
	action.sa_flags     = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	for (size_t index = 0; index < SDL_arraysize(k_crash_signals); ++index) {
		sigaction(k_crash_signals[index], &action, NULL);
	}
	Aeron_LogInfo("aeron.crash", "build id %s; crashes are reported in the session log", s_crash.build_id);
}
