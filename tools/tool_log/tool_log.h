#ifndef AERON_TOOL_LOG_H
#define AERON_TOOL_LOG_H

/*
 * The log of the tool libraries: opt2gltf, the cooker, imgbake, the effect
 * libraries, the XWAU importer and the bakes.
 *
 * Each call writes one line. Without a sink, the line goes to stderr in one
 * write, so lines from different threads don't mix. A program that runs the
 * tools in its own process sets a sink to receive the lines instead.
 */

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define TOOL_LOG_PRINTF(format_index, first_arg) __attribute__((format(printf, format_index, first_arg)))
#else
#define TOOL_LOG_PRINTF(format_index, first_arg)
#endif

typedef enum ToolLogLevel {
	TOOL_LOG_INFO,
	TOOL_LOG_WARNING,
	TOOL_LOG_ERROR,
} ToolLogLevel;

/* Receives each line, without its newline. It can be called from any thread
 * that logs. */
typedef void (*ToolLogSink)(void* user, ToolLogLevel level, const char* line);

/* Sets the sink, or NULL to write to stderr. Set it before any thread logs. */
void ToolLog_SetSink(ToolLogSink sink, void* user);

/* Writes one line. `format` has no newline at its end. The line is cut at
 * 4 KB. */
TOOL_LOG_PRINTF(2, 3) void ToolLog_Write(ToolLogLevel level, const char* format, ...);
void ToolLog_WriteV(ToolLogLevel level, const char* format, va_list args);

#ifdef __cplusplus
}
#endif

#endif /* AERON_TOOL_LOG_H */
