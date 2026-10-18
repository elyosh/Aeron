#ifndef AERON_XWAU_DAT_H
#define AERON_XWAU_DAT_H

/*
 * Reads DAT files, the image containers of XWA (DatFile.cs in
 * JeremyAnsel.Xwa.Dat). A DAT file holds groups of images, and each image is
 * addressed by a group id and an image id. It decodes the stock formats 7
 * and 23 (run-length rows) and 24 (a palette index and an alpha value per
 * texel). Format 25 is the 32-bit format of the XWAU tools, which XWAU's
 * hook_32bpp reads in game. Its `colors` header field selects the encoding:
 *   0  raw B,G,R,A (or BC7 when the data is shorter than W*H*4)
 *   1  LZMA: 5 property bytes, then the raw B,G,R,A stream
 *   2  BC3
 *   3  BC5
 * The block-compressed encodings hold standard R,G,B,A blocks. XWAU uploads
 * BC7 images as DXGI_FORMAT_BC7_UNORM without swizzling
 * (XwaTextureData::CreateSRVFromBuffer). Decoded without a swizzle, they match
 * the OPT textures they animate, as MillenniumFalcon2Cockpit.dat does with the
 * panels of CorellianTransport2Cockpit. BC3 comes from the same family of
 * encoders. BC5 holds red and green, which are the X and Y of a normal map.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum XwauDatEncoding {
	XWAU_DAT_ENCODING_BGRA = 0,
	XWAU_DAT_ENCODING_LZMA,
	XWAU_DAT_ENCODING_BC3,
	XWAU_DAT_ENCODING_BC5,
	XWAU_DAT_ENCODING_BC7,
	XWAU_DAT_ENCODING_INDEXED, /* stock format 24: palette index and alpha */
	XWAU_DAT_ENCODING_RLE,     /* stock formats 7 and 23: rows of runs */
} XwauDatEncoding;

typedef struct XwauDatImage {
	uint32_t        width;
	uint32_t        height;
	XwauDatEncoding encoding;
	/* width*height R,G,B,A texels, with rows in file order (the first row is at
	 * UV v = 0). For BC5, R and G are the stored channels, B is 0 and A is 255. */
	uint8_t* rgba;
} XwauDatImage;

/* An image's header fields, read without decoding the image. */
typedef struct XwauDatImageInfo {
	uint16_t format; /* 7, 23, 24 or 25 */
	uint32_t width;
	uint32_t height;
	/* Offset of the image from the position the game draws it at. When the
	 * value isn't whole, XWAU's tools store 8000 + 10 * value, in tenths of a
	 * pixel. */
	int32_t offset_x;
	int32_t offset_y;
} XwauDatImageInfo;

/* A DAT file read into memory, with its image directory. */
typedef struct XwauDat XwauDat;

/* Reads and indexes the DAT file at `path`. Returns NULL and sets `error` on
 * failure. */
XwauDat* XwauDat_Open(const char* path, char* error, size_t error_size);

void XwauDat_Close(XwauDat* dat);

/* Sets `images` to the image ids of `group` in file order, and returns their
 * count (0 when the group has none). The ids are owned by `dat`. */
size_t XwauDat_ListGroup(const XwauDat* dat, int group, const int** images);

/* Sets `groups` to the file's distinct group ids, in file order, and returns
 * their count. The ids are owned by `dat`. The next call to
 * XwauDat_ListGroups or XwauDat_ListGroup overwrites them. */
size_t XwauDat_ListGroups(const XwauDat* dat, const int** groups);

/* Reads the header of the image (group, image). Returns false when the file
 * has no such image. */
bool XwauDat_GetImageInfo(const XwauDat* dat, int group, int image, XwauDatImageInfo* out);

/* Decodes the image (group, image) of an open DAT file. */
bool XwauDat_LoadImageFrom(const XwauDat* dat, int group, int image, XwauDatImage* out, char* error,
						   size_t error_size);

/* Decodes the image (group, image) of the DAT file at `path`. */
bool XwauDat_LoadImage(const char* path, int group, int image, XwauDatImage* out, char* error,
					   size_t error_size);

void XwauDat_FreeImage(XwauDatImage* image);

#endif /* AERON_XWAU_DAT_H */
