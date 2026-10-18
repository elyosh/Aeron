#include "util.h"

#include <dirent.h>
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

/* ---- Reporting ---------------------------------------------------------- */

static const char* g_report_program;

void SetReportProgram(const char* name) { g_report_program = name; }

void ReportV(const char* label, const char* format, va_list args) {
	if (g_report_program)
		fprintf(stderr, "%s: ", g_report_program);
	if (label)
		fprintf(stderr, "%s: ", label);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}

void Report(const char* label, const char* format, ...) {
	va_list args;
	va_start(args, format);
	ReportV(label, format, args);
	va_end(args);
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
	if (stat(path, &st) != 0)
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
