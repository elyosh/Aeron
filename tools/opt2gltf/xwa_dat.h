#ifndef AERON_XWA_DAT_H
#define AERON_XWA_DAT_H

/*
 * XWA DAT image containers (JeremyAnsel.Xwa.Dat DatFile.cs): groups of
 * images addressed by (group id, image id). Only the 32-bit image format 25
 * is decoded; its `colors` header field selects the encoding:
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

typedef enum XwaDatEncoding {
	XWA_DAT_ENCODING_BGRA = 0,
	XWA_DAT_ENCODING_LZMA,
	XWA_DAT_ENCODING_BC3,
	XWA_DAT_ENCODING_BC5,
	XWA_DAT_ENCODING_BC7,
} XwaDatEncoding;

typedef struct XwaDatImage {
	uint32_t       width;
	uint32_t       height;
	XwaDatEncoding encoding;
	/* width*height R,G,B,A rows in file order (first row = UV v 0). For BC5,
	 * R and G are the stored channels, B is 0 and A is 255. */
	uint8_t* rgba;
} XwaDatImage;

/* Decode image (group, image) of the DAT file at `path`. */
bool XwaDat_LoadImage(const char* path, int group, int image, XwaDatImage* out, char* error,
					  size_t error_size);

void XwaDat_FreeImage(XwaDatImage* image);

#endif /* AERON_XWA_DAT_H */
