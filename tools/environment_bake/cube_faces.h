/*
 * cube_faces — the six faces of a cube, read from files or from memory.
 *
 * The faces are in the KTX2 order +X, -X, +Y, -Y, +Z, -Z, in the authored
 * frame (+X right, +Y up, -Z forward), so the faces that the game captures
 * can be used too. The files of a prefix are <prefix>_px, <prefix>_nx,
 * <prefix>_py, <prefix>_ny, <prefix>_pz and <prefix>_nz, each with the
 * extension .hdr, .png or .jpg. The faces are all HDR, or all PNG and JPEG.
 * They are square and have the same size.
 *
 * HDR faces are linear. PNG and JPEG faces are decoded from sRGB, and
 * multiplied by an intensity.
 */
#ifndef ENVIRONMENT_BAKE_CUBE_FACES_H
#define ENVIRONMENT_BAKE_CUBE_FACES_H

#include "envmap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CubeFaces {
	int    size;        /* face size after resampling */
	int    source_size; /* face size of the input */
	int    channels;    /* 3, or 4 with alpha */
	float* faces[6];    /* size * size * channels */
} CubeFaces;

/* Six sRGB RGBA8 faces in memory, in the same order and frame as the files.
 * `name` names them in messages. */
typedef struct CubeFacePixels {
	const char*    name;
	int            size;
	const uint8_t* rgba[6];
} CubeFacePixels;

/* Reads the faces of `prefix` and resamples each one to `size` pixels by
 * area averaging. `size` 0 keeps the size of the files, and a size larger
 * than the files' is an error. Without `alpha`, the faces are linear RGB.
 * With it, they are linear RGB premultiplied by alpha, then alpha. Faces
 * with alpha must be PNG or JPEG. `ldr_intensity` multiplies the colour of
 * PNG and JPEG faces without alpha. The six faces are read and resampled on
 * six threads. */
bool cube_faces_load(const char* prefix, int size, bool alpha, float ldr_intensity, CubeFaces* out, char* err,
					 size_t errsz);

/* Does the same as cube_faces_load with faces that are already in memory. */
bool cube_faces_from_pixels(const CubeFacePixels* pixels, int size, bool alpha, float ldr_intensity,
							CubeFaces* out, char* err, size_t errsz);

void cube_faces_free(CubeFaces* faces);

/* Copies the faces into level 0 of `cube`, which has their size. The faces
 * must have no alpha. */
void cube_faces_to_cube(const CubeFaces* faces, EnvCube* cube);

#endif
