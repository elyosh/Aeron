#include "xwau_cube_faces.h"

#include "ldr_read.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The XWAU face of a file name: the digit just before its last '.', or -1. */
static int face_from_file_name(const char* name) {
	const char* dot = strrchr(name, '.');
	if (!dot || dot == name || dot[-1] < '0' || dot[-1] > '5')
		return -1;
	return dot[-1] - '0';
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
	size_t count = 0, files = 0;
	char** names = ListDir(dir, true, &count);
	char   paths[6][1300];
	bool   found[6] = { false };
	bool   ok       = names != NULL || set_error_message(err, errsz, "%s: the folder can't be read", dir);
	for (size_t i = 0; i < count && ok; ++i) {
		char path[1300];
		if (names[i][0] == '.' || !PathFormat(path, sizeof path, "%s/%s", dir, names[i]) || !IsFile(path))
			continue;
		const int face = face_from_file_name(names[i]);
		++files;
		if (face < 0)
			ok = set_error_message(err, errsz, "%s: %s has no face digit (0 to 5) before its extension", dir,
								   names[i]);
		else if (found[face])
			ok = set_error_message(err, errsz, "%s: two files are face %d", dir, face);
		else {
			found[face] = true;
			snprintf(paths[face], sizeof paths[face], "%s", path);
		}
	}
	FreeList(names, count);
	if (ok && files != 6)
		ok = set_error_message(err, errsz, "%s: a cube folder holds exactly six files, and it has %zu", dir,
							   files);

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
