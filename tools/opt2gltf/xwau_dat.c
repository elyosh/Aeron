#include "xwau_dat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "LzmaDec.h"

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "bcdec.h"

#define XWAU_DAT_SIGNATURE 0x5602235657062357ull
#define XWAU_DAT_FORMAT_8BPP_ALPHA 24
#define XWAU_DAT_FORMAT_32BPP 25
#define XWAU_DAT_IMAGE_HEADER_BYTES 0x2C
#define XWAU_DAT_IMAGE_COLORS_OFFSET 0x28

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
static bool decode_blocks(XwauDatEncoding encoding, const uint8_t* data, size_t size, uint32_t w, uint32_t h,
						  uint8_t* rgba) {
	const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
	const size_t   block_bytes = encoding == XWAU_DAT_ENCODING_BC3   ? BCDEC_BC3_BLOCK_SIZE
								 : encoding == XWAU_DAT_ENCODING_BC5 ? BCDEC_BC5_BLOCK_SIZE
																	 : BCDEC_BC7_BLOCK_SIZE;
	if (size < (size_t)bw * bh * block_bytes)
		return false;
	uint8_t tile[4 * 4 * 4];
	for (uint32_t by = 0; by < bh; ++by) {
		for (uint32_t bx = 0; bx < bw; ++bx) {
			const uint8_t* block = data + ((size_t)by * bw + bx) * block_bytes;
			int            texel_bytes;
			if (encoding == XWAU_DAT_ENCODING_BC5) {
				bcdec_bc5(block, tile, 4 * 2);
				texel_bytes = 2;
			} else {
				if (encoding == XWAU_DAT_ENCODING_BC3)
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
						d[0] = s[0];
						d[1] = s[1];
						d[2] = s[2];
						d[3] = s[3];
					}
				}
			}
		}
	}
	return true;
}

/* Stock format 24: the image header gives the palette (RGB triplets) and pixel
 * offsets; each texel is a palette index and an alpha byte (DatImage.cs;
 * src/xwa_formats/xwa_2d/xwa_dat.c). */
static bool decode_indexed(const char* path, uint32_t w, uint32_t h, const uint8_t* payload, size_t size,
						   XwauDatImage* out, char* error, size_t error_size) {
	const uint32_t color_offset = rd32(payload + 4);
	const uint32_t rows_offset  = rd32(payload + 8);
	const uint32_t color_count  = rd32(payload + XWAU_DAT_IMAGE_COLORS_OFFSET);
	const size_t   texels       = (size_t)w * h;
	if (!color_count || color_count > 256 || color_offset > size ||
		3u * (size_t)color_count > size - color_offset || rows_offset > size ||
		size - rows_offset < texels * 2) {
		set_error(error, error_size, path, "invalid format-24 image");
		return false;
	}
	out->rgba = (uint8_t*)calloc(texels, 4);
	if (!out->rgba) {
		set_error(error, error_size, path, "out of memory");
		return false;
	}
	out->width            = w;
	out->height           = h;
	out->encoding         = XWAU_DAT_ENCODING_INDEXED;
	const uint8_t* colors = payload + color_offset;
	const uint8_t* p      = payload + rows_offset;
	for (size_t i = 0; i < texels; ++i) {
		const uint8_t index = p[2 * i], alpha = p[2 * i + 1];
		if (index >= color_count || !alpha)
			continue;
		memcpy(&out->rgba[i * 4], &colors[3u * index], 3);
		out->rgba[i * 4 + 3] = alpha;
	}
	return true;
}

static bool decode_image(const char* path, uint16_t format, uint32_t w, uint32_t h, int32_t colors,
						 const uint8_t* data, size_t size, XwauDatImage* out, char* error,
						 size_t error_size) {
	if (format == XWAU_DAT_FORMAT_8BPP_ALPHA)
		return decode_indexed(path, w, h, data - XWAU_DAT_IMAGE_HEADER_BYTES,
							  size + XWAU_DAT_IMAGE_HEADER_BYTES, out, error, error_size);
	if (format != XWAU_DAT_FORMAT_32BPP) {
		char msg[64];
		snprintf(msg, sizeof msg, "unsupported DAT image format %u", (unsigned)format);
		set_error(error, error_size, path, msg);
		return false;
	}
	const size_t texels = (size_t)w * h;
	switch (colors) {
		case 0:
			out->encoding = size < texels * 4 ? XWAU_DAT_ENCODING_BC7 : XWAU_DAT_ENCODING_BGRA;
			break;
		case 1:
			out->encoding = XWAU_DAT_ENCODING_LZMA;
			break;
		case 2:
			out->encoding = XWAU_DAT_ENCODING_BC3;
			break;
		case 3:
			out->encoding = XWAU_DAT_ENCODING_BC5;
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
		case XWAU_DAT_ENCODING_BGRA:
			memcpy(out->rgba, data, texels * 4);
			bgra_to_rgba(out->rgba, texels);
			break;
		case XWAU_DAT_ENCODING_LZMA: {
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
		XwauDat_FreeImage(out);
	}
	return ok;
}

typedef struct XwauDatEntry {
	int      group, image;
	uint16_t format, width, height;
	size_t   offset; /* of the 0x2C-byte image header */
	uint32_t bytes;  /* header and data */
} XwauDatEntry;

struct XwauDat {
	char*         path;
	uint8_t*      file;
	size_t        size;
	XwauDatEntry* entries;
	size_t        count;
	int*          ids; /* ListGroup scratch */
};

XwauDat* XwauDat_Open(const char* path, char* error, size_t error_size) {
	XwauDat* dat = (XwauDat*)calloc(1, sizeof *dat);
	if (!dat) {
		set_error(error, error_size, path, "out of memory");
		return NULL;
	}
	dat->path = (char*)malloc(strlen(path) + 1);
	if (dat->path)
		strcpy(dat->path, path);
	dat->file = read_file(path, &dat->size);
	if (!dat->path || !dat->file) {
		set_error(error, error_size, path, "cannot read file");
		XwauDat_Close(dat);
		return NULL;
	}
	const uint8_t* file = dat->file;
	const size_t   size = dat->size;
	/* File header: signature, group format, group count, image count,
	 * length, then 12 bytes more when the group format is non-zero, then 4. */
	if (size < 18 || (rd32(file) | ((uint64_t)rd32(file + 4) << 32)) != XWAU_DAT_SIGNATURE) {
		set_error(error, error_size, path, "not a DAT file");
		XwauDat_Close(dat);
		return NULL;
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
	dat->entries = (XwauDatEntry*)calloc(total_images ? total_images : 1, sizeof *dat->entries);
	dat->ids     = (int*)calloc(total_images ? total_images : 1, sizeof *dat->ids);
	if (!dat->entries || !dat->ids) {
		set_error(error, error_size, path, "out of memory");
		XwauDat_Close(dat);
		return NULL;
	}
	/* Images follow, group by group: an 18-byte sub-header (format, width,
	 * height, color key, colors count, group, image, data length), then a
	 * 0x2C-byte image header and the data. */
	for (uint32_t i = 0; i < total_images; ++i) {
		if (pos + 18 > size)
			goto truncated;
		const uint8_t* h = file + pos;
		XwauDatEntry*  e = &dat->entries[dat->count++];
		e->format        = rd16(h);
		e->width         = rd16(h + 2);
		e->height        = rd16(h + 4);
		e->group         = (int16_t)rd16(h + 10);
		e->image         = (int16_t)rd16(h + 12);
		e->bytes         = rd32(h + 14);
		pos += 18;
		if (e->bytes < XWAU_DAT_IMAGE_HEADER_BYTES || pos + e->bytes > size)
			goto truncated;
		e->offset = pos;
		pos += e->bytes;
	}
	return dat;
truncated:
	set_error(error, error_size, path, "truncated DAT file");
	XwauDat_Close(dat);
	return NULL;
}

void XwauDat_Close(XwauDat* dat) {
	if (!dat)
		return;
	free(dat->path);
	free(dat->file);
	free(dat->entries);
	free(dat->ids);
	free(dat);
}

size_t XwauDat_ListGroup(const XwauDat* dat, int group, const int** images) {
	size_t n = 0;
	for (size_t i = 0; i < dat->count; ++i)
		if (dat->entries[i].group == group)
			dat->ids[n++] = dat->entries[i].image;
	*images = dat->ids;
	return n;
}

bool XwauDat_LoadImageFrom(const XwauDat* dat, int group, int image, XwauDatImage* out, char* error,
						   size_t error_size) {
	memset(out, 0, sizeof *out);
	for (size_t i = 0; i < dat->count; ++i) {
		const XwauDatEntry* e = &dat->entries[i];
		if (e->group != group || e->image != image)
			continue;
		const int32_t colors = (int32_t)rd32(dat->file + e->offset + XWAU_DAT_IMAGE_COLORS_OFFSET);
		return decode_image(dat->path, e->format, e->width, e->height, colors,
							dat->file + e->offset + XWAU_DAT_IMAGE_HEADER_BYTES,
							e->bytes - XWAU_DAT_IMAGE_HEADER_BYTES, out, error, error_size);
	}
	char msg[64];
	snprintf(msg, sizeof msg, "no image %d-%d", group, image);
	set_error(error, error_size, dat->path, msg);
	return false;
}

bool XwauDat_LoadImage(const char* path, int group, int image, XwauDatImage* out, char* error,
					   size_t error_size) {
	memset(out, 0, sizeof *out);
	XwauDat* dat = XwauDat_Open(path, error, error_size);
	if (!dat)
		return false;
	const bool ok = XwauDat_LoadImageFrom(dat, group, image, out, error, error_size);
	XwauDat_Close(dat);
	return ok;
}

void XwauDat_FreeImage(XwauDatImage* image) {
	if (!image)
		return;
	free(image->rgba);
	image->rgba = NULL;
}
