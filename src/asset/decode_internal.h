#ifndef AERON_ASSET_DECODE_INTERNAL_H
#define AERON_ASSET_DECODE_INTERNAL_H

#include "aeron/asset/decode_types.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AERON_DECODE_MAX_ENTRY_SIZE (64u * 1024u * 1024u)

static inline uint32_t decode_u32(const uint8_t* p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint16_t decode_u16(const uint8_t* p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }

static inline int16_t decode_i16(const uint8_t* p) { return (int16_t)decode_u16(p); }

static inline int32_t decode_i32(const uint8_t* p) { return (int32_t)decode_u32(p); }

static inline bool decode_error(AeronDecodeError* error, int code, const char* format, ...) {
	if (error) {
		error->code = code;
		va_list args;
		va_start(args, format);
		vsnprintf(error->message, sizeof error->message, format, args);
		va_end(args);
	}
	return false;
}
#endif
