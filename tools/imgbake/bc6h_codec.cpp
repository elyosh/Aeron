/*
 * bc6h_codec — implementation. Wraps the ISPC Texture Compressor's BC6H
 * encoder (CompressBlocksBC6H, "veryslow" profile).
 *
 * The encoder takes an RGBA half-float surface whose dimensions are
 * multiples of 4 and writes blocks in raster order. We convert a range of
 * block rows from float RGB to a padded half surface and encode it in one
 * call, so the kernel fills its SIMD lanes with neighbouring blocks.
 */

#include "bc6h_codec.h"

#include "ispc_texcomp.h"

#include <cstring>
#include <thread>
#include <vector>

extern "C" void bc6h_codec_init(void) {
	/* The encoder needs no initialisation; kept for parity with
	 * bc7_codec_init. */
}

extern "C" size_t bc6h_codec_image_size(int w, int h) {
	int bx = (w + 3) / 4;
	int by = (h + 3) / 4;
	return (size_t)bx * (size_t)by * 16u;
}

/* Float to IEEE half, round to nearest even, clamped to the BC6H unsigned
 * input range [0, 65504]; NaN and negatives become 0. */
static uint16_t to_half(float value) {
	if (!(value > 0.0f))
		return 0;
	if (value > 65504.0f)
		value = 65504.0f;
	uint32_t bits;
	std::memcpy(&bits, &value, sizeof bits);
	const uint32_t exponent = (bits >> 23) & 0xFFu;
	uint32_t       mantissa = bits & 0x7FFFFFu;
	if (exponent < 103)
		return 0;
	if (exponent < 113) {
		/* Subnormal half: shift the implicit-one mantissa, round to nearest even. */
		mantissa |= 0x800000u;
		const uint32_t shift   = 126u - exponent;
		uint32_t       half    = mantissa >> shift;
		const uint32_t rest    = mantissa & ((1u << shift) - 1u);
		const uint32_t halfway = 1u << (shift - 1u);
		if (rest > halfway || (rest == halfway && (half & 1u)))
			++half;
		return (uint16_t)half;
	}
	uint32_t       half = ((exponent - 112u) << 10) | (mantissa >> 13);
	const uint32_t rest = mantissa & 0x1FFFu;
	if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
		++half;
	return (uint16_t)half;
}

static void veryslow_settings(bc6h_enc_settings* settings) { GetProfile_bc6h_veryslow(settings); }

/* Encodes block rows [row_begin, row_end) of a w x h image into
 * out_blocks (the first of those rows' blocks). Edge texels clamp. */
static void encode_rows(uint8_t* out_blocks, const float* rgb, int w, int h, int row_begin, int row_end) {
	const int             bx     = (w + 3) / 4;
	const int             surf_w = bx * 4;
	const int             surf_h = (row_end - row_begin) * 4;
	std::vector<uint16_t> surface((size_t)surf_w * surf_h * 4u);
	for (int y = 0; y < surf_h; ++y) {
		int sy = row_begin * 4 + y;
		if (sy >= h)
			sy = h - 1;
		uint16_t* dst = &surface[(size_t)y * surf_w * 4u];
		for (int x = 0; x < surf_w; ++x) {
			const int    sx  = x < w ? x : w - 1;
			const float* src = rgb + ((size_t)sy * (size_t)w + (size_t)sx) * 3u;
			dst[x * 4 + 0]   = to_half(src[0]);
			dst[x * 4 + 1]   = to_half(src[1]);
			dst[x * 4 + 2]   = to_half(src[2]);
			dst[x * 4 + 3]   = 0x3C00u; /* 1.0; BC6H ignores alpha */
		}
	}
	rgba_surface surf;
	surf.ptr    = reinterpret_cast<uint8_t*>(surface.data());
	surf.width  = surf_w;
	surf.height = surf_h;
	surf.stride = surf_w * 8;
	bc6h_enc_settings settings;
	veryslow_settings(&settings);
	CompressBlocksBC6H(&surf, out_blocks, &settings);
}

extern "C" void bc6h_codec_encode_block(uint8_t out_block[16], const float in_rgb_4x4[48],
										Bc6hQuality quality) {
	(void)quality;
	encode_rows(out_block, in_rgb_4x4, 4, 4, 0, 1);
}

extern "C" size_t bc6h_codec_encode_image_serial(uint8_t* out_blocks, size_t out_cap, const float* rgb, int w,
												 int h, Bc6hQuality quality) {
	(void)quality;
	if (!out_blocks || !rgb || w < 1 || h < 1)
		return 0;
	const size_t need = bc6h_codec_image_size(w, h);
	if (out_cap < need)
		return 0;
	encode_rows(out_blocks, rgb, w, h, 0, (h + 3) / 4);
	return need;
}

extern "C" size_t bc6h_codec_encode_image(uint8_t* out_blocks, size_t out_cap, const float* rgb, int w, int h,
										  Bc6hQuality quality) {
	(void)quality;
	if (!out_blocks || !rgb || w < 1 || h < 1)
		return 0;
	const size_t need = bc6h_codec_image_size(w, h);
	if (out_cap < need)
		return 0;
	const int bx   = (w + 3) / 4;
	const int rows = (h + 3) / 4;

	/* Static partitioning by block rows across the hardware threads. */
	unsigned int nthreads = std::thread::hardware_concurrency();
	if (nthreads == 0)
		nthreads = 1;
	/* Small images: not worth the thread spin-up overhead. */
	if (bx * rows < 256)
		nthreads = 1;
	if ((int)nthreads > rows)
		nthreads = (unsigned int)rows;

	if (nthreads == 1) {
		encode_rows(out_blocks, rgb, w, h, 0, rows);
		return need;
	}
	std::vector<std::thread> threads;
	threads.reserve(nthreads);
	const int per_thread = (rows + (int)nthreads - 1) / (int)nthreads;
	for (unsigned int t = 0; t < nthreads; ++t) {
		const int begin = (int)t * per_thread;
		if (begin >= rows)
			break;
		const int end = begin + per_thread < rows ? begin + per_thread : rows;
		threads.emplace_back(encode_rows, out_blocks + (size_t)begin * bx * 16u, rgb, w, h, begin, end);
	}
	for (auto& th : threads)
		th.join();
	return need;
}
