#include "xwa_dat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "LzmaDec.h"

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "bcdec.h"

#define XWA_DAT_SIGNATURE 0x5602235657062357ull
#define XWA_DAT_FORMAT_32BPP 25
#define XWA_DAT_IMAGE_HEADER_BYTES 0x2C
#define XWA_DAT_IMAGE_COLORS_OFFSET 0x28

static void set_error(char* error, size_t error_size, const char* path, const char* message) {
	if (error && error_size)
		snprintf(error, error_size, "%s: %s", path, message);
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t* read_file(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t* data = NULL;
	if (fseek(f, 0, SEEK_END) == 0) {
		long len = ftell(f);
		if (len > 0 && fseek(f, 0, SEEK_SET) == 0) {
			data = (uint8_t*)malloc((size_t)len);
			if (data && fread(data, 1, (size_t)len, f) != (size_t)len) {
				free(data);
				data = NULL;
			}
			*size = (size_t)len;
		}
	}
	fclose(f);
	return data;
}

static void* lzma_alloc(ISzAllocPtr p, size_t size) {
	(void)p;
	return malloc(size);
}

static void lzma_free(ISzAllocPtr p, void* address) {
	(void)p;
	free(address);
}

static void bgra_to_rgba(uint8_t* px, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		uint8_t b     = px[i * 4 + 0];
		px[i * 4 + 0] = px[i * 4 + 2];
		px[i * 4 + 2] = b;
	}
}

/* Decode a block-compressed image into width*height RGBA, cropping the
 * 4x4-padded block grid. */
static bool decode_blocks(XwaDatEncoding encoding, const uint8_t* data, size_t size, uint32_t w, uint32_t h,
						  uint8_t* rgba) {
	const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
	const size_t   block_bytes = encoding == XWA_DAT_ENCODING_BC3   ? BCDEC_BC3_BLOCK_SIZE
								 : encoding == XWA_DAT_ENCODING_BC5 ? BCDEC_BC5_BLOCK_SIZE
																	: BCDEC_BC7_BLOCK_SIZE;
	if (size < (size_t)bw * bh * block_bytes)
		return false;
	uint8_t tile[4 * 4 * 4];
	for (uint32_t by = 0; by < bh; ++by) {
		for (uint32_t bx = 0; bx < bw; ++bx) {
			const uint8_t* block = data + ((size_t)by * bw + bx) * block_bytes;
			int            texel_bytes;
			if (encoding == XWA_DAT_ENCODING_BC5) {
				bcdec_bc5(block, tile, 4 * 2);
				texel_bytes = 2;
			} else {
				if (encoding == XWA_DAT_ENCODING_BC3)
					bcdec_bc3(block, tile, 4 * 4);
				else
					bcdec_bc7(block, tile, 4 * 4);
				texel_bytes = 4;
			}
			for (uint32_t y = 0; y < 4 && by * 4 + y < h; ++y) {
				for (uint32_t x = 0; x < 4 && bx * 4 + x < w; ++x) {
					const uint8_t* s = tile + (y * 4 + x) * (uint32_t)texel_bytes;
					uint8_t*       d = rgba + (((size_t)(by * 4 + y) * w) + bx * 4 + x) * 4;
					if (texel_bytes == 2) {
						d[0] = s[0];
						d[1] = s[1];
						d[2] = 0;
						d[3] = 255;
					} else {
						/* Encoded from B,G,R,A bytes: channel 0 is blue. */
						d[0] = s[2];
						d[1] = s[1];
						d[2] = s[0];
						d[3] = s[3];
					}
				}
			}
		}
	}
	return true;
}

static bool decode_image(const char* path, uint16_t format, uint32_t w, uint32_t h, int32_t colors,
						 const uint8_t* data, size_t size, XwaDatImage* out, char* error, size_t error_size) {
	if (format != XWA_DAT_FORMAT_32BPP) {
		char msg[64];
		snprintf(msg, sizeof msg, "unsupported DAT image format %u", (unsigned)format);
		set_error(error, error_size, path, msg);
		return false;
	}
	const size_t texels = (size_t)w * h;
	switch (colors) {
		case 0:
			out->encoding = size < texels * 4 ? XWA_DAT_ENCODING_BC7 : XWA_DAT_ENCODING_BGRA;
			break;
		case 1:
			out->encoding = XWA_DAT_ENCODING_LZMA;
			break;
		case 2:
			out->encoding = XWA_DAT_ENCODING_BC3;
			break;
		case 3:
			out->encoding = XWA_DAT_ENCODING_BC5;
			break;
		default:
			set_error(error, error_size, path, "unknown format-25 encoding");
			return false;
	}
	out->width  = w;
	out->height = h;
	out->rgba   = (uint8_t*)malloc(texels * 4);
	if (!out->rgba) {
		set_error(error, error_size, path, "out of memory");
		return false;
	}
	bool ok = true;
	switch (out->encoding) {
		case XWA_DAT_ENCODING_BGRA:
			memcpy(out->rgba, data, texels * 4);
			bgra_to_rgba(out->rgba, texels);
			break;
		case XWA_DAT_ENCODING_LZMA: {
			const ISzAlloc alloc  = { lzma_alloc, lzma_free };
			SizeT          outLen = texels * 4;
			SizeT          inLen  = size > LZMA_PROPS_SIZE ? size - LZMA_PROPS_SIZE : 0;
			ELzmaStatus    status;
			ok = size > LZMA_PROPS_SIZE &&
				 LzmaDecode(out->rgba, &outLen, data + LZMA_PROPS_SIZE, &inLen, data, LZMA_PROPS_SIZE,
							LZMA_FINISH_ANY, &status, &alloc) == SZ_OK &&
				 outLen == texels * 4;
			if (ok)
				bgra_to_rgba(out->rgba, texels);
			break;
		}
		default:
			ok = decode_blocks(out->encoding, data, size, w, h, out->rgba);
			break;
	}
	if (!ok) {
		set_error(error, error_size, path, "corrupt image data");
		XwaDat_FreeImage(out);
	}
	return ok;
}

bool XwaDat_LoadImage(const char* path, int group, int image, XwaDatImage* out, char* error,
					  size_t error_size) {
	memset(out, 0, sizeof *out);
	size_t   size = 0;
	uint8_t* file = read_file(path, &size);
	if (!file) {
		set_error(error, error_size, path, "cannot read file");
		return false;
	}
	bool found = false, ok = false;
	/* File header: signature, group format, group count, image count,
	 * length, then 12 bytes more when the group format is non-zero, then 4. */
	if (size < 18 || (rd32(file) | ((uint64_t)rd32(file + 4) << 32)) != XWA_DAT_SIGNATURE) {
		set_error(error, error_size, path, "not a DAT file");
		goto done;
	}
	const uint16_t group_format = rd16(file + 8);
	const uint16_t group_count  = rd16(file + 10);
	size_t         pos          = 18 + (group_format ? 12 : 0) + 4;
	uint32_t       total_images = 0;
	for (uint16_t g = 0; g < group_count; ++g) {
		if (pos + 8 > size)
			goto truncated;
		total_images += rd16(file + pos + 2);
		pos += 8 + (group_format ? 12 : 0) + 4;
	}
	/* Images follow, group by group: an 18-byte sub-header (format, width,
	 * height, color key, colors count, group, image, data length), then a
	 * 0x2C-byte image header and the data. */
	for (uint32_t i = 0; i < total_images; ++i) {
		if (pos + 18 > size)
			goto truncated;
		const uint8_t* h          = file + pos;
		const uint16_t format     = rd16(h);
		const uint16_t width      = rd16(h + 2);
		const uint16_t height     = rd16(h + 4);
		const int16_t  img_group  = (int16_t)rd16(h + 10);
		const int16_t  img_id     = (int16_t)rd16(h + 12);
		const uint32_t data_bytes = rd32(h + 14);
		pos += 18;
		if (data_bytes < XWA_DAT_IMAGE_HEADER_BYTES || pos + data_bytes > size)
			goto truncated;
		if (img_group == group && img_id == image) {
			const int32_t colors = (int32_t)rd32(file + pos + XWA_DAT_IMAGE_COLORS_OFFSET);
			found                = true;
			ok = decode_image(path, format, width, height, colors, file + pos + XWA_DAT_IMAGE_HEADER_BYTES,
							  data_bytes - XWA_DAT_IMAGE_HEADER_BYTES, out, error, error_size);
			break;
		}
		pos += data_bytes;
	}
	if (!found) {
		char msg[64];
		snprintf(msg, sizeof msg, "no image %d-%d", group, image);
		set_error(error, error_size, path, msg);
	}
	goto done;
truncated:
	set_error(error, error_size, path, "truncated DAT file");
done:
	free(file);
	return ok;
}

void XwaDat_FreeImage(XwaDatImage* image) {
	if (!image)
		return;
	free(image->rgba);
	image->rgba = NULL;
}
