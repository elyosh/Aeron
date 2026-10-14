/*
 * Minimal KTX2 writer — implementation.
 */

#include "ktx2_writer.h"
#include "aeron/image.h"
#include "bc5_codec.h"
#include "bc7_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

static int g_zstd_level = KTX2_ZSTD_LEVEL_DEFAULT;

bool ktx2_set_zstd_level(int level) {
	if (level < 1 || level > ZSTD_maxCLevel())
		return false;
	g_zstd_level = level;
	return true;
}

int ktx2_zstd_level(void) { return g_zstd_level; }

/* Encode level: zstd-compress the given bytes if `enabled`, else
 * memcpy into a fresh malloc. Caller frees the returned buffer.
 * Returns NULL on failure. */
static uint8_t* encode_level(const uint8_t* src, size_t src_size, bool enabled, size_t* out_size) {
	if (!enabled) {
		uint8_t* raw = (uint8_t*)malloc(src_size);
		if (!raw)
			return NULL;
		memcpy(raw, src, src_size);
		*out_size = src_size;
		return raw;
	}
	size_t   bound = ZSTD_compressBound(src_size);
	uint8_t* buf   = (uint8_t*)malloc(bound);
	if (!buf)
		return NULL;
	size_t got = ZSTD_compress(buf, bound, src, src_size, g_zstd_level);
	if (ZSTD_isError(got)) {
		fprintf(stderr, "[ktx2-write] zstd compress failed: %s\n", ZSTD_getErrorName(got));
		free(buf);
		return NULL;
	}
	*out_size = got;
	return buf;
}

static const uint8_t kKtx2Magic[12] = {
	0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_FORMAT_R8G8B8A8_SRGB 43
#define VK_FORMAT_R16G16B16A16_SFLOAT 97
#define VK_FORMAT_BC5_UNORM_BLOCK 141
#define VK_FORMAT_BC6H_UFLOAT_BLOCK 143
#define VK_FORMAT_BC6H_SFLOAT_BLOCK 144
#define VK_FORMAT_BC7_UNORM_BLOCK 145
#define VK_FORMAT_BC7_SRGB_BLOCK 146

static void wr_u32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr_u64(uint8_t* p, uint64_t v) {
	wr_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
	wr_u32(p + 4, (uint32_t)(v >> 32));
}

/* Round n up to the nearest multiple of `align`. align must be >= 1. */
static size_t align_up(size_t n, size_t align) { return ((n + align - 1) / align) * align; }

/* Texel block size for our supported formats. RGBA8: 4 bytes per
 * texel, no block compression. BC5 + BC6H + BC7 (all variants):
 * 16 bytes per 4×4 block — all share identical block geometry. */
static bool format_is_bc_4x4_16b(uint32_t vk) {
	return vk == VK_FORMAT_BC5_UNORM_BLOCK || vk == VK_FORMAT_BC6H_UFLOAT_BLOCK ||
		   vk == VK_FORMAT_BC6H_SFLOAT_BLOCK || vk == VK_FORMAT_BC7_UNORM_BLOCK ||
		   vk == VK_FORMAT_BC7_SRGB_BLOCK;
}

static int format_block_w(uint32_t vk) { return format_is_bc_4x4_16b(vk) ? 4 : 1; }

static int format_block_h(uint32_t vk) { return format_is_bc_4x4_16b(vk) ? 4 : 1; }

static int format_block_bytes(uint32_t vk) {
	if (format_is_bc_4x4_16b(vk))
		return 16;
	return vk == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
}

/* Compute the bytes for a level at (w, h) for a given format. Block
 * formats round up to block dim. */
static size_t level_bytes(uint32_t vk, int w, int h) {
	int bw       = format_block_w(vk);
	int bh       = format_block_h(vk);
	int bb       = format_block_bytes(vk);
	int blocks_x = (w + bw - 1) / bw;
	int blocks_y = (h + bh - 1) / bh;
	return (size_t)blocks_x * (size_t)blocks_y * (size_t)bb;
}

/* Premultiply RGB by alpha into a fresh malloc'd buffer. Legacy callers use
 * premultiplied-alpha blending; glTF base color selects straight storage via
 * the explicit encoding parameter below. */
static uint8_t* make_premultiplied_copy(const uint8_t* rgba, int w, int h) {
	size_t   n   = (size_t)w * (size_t)h;
	uint8_t* out = (uint8_t*)malloc(n * 4u);
	if (!out)
		return NULL;
	memcpy(out, rgba, n * 4u);
	Aeron_ImagePremultiplyRgba8(out, n);
	return out;
}

static uint8_t* make_alpha_encoded_copy(const uint8_t* rgba, int w, int h, Ktx2AlphaEncoding encoding) {
	if (encoding == KTX2_ALPHA_PREMULTIPLIED)
		return make_premultiplied_copy(rgba, w, h);
	if (encoding != KTX2_ALPHA_STRAIGHT && encoding != KTX2_ALPHA_INDEPENDENT)
		return NULL;
	const size_t bytes = (size_t)w * (size_t)h * 4u;
	uint8_t*     copy  = (uint8_t*)malloc(bytes);
	if (copy)
		memcpy(copy, rgba, bytes);
	return copy;
}

static uint8_t* downsample_alpha_encoded_rgba8(const uint8_t* source, int width, int height, int* out_width,
											   int* out_height, Ktx2AlphaEncoding encoding) {
	if (encoding == KTX2_ALPHA_STRAIGHT)
		return Aeron_ImageDownsampleStraightAlphaRgba8(source, width, height, out_width, out_height);
	return encoding == KTX2_ALPHA_PREMULTIPLIED || encoding == KTX2_ALPHA_INDEPENDENT
			   ? Aeron_ImageDownsampleRgba8(source, width, height, out_width, out_height)
			   : NULL;
}

/* Build the complete KTX2 file image into a fresh malloc'd buffer.
 * Caller owns the result and frees it. Returns NULL on failure. Used by
 * both the path-based writers (which fwrite the buffer) and by callers
 * that need to embed the KTX2 payload elsewhere (aeron_gltf_cook packs them
 * into a GLB BIN chunk). Each level is optionally zstd-compressed before
 * placement; the supercompressionScheme header field flips between 0
 * (raw) and 2 (zstd) accordingly. For cubemaps (face_count=6) the caller
 * pre-concatenates the 6 face slabs into each level's `.data`; we just
 * see one bigger blob per level and tag faceCount=6 in the header. */
static uint8_t* build_ktx2_buf(uint32_t vkFormat, int face_count, int layer_count,
							   const Ktx2WriteLevel* levels, int level_count, bool zstd, size_t* out_size) {
	if (!levels || level_count <= 0 || !out_size)
		return NULL;
	if (face_count != 1 && face_count != 6)
		return NULL;
	/* Zero is a non-array texture; with faces, a cube array. */
	if (layer_count < 0)
		return NULL;

	/* Validate per-level uncompressed size: layers × faces × per-face bytes. */
	const size_t slices = (size_t)face_count * (size_t)(layer_count ? layer_count : 1);
	for (int i = 0; i < level_count; i++) {
		size_t expect = level_bytes(vkFormat, levels[i].width, levels[i].height) * slices;
		if (levels[i].size != expect || !levels[i].data) {
			fprintf(stderr,
					"[ktx2-write] level %d size %zu != expected %zu "
					"(w=%d h=%d fmt=%u faces=%d layers=%d)\n",
					i, levels[i].size, expect, levels[i].width, levels[i].height, vkFormat, face_count,
					layer_count);
			return NULL;
		}
	}

	/* Encode (zstd-compress or copy) each level into its own owned
	 * buffer; we'll memcpy them into the final file blob below. */
	uint8_t** encoded    = (uint8_t**)calloc(level_count, sizeof *encoded);
	size_t*   encoded_sz = (size_t*)calloc(level_count, sizeof *encoded_sz);
	if (!encoded || !encoded_sz) {
		free(encoded);
		free(encoded_sz);
		return NULL;
	}
	for (int i = 0; i < level_count; i++) {
		encoded[i] = encode_level(levels[i].data, levels[i].size, zstd, &encoded_sz[i]);
		if (!encoded[i]) {
			for (int j = 0; j < i; j++)
				free(encoded[j]);
			free(encoded);
			free(encoded_sz);
			return NULL;
		}
	}

	/* Layout:
	 *   [0,     12)   magic
	 *   [12,    80)   header (17×4 = 68 bytes)
	 *   [80,    80 + N*24)   level index (N = level_count)
	 *   [aligned)     mip data (smallest-first). For raw payloads each
	 *                 level is aligned to the format's texel-block
	 *                 size (KTX2 §3.9.6). For zstd payloads alignment
	 *                 doesn't matter — the compressed bytes are
	 *                 opaque — but we keep the format-block alignment
	 *                 anyway for consistency.
	 *
	 * DFD/KVD/SGD: omitted (offset=0, length=0). The runtime reader
	 * skips them; external validators may warn. */

	size_t  header_end   = 80 + (size_t)level_count * 24u;
	size_t  cursor       = header_end;
	size_t  align        = (size_t)format_block_bytes(vkFormat);
	size_t* level_offset = (size_t*)calloc(level_count, sizeof *level_offset);
	if (!level_offset) {
		for (int i = 0; i < level_count; i++)
			free(encoded[i]);
		free(encoded);
		free(encoded_sz);
		return NULL;
	}

	for (int i = level_count - 1; i >= 0; --i) {
		cursor          = align_up(cursor, align);
		level_offset[i] = cursor;
		cursor += encoded_sz[i];
	}
	size_t total_size = cursor;

	uint8_t* buf = (uint8_t*)calloc(1, total_size);
	if (!buf) {
		free(level_offset);
		for (int i = 0; i < level_count; i++)
			free(encoded[i]);
		free(encoded);
		free(encoded_sz);
		return NULL;
	}

	memcpy(buf, kKtx2Magic, 12);

	/* Header at offset 12. Field order from the spec. */
	uint8_t* h = buf + 12;
	wr_u32(h + 0, vkFormat);
	wr_u32(h + 4, 1);                           /* typeSize */
	wr_u32(h + 8, (uint32_t)levels[0].width);   /* pixelWidth */
	wr_u32(h + 12, (uint32_t)levels[0].height); /* pixelHeight */
	wr_u32(h + 16, 0);                          /* pixelDepth (2D / cube) */
	wr_u32(h + 20, (uint32_t)layer_count);      /* layerCount */
	wr_u32(h + 24, (uint32_t)face_count);       /* faceCount (1 or 6) */
	wr_u32(h + 28, (uint32_t)level_count);      /* levelCount */
	wr_u32(h + 32, zstd ? 2u : 0u);             /* supercompressionScheme */
	wr_u32(h + 36, 0);                          /* dfdByteOffset */
	wr_u32(h + 40, 0);                          /* dfdByteLength */
	wr_u32(h + 44, 0);                          /* kvdByteOffset */
	wr_u32(h + 48, 0);                          /* kvdByteLength */
	wr_u64(h + 52, 0);                          /* sgdByteOffset */
	wr_u64(h + 60, 0);                          /* sgdByteLength */

	/* Level index — byteLength is the on-disk (encoded) size,
	 * uncompressedByteLength stays the original. They're equal for
	 * scheme=0 and differ for scheme=2. */
	for (int i = 0; i < level_count; i++) {
		uint8_t* li = buf + 80 + i * 24u;
		wr_u64(li + 0, (uint64_t)level_offset[i]);
		wr_u64(li + 8, (uint64_t)encoded_sz[i]);
		wr_u64(li + 16, (uint64_t)levels[i].size);
	}

	for (int i = 0; i < level_count; i++)
		memcpy(buf + level_offset[i], encoded[i], encoded_sz[i]);

	for (int i = 0; i < level_count; i++)
		free(encoded[i]);
	free(encoded);
	free(encoded_sz);
	free(level_offset);

	*out_size = total_size;
	return buf;
}

/* Thin wrapper: build the KTX2 image in memory, fwrite it to disk. */
static bool write_ktx2_levels(const char* path, uint32_t vkFormat, int face_count, int layer_count,
							  const Ktx2WriteLevel* levels, int level_count, bool zstd) {
	if (!path)
		return false;
	size_t   total_size = 0;
	uint8_t* buf = build_ktx2_buf(vkFormat, face_count, layer_count, levels, level_count, zstd, &total_size);
	if (!buf)
		return false;
	FILE* f = fopen(path, "wb");
	if (!f) {
		free(buf);
		fprintf(stderr, "[ktx2-write] open %s failed\n", path);
		return false;
	}
	size_t wrote = fwrite(buf, 1, total_size, f);
	fclose(f);
	free(buf);
	if (wrote != total_size) {
		fprintf(stderr,
				"[ktx2-write] short write to %s "
				"(%zu/%zu)\n",
				path, wrote, total_size);
		return false;
	}
	return true;
}

bool write_ktx2_rgba_mips(const char* path, const Ktx2WriteLevel* levels, int level_count, Ktx2TransferFn tf,
						  bool zstd) {
	uint32_t vk = (tf == KTX2_TF_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	return write_ktx2_levels(path, vk, 1, 0, levels, level_count, zstd);
}

bool write_ktx2_bc6h_levels(const char* path, int face_count, int layer_count, const Ktx2WriteLevel* levels,
							int level_count, bool zstd) {
	return write_ktx2_levels(path, VK_FORMAT_BC6H_UFLOAT_BLOCK, face_count, layer_count, levels, level_count,
							 zstd);
}

bool write_ktx2_rgba16f_levels(const char* path, int face_count, int layer_count,
							   const Ktx2WriteLevel* levels, int level_count, bool zstd) {
	return write_ktx2_levels(path, VK_FORMAT_R16G16B16A16_SFLOAT, face_count, layer_count, levels,
							 level_count, zstd);
}

bool write_ktx2_bc7_mips(const char* path, const Ktx2WriteLevel* levels, int level_count, Ktx2TransferFn tf,
						 bool zstd) {
	uint32_t vk = (tf == KTX2_TF_SRGB) ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
	return write_ktx2_levels(path, vk, 1, 0, levels, level_count, zstd);
}

/* ---------- mip-chain generator ----------
 *
 * Iterative 2×2 box downsample producing levels until both dims hit
 * 1. The base level (level_count-1 from a smallest-first POV) is
 * indexed 0 here per the spec; we mirror the runtime reader's
 * convention. Each downsampled level is its own malloc'd buffer; we
 * free them all before returning. */

/* Worst case: 8K × 8K → 14 mip levels. 16-slot fixed buffer is plenty. */
#define KTX2_MAX_LEVELS 16

/* Map our local Ktx2Bc7Quality enum onto the bc7_codec one without
 * leaking the codec header into ktx2_writer.h. */
static Bc7Quality bc7_quality_map(Ktx2Bc7Quality q) {
	switch (q) {
		case KTX2_BC7_QUALITY_UBER:
			return BC7_QUALITY_UBER;
		case KTX2_BC7_QUALITY_MED:
			return BC7_QUALITY_MED;
		case KTX2_BC7_QUALITY_FAST: /* fall-through */
		default:
			return BC7_QUALITY_FAST;
	}
}

/* Core of the path-based BC7 with-generated-mips writer. */
static bool bc7_with_generated_mips_core(int base_w, int base_h, const uint8_t* base_rgba,
										 Ktx2Bc7Quality quality, Ktx2TransferFn tf, bool zstd, int max_levels,
										 Ktx2AlphaEncoding alpha_encoding, const char* path) {
	if (!base_rgba || base_w < 1 || base_h < 1 || !path)
		return false;
	if (max_levels < 0)
		return false;

	bc7_codec_init();
	Bc7Quality bcq = bc7_quality_map(quality);

	/* Copy into the requested representation before mip generation. */
	uint8_t* alpha_base = make_alpha_encoded_copy(base_rgba, base_w, base_h, alpha_encoding);
	if (!alpha_base)
		return false;

	/* Generate the mip chain in RGBA, then encode every level to BC7
	 * up front. Holding both representations briefly keeps the code
	 * straightforward — the BC7 buffers are small (~25 % of RGBA at
	 * 4× compression with mips) and freed before the file write. */
	int      widths[KTX2_MAX_LEVELS]     = { 0 };
	int      heights[KTX2_MAX_LEVELS]    = { 0 };
	uint8_t* rgba_owned[KTX2_MAX_LEVELS] = { 0 };
	uint8_t* bc7_owned[KTX2_MAX_LEVELS]  = { 0 };
	int      n                           = 0;

	widths[n]  = base_w;
	heights[n] = base_h;
	/* Level 0 RGBA — owned copy in the requested representation. */
	rgba_owned[n] = alpha_base;
	n++;
	int            w = base_w, h = base_h;
	const uint8_t* src = alpha_base;
	while (w > 1 && h > 1 && n < KTX2_MAX_LEVELS && (max_levels == 0 || n < max_levels)) {
		int      dw = 0, dh = 0;
		uint8_t* down = downsample_alpha_encoded_rgba8(src, w, h, &dw, &dh, alpha_encoding);
		if (!down)
			break;
		rgba_owned[n] = down;
		widths[n]     = dw;
		heights[n]    = dh;
		src           = down;
		w             = dw;
		h             = dh;
		n++;
	}

	/* Encode each level from its alpha-encoded buffer. */
	Ktx2WriteLevel levels[KTX2_MAX_LEVELS] = { 0 };
	bool           ok                      = true;
	for (int i = 0; i < n; ++i) {
		const uint8_t* level_rgba = rgba_owned[i];
		size_t         bc_bytes   = bc7_codec_image_size(widths[i], heights[i]);
		bc7_owned[i]              = (uint8_t*)malloc(bc_bytes);
		if (!bc7_owned[i]) {
			ok = false;
			break;
		}
		size_t got = bc7_codec_encode_image(bc7_owned[i], bc_bytes, level_rgba, widths[i], heights[i], bcq);
		if (got != bc_bytes) {
			ok = false;
			break;
		}
		levels[i].width  = widths[i];
		levels[i].height = heights[i];
		levels[i].data   = bc7_owned[i];
		levels[i].size   = bc_bytes;
	}

	if (ok)
		ok = write_ktx2_bc7_mips(path, levels, n, tf, zstd);

	for (int i = 0; i < n; ++i) {
		free(rgba_owned[i]);
		free(bc7_owned[i]);
	}
	return ok;
}

bool write_ktx2_bc7_with_generated_mips(const char* path, int base_w, int base_h, const uint8_t* base_rgba,
										Ktx2Bc7Quality quality, Ktx2TransferFn tf, bool zstd) {
	if (!path)
		return false;
	return bc7_with_generated_mips_core(base_w, base_h, base_rgba, quality, tf, zstd, 0,
										KTX2_ALPHA_PREMULTIPLIED, path);
}

typedef enum {
	KTX2_PAYLOAD_RGBA8 = 0,
	KTX2_PAYLOAD_BC7,
	KTX2_PAYLOAD_BC5,
} Ktx2Payload;

/* Core of the buffer-mode writers: `layer_count` base images of
 * base_w × base_h RGBA8 back to back. Each level is generated per layer by
 * 2×2 box downsample and stores its layers consecutively, as KTX2 orders
 * array slices. The chain continues until both dimensions reach 1, or
 * stops at `max_levels` when that is non-zero. `encode_base_alpha` false
 * keeps the base bytes as given (BC5 normals) while the downsample still
 * follows `alpha_encoding`. A `layer_count` of zero writes one non-array
 * image. */
static bool buffer_with_generated_mips(Ktx2Payload payload, uint32_t vk, int base_w, int base_h,
									   int layer_count, const uint8_t* base_rgba, Bc7Quality bc7_quality,
									   bool zstd, int max_levels, Ktx2AlphaEncoding alpha_encoding,
									   bool encode_base_alpha, uint8_t** out_buf, size_t* out_size) {
	if (!out_buf || !out_size)
		return false;
	*out_buf  = NULL;
	*out_size = 0;
	if (!base_rgba || base_w < 1 || base_h < 1 || layer_count < 0 || max_levels < 0)
		return false;
	const int layers = layer_count ? layer_count : 1;
	if (payload == KTX2_PAYLOAD_BC7)
		bc7_codec_init();

	int widths[KTX2_MAX_LEVELS]  = { base_w };
	int heights[KTX2_MAX_LEVELS] = { base_h };
	int n                        = 1;
	while ((widths[n - 1] > 1 || heights[n - 1] > 1) && n < KTX2_MAX_LEVELS &&
		   (max_levels == 0 || n < max_levels)) {
		widths[n]  = widths[n - 1] > 1 ? widths[n - 1] / 2 : 1;
		heights[n] = heights[n - 1] > 1 ? heights[n - 1] / 2 : 1;
		n++;
	}

	Ktx2WriteLevel levels[KTX2_MAX_LEVELS] = { 0 };
	uint8_t*       blobs[KTX2_MAX_LEVELS]  = { 0 };
	bool           ok                      = true;
	for (int i = 0; i < n && ok; ++i) {
		levels[i].width  = widths[i];
		levels[i].height = heights[i];
		levels[i].size   = level_bytes(vk, widths[i], heights[i]) * (size_t)layers;
		blobs[i]         = (uint8_t*)malloc(levels[i].size);
		levels[i].data   = blobs[i];
		ok               = blobs[i] != NULL;
	}

	const size_t base_bytes = (size_t)base_w * (size_t)base_h * 4u;
	for (int layer = 0; layer < layers && ok; ++layer) {
		const uint8_t* layer_rgba = base_rgba + (size_t)layer * base_bytes;
		uint8_t* rgba = make_alpha_encoded_copy(layer_rgba, base_w, base_h,
												encode_base_alpha ? alpha_encoding : KTX2_ALPHA_STRAIGHT);
		for (int i = 0; i < n && rgba && ok; ++i) {
			if (i > 0) {
				int      dw = 0, dh = 0;
				uint8_t* down = downsample_alpha_encoded_rgba8(rgba, widths[i - 1], heights[i - 1], &dw, &dh,
															   alpha_encoding);
				free(rgba);
				rgba = down;
				if (!rgba || dw != widths[i] || dh != heights[i]) {
					ok = false;
					break;
				}
			}
			const size_t slice = levels[i].size / (size_t)layers;
			uint8_t*     dst   = blobs[i] + (size_t)layer * slice;
			switch (payload) {
				case KTX2_PAYLOAD_RGBA8:
					memcpy(dst, rgba, slice);
					break;
				case KTX2_PAYLOAD_BC7:
					ok =
						bc7_codec_encode_image(dst, slice, rgba, widths[i], heights[i], bc7_quality) == slice;
					break;
				case KTX2_PAYLOAD_BC5:
					ok = bc5_codec_encode_image_from_rgba(dst, slice, rgba, widths[i], heights[i]) == slice;
					break;
			}
		}
		if (!rgba)
			ok = false;
		free(rgba);
	}

	if (ok) {
		*out_buf = build_ktx2_buf(vk, 1, layer_count, levels, n, zstd, out_size);
		ok       = *out_buf != NULL;
	}
	for (int i = 0; i < n; ++i)
		free(blobs[i]);
	return ok;
}

bool write_ktx2_bc7_with_generated_mips_to_buffer(int base_w, int base_h, const uint8_t* base_rgba,
												  Ktx2Bc7Quality quality, Ktx2TransferFn tf, bool zstd,
												  uint8_t** out_buf, size_t* out_size) {
	return write_ktx2_bc7_with_generated_mips_to_buffer_limited(base_w, base_h, base_rgba, quality, tf, zstd,
																0, out_buf, out_size);
}

bool write_ktx2_bc7_with_generated_mips_to_buffer_limited(int base_w, int base_h, const uint8_t* base_rgba,
														  Ktx2Bc7Quality quality, Ktx2TransferFn tf,
														  bool zstd, int max_levels, uint8_t** out_buf,
														  size_t* out_size) {
	return write_ktx2_bc7_with_generated_mips_to_buffer_limited_alpha(
		base_w, base_h, 0, base_rgba, quality, tf, zstd, max_levels, KTX2_ALPHA_PREMULTIPLIED, out_buf,
		out_size);
}

bool write_ktx2_bc7_with_generated_mips_to_buffer_limited_alpha(int base_w, int base_h, int layer_count,
																const uint8_t* base_rgba,
																Ktx2Bc7Quality quality, Ktx2TransferFn tf,
																bool zstd, int max_levels,
																Ktx2AlphaEncoding alpha_encoding,
																uint8_t** out_buf, size_t* out_size) {
	const uint32_t vk = (tf == KTX2_TF_SRGB) ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
	return buffer_with_generated_mips(KTX2_PAYLOAD_BC7, vk, base_w, base_h, layer_count, base_rgba,
									  bc7_quality_map(quality), zstd, max_levels, alpha_encoding, true,
									  out_buf, out_size);
}

/* BC5 buffer entry point. Mirrors the BC7 version but skips the PMA
 * premultiply (BC5 is linear-only and used for normal maps — there's
 * no alpha to pre-multiply against) and the quality/transfer-function
 * knobs (BC5 has neither). Mip generation is the same 2× box downsample
 * already used by the BC7 path; encode dispatch lives in
 * bc5_codec.cpp (vendored rgbcx). */
bool write_ktx2_bc5_with_generated_mips_to_buffer(int base_w, int base_h, const uint8_t* base_rgba, bool zstd,
												  uint8_t** out_buf, size_t* out_size) {
	return write_ktx2_bc5_with_generated_mips_to_buffer_limited(base_w, base_h, 0, base_rgba, zstd, 0,
																out_buf, out_size);
}

bool write_ktx2_bc5_with_generated_mips_to_buffer_limited(int base_w, int base_h, int layer_count,
														  const uint8_t* base_rgba, bool zstd, int max_levels,
														  uint8_t** out_buf, size_t* out_size) {
	return buffer_with_generated_mips(KTX2_PAYLOAD_BC5, VK_FORMAT_BC5_UNORM_BLOCK, base_w, base_h,
									  layer_count, base_rgba, BC7_QUALITY_FAST, zstd, max_levels,
									  KTX2_ALPHA_PREMULTIPLIED, false, out_buf, out_size);
}

/* Cubemap variant — encodes 6 faces independently into a BC7 mip
 * chain, concatenates same-level face slabs into one level blob,
 * writes faceCount=6.
 *
 * The reader (ktx2_reader.c::ktx2_level_face) recovers per-face data
 * by slicing `level.size / face_count` bytes per face from the level
 * blob, so the concatenation order in `faces[]` IS the run-time face
 * order (+X, -X, +Y, -Y, +Z, -Z per KTX2 §3.10).
 *
 * Skybox faces are opaque, so we skip the make_premultiplied_copy
 * step the 2D path uses for cels — alpha=255 makes PMA a no-op
 * anyway, and the runtime skybox shader writes alpha=1 unconditionally. */
bool write_ktx2_bc7_cubemap_with_generated_mips(const char* path, int face_size,
												const uint8_t* const faces[6], Ktx2Bc7Quality quality,
												Ktx2TransferFn tf, bool zstd) {
	if (!path || !faces || face_size < 1)
		return false;
	for (int f = 0; f < 6; ++f)
		if (!faces[f])
			return false;

	bc7_codec_init();
	Bc7Quality bcq = bc7_quality_map(quality);

	/* Per-face mip chain of RGBA buffers. Level 0's pointer aliases
	 * the caller's `faces[f]` (no copy); levels 1..n are owned. */
	int            widths[KTX2_MAX_LEVELS]        = { 0 };
	int            heights[KTX2_MAX_LEVELS]       = { 0 };
	uint8_t*       rgba_owned[KTX2_MAX_LEVELS][6] = { { 0 } };
	const uint8_t* rgba_view[KTX2_MAX_LEVELS][6]  = { { 0 } };
	uint8_t*       level_blob[KTX2_MAX_LEVELS]    = { 0 };
	int            n                              = 1;

	widths[0]  = face_size;
	heights[0] = face_size;
	for (int f = 0; f < 6; ++f)
		rgba_view[0][f] = faces[f];

	/* Halve until 1×1; KTX2 mandates equal dims across faces of a
	 * level, so all 6 faces share one (w, h) per level. */
	int w = face_size, h = face_size;
	while (w > 1 && h > 1 && n < KTX2_MAX_LEVELS) {
		int  dw = 0, dh = 0;
		bool ok = true;
		for (int f = 0; f < 6; ++f) {
			uint8_t* down = Aeron_ImageDownsampleRgba8(rgba_view[n - 1][f], w, h, &dw, &dh);
			if (!down) {
				ok = false;
				break;
			}
			rgba_owned[n][f] = down;
			rgba_view[n][f]  = down;
		}
		if (!ok)
			break;
		widths[n]  = dw;
		heights[n] = dh;
		w          = dw;
		h          = dh;
		++n;
	}

	/* Encode every (level, face) to BC7, concatenate the 6 face slabs
	 * for each level into one level blob in face order. */
	Ktx2WriteLevel levels[KTX2_MAX_LEVELS] = { 0 };
	bool           ok                      = true;
	for (int i = 0; i < n && ok; ++i) {
		size_t face_bytes  = bc7_codec_image_size(widths[i], heights[i]);
		size_t level_bytes = face_bytes * 6u;
		level_blob[i]      = (uint8_t*)malloc(level_bytes);
		if (!level_blob[i]) {
			ok = false;
			break;
		}
		for (int f = 0; f < 6 && ok; ++f) {
			size_t got = bc7_codec_encode_image(level_blob[i] + (size_t)f * face_bytes, face_bytes,
												rgba_view[i][f], widths[i], heights[i], bcq);
			if (got != face_bytes)
				ok = false;
		}
		levels[i].width  = widths[i];
		levels[i].height = heights[i];
		levels[i].data   = level_blob[i];
		levels[i].size   = level_bytes;
	}

	if (ok) {
		uint32_t vk = (tf == KTX2_TF_SRGB) ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
		ok          = write_ktx2_levels(path, vk, 6, 0, levels, n, zstd);
	}

	/* rgba_owned[0][*] is NULL (level 0 aliases the caller's buffers),
	 * levels 1..n-1 own the downsampled buffers — free those. */
	for (int i = 0; i < n; ++i) {
		for (int f = 0; f < 6; ++f)
			free(rgba_owned[i][f]);
		free(level_blob[i]);
	}
	return ok;
}

static bool rgba_with_generated_mips_core(const char* path, int base_w, int base_h, const uint8_t* base_rgba,
										  Ktx2TransferFn tf, bool zstd, int max_levels,
										  Ktx2AlphaEncoding alpha_encoding) {
	if (!path || !base_rgba || base_w < 1 || base_h < 1 || max_levels < 0)
		return false;

	/* Copy once into the requested representation before mip generation. */
	uint8_t* alpha_base = make_alpha_encoded_copy(base_rgba, base_w, base_h, alpha_encoding);
	if (!alpha_base)
		return false;

	Ktx2WriteLevel levels[KTX2_MAX_LEVELS] = { 0 };
	uint8_t*       owned[KTX2_MAX_LEVELS]  = { 0 };
	int            n                       = 0;

	/* Level 0 — owned copy of the base image. */
	owned[n]         = alpha_base;
	levels[n].width  = base_w;
	levels[n].height = base_h;
	levels[n].data   = alpha_base;
	levels[n].size   = (size_t)base_w * (size_t)base_h * 4u;
	n++;

	/* Generate levels 1..N until 1×1. */
	int            w = base_w, h = base_h;
	const uint8_t* src = alpha_base;
	while (w > 1 && h > 1 && n < KTX2_MAX_LEVELS && (max_levels == 0 || n < max_levels)) {
		int      dw = 0, dh = 0;
		uint8_t* down = downsample_alpha_encoded_rgba8(src, w, h, &dw, &dh, alpha_encoding);
		if (!down)
			break;
		owned[n]         = down; /* freed at end */
		levels[n].width  = dw;
		levels[n].height = dh;
		levels[n].data   = down;
		levels[n].size   = (size_t)dw * (size_t)dh * 4u;
		src              = down;
		w                = dw;
		h                = dh;
		n++;
	}

	const bool ok = write_ktx2_rgba_mips(path, levels, n, tf, zstd);

	for (int i = 0; i < n; i++)
		free(owned[i]);
	return ok;
}

bool write_ktx2_rgba_with_generated_mips(const char* path, int base_w, int base_h, const uint8_t* base_rgba,
										 Ktx2TransferFn tf, bool zstd) {
	return rgba_with_generated_mips_core(path, base_w, base_h, base_rgba, tf, zstd, 0,
										 KTX2_ALPHA_PREMULTIPLIED);
}

bool write_ktx2_rgba_with_generated_mips_to_buffer(int base_w, int base_h, const uint8_t* base_rgba,
												   Ktx2TransferFn tf, bool zstd, uint8_t** out_buf,
												   size_t* out_size) {
	return write_ktx2_rgba_with_generated_mips_to_buffer_limited(base_w, base_h, base_rgba, tf, zstd, 0,
																 out_buf, out_size);
}

bool write_ktx2_rgba_with_generated_mips_to_buffer_limited(int base_w, int base_h, const uint8_t* base_rgba,
														   Ktx2TransferFn tf, bool zstd, int max_levels,
														   uint8_t** out_buf, size_t* out_size) {
	return write_ktx2_rgba_with_generated_mips_to_buffer_limited_alpha(
		base_w, base_h, 0, base_rgba, tf, zstd, max_levels, KTX2_ALPHA_PREMULTIPLIED, out_buf, out_size);
}

bool write_ktx2_rgba_with_generated_mips_to_buffer_limited_alpha(int base_w, int base_h, int layer_count,
																 const uint8_t* base_rgba, Ktx2TransferFn tf,
																 bool zstd, int max_levels,
																 Ktx2AlphaEncoding alpha_encoding,
																 uint8_t** out_buf, size_t* out_size) {
	const uint32_t vk = tf == KTX2_TF_SRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	return buffer_with_generated_mips(KTX2_PAYLOAD_RGBA8, vk, base_w, base_h, layer_count, base_rgba,
									  BC7_QUALITY_FAST, zstd, max_levels, alpha_encoding, true, out_buf,
									  out_size);
}
