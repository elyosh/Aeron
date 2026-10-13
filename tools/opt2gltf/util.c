#include "util.h"

#include <stdio.h>

void ReportV(const char* label, const char* format, va_list args) {
	if (label)
		fprintf(stderr, "opt2gltf: %s: ", label);
	else
		fputs("opt2gltf: ", stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}

void Report(const char* label, const char* format, ...) {
	va_list args;
	va_start(args, format);
	ReportV(label, format, args);
	va_end(args);
}
