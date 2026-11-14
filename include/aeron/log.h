#ifndef AERON_LOG_H
#define AERON_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AeronLogLevel {
	AERON_LOG_TRACE,
	AERON_LOG_VERBOSE,
	AERON_LOG_DEBUG,
	AERON_LOG_INFO,
	AERON_LOG_WARN,
	AERON_LOG_ERROR,
	AERON_LOG_CRITICAL,
} AeronLogLevel;

/* Dynamic-level entry points for log adapters and forwarding callbacks. */
void Aeron_LogMessageV(AeronLogLevel level, const char* category, const char* fmt, va_list args);
void Aeron_LogMessage(AeronLogLevel level, const char* category, const char* fmt, ...);

/* Trace and debug calls are removed from builds that define NDEBUG. */
#if defined(NDEBUG)
#define Aeron_LogTrace(...) ((void)0)
#else
void Aeron_LogTrace(const char* category, const char* fmt, ...);
#endif
void Aeron_LogVerbose(const char* category, const char* fmt, ...);
#if defined(NDEBUG)
#define Aeron_LogDebug(...) ((void)0)
#else
void Aeron_LogDebug(const char* category, const char* fmt, ...);
#endif
void Aeron_LogInfo(const char* category, const char* fmt, ...);
void Aeron_LogWarn(const char* category, const char* fmt, ...);
void Aeron_LogError(const char* category, const char* fmt, ...);
/* Critical records severity only; it does not request shutdown or show a dialog. */
void Aeron_LogCritical(const char* category, const char* fmt, ...);

/*
 * Thread sinks. A thread can send its log lines to a sink instead of the
 * log. A tool that runs inside a program, such as the mod importer in the
 * game, uses one to collect its own lines. The sink gets every line that the
 * thread logs through SDL: Aeron_Log* lines, direct SDL_Log calls and SDL's
 * own messages. Those lines don't go to the log. Threads without a sink log
 * normally.
 *
 * Rules:
 *   - A thread made with Aeron_ThreadCreate gets the sink of the thread that
 *     made it. Threads made in other ways, such as with pthread_create or
 *     std::thread, start without a sink.
 *   - The sink is called while SDL holds its log lock. If the sink takes a
 *     lock of its own, code that holds that lock must never log, or two
 *     threads can deadlock.
 *   - A thread that got a sink must end before the sink's owner frees what
 *     the sink uses.
 *   - Sinks need Aeron's SDL log output. Aeron_Init installs it. A program
 *     that doesn't call Aeron_Init, such as a command line tool, calls
 *     Aeron_LogInstallOutput at its start.
 */

/* Receives a line of a thread that has this sink. `message` has no newline.
 * SDL's own messages start with their category, such as "sdl.video: ". */
typedef void (*AeronLogSink)(void* user, AeronLogLevel level, const char* message);

/* Installs Aeron's SDL log output. Later calls do nothing. */
void Aeron_LogInstallOutput(void);
/* Sets the calling thread's sink, or removes it when `sink` is NULL. Returns
 * 0 when the sink can't be stored. The lines then go to the log. */
int Aeron_LogSetThreadSink(AeronLogSink sink, void* user);
/* Gets the calling thread's sink. Both are NULL when it has none. */
void Aeron_LogGetThreadSink(AeronLogSink* sink, void** user);

/* Shows a modal fatal-error message box attached to the Aeron window when
 * available. It tells the player where the log files are, asks them to send
 * them, and can open their folder. Errors that the player can fix themselves
 * should use Aeron_ShowMessageBox. */
void Aeron_FatalError(const char* title, const char* message);

#ifdef __cplusplus
}
#endif

#endif
