#ifndef AERON_OPT2GLTF_UTIL_H
#define AERON_OPT2GLTF_UTIL_H

/*
 * What the converter's modules share: reporting, strings, files, folders,
 * paths and growing arrays.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(__GNUC__)
#define UTIL_PRINTF(format_index, first_arg) __attribute__((format(printf, format_index, first_arg)))
#else
#define UTIL_PRINTF(format_index, first_arg)
#endif

/* ---- Reporting ---------------------------------------------------------- */

/* Reports a problem or a summary to stderr: "opt2gltf: <label>: <message>",
 * or "opt2gltf: <message>" when `label` is NULL. */
UTIL_PRINTF(2, 3) void Report(const char* label, const char* format, ...);
void ReportV(const char* label, const char* format, va_list args);

/* ---- Strings ------------------------------------------------------------ */

/* A copy of `text`, or of its `len` first bytes; NULL when out of memory. */
char* StrDup(const char* text);
char* StrNDup(const char* text, size_t len);

/* ASCII lower case (file names are UTF-8): of `text` into `out` (truncated
 * to `cap`), or of `text` in place. */
void LowerCopy(char* out, size_t cap, const char* text);
void LowerInPlace(char* text);

/* Whether `text` ends with `suffix`, ASCII letters compared without case. */
bool EndsWithNoCase(const char* text, const char* suffix);

/* qsort's comparison of `char*` elements, by strcmp. */
int CompareStrings(const void* a, const void* b);

/* ---- Files and folders -------------------------------------------------- */

/* The bytes of file `path` and their count, followed by a 0 byte; NULL when
 * it cannot be read. */
char* ReadWholeFile(const char* path, size_t* size);

/* Writes `size` bytes as file `path`; false on an I/O error. */
bool WriteWholeFile(const char* path, const void* data, size_t size);

bool IsFile(const char* path);
bool IsDir(const char* path);

/* The entries of folder `dir` (not "." and ".."), in the order the file
 * system lists them, or sorted by name with `sorted`. NULL with *count 0
 * when it cannot be read. */
char** ListDir(const char* dir, bool sorted, size_t* count);
void   FreeList(char** names, size_t count);

/* Creates folder `path` and its missing parents; true when it exists. */
bool MakeDirs(const char* path);

/* Removes `path`, a file or a folder and everything in it. */
bool RemoveTree(const char* path);

/* ---- Paths -------------------------------------------------------------- */

/* Formats a path into `out`. False, with `out` empty, when it does not fit
 * (reported): a path too long is an error, not another file. */
UTIL_PRINTF(3, 4) bool PathFormat(char* out, size_t cap, const char* format, ...);

/* ---- Arrays ------------------------------------------------------------- */

/* Grows `*items`, holding `count` elements of `size` bytes, by one zeroed
 * element; false when out of memory (the array is unchanged). */
bool ArrayGrow(void** items, size_t count, size_t size);

/* Appends a zeroed element to the array `items` holding `count` (an integer
 * lvalue, incremented) and returns it; NULL when out of memory. */
#define ARRAY_PUSH(items, count)                                                                             \
	(ArrayGrow((void**)&(items), (size_t)(count), sizeof *(items)) ? &(items)[(count)++] : NULL)

#endif /* AERON_OPT2GLTF_UTIL_H */
