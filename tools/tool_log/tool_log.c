#include "tool_log.h"

#include <stdio.h>
#include <string.h>

#define LINE_CAPACITY 4096

static ToolLogSink g_sink;
static void*       g_sink_user;
static int         g_details;

void ToolLog_SetSink(ToolLogSink sink, void* user) {
	g_sink      = sink;
	g_sink_user = user;
}

void ToolLog_SetDetails(int on) { g_details = on; }

int ToolLog_Details(void) { return g_details; }

void ToolLog_WriteV(ToolLogLevel level, const char* format, va_list args) {
	/* One more byte for the newline that stderr gets. */
	char line[LINE_CAPACITY + 1];
	if (level == TOOL_LOG_DETAIL && !g_details)
		return;
	int length = vsnprintf(line, LINE_CAPACITY, format, args);
	if (length < 0)
		return;
	if (length >= LINE_CAPACITY)
		length = LINE_CAPACITY - 1;
	if (g_sink) {
		g_sink(g_sink_user, level, line);
		return;
	}
	line[length]     = '\n';
	line[length + 1] = '\0';
	fputs(line, stderr);
}

void ToolLog_Write(ToolLogLevel level, const char* format, ...) {
	va_list args;
	va_start(args, format);
	ToolLog_WriteV(level, format, args);
	va_end(args);
}
