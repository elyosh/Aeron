#ifndef AERON_OPT2GLTF_UTIL_H
#define AERON_OPT2GLTF_UTIL_H

/*
 * Helpers shared by the converter's modules: reporting, strings, files,
 * folders, paths and growing arrays.
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

/* Prints a problem or a summary to stderr as "opt2gltf: <label>: <message>",
 * or as "opt2gltf: <message>" when `label` is NULL. */
UTIL_PRINTF(2, 3) void Report(const char* label, const char* format, ...);
void ReportV(const char* label, const char* format, va_list args);

/* ---- Strings ------------------------------------------------------------ */

/* Returns a copy of `text`, or of its first `len` bytes. Returns NULL when out
 * of memory. */
char* StrDup(const char* text);
char* StrNDup(const char* text, size_t len);

/* Converts ASCII letters to lower case and leaves other bytes alone, because
 * file names are UTF-8. LowerCopy writes the result into `out` (truncated to
 * `cap`), and LowerInPlace changes `text` itself. */
void LowerCopy(char* out, size_t cap, const char* text);
void LowerInPlace(char* text);

/* Whether `text` ends with `suffix`, comparing ASCII letters in any case. */
bool EndsWithNoCase(const char* text, const char* suffix);

/* qsort comparison function for `char*` elements, using strcmp. */
int CompareStrings(const void* a, const void* b);

/* ---- Files and folders -------------------------------------------------- */

/* Returns the bytes of file `path` followed by a 0 byte, and sets `*size` to
 * the byte count. Returns NULL when the file cannot be read. */
char* ReadWholeFile(const char* path, size_t* size);

/* Writes `size` bytes to file `path`. Returns false on an I/O error. */
bool WriteWholeFile(const char* path, const void* data, size_t size);

bool IsFile(const char* path);
bool IsDir(const char* path);

/* Lists the entries of folder `dir`, without "." and "..". They are in the
 * file system's order, or sorted by name when `sorted` is true. Returns NULL
 * with *count 0 when the folder cannot be read. */
char** ListDir(const char* dir, bool sorted, size_t* count);
void   FreeList(char** names, size_t count);

/* Creates folder `path` and any missing parents. Returns true when the folder
 * exists afterwards. */
bool MakeDirs(const char* path);

/* Removes `path`. If it is a folder, everything in it is removed too. */
bool RemoveTree(const char* path);

/* ---- Paths -------------------------------------------------------------- */

/* Formats a path into `out`. When the path does not fit, reports it, empties
 * `out` and returns false. A path that is too long is an error: it is not cut
 * short, which would name another file. */
UTIL_PRINTF(3, 4) bool PathFormat(char* out, size_t cap, const char* format, ...);

/* ---- Arrays ------------------------------------------------------------- */

/* Adds one zeroed element to `*items`, which holds `count` elements of `size`
 * bytes. Returns false when out of memory, and the array is then unchanged. */
bool ArrayGrow(void** items, size_t count, size_t size);

/* Appends a zeroed element to the array `items` and returns a pointer to it,
 * or NULL when out of memory. `count` is an integer lvalue that holds the
 * element count, and it is incremented. */
#define ARRAY_PUSH(items, count)                                                                             \
	(ArrayGrow((void**)&(items), (size_t)(count), sizeof *(items)) ? &(items)[(count)++] : NULL)

#endif /* AERON_OPT2GLTF_UTIL_H */
