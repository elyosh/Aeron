/*
 * KTX2 writer, BC6H cubemaps (ktx2_writer.h): with the BC6H encoder, in
 * imgbake_bc6h, apart from the rest of the writer.
 */

#include "bc6h_codec.h"
#include "ktx2_writer.h"

#include <stdlib.h>

/* Worst case: 8K × 8K → 14 mip levels. 16-slot fixed buffer is plenty. */
#define KTX2_MAX_LEVELS 16

/* Linear-HDR sibling: float RGB (3 channels, tight, top-row-first).
 * No gamma round-trip — averages happen in the linear-light space
 * the equirect→cube renderer already produced. */
static float* box_downsample_2x_rgb_f32(const float* src, int sw, int sh, int* out_w, int* out_h) {
	int dw = sw / 2;
	int dh = sh / 2;
	if (dw < 1 || dh < 1)
		return NULL;
	float* dst = (float*)malloc((size_t)dw * (size_t)dh * 3u * sizeof(float));
	if (!dst)
		return NULL;
	for (int y = 0; y < dh; y++) {
		const float* r0 = src + ((size_t)(2 * y) * (size_t)sw) * 3u;
		const float* r1 = src + ((size_t)(2 * y + 1) * (size_t)sw) * 3u;
		float*       d  = dst + ((size_t)y * (size_t)dw) * 3u;
		for (int x = 0; x < dw; x++) {
			const float* p00 = r0 + (size_t)(2 * x) * 3u;
			const float* p10 = r0 + (size_t)(2 * x + 1) * 3u;
			const float* p01 = r1 + (size_t)(2 * x) * 3u;
			const float* p11 = r1 + (size_t)(2 * x + 1) * 3u;
			for (int c = 0; c < 3; c++)
				d[c] = 0.25f * (p00[c] + p10[c] + p01[c] + p11[c]);
			d += 3;
		}
	}
	*out_w = dw;
	*out_h = dh;
	return dst;
}

/* HDR sibling of write_ktx2_bc7_cubemap_with_generated_mips: linear
 * RGB float input → BC6H KTX2 with VK_FORMAT_BC6H_UFLOAT_BLOCK. Mip
 * chain built per face by 2× box downsample in linear-HDR space, then
 * each face is BC6H-encoded and the 6 face slabs are concatenated
 * into a single level blob in KTX2 face order. */
static Bc6hQuality bc6h_quality_map(Ktx2Bc6hQuality q) {
	switch (q) {
		case KTX2_BC6H_QUALITY_UBER:
			return BC6H_QUALITY_UBER;
		case KTX2_BC6H_QUALITY_MED:
			return BC6H_QUALITY_MED;
		case KTX2_BC6H_QUALITY_FAST: /* fall-through */
		default:
			return BC6H_QUALITY_FAST;
	}
}

bool write_ktx2_bc6h_cubemap_with_generated_mips(const char* path, int face_size, const float* const faces[6],
												 Ktx2Bc6hQuality quality, bool zstd) {
	if (!path || !faces || face_size < 1)
		return false;
	for (int f = 0; f < 6; ++f)
		if (!faces[f])
			return false;

	bc6h_codec_init();
	Bc6hQuality bcq = bc6h_quality_map(quality);

	int          widths[KTX2_MAX_LEVELS]       = { 0 };
	int          heights[KTX2_MAX_LEVELS]      = { 0 };
	float*       rgb_owned[KTX2_MAX_LEVELS][6] = { { 0 } };
	const float* rgb_view[KTX2_MAX_LEVELS][6]  = { { 0 } };
	uint8_t*     level_blob[KTX2_MAX_LEVELS]   = { 0 };
	int          n                             = 1;

	widths[0]  = face_size;
	heights[0] = face_size;
	for (int f = 0; f < 6; ++f)
		rgb_view[0][f] = faces[f];

	int w = face_size, h = face_size;
	while (w > 1 && h > 1 && n < KTX2_MAX_LEVELS) {
		int  dw = 0, dh = 0;
		bool ok = true;
		for (int f = 0; f < 6; ++f) {
			float* down = box_downsample_2x_rgb_f32(rgb_view[n - 1][f], w, h, &dw, &dh);
			if (!down) {
				ok = false;
				break;
			}
			rgb_owned[n][f] = down;
			rgb_view[n][f]  = down;
		}
		if (!ok)
			break;
		widths[n]  = dw;
		heights[n] = dh;
		w          = dw;
		h          = dh;
		++n;
	}

	Ktx2WriteLevel levels[KTX2_MAX_LEVELS] = { 0 };
	bool           ok                      = true;
	for (int i = 0; i < n && ok; ++i) {
		size_t face_bytes  = bc6h_codec_image_size(widths[i], heights[i]);
		size_t total_bytes = face_bytes * 6u;
		level_blob[i]      = (uint8_t*)malloc(total_bytes);
		if (!level_blob[i]) {
			ok = false;
			break;
		}
		for (int f = 0; f < 6 && ok; ++f) {
			size_t got = bc6h_codec_encode_image(level_blob[i] + (size_t)f * face_bytes, face_bytes,
												 rgb_view[i][f], widths[i], heights[i], bcq);
			if (got != face_bytes)
				ok = false;
		}
		levels[i].width  = widths[i];
		levels[i].height = heights[i];
		levels[i].data   = level_blob[i];
		levels[i].size   = total_bytes;
	}

	if (ok) {
		ok = write_ktx2_bc6h_levels(path, 6, levels, n, zstd);
	}

	for (int i = 0; i < n; ++i) {
		for (int f = 0; f < 6; ++f)
			free(rgb_owned[i][f]);
		free(level_blob[i]);
	}
	return ok;
}
