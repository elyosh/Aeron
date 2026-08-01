#ifndef AERON_NUMERIC_H
#define AERON_NUMERIC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parses a complete ASCII floating-point token using '.' as the decimal
 * separator, independently of the calling thread's locale. Thread-safe. */
int Aeron_ParseAsciiDouble(const char* text, size_t length, double* out_value);

/* Formats a finite value with the requested significant digits and an ASCII
 * '.' decimal separator. Returns zero on invalid arguments or truncation.
 * Thread-safe. */
int Aeron_FormatAsciiDouble(char* text, size_t capacity, double value, int significant_digits);

#ifdef __cplusplus
}
#endif

#endif
