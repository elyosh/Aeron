#include "xwau_cube_faces.h"

#include "ldr_read.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Each OpenXWA face is XWAU face `source`, mirrored left-right or flipped
 * top-bottom. */
static const struct {
	int  source;
	bool mirror_x;
	bool flip_y;
} kFaces[6] = {
	{ 0, true, false }, /* +X: Right */
	{ 1, true, false }, /* -X: Left */
	{ 2, false, true }, /* +Y: Top */
	{ 3, false, true }, /* -Y: Bottom */
	{ 5, true, false }, /* +Z: Back */
	{ 4, true, false }, /* -Z: Front */
};

static bool set_error_message(char* err, size_t errsz, const char* format, ...) {
	if (err && errsz) {
		va_list args;
		va_start(args, format);
		vsnprintf(err, errsz, format, args);
		va_end(args);
	}
	return false;
}

/* The key that ddraw gives a file: the character just before the last '.'
 * of its name, read with atoi. So a digit gives its value, and any other
 * character gives 0. */
static int file_key(const char* name) {
	const char* dot = strrchr(name, '.');
	if (!dot || dot == name || dot[-1] < '0' || dot[-1] > '9')
		return 0;
	return dot[-1] - '0';
}

static int compare_names_no_case(const void* a, const void* b) {
	return strcasecmp(*(const char* const*)a, *(const char* const*)b);
}

bool XwauCubeFaces_Find(const char* dir, char paths[6][XWAU_CUBE_FACE_PATH_MAX]) {
	size_t count = 0, files = 0;
	char** names = ListDir(dir, false, &count);
	if (!names)
		return false;
	/* Windows lists the files of an NTFS folder in name order, without
	   case. */
	for (size_t i = 0; i < count; ++i) {
		char path[XWAU_CUBE_FACE_PATH_MAX];
		if (names[i][0] != '.' && PathFormat(path, sizeof path, "%s/%s", dir, names[i]) && IsFile(path))
			names[files++] = names[i];
		else
			free(names[i]);
	}
	qsort(names, files, sizeof *names, compare_names_no_case);
	/* For each number from 0 to the file count minus 1, ddraw takes the
	   first file whose key is that number. The folder loads when this gives
	   six files, which are faces 0 to 5 in that order. */
	size_t faces = 0;
	for (size_t i = 0; i < files; ++i)
		for (size_t k = 0; k < files; ++k)
			if ((size_t)file_key(names[k]) == i) {
				if (faces < 6)
					PathFormat(paths[faces], XWAU_CUBE_FACE_PATH_MAX, "%s/%s", dir, names[k]);
				++faces;
				break;
			}
	FreeList(names, files);
	return faces == 6;
}

/* Mirrors an `n`-pixel RGBA8 face in place, left-right or top-bottom. */
static void mirror_face_in_place(uint8_t* rgba, int n, bool mirror_x, bool flip_y) {
	for (int y = 0; y < (flip_y ? n / 2 : n); ++y) {
		for (int x = 0; x < (flip_y ? n : n / 2); ++x) {
			const int sx = mirror_x ? n - 1 - x : x;
			const int sy = flip_y ? n - 1 - y : y;
			uint8_t*  a  = rgba + ((size_t)y * n + x) * 4u;
			uint8_t*  b  = rgba + ((size_t)sy * n + sx) * 4u;
			uint8_t   texel[4];
			memcpy(texel, a, 4);
			memcpy(a, b, 4);
			memcpy(b, texel, 4);
		}
	}
}

bool XwauCubeFaces_Read(const char* dir, XwauCubeFaces* out, char* err, size_t errsz) {
	memset(out, 0, sizeof *out);
	char paths[6][XWAU_CUBE_FACE_PATH_MAX];
	bool ok =
		XwauCubeFaces_Find(dir, paths) ||
		set_error_message(err, errsz, "%s: ddraw doesn't find the six faces of a cube in this folder", dir);

	uint8_t* source[6] = { NULL };
	for (int face = 0; face < 6 && ok; ++face) {
		int  w = 0, h = 0;
		char reason[512];
		if (!read_ldr_rgba(paths[face], &source[face], &w, &h, reason, sizeof reason))
			ok = set_error_message(err, errsz, "%s", reason);
		else if (w != h)
			ok = set_error_message(err, errsz, "%s is not square", paths[face]);
		else if (face > 0 && w != out->size)
			ok = set_error_message(err, errsz, "%s is not the size of the other faces", paths[face]);
		out->size = w;
	}
	for (int face = 0; face < 6 && ok; ++face) {
		const int from = kFaces[face].source;
		mirror_face_in_place(source[from], out->size, kFaces[face].mirror_x, kFaces[face].flip_y);
		out->rgba[face] = source[from];
		source[from]    = NULL;
	}
	for (int face = 0; face < 6; ++face)
		free(source[face]);
	if (!ok)
		XwauCubeFaces_Free(out);
	return ok;
}

void XwauCubeFaces_Free(XwauCubeFaces* faces) {
	for (int face = 0; face < 6; ++face) {
		free(faces->rgba[face]);
		faces->rgba[face] = NULL;
	}
}
