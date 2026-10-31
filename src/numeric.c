#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include "aeron/numeric.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { AERON_NUMERIC_STACK_CAPACITY = 128 };

#if defined(_WIN32)

typedef _locale_t AeronNumericLocale;

static AeronNumericLocale AeronNumeric_CreateCLocale(void) { return _create_locale(LC_NUMERIC, "C"); }

static void AeronNumeric_DestroyLocale(AeronNumericLocale locale) { _free_locale(locale); }

static double AeronNumeric_Parse(AeronNumericLocale locale, const char* text, char** end, int* parse_error) {
	double value;

	errno        = 0;
	value        = _strtod_l(text, end, locale);
	*parse_error = errno;
	return value;
}

/* `fixed` selects %f, with `precision` decimals, instead of %g, with
   `precision` significant digits. */
static int AeronNumeric_Format(AeronNumericLocale locale, char* text, size_t capacity, double value,
							   int precision, int fixed) {
	return fixed ? _snprintf_l(text, capacity, "%.*f", locale, precision, value)
				 : _snprintf_l(text, capacity, "%.*g", locale, precision, value);
}

#else

typedef locale_t AeronNumericLocale;

static AeronNumericLocale AeronNumeric_CreateCLocale(void) {
	return newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}

static void AeronNumeric_DestroyLocale(AeronNumericLocale locale) { freelocale(locale); }

static double AeronNumeric_Parse(AeronNumericLocale locale, const char* text, char** end, int* parse_error) {
	locale_t previous = uselocale(locale);
	double   value;

	if (!previous) {
		*parse_error = errno;
		return 0.0;
	}
	errno        = 0;
	value        = strtod(text, end);
	*parse_error = errno;
	(void)uselocale(previous);
	return value;
}

static int AeronNumeric_Format(AeronNumericLocale locale, char* text, size_t capacity, double value,
							   int precision, int fixed) {
	locale_t previous = uselocale(locale);
	int      written;

	if (!previous) {
		return -1;
	}
	written = fixed ? snprintf(text, capacity, "%.*f", precision, value)
					: snprintf(text, capacity, "%.*g", precision, value);
	(void)uselocale(previous);
	return written;
}

#endif

int Aeron_ParseAsciiDouble(const char* text, size_t length, double* out_value) {
	char               stack[AERON_NUMERIC_STACK_CAPACITY];
	char*              copy;
	char*              end;
	double             value;
	int                parse_error;
	int                valid;
	AeronNumericLocale locale;

	if (!text || !out_value || length == SIZE_MAX) {
		return 0;
	}
	copy = length < sizeof(stack) ? stack : (char*)malloc(length + 1);
	if (!copy) {
		return 0;
	}
	memcpy(copy, text, length);
	copy[length] = '\0';
	locale       = AeronNumeric_CreateCLocale();
	if (!locale) {
		if (copy != stack) {
			free(copy);
		}
		return 0;
	}
	value = AeronNumeric_Parse(locale, copy, &end, &parse_error);
	AeronNumeric_DestroyLocale(locale);
	valid = parse_error == 0 && end != copy && (size_t)(end - copy) == length;
	if (copy != stack) {
		free(copy);
	}
	if (!valid) {
		return 0;
	}
	*out_value = value;
	return 1;
}

static int AeronNumeric_FormatAscii(char* text, size_t capacity, double value, int precision, int fixed) {
	AeronNumericLocale locale;
	int                written;

	locale = AeronNumeric_CreateCLocale();
	if (!locale) {
		text[0] = '\0';
		return 0;
	}
	written = AeronNumeric_Format(locale, text, capacity, value, precision, fixed);
	AeronNumeric_DestroyLocale(locale);
	if (written < 0 || (size_t)written >= capacity) {
		text[0] = '\0';
		return 0;
	}
	return 1;
}

int Aeron_FormatAsciiDouble(char* text, size_t capacity, double value, int significant_digits) {
	if (!text || capacity == 0 || !isfinite(value) || significant_digits < 1 || significant_digits > 17) {
		return 0;
	}
	return AeronNumeric_FormatAscii(text, capacity, value, significant_digits, 0);
}

int Aeron_FormatAsciiFixed(char* text, size_t capacity, double value, int decimals) {
	if (!text || capacity == 0 || !isfinite(value) || decimals < 0 || decimals > 40) {
		return 0;
	}
	return AeronNumeric_FormatAscii(text, capacity, value, decimals, 1);
}
