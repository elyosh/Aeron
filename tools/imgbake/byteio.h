#ifndef IMGBAKE_BYTEIO_H
#define IMGBAKE_BYTEIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

static inline uint16_t rd_u16(const uint8_t *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline int16_t rd_i16(const uint8_t *p) {
	return (int16_t)rd_u16(p);
}

static inline uint32_t rd_u32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr_be32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

#ifdef __cplusplus
}
#endif

#endif
