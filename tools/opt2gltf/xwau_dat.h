#ifndef AERON_XWAU_DAT_H
#define AERON_XWAU_DAT_H

/*
 * DAT image containers (JeremyAnsel.Xwa.Dat DatFile.cs): XWA's container,
 * groups of images addressed by (group id, image id). Only image format 25 is
 * decoded: the 32-bit format of the XWAU tools (read in game by XWAU's
 * hook_32bpp), whose `colors` header field selects the XWAU encoding:
 *   0  raw B,G,R,A (or BC7 when the data is shorter than W*H*4)
 *   1  LZMA: 5 property bytes, then the raw B,G,R,A stream
 *   2  BC3
 *   3  BC5
 * The block-compressed encodings are produced by JeremyAnsel's encoders
 * from the B,G,R,A bytes, so BC3/BC7 channel 0 holds blue; they are swapped
 * back here. BC5 holds red and green (normal-map X and Y).
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
} XwauDatEncoding;

typedef struct XwauDatImage {
	uint32_t        width;
	uint32_t        height;
	XwauDatEncoding encoding;
	/* width*height R,G,B,A rows in file order (first row = UV v 0). For BC5,
	 * R and G are the stored channels, B is 0 and A is 255. */
	uint8_t* rgba;
} XwauDatImage;

/* Decode image (group, image) of the DAT file at `path`. */
bool XwauDat_LoadImage(const char* path, int group, int image, XwauDatImage* out, char* error,
					   size_t error_size);

void XwauDat_FreeImage(XwauDatImage* image);

#endif /* AERON_XWAU_DAT_H */
