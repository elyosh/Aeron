#ifndef AERON_XWAU_CUBE_FACES_H
#define AERON_XWAU_CUBE_FACES_H

/*
 * Reads the six faces of an XWA Upgrade cube folder (Effects/CubeMaps/<name>)
 * and puts them in the face layout of environment_bake --faces.
 *
 * An XWAU cube folder holds six PNG or JPEG files, one for each face. The
 * digit just before the last '.' of a file name gives its face: 0 Right,
 * 1 Left, 2 Top, 3 Bottom, 4 Front, 5 Back, which are the D3D cube faces +X,
 * -X, +Y, -Y, +Z, -Z. The faces are square and have the same size. Overlay
 * folders hold the same six faces with alpha. The folder can have other
 * files, which ddraw ignores in some cases (XwauCubeFaces_Find). Files whose
 * names start with '.' are ignored.
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

#define XWAU_CUBE_FACE_PATH_MAX 1300

/* Finds the six face files of the cube folder `dir` as ddraw does, and
 * writes their paths to `paths`, from face 0 to 5. ddraw gives each file a
 * key: the character just before the last '.' of its name, read as a number,
 * or 0 when it isn't a digit. For each number from 0 to the file count minus
 * 1, it takes the first file, in name order, whose key is that number. It
 * loads the folder only when this gives exactly six files. Returns false
 * when it doesn't. */
bool XwauCubeFaces_Find(const char* dir, char paths[6][XWAU_CUBE_FACE_PATH_MAX]);

typedef struct XwauCubeFaces {
	int      size;    /* face size in pixels */
	uint8_t* rgba[6]; /* sRGB RGBA8 faces, +X to -Z, in OpenXWA's frame */
} XwauCubeFaces;

/* Reads the cube folder `dir`. Returns false with a message in `err` when
 * the folder isn't a valid cube folder or a file can't be read. */
bool XwauCubeFaces_Read(const char* dir, XwauCubeFaces* out, char* err, size_t errsz);
void XwauCubeFaces_Free(XwauCubeFaces* faces);

#endif /* AERON_XWAU_CUBE_FACES_H */
