#include "util.h"

#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "tool_log.h"

/* ---- Reporting ---------------------------------------------------------- */

static const char* g_report_program;

void SetReportProgram(const char* name) { g_report_program = name; }

void ReportV(const char* label, const char* format, va_list args) {
	char message[4096];
	vsnprintf(message, sizeof message, format, args);
	ToolLog_Write(TOOL_LOG_WARNING, "%s%s%s%s%s", g_report_program ? g_report_program : "",
				  g_report_program ? ": " : "", label ? label : "", label ? ": " : "", message);
}

void Report(const char* label, const char* format, ...) {
	va_list args;
	va_start(args, format);
	ReportV(label, format, args);
	va_end(args);
}

void ReportDetail(const char* label, const char* format, ...) {
	char    message[4096];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof message, format, args);
	va_end(args);
	ToolLog_Write(TOOL_LOG_DETAIL, "%s%s%s%s%s", g_report_program ? g_report_program : "",
				  g_report_program ? ": " : "", label ? label : "", label ? ": " : "", message);
}

typedef struct UnsupportedFeature {
	char*  name;
	size_t count;
} UnsupportedFeature;

static pthread_mutex_t     g_unsupported_mutex = PTHREAD_MUTEX_INITIALIZER;
static UnsupportedFeature* g_unsupported;
static size_t              g_unsupported_count;

void ReportUnsupported(const char* format, ...) {
	char    name[512];
	va_list args;
	va_start(args, format);
	vsnprintf(name, sizeof name, format, args);
	va_end(args);
	pthread_mutex_lock(&g_unsupported_mutex);
	size_t i = 0;
	while (i < g_unsupported_count && strcmp(g_unsupported[i].name, name) != 0)
		++i;
	if (i < g_unsupported_count) {
		g_unsupported[i].count++;
	} else {
		UnsupportedFeature* item = ARRAY_PUSH(g_unsupported, g_unsupported_count);
		if (item && !(item->name = StrDup(name)))
			g_unsupported_count--;
		else if (item)
			item->count = 1;
	}
	pthread_mutex_unlock(&g_unsupported_mutex);
}

void ReportUnsupportedSummary(void) {
	pthread_mutex_lock(&g_unsupported_mutex);
	for (size_t i = 0; i < g_unsupported_count; ++i) {
		Report(NULL, "the import doesn't convert %s (%zu %s)", g_unsupported[i].name, g_unsupported[i].count,
			   g_unsupported[i].count == 1 ? "time" : "times");
		free(g_unsupported[i].name);
	}
	free(g_unsupported);
	g_unsupported       = NULL;
	g_unsupported_count = 0;
	pthread_mutex_unlock(&g_unsupported_mutex);
}

static pthread_mutex_t g_fatal_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool            g_fatal;
static char            g_fatal_message[4096];

void ReportFatal(const char* label, const char* format, ...) {
	char    message[4096];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof message, format, args);
	va_end(args);
	ToolLog_Write(TOOL_LOG_ERROR, "%s%s%s%s%s", g_report_program ? g_report_program : "",
				  g_report_program ? ": " : "", label ? label : "", label ? ": " : "", message);
	pthread_mutex_lock(&g_fatal_mutex);
	if (!g_fatal) {
		snprintf(g_fatal_message, sizeof g_fatal_message, "%s%s%s", label ? label : "", label ? ": " : "",
				 message);
		g_fatal = true;
	}
	pthread_mutex_unlock(&g_fatal_mutex);
}

const char* ReportFatalMessage(void) {
	pthread_mutex_lock(&g_fatal_mutex);
	const bool fatal = g_fatal;
	pthread_mutex_unlock(&g_fatal_mutex);
	return fatal ? g_fatal_message : NULL;
}

void ClearReportFatal(void) {
	pthread_mutex_lock(&g_fatal_mutex);
	g_fatal = false;
	pthread_mutex_unlock(&g_fatal_mutex);
}

/* ---- Strings ------------------------------------------------------------ */

char* StrDup(const char* text) { return StrNDup(text, strlen(text)); }

char* StrNDup(const char* text, size_t len) {
	char* copy = (char*)malloc(len + 1);
	if (copy) {
		memcpy(copy, text, len);
		copy[len] = 0;
	}
	return copy;
}

void LowerCopy(char* out, size_t cap, const char* text) {
	size_t n = 0;
	for (; text[n] && n + 1 < cap; ++n)
		out[n] = (text[n] >= 'A' && text[n] <= 'Z') ? (char)(text[n] + 32) : text[n];
	out[n] = 0;
}

void LowerInPlace(char* text) {
	for (; *text; ++text)
		if (*text >= 'A' && *text <= 'Z')
			*text = (char)(*text + 32);
}

bool EndsWithNoCase(const char* text, const char* suffix) {
	const size_t a = strlen(text), b = strlen(suffix);
	return a >= b && strcasecmp(text + a - b, suffix) == 0;
}

int CompareStrings(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* ---- Files and folders -------------------------------------------------- */

char* ReadWholeFile(const char* path, size_t* size) {
	*size   = 0;
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	char* data = NULL;
	long  n    = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
	if (n >= 0 && fseek(f, 0, SEEK_SET) == 0 && (data = (char*)malloc((size_t)n + 1)) != NULL) {
		if (fread(data, 1, (size_t)n, f) != (size_t)n) {
			free(data);
			data = NULL;
		} else {
			data[n] = 0;
			*size   = (size_t)n;
		}
	}
	fclose(f);
	return data;
}

bool WriteWholeFile(const char* path, const void* data, size_t size) {
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	const bool ok = fwrite(data, 1, size, f) == size;
	return fclose(f) == 0 && ok;
}

bool IsFile(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDir(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

char** ListDir(const char* dir, bool sorted, size_t* count) {
	*count = 0;
	DIR* d = opendir(dir);
	if (!d)
		return NULL;
	char**         names = NULL;
	size_t         n     = 0;
	struct dirent* de;
	while ((de = readdir(d))) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		char*  name = StrDup(de->d_name);
		char** slot = name ? ARRAY_PUSH(names, n) : NULL;
		if (!slot) {
			free(name);
			FreeList(names, n);
			closedir(d);
			return NULL;
		}
		*slot = name;
	}
	closedir(d);
	if (sorted && n)
		qsort(names, n, sizeof *names, CompareStrings);
	*count = n;
	return names;
}

void FreeList(char** names, size_t count) {
	for (size_t i = 0; names && i < count; ++i)
		free(names[i]);
	free(names);
}

bool MakeDirs(const char* path) {
	if (IsDir(path))
		return true;
	struct stat st;
	if (stat(path, &st) == 0)
		return false;
	/* Create the parent first. */
	char* parent = StrDup(path);
	if (!parent)
		return false;
	for (size_t i = strlen(parent); i > 1; --i) {
		if (parent[i - 1] == '/' || parent[i - 1] == '\\') {
			parent[i - 1] = 0;
			if (parent[0] && !MakeDirs(parent)) {
				free(parent);
				return false;
			}
			break;
		}
	}
	free(parent);
#ifdef _WIN32
	const int made = _mkdir(path);
#else
	const int made = mkdir(path, 0755);
#endif
	return made == 0 || IsDir(path);
}

bool RemoveTree(const char* path) {
	struct stat st;
#ifdef _WIN32
	if (stat(path, &st) != 0)
#else
	/* A link is removed, not what it points to. */
	if (lstat(path, &st) != 0)
#endif
		return true;
	if (!S_ISDIR(st.st_mode))
		return remove(path) == 0;
	size_t count   = 0;
	char** entries = ListDir(path, false, &count);
	bool   ok      = true;
	for (size_t i = 0; ok && i < count; ++i) {
		char child[4096];
		ok = PathFormat(child, sizeof child, "%s/%s", path, entries[i]) && RemoveTree(child);
	}
	FreeList(entries, count);
#ifdef _WIN32
	return ok && _rmdir(path) == 0;
#else
	return ok && rmdir(path) == 0;
#endif
}

/* ---- Paths -------------------------------------------------------------- */

bool PathFormat(char* out, size_t cap, const char* format, ...) {
	va_list args;
	va_start(args, format);
	const int n = vsnprintf(out, cap, format, args);
	va_end(args);
	if (n >= 0 && (size_t)n < cap)
		return true;
	Report(NULL, "path too long: %s...", out);
	if (cap)
		out[0] = 0;
	return false;
}

/* ---- Arrays ------------------------------------------------------------- */

bool ArrayGrow(void** items, size_t count, size_t size) {
	void* grown = realloc(*items, (count + 1) * size);
	if (!grown)
		return false;
	memset((char*)grown + count * size, 0, size);
	*items = grown;
	return true;
}

/* Makes room for `more` bytes and the terminating zero. */
static bool text_reserve(TextBuffer* buffer, size_t more) {
	if (buffer->failed)
		return false;
	if (buffer->length + more + 1 <= buffer->capacity)
		return true;
	size_t capacity = buffer->capacity ? buffer->capacity * 2 : 4096;
	while (capacity < buffer->length + more + 1)
		capacity *= 2;
	char* grown = (char*)realloc(buffer->text, capacity);
	if (!grown) {
		free(buffer->text);
		*buffer = (TextBuffer) { .failed = true };
		return false;
	}
	buffer->text     = grown;
	buffer->capacity = capacity;
	return true;
}

void TextAppend(TextBuffer* buffer, const char* format, ...) {
	va_list args;
	va_start(args, format);
	const int n = vsnprintf(NULL, 0, format, args);
	va_end(args);
	if (n < 0 || !text_reserve(buffer, (size_t)n))
		return;
	va_start(args, format);
	vsnprintf(buffer->text + buffer->length, buffer->capacity - buffer->length, format, args);
	va_end(args);
	buffer->length += (size_t)n;
}

void TextAppendBytes(TextBuffer* buffer, const void* bytes, size_t length) {
	if (!text_reserve(buffer, length))
		return;
	memcpy(buffer->text + buffer->length, bytes, length);
	buffer->length += length;
	buffer->text[buffer->length] = '\0';
}
