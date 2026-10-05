#include "hdr_read.h"

/* Keep stb_image implementation symbols translation-unit-local so this
 * float loader can coexist with png_read's PNG-only copy. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_HDR
#if defined(__GNUC__)
/* The HDR-only configuration leaves some shared stb helpers unused. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdio.h>

bool read_hdr_rgb(const char* path, float** out_rgb, int* out_w, int* out_h, char* err, size_t errsz) {
	if (!path || !out_rgb || !out_w || !out_h) {
		if (err && errsz)
			snprintf(err, errsz, "invalid args");
		return false;
	}
	*out_rgb = NULL;
	if (!stbi_is_hdr(path)) {
		if (err && errsz)
			snprintf(err, errsz, "%s: not a Radiance .hdr image", path);
		return false;
	}
	/* stb applies no gamma or scale to HDR input. */
	int    n = 0;
	float* p = stbi_loadf(path, out_w, out_h, &n, /*req_comp=*/3);
	if (!p) {
		const char* r = stbi_failure_reason();
		if (err && errsz)
			snprintf(err, errsz, "%s: %s", path, r ? r : "(no detail)");
		return false;
	}
	*out_rgb = p;
	return true;
}
