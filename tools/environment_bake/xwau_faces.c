#include "xwau_faces.h"

#include "aeron/vfs.h"
#include "ldr_read.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XWAU_NAME_MAX 256

/* OpenXWA face `face` is XWAU face `source`, mirrored left-right or flipped
 * top-bottom (xwau_faces.h). */
static const struct {
	int  source;
	bool mirror_x;
	bool flip_y;
} k_remap[6] = {
	{ 0, true, false }, /* +X: Right */
	{ 1, true, false }, /* -X: Left */
	{ 2, false, true }, /* +Y: Top */
	{ 3, false, true }, /* -Y: Bottom */
	{ 5, true, false }, /* +Z: Back */
	{ 4, true, false }, /* -Z: Front */
};

typedef struct FileList {
	char names[7][XWAU_NAME_MAX];
	int  count; /* stops counting at 7 */
} FileList;

static int list_file(void* userdata, const AeronVfsEntry* entry) {
	FileList* list = (FileList*)userdata;
	if (entry->is_directory || entry->name[0] == '.')
		return 1;
	if (list->count < 7) {
		snprintf(list->names[list->count], XWAU_NAME_MAX, "%s", entry->name);
		list->count++;
	}
	return 1;
}

/* The face of a file name: the digit just before its last '.', or -1. */
static int face_of(const char* name) {
	const char* dot = strrchr(name, '.');
	if (!dot || dot == name || dot[-1] < '0' || dot[-1] > '5')
		return -1;
	return dot[-1] - '0';
}

static float s_srgb_to_linear[256];

static void init_srgb_table(void) {
	for (int i = 0; i < 256; ++i) {
		const double c      = i / 255.0;
		s_srgb_to_linear[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
	}
}

/* Source texels [first, first + count) of output texel `o` when `n` texels
 * become `m`, and the share of the first and last ones. */
typedef struct Span {
	int   first, count;
	float first_weight, last_weight;
} Span;

static Span span_of(int o, int n, int m) {
	const double lo = (double)o * n / m, hi = (double)(o + 1) * n / m;
	Span         s;
	s.first = (int)floor(lo);
	s.count = (int)ceil(hi) - s.first;
	if (s.first + s.count > n) /* rounding */
		s.count = n - s.first;
	const double first_end = fmin(hi, s.first + 1.0), last_start = fmax(lo, s.first + s.count - 1.0);
	s.first_weight = (float)((first_end - lo) / (hi - lo));
	s.last_weight  = (float)((hi - last_start) / (hi - lo));
	return s;
}

static float span_weight(const Span* s, int i, int n, int m) {
	if (s->count == 1)
		return 1.0f;
	if (i == 0)
		return s->first_weight;
	if (i == s->count - 1)
		return s->last_weight;
	return (float)m / (float)n;
}

/* Resamples an `n`-pixel RGBA8 sRGB face to `m` linear RGB pixels (m <= n)
 * by area averaging: rows first, then columns. */
static float* resample_face(const uint8_t* rgba, int n, int m) {
	float* rows = (float*)malloc((size_t)n * m * 3u * sizeof(float));
	float* out  = (float*)malloc((size_t)m * m * 3u * sizeof(float));
	if (!rows || !out) {
		free(rows);
		free(out);
		return NULL;
	}
	for (int y = 0; y < n; ++y) {
		const uint8_t* src = rgba + (size_t)y * n * 4u;
		float*         dst = rows + (size_t)y * m * 3u;
		for (int x = 0; x < m; ++x) {
			const Span s      = span_of(x, n, m);
			float      sum[3] = { 0.0f, 0.0f, 0.0f };
			for (int i = 0; i < s.count; ++i) {
				const float    w = span_weight(&s, i, n, m);
				const uint8_t* p = src + (size_t)(s.first + i) * 4u;
				for (int c = 0; c < 3; ++c)
					sum[c] += w * s_srgb_to_linear[p[c]];
			}
			memcpy(dst + (size_t)x * 3u, sum, sizeof sum);
		}
	}
	for (int y = 0; y < m; ++y) {
		const Span s   = span_of(y, n, m);
		float*     dst = out + (size_t)y * m * 3u;
		memset(dst, 0, (size_t)m * 3u * sizeof(float));
		for (int i = 0; i < s.count; ++i) {
			const float  w   = span_weight(&s, i, n, m);
			const float* src = rows + (size_t)(s.first + i) * m * 3u;
			for (int k = 0; k < m * 3; ++k)
				dst[k] += w * src[k];
		}
	}
	free(rows);
	return out;
}

static bool fail(char* err, size_t errsz, const char* format, ...) {
	if (err && errsz) {
		va_list args;
		va_start(args, format);
		vsnprintf(err, errsz, format, args);
		va_end(args);
	}
	return false;
}

bool xwau_faces_load(const char* dir, int size, XwauFaces* out, char* err, size_t errsz) {
	memset(out, 0, sizeof *out);
	init_srgb_table();
	FileList   list = { .count = 0 };
	AeronVfs*  vfs  = AeronVfs_Create(NULL);
	const bool listed =
		vfs && AeronVfs_SetRoot(vfs, AERON_VFS_ROOT_ASSET, dir) &&
		AeronVfs_Glob(vfs, AERON_VFS_ROOT_ASSET, "", "*", AERON_VFS_GLOB_FILES, list_file, &list);
	AeronVfs_Destroy(vfs);
	if (!listed)
		return fail(err, errsz, "%s: cannot list the folder", dir);
	if (list.count != 6)
		return fail(err, errsz, "%s: a cube folder holds exactly six files (it has %s)", dir,
					list.count > 6 ? "more" : "fewer");

	const char* names[6] = { NULL };
	for (int i = 0; i < 6; ++i) {
		const int face = face_of(list.names[i]);
		if (face < 0)
			return fail(err, errsz, "%s: %s has no face digit (0 to 5) before its extension", dir,
						list.names[i]);
		if (names[face])
			return fail(err, errsz, "%s: two files are face %s", dir, list.names[i]);
		names[face] = list.names[i];
	}

	for (int face = 0; face < 6; ++face) {
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", dir, names[face]);
		uint8_t* rgba = NULL;
		int      w = 0, h = 0;
		char     reason[512];
		if (!read_ldr_rgba(path, &rgba, &w, &h, reason, sizeof reason)) {
			xwau_faces_free(out);
			return fail(err, errsz, "%s", reason);
		}
		bool ok = true;
		if (w != h) {
			ok = fail(err, errsz, "%s: %s is not square", dir, names[face]);
		} else if (face > 0 && w != out->source_size) {
			ok = fail(err, errsz, "%s: %s is not the size of the other faces", dir, names[face]);
		} else if (face == 0) {
			out->source_size = w;
			out->size        = size > 0 ? size : w;
			if (out->size > w)
				ok = fail(err, errsz, "%s: the faces are %d px, smaller than the %d px asked for", dir, w,
						  out->size);
		}
		if (ok) {
			out->faces[face] = resample_face(rgba, w, out->size);
			if (!out->faces[face])
				ok = fail(err, errsz, "%s: out of memory", dir);
		}
		free(rgba);
		if (!ok) {
			xwau_faces_free(out);
			return false;
		}
	}
	return true;
}

void xwau_faces_free(XwauFaces* faces) {
	for (int face = 0; face < 6; ++face) {
		free(faces->faces[face]);
		faces->faces[face] = NULL;
	}
}

void xwau_faces_to_cube(const XwauFaces* faces, EnvCube* cube, bool remap) {
	const int n = faces->size;
	for (int face = 0; face < 6; ++face) {
		const int    source   = remap ? k_remap[face].source : face;
		const bool   mirror_x = remap && k_remap[face].mirror_x;
		const bool   flip_y   = remap && k_remap[face].flip_y;
		const float* src      = faces->faces[source];
		float*       dst      = cube->faces[0][face];
		for (int y = 0; y < n; ++y) {
			const int sy = flip_y ? n - 1 - y : y;
			for (int x = 0; x < n; ++x) {
				const int sx = mirror_x ? n - 1 - x : x;
				memcpy(dst + ((size_t)y * n + x) * 3u, src + ((size_t)sy * n + sx) * 3u, 3u * sizeof(float));
			}
		}
	}
}
