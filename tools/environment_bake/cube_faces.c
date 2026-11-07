#include "cube_faces.h"

#include "hdr_read.h"
#include "ldr_read.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* const k_face_names[6] = { "px", "nx", "py", "ny", "pz", "nz" };
static const char* const k_extensions[3] = { ".hdr", ".png", ".jpg" };

/* Filled once, whichever thread loads faces first. */
static float          s_srgb_to_linear[256];
static pthread_once_t s_srgb_once = PTHREAD_ONCE_INIT;

static void fill_srgb_to_linear_table(void) {
	for (int i = 0; i < 256; ++i) {
		const double c      = i / 255.0;
		s_srgb_to_linear[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
	}
}

static bool set_error_message(char* err, size_t errsz, const char* format, ...) {
	if (err && errsz) {
		va_list args;
		va_start(args, format);
		vsnprintf(err, errsz, format, args);
		va_end(args);
	}
	return false;
}

/* One face of the input: sRGB RGBA8 pixels, or linear RGB floats. */
typedef struct FaceSource {
	const uint8_t* rgba;
	const float*   rgb;
	int            n;
} FaceSource;

/* Writes row `y` of `source` as linear values with `channels` channels. With
 * 4 channels, the colour is premultiplied by alpha. */
static void read_face_row_as_linear(const FaceSource* source, int y, int channels, float intensity,
									float* out) {
	const size_t k = (size_t)channels;
	for (int x = 0; x < source->n; ++x) {
		float* dst = out + (size_t)x * k;
		if (source->rgb) {
			memcpy(dst, source->rgb + ((size_t)y * source->n + x) * 3u, 3 * sizeof(float));
			continue;
		}
		const uint8_t* p = source->rgba + ((size_t)y * source->n + x) * 4u;
		const float    a = channels == 4 ? p[3] / 255.0f : 1.0f;
		for (int c = 0; c < 3; ++c)
			dst[c] = s_srgb_to_linear[p[c]] * a * intensity;
		if (channels == 4)
			dst[3] = a;
	}
}

/* Source texels [first, first + count) of output texel `o` when `n` texels
 * become `m`, and the share of the first and last ones. */
typedef struct Span {
	int   first, count;
	float first_weight, last_weight;
} Span;

static Span source_span_of_texel(int o, int n, int m) {
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

static float source_texel_weight(const Span* s, int i, int n, int m) {
	if (s->count == 1)
		return 1.0f;
	if (i == 0)
		return s->first_weight;
	if (i == s->count - 1)
		return s->last_weight;
	return (float)m / (float)n;
}

/* Resamples an `n`-pixel face to `m` pixels (m <= n) by area averaging: rows
 * first, then columns. */
static float* resample_face_by_area(const FaceSource* source, int m, int channels, float intensity) {
	const int    n   = source->n;
	const size_t k   = (size_t)channels;
	float*       out = (float*)malloc((size_t)m * m * k * sizeof(float));
	if (out && m == n) {
		/* Each texel is its own average. Adding 0 turns -0 into +0, as the
		   two passes below do. */
		for (int y = 0; y < n; ++y) {
			float* dst = out + (size_t)y * n * k;
			read_face_row_as_linear(source, y, channels, intensity, dst);
			for (size_t j = 0; j < (size_t)n * k; ++j)
				dst[j] += 0.0f;
		}
		return out;
	}
	float* line = (float*)malloc((size_t)n * k * sizeof(float));
	float* rows = (float*)malloc((size_t)n * m * k * sizeof(float));
	if (!line || !rows || !out) {
		free(line);
		free(rows);
		free(out);
		return NULL;
	}
	for (int y = 0; y < n; ++y) {
		float* dst = rows + (size_t)y * m * k;
		read_face_row_as_linear(source, y, channels, intensity, line);
		for (int x = 0; x < m; ++x) {
			const Span s      = source_span_of_texel(x, n, m);
			float      sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (int i = 0; i < s.count; ++i) {
				const float  w = source_texel_weight(&s, i, n, m);
				const float* p = line + (size_t)(s.first + i) * k;
				for (size_t c = 0; c < k; ++c)
					sum[c] += w * p[c];
			}
			memcpy(dst + (size_t)x * k, sum, k * sizeof(float));
		}
	}
	for (int y = 0; y < m; ++y) {
		const Span s   = source_span_of_texel(y, n, m);
		float*     dst = out + (size_t)y * m * k;
		memset(dst, 0, (size_t)m * k * sizeof(float));
		for (int i = 0; i < s.count; ++i) {
			const float  w   = source_texel_weight(&s, i, n, m);
			const float* src = rows + (size_t)(s.first + i) * m * k;
			for (size_t j = 0; j < (size_t)m * k; ++j)
				dst[j] += w * src[j];
		}
	}
	free(line);
	free(rows);
	return out;
}

/* One face, read and resampled on its own thread. */
typedef struct FaceJob {
	const char* path; /* NULL when the pixels are in memory */
	FaceSource  source;
	int         width, height;
	bool        read; /* false: `reason` says why */
	char        reason[512];
	float*      resampled; /* NULL if the face can't be resampled */
} FaceJob;

typedef struct FacesTask {
	FaceJob jobs[6];
	bool    hdr;
	int     size; /* 0: the size of the faces */
	int     channels;
	float   intensity;
} FacesTask;

static void face_task(void* context, int face) {
	FacesTask* task = (FacesTask*)context;
	FaceJob*   job  = &task->jobs[face];
	uint8_t*   rgba = NULL;
	float*     rgb  = NULL;
	if (job->path) {
		job->read =
			task->hdr
				? read_hdr_rgb(job->path, &rgb, &job->width, &job->height, job->reason, sizeof job->reason)
				: read_ldr_rgba(job->path, &rgba, &job->width, &job->height, job->reason, sizeof job->reason);
		for (size_t i = 0; job->read && rgb && i < (size_t)job->width * job->height * 3u; ++i)
			if (!isfinite(rgb[i]) || rgb[i] < 0.0f)
				job->read =
					set_error_message(job->reason, sizeof job->reason,
									  "%s has a non-finite or negative pixel at %zu", job->path, i / 3u);
		job->source = (FaceSource) { rgba, rgb, job->width };
	}
	/* A face that fails the size checks isn't resampled. run_faces then
	   reports the error. */
	const int m = task->size > 0 ? task->size : job->width;
	if (job->read && job->width == job->height && m <= job->width)
		job->resampled = resample_face_by_area(&job->source, m, task->channels, task->intensity);
	free(rgba);
	free(rgb);
}

/* Reads and resamples the six faces at the same time. Then checks them in
 * order, and reports the first error. */
static bool run_faces(FacesTask* task, const char* what, CubeFaces* out, char* err, size_t errsz) {
	pthread_once(&s_srgb_once, fill_srgb_to_linear_table);
	envmap_parallel_for(6, 6, face_task, task);
	bool ok = true;
	for (int face = 0; face < 6 && ok; ++face) {
		const FaceJob* job = &task->jobs[face];
		if (!job->read)
			ok = set_error_message(err, errsz, "%s", job->reason);
		else if (job->width != job->height)
			ok = set_error_message(err, errsz, "%s: face %s is %dx%d, and a face must be square", what,
								   k_face_names[face], job->width, job->height);
		else if (face == 0) {
			out->source_size = job->width;
			out->size        = task->size > 0 ? task->size : job->width;
			if (out->size > job->width)
				ok = set_error_message(err, errsz, "%s: the faces are %d px, smaller than the %d px cube",
									   what, job->width, out->size);
		} else if (job->width != out->source_size)
			ok = set_error_message(err, errsz, "%s: face %s is %d px, and face px is %d px", what,
								   k_face_names[face], job->width, out->source_size);
		if (ok && !job->resampled)
			ok = set_error_message(err, errsz, "%s: out of memory", what);
	}
	for (int face = 0; face < 6; ++face) {
		if (ok)
			out->faces[face] = task->jobs[face].resampled;
		else
			free(task->jobs[face].resampled);
	}
	return ok;
}

/* Finds the file of face `face`. Returns its extension's index, or -1 after
 * setting `err`. */
static int find_face_file(const char* prefix, int face, char* path, size_t path_size, char* err,
						  size_t errsz) {
	int found = -1;
	for (int e = 0; e < 3; ++e) {
		char candidate[1024];
		snprintf(candidate, sizeof candidate, "%s_%s%s", prefix, k_face_names[face], k_extensions[e]);
		FILE* f = fopen(candidate, "rb");
		if (!f)
			continue;
		fclose(f);
		if (found >= 0) {
			set_error_message(err, errsz, "%s_%s: there is more than one file for this face", prefix,
							  k_face_names[face]);
			return -1;
		}
		found = e;
		snprintf(path, path_size, "%s", candidate);
	}
	if (found < 0)
		set_error_message(err, errsz, "%s_%s: there is no .hdr, .png or .jpg file for this face", prefix,
						  k_face_names[face]);
	return found;
}

bool cube_faces_load(const char* prefix, int size, bool alpha, float ldr_intensity, CubeFaces* out, char* err,
					 size_t errsz) {
	memset(out, 0, sizeof *out);
	out->channels = alpha ? 4 : 3;
	char paths[6][1024];
	bool hdr = false;
	for (int face = 0; face < 6; ++face) {
		const int extension = find_face_file(prefix, face, paths[face], sizeof paths[face], err, errsz);
		if (extension < 0)
			return false;
		if (face > 0 && (extension == 0) != hdr)
			return set_error_message(err, errsz, "%s: the faces must all be HDR, or all PNG and JPEG",
									 prefix);
		hdr = extension == 0;
	}
	if (hdr && alpha)
		return set_error_message(err, errsz, "%s: an overlay needs alpha, so its faces must be PNG or JPEG",
								 prefix);

	FacesTask task = {
		.hdr = hdr, .size = size, .channels = out->channels, .intensity = hdr || alpha ? 1.0f : ldr_intensity
	};
	for (int face = 0; face < 6; ++face)
		task.jobs[face].path = paths[face];
	return run_faces(&task, prefix, out, err, errsz);
}

bool cube_faces_from_pixels(const CubeFacePixels* pixels, int size, bool alpha, float ldr_intensity,
							CubeFaces* out, char* err, size_t errsz) {
	memset(out, 0, sizeof *out);
	out->channels  = alpha ? 4 : 3;
	FacesTask task = { .size = size, .channels = out->channels, .intensity = alpha ? 1.0f : ldr_intensity };
	for (int face = 0; face < 6; ++face)
		task.jobs[face] = (FaceJob) { .source = { pixels->rgba[face], NULL, pixels->size },
									  .width  = pixels->size,
									  .height = pixels->size,
									  .read   = true };
	return run_faces(&task, pixels->name, out, err, errsz);
}

void cube_faces_free(CubeFaces* faces) {
	for (int face = 0; face < 6; ++face) {
		free(faces->faces[face]);
		faces->faces[face] = NULL;
	}
}

void cube_faces_to_cube(const CubeFaces* faces, EnvCube* cube) {
	for (int face = 0; face < 6; ++face)
		memcpy(cube->faces[0][face], faces->faces[face],
			   (size_t)faces->size * faces->size * 3u * sizeof(float));
}
