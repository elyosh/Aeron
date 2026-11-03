#ifndef AERON_XWAU_CUBE_FACES_H
#define AERON_XWAU_CUBE_FACES_H

/*
 * Reads the six faces of an XWA Upgrade cube folder (Effects/CubeMaps/<name>)
 * and puts them in the face layout of environment_bake --faces.
 *
 * An XWAU cube folder holds exactly six PNG or JPEG files. Files whose names
 * start with '.' are ignored. The digit just before the last '.' of a file
 * name gives its face: 0 Right, 1 Left, 2 Top, 3 Bottom, 4 Front, 5 Back,
 * which are the D3D cube faces +X, -X, +Y, -Y, +Z, -Z. The faces are square
 * and have the same size. Overlay folders hold the same six faces with
 * alpha.
 *
 * XWAU reads its cube along (Xw, Zw, Yw) of a world ray. OpenXWA reads its
 * environment cube along (Xw, Zw, -Yw). So an OpenXWA direction d reads the
 * XWAU cube at M d, with M = diag(1, 1, -1). Both cubes use the same texel
 * layout, so each OpenXWA face is one XWAU face, mirrored:
 *
 *   OpenXWA +X = Right  (mirrored left-right)
 *   OpenXWA -X = Left   (mirrored left-right)
 *   OpenXWA +Y = Top    (flipped top-bottom)
 *   OpenXWA -Y = Bottom (flipped top-bottom)
 *   OpenXWA +Z = Back   (mirrored left-right)
 *   OpenXWA -Z = Front  (mirrored left-right)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XwauCubeFaces {
	int      size;    /* face size in pixels */
	uint8_t* rgba[6]; /* sRGB RGBA8 faces, +X to -Z, in OpenXWA's frame */
} XwauCubeFaces;

/* Reads the cube folder `dir`. Returns false with a message in `err` when
 * the folder isn't a valid cube folder or a file can't be read. */
bool XwauCubeFaces_Read(const char* dir, XwauCubeFaces* out, char* err, size_t errsz);
void XwauCubeFaces_Free(XwauCubeFaces* faces);

#endif /* AERON_XWAU_CUBE_FACES_H */
