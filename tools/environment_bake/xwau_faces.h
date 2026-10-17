/*
 * xwau_faces — the six faces of an XWA Upgrade skybox folder.
 *
 * An XWAU cube folder (Effects/CubeMaps/<name>) holds exactly six PNG or
 * JPEG files. The digit just before the last '.' of a file name is its face:
 * 0 Right, 1 Left, 2 Top, 3 Bottom, 4 Front, 5 Back, which are the D3D cube
 * faces +X, -X, +Y, -Y, +Z, -Z. The faces are square and the same size.
 * Files whose names start with '.' are ignored.
 *
 * XWAU reads its cube along (Xw, Zw, Yw) of a world ray; OpenXWA reads its
 * environment cube along (Xw, Zw, -Yw) (envmap.h). So an OpenXWA direction d
 * reads the XWAU cube at M d, with M = diag(1, 1, -1). Both cubes use the
 * same texel layout, so each OpenXWA face is one XWAU face, mirrored:
 *
 *   OpenXWA +X = Right  (mirrored left-right)
 *   OpenXWA -X = Left   (mirrored left-right)
 *   OpenXWA +Y = Top    (flipped top-bottom)
 *   OpenXWA -Y = Bottom (flipped top-bottom)
 *   OpenXWA +Z = Back   (mirrored left-right)
 *   OpenXWA -Z = Front  (mirrored left-right)
 *
 * Overlay folders hold the same six faces with alpha.
 */
#ifndef ENVIRONMENT_BAKE_XWAU_FACES_H
#define ENVIRONMENT_BAKE_XWAU_FACES_H

#include "envmap.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct XwauFaces {
	int    size;        /* face size after resampling */
	int    source_size; /* face size in the files */
	int    channels;    /* 3, or 4 with alpha */
	float* faces[6];    /* size * size * channels, in XWAU face order */
} XwauFaces;

/* Reads the folder `dir` and resamples each face to `size` pixels by area
 * averaging, decoding the 8-bit sRGB values to linear. Without `alpha`, the
 * faces are linear RGB. With it, they are linear RGB premultiplied by
 * alpha, then alpha. `size` 0 keeps the file size. A size larger than the
 * files' is an error. */
bool xwau_faces_load(const char* dir, int size, bool alpha, XwauFaces* out, char* err, size_t errsz);
void xwau_faces_free(XwauFaces* faces);

/* Writes face `face` (0 to 5, +X to -Z) of the cube, remapped to OpenXWA's
 * cube frame or in XWAU's own layout, to `out` (size * size * channels). */
void xwau_faces_face(const XwauFaces* faces, int face, bool remap, float* out);

/* Writes the faces into level 0 of `cube`, which has their size, as
 * xwau_faces_face does. The faces must have no alpha. */
void xwau_faces_to_cube(const XwauFaces* faces, EnvCube* cube, bool remap);

#endif
