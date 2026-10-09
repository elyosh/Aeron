#ifndef AERON_XWAU_DAT_H
#define AERON_XWAU_DAT_H

/*
 * DAT image containers (JeremyAnsel.Xwa.Dat DatFile.cs): XWA's container,
 * groups of images addressed by (group id, image id). Image formats 24 (stock,
 * palette index and alpha per texel) and 25 are decoded; 25 is the 32-bit format
 * of the XWAU tools (read in game by XWAU's
 * hook_32bpp), whose `colors` header field selects the XWAU encoding:
 *   0  raw B,G,R,A (or BC7 when the data is shorter than W*H*4)
 *   1  LZMA: 5 property bytes, then the raw B,G,R,A stream
 *   2  BC3
 *   3  BC5
 * The block-compressed encodings hold standard R,G,B,A blocks: XWAU uploads
 * BC7 images as DXGI_FORMAT_BC7_UNORM without swizzling
 * (XwaTextureData::CreateSRVFromBuffer), and decoded plainly they match the
 * OPT textures they animate (MillenniumFalcon2Cockpit.dat against
 * CorellianTransport2Cockpit's panels). BC3 comes from the same encoder
 * family. BC5 holds red and green (normal-map X and Y).
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
} XwauDatEncoding;

typedef struct XwauDatImage {
	uint32_t        width;
	uint32_t        height;
	XwauDatEncoding encoding;
	/* width*height R,G,B,A rows in file order (first row = UV v 0). For BC5,
	 * R and G are the stored channels, B is 0 and A is 255. */
	uint8_t* rgba;
} XwauDatImage;

/* A DAT file read into memory, with its image directory. */
typedef struct XwauDat XwauDat;

/* Read and index the DAT file at `path`; NULL with `error` set on failure. */
XwauDat* XwauDat_Open(const char* path, char* error, size_t error_size);

void XwauDat_Close(XwauDat* dat);

/* The image ids of `group` in file order (owned by `dat`); 0 when the group
 * has none. */
size_t XwauDat_ListGroup(const XwauDat* dat, int group, const int** images);

/* Decode image (group, image) of an opened DAT file. */
bool XwauDat_LoadImageFrom(const XwauDat* dat, int group, int image, XwauDatImage* out, char* error,
						   size_t error_size);

/* Decode image (group, image) of the DAT file at `path`. */
bool XwauDat_LoadImage(const char* path, int group, int image, XwauDatImage* out, char* error,
					   size_t error_size);

void XwauDat_FreeImage(XwauDatImage* image);

#endif /* AERON_XWAU_DAT_H */
