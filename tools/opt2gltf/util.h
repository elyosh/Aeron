#ifndef AERON_OPT2GLTF_UTIL_H
#define AERON_OPT2GLTF_UTIL_H

/*
 * What the converter's modules share.
 */

#include <stdarg.h>

/* Reports a problem or a summary to stderr: "opt2gltf: <label>: <message>",
 * or "opt2gltf: <message>" when `label` is NULL. */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void Report(const char* label, const char* format, ...);
void ReportV(const char* label, const char* format, va_list args);

#endif /* AERON_OPT2GLTF_UTIL_H */
