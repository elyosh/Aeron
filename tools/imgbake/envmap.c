#include "envmap.h"

#include "aeron/sync.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ENV_PI 3.14159265358979323846

/* ---- cube storage --------------------------------------------------- */

int envmap_mip_count(int size) {
	int count = 1;
	while (size > 1 && count < ENVMAP_MAX_LEVELS) {
		size /= 2;
		++count;
	}
	return count;
}

int envmap_level_size(const EnvCube* cube, int level) {
	const int size = cube->size >> level;
	return size > 0 ? size : 1;
}

bool envmap_cube_alloc(EnvCube* cube, int size, int level_count) {
	memset(cube, 0, sizeof *cube);
	if (size < 1 || level_count < 1 || level_count > envmap_mip_count(size))
		return false;
	cube->size        = size;
	cube->level_count = level_count;
	for (int level = 0; level < level_count; ++level) {
		const size_t n = (size_t)envmap_level_size(cube, level);
		for (int face = 0; face < 6; ++face) {
			cube->faces[level][face] = (float*)calloc(n * n * 3u, sizeof(float));
			if (!cube->faces[level][face]) {
				envmap_cube_free(cube);
				return false;
			}
		}
	}
	return true;
}

void envmap_cube_free(EnvCube* cube) {
	for (int level = 0; level < ENVMAP_MAX_LEVELS; ++level)
		for (int face = 0; face < 6; ++face)
			free(cube->faces[level][face]);
	memset(cube, 0, sizeof *cube);
}

/* ---- directions ------------------------------------------------------ */

static void normalize3(float v[3]) {
	const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (length > 0.0f) {
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

static void face_direction(int face, float s, float t, float out[3]) {
	switch (face) {
		case 0:
			out[0] = 1.0f, out[1] = -t, out[2] = -s;
			break;
		case 1:
			out[0] = -1.0f, out[1] = -t, out[2] = s;
			break;
		case 2:
			out[0] = s, out[1] = 1.0f, out[2] = t;
			break;
		case 3:
			out[0] = s, out[1] = -1.0f, out[2] = -t;
			break;
		case 4:
			out[0] = s, out[1] = -t, out[2] = 1.0f;
			break;
		default:
			out[0] = -s, out[1] = -t, out[2] = -1.0f;
			break;
	}
}

void envmap_texel_direction(int face, int x, int y, int size, float out[3]) {
	const float s = 2.0f * ((float)x + 0.5f) / (float)size - 1.0f;
	const float t = 2.0f * ((float)y + 0.5f) / (float)size - 1.0f;
	face_direction(face, s, t, out);
	normalize3(out);
}

/* Face and [0, 1] face coordinates of a direction (inverse of the table). */
static void direction_to_face(const float d[3], int* face, float* u, float* v) {
	const float ax = fabsf(d[0]), ay = fabsf(d[1]), az = fabsf(d[2]);
	float       sc, tc, ma;
	if (ax >= ay && ax >= az) {
		ma    = ax;
		*face = d[0] > 0.0f ? 0 : 1;
		sc    = d[0] > 0.0f ? -d[2] : d[2];
		tc    = -d[1];
	} else if (ay >= az) {
		ma    = ay;
		*face = d[1] > 0.0f ? 2 : 3;
		sc    = d[0];
		tc    = d[1] > 0.0f ? d[2] : -d[2];
	} else {
		ma    = az;
		*face = d[2] > 0.0f ? 4 : 5;
		sc    = d[2] > 0.0f ? d[0] : -d[0];
		tc    = -d[1];
	}
	if (ma <= 0.0f)
		ma = 1.0f;
	*u = 0.5f * (sc / ma + 1.0f);
	*v = 0.5f * (tc / ma + 1.0f);
}

static double corner_area(double x, double y) { return atan2(x * y, sqrt(x * x + y * y + 1.0)); }

double envmap_texel_solid_angle(int x, int y, int size) {
	const double inv = 1.0 / (double)size;
	const double s0  = 2.0 * (double)x * inv - 1.0;
	const double t0  = 2.0 * (double)y * inv - 1.0;
	const double s1  = s0 + 2.0 * inv;
	const double t1  = t0 + 2.0 * inv;
	return corner_area(s0, t0) - corner_area(s0, t1) - corner_area(s1, t0) + corner_area(s1, t1);
}

/* ---- sampling -------------------------------------------------------- */

static void sample_level(const EnvCube* cube, int level, int face, float u, float v, float out[3]) {
	const int    n  = envmap_level_size(cube, level);
	const float* px = cube->faces[level][face];
	float        fx = u * (float)n - 0.5f;
	float        fy = v * (float)n - 0.5f;
	if (fx < 0.0f)
		fx = 0.0f;
	if (fy < 0.0f)
		fy = 0.0f;
	if (fx > (float)(n - 1))
		fx = (float)(n - 1);
	if (fy > (float)(n - 1))
		fy = (float)(n - 1);
	const int    x0 = (int)fx, y0 = (int)fy;
	const int    x1 = x0 + 1 < n ? x0 + 1 : x0;
	const int    y1 = y0 + 1 < n ? y0 + 1 : y0;
	const float  ax = fx - (float)x0, ay = fy - (float)y0;
	const float* p00 = px + ((size_t)y0 * n + x0) * 3u;
	const float* p10 = px + ((size_t)y0 * n + x1) * 3u;
	const float* p01 = px + ((size_t)y1 * n + x0) * 3u;
	const float* p11 = px + ((size_t)y1 * n + x1) * 3u;
	for (int c = 0; c < 3; ++c) {
		const float top    = p00[c] + (p10[c] - p00[c]) * ax;
		const float bottom = p01[c] + (p11[c] - p01[c]) * ax;
		out[c]             = top + (bottom - top) * ay;
	}
}

void envmap_sample(const EnvCube* cube, const float dir[3], float lod, float out[3]) {
	int   face;
	float u, v;
	direction_to_face(dir, &face, &u, &v);
	const float max_lod = (float)(cube->level_count - 1);
	if (!(lod > 0.0f))
		lod = 0.0f;
	if (lod > max_lod)
		lod = max_lod;
	const int level = (int)lod;
	sample_level(cube, level, face, u, v, out);
	const float frac = lod - (float)level;
	if (frac > 0.0f && level + 1 < cube->level_count) {
		float next[3];
		sample_level(cube, level + 1, face, u, v, next);
		for (int c = 0; c < 3; ++c)
			out[c] += (next[c] - out[c]) * frac;
	}
}

void envmap_build_box_mips(EnvCube* cube) {
	for (int level = 1; level < cube->level_count; ++level) {
		const int sn = envmap_level_size(cube, level - 1);
		const int dn = envmap_level_size(cube, level);
		for (int face = 0; face < 6; ++face) {
			const float* src = cube->faces[level - 1][face];
			float*       dst = cube->faces[level][face];
			for (int y = 0; y < dn; ++y) {
				const int y0 = y * 2 < sn ? y * 2 : sn - 1;
				const int y1 = y * 2 + 1 < sn ? y * 2 + 1 : sn - 1;
				for (int x = 0; x < dn; ++x) {
					const int x0 = x * 2 < sn ? x * 2 : sn - 1;
					const int x1 = x * 2 + 1 < sn ? x * 2 + 1 : sn - 1;
					for (int c = 0; c < 3; ++c) {
						dst[((size_t)y * dn + x) * 3u + c] =
							0.25f *
							(src[((size_t)y0 * sn + x0) * 3u + c] + src[((size_t)y0 * sn + x1) * 3u + c] +
							 src[((size_t)y1 * sn + x0) * 3u + c] + src[((size_t)y1 * sn + x1) * 3u + c]);
					}
				}
			}
		}
	}
}

/* ---- parallel rows --------------------------------------------------- */

typedef struct EnvParallelJob {
	EnvmapTaskFn fn;
	void*        context;
	int          count;
	volatile int next;
} EnvParallelJob;

static int env_parallel_worker(void* user) {
	EnvParallelJob* job = (EnvParallelJob*)user;
	for (;;) {
		const int index = Aeron_AtomicAdd(&job->next, 1);
		if (index >= job->count)
			return 0;
		job->fn(job->context, index);
	}
}

void envmap_parallel_for(int count, int jobs, EnvmapTaskFn fn, void* context) {
	EnvParallelJob job = { fn, context, count, 0 };
	if (jobs > ENVMAP_MAX_JOBS)
		jobs = ENVMAP_MAX_JOBS;
	if (jobs > count)
		jobs = count;
	AeronThread* threads[ENVMAP_MAX_JOBS];
	int          started = 0;
	for (int i = 1; i < jobs; ++i) {
		threads[started] = Aeron_ThreadCreate("envmap", env_parallel_worker, &job);
		if (threads[started])
			++started;
	}
	env_parallel_worker(&job);
	for (int i = 0; i < started; ++i)
		Aeron_ThreadJoin(threads[i]);
}

typedef void (*EnvRowFn)(void* context, int face, int y);

typedef struct EnvRowTask {
	EnvRowFn fn;
	void*    context;
	int      size;
} EnvRowTask;

static void env_row_task(void* context, int index) {
	const EnvRowTask* task = (const EnvRowTask*)context;
	task->fn(task->context, index / task->size, index % task->size);
}

/* Runs fn over every (face, row) of a `size` cube on up to `jobs` threads. */
static void env_parallel_rows(int size, int jobs, EnvRowFn fn, void* context) {
	EnvRowTask task = { fn, context, size };
	envmap_parallel_for(6 * size, jobs, env_row_task, &task);
}

/* ---- equirectangular input ------------------------------------------- */

typedef struct EquirectContext {
	EnvCube*     cube;
	const float* rgb;
	int          w, h;
	int          sub; /* subsamples per texel axis */
} EquirectContext;

static void equirect_sample(const EquirectContext* ctx, const float d[3], float out[3]) {
	const float lon = atan2f(d[0], -d[2]);
	const float lat = asinf(d[1] < -1.0f ? -1.0f : (d[1] > 1.0f ? 1.0f : d[1]));
	const float u   = 0.5f + lon / (float)(2.0 * ENV_PI);
	const float v   = 0.5f - lat / (float)ENV_PI;
	float       fx  = u * (float)ctx->w - 0.5f;
	float       fy  = v * (float)ctx->h - 0.5f;
	if (fy < 0.0f)
		fy = 0.0f;
	if (fy > (float)(ctx->h - 1))
		fy = (float)(ctx->h - 1);
	const float flx = floorf(fx);
	const float ax  = fx - flx;
	int         x0  = (int)flx % ctx->w;
	if (x0 < 0)
		x0 += ctx->w;
	const int    x1  = (x0 + 1) % ctx->w;
	const int    y0  = (int)fy;
	const int    y1  = y0 + 1 < ctx->h ? y0 + 1 : y0;
	const float  ay  = fy - (float)y0;
	const float* p00 = ctx->rgb + ((size_t)y0 * ctx->w + x0) * 3u;
	const float* p10 = ctx->rgb + ((size_t)y0 * ctx->w + x1) * 3u;
	const float* p01 = ctx->rgb + ((size_t)y1 * ctx->w + x0) * 3u;
	const float* p11 = ctx->rgb + ((size_t)y1 * ctx->w + x1) * 3u;
	for (int c = 0; c < 3; ++c) {
		const float top    = p00[c] + (p10[c] - p00[c]) * ax;
		const float bottom = p01[c] + (p11[c] - p01[c]) * ax;
		out[c]             = top + (bottom - top) * ay;
	}
}

static void equirect_row(void* context, int face, int y) {
	EquirectContext* ctx = (EquirectContext*)context;
	const int        n   = ctx->cube->size;
	float*           row = ctx->cube->faces[0][face] + (size_t)y * n * 3u;
	const float      inv = 1.0f / (float)ctx->sub;
	for (int x = 0; x < n; ++x) {
		float sum[3] = { 0.0f, 0.0f, 0.0f };
		for (int sy = 0; sy < ctx->sub; ++sy) {
			for (int sx = 0; sx < ctx->sub; ++sx) {
				const float s = 2.0f * ((float)x + ((float)sx + 0.5f) * inv) / (float)n - 1.0f;
				const float t = 2.0f * ((float)y + ((float)sy + 0.5f) * inv) / (float)n - 1.0f;
				float       d[3], value[3];
				face_direction(face, s, t, d);
				normalize3(d);
				equirect_sample(ctx, d, value);
				sum[0] += value[0];
				sum[1] += value[1];
				sum[2] += value[2];
			}
		}
		const float weight = inv * inv;
		row[x * 3 + 0]     = sum[0] * weight;
		row[x * 3 + 1]     = sum[1] * weight;
		row[x * 3 + 2]     = sum[2] * weight;
	}
}

void envmap_from_equirect(EnvCube* cube, const float* rgb, int w, int h, int jobs) {
	/* A face-centre texel spans about w / (pi N) panorama texels per axis. */
	int sub = (int)ceil((double)w / (ENV_PI * (double)cube->size));
	if (sub < 2)
		sub = 2;
	if (sub > 8)
		sub = 8;
	EquirectContext ctx = { cube, rgb, w, h, sub };
	env_parallel_rows(cube->size, jobs, equirect_row, &ctx);
}

/* ---- diffuse irradiance ---------------------------------------------- */

typedef struct IrradianceContext {
	EnvCube*     out;
	const float* dirs;     /* source texel directions, xyz */
	const float* radiance; /* source radiance x solid angle, rgb */
	size_t       count;
} IrradianceContext;

static void irradiance_row(void* context, int face, int y) {
	IrradianceContext* ctx = (IrradianceContext*)context;
	const int          n   = ctx->out->size;
	float*             row = ctx->out->faces[0][face] + (size_t)y * n * 3u;
	for (int x = 0; x < n; ++x) {
		float normal[3];
		envmap_texel_direction(face, x, y, n, normal);
		double sum[3] = { 0.0, 0.0, 0.0 };
		for (size_t i = 0; i < ctx->count; ++i) {
			const float* d     = ctx->dirs + i * 3u;
			const float  cos_t = normal[0] * d[0] + normal[1] * d[1] + normal[2] * d[2];
			if (cos_t > 0.0f) {
				const float* l = ctx->radiance + i * 3u;
				sum[0] += (double)(l[0] * cos_t);
				sum[1] += (double)(l[1] * cos_t);
				sum[2] += (double)(l[2] * cos_t);
			}
		}
		row[x * 3 + 0] = (float)(sum[0] / ENV_PI);
		row[x * 3 + 1] = (float)(sum[1] / ENV_PI);
		row[x * 3 + 2] = (float)(sum[2] / ENV_PI);
	}
}

void envmap_irradiance(EnvCube* out, const EnvCube* src, int jobs) {
	int level = 0;
	while (level + 1 < src->level_count && envmap_level_size(src, level) > 64)
		++level;
	const int    n     = envmap_level_size(src, level);
	const size_t count = (size_t)6 * n * n;
	float*       dirs  = (float*)malloc(count * 3u * sizeof(float));
	float*       rad   = (float*)malloc(count * 3u * sizeof(float));
	if (!dirs || !rad) {
		free(dirs);
		free(rad);
		return;
	}
	size_t i = 0;
	for (int face = 0; face < 6; ++face) {
		const float* px = src->faces[level][face];
		for (int y = 0; y < n; ++y) {
			for (int x = 0; x < n; ++x, ++i) {
				const float omega = (float)envmap_texel_solid_angle(x, y, n);
				envmap_texel_direction(face, x, y, n, dirs + i * 3u);
				for (int c = 0; c < 3; ++c)
					rad[i * 3u + c] = px[((size_t)y * n + x) * 3u + c] * omega;
			}
		}
	}
	IrradianceContext ctx = { out, dirs, rad, count };
	env_parallel_rows(out->size, jobs, irradiance_row, &ctx);
	free(dirs);
	free(rad);
}

void envmap_axis_irradiance(const EnvCube* src, float out[7][3]) {
	double    sum[7][3] = { { 0.0 } };
	const int n         = src->size;
	for (int face = 0; face < 6; ++face) {
		const float* px = src->faces[0][face];
		for (int y = 0; y < n; ++y) {
			for (int x = 0; x < n; ++x) {
				float d[3];
				envmap_texel_direction(face, x, y, n, d);
				const double omega = envmap_texel_solid_angle(x, y, n);
				const float* l     = px + ((size_t)y * n + x) * 3u;
				for (int a = 0; a < 3; ++a) {
					const int    toward = d[a] >= 0.0f ? 2 * a : 2 * a + 1;
					const double cosine = fabs((double)d[a]) * omega;
					for (int c = 0; c < 3; ++c)
						sum[toward][c] += (double)l[c] * cosine;
				}
				for (int c = 0; c < 3; ++c)
					sum[6][c] += (double)l[c] * omega;
			}
		}
	}
	for (int c = 0; c < 3; ++c) {
		for (int f = 0; f < 6; ++f)
			out[f][c] = (float)(sum[f][c] / ENV_PI);
		out[6][c] = (float)(sum[6][c] / (4.0 * ENV_PI));
	}
}

/* ---- GGX prefiltering ------------------------------------------------ */

typedef struct GgxSample {
	float l[3];   /* tangent-space light direction, N = V = +Z */
	float weight; /* N.L */
	float lod;
} GgxSample;

typedef struct GgxContext {
	EnvCube*         out;
	const EnvCube*   src;
	int              level;
	float            base_lod; /* level 0: src lod matching out's size */
	const GgxSample* samples;
	int              sample_count;
} GgxContext;

static float radical_inverse(uint32_t bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return (float)bits * 2.3283064365386963e-10f;
}

/* Importance samples for one roughness. Returns the number kept (N.L > 0). */
static int ggx_build_samples(GgxSample* out, int sample_count, float roughness, int src_size,
							 int src_levels) {
	const float  alpha       = roughness * roughness;
	const float  a2          = alpha * alpha;
	const double texel_omega = 4.0 * ENV_PI / (6.0 * (double)src_size * (double)src_size);
	int          kept        = 0;
	for (int i = 0; i < sample_count; ++i) {
		const float u1        = ((float)i + 0.5f) / (float)sample_count;
		const float u2        = radical_inverse((uint32_t)i);
		const float phi       = (float)(2.0 * ENV_PI) * u2;
		const float cos_theta = sqrtf((1.0f - u1) / (1.0f + (a2 - 1.0f) * u1));
		const float sin_theta = sqrtf(1.0f - cos_theta * cos_theta);
		const float h[3]      = { sin_theta * cosf(phi), sin_theta * sinf(phi), cos_theta };
		/* L = 2 (V.H) H - V with V = N = +Z. */
		const float l[3] = { 2.0f * h[2] * h[0], 2.0f * h[2] * h[1], 2.0f * h[2] * h[2] - 1.0f };
		if (l[2] <= 0.0f)
			continue;
		/* pdf(L) = D(H) (N.H) / (4 V.H) = D / 4 when N = V. */
		const float  denom = cos_theta * cos_theta * (a2 - 1.0f) + 1.0f;
		const double d     = (double)a2 / (ENV_PI * (double)denom * (double)denom);
		const double pdf   = d / 4.0;
		const double omega = 1.0 / ((double)sample_count * pdf);
		double       lod   = 0.5 * log2(omega / texel_omega) + 1.0;
		if (lod < 0.0)
			lod = 0.0;
		if (lod > (double)(src_levels - 1))
			lod = (double)(src_levels - 1);
		out[kept].l[0]   = l[0];
		out[kept].l[1]   = l[1];
		out[kept].l[2]   = l[2];
		out[kept].weight = l[2];
		out[kept].lod    = (float)lod;
		++kept;
	}
	return kept;
}

static void ggx_row(void* context, int face, int y) {
	GgxContext* ctx = (GgxContext*)context;
	const int   n   = envmap_level_size(ctx->out, ctx->level);
	float*      row = ctx->out->faces[ctx->level][face] + (size_t)y * n * 3u;
	for (int x = 0; x < n; ++x) {
		float normal[3];
		envmap_texel_direction(face, x, y, n, normal);
		float* dst = row + x * 3;
		if (ctx->level == 0) {
			envmap_sample(ctx->src, normal, ctx->base_lod, dst);
			continue;
		}
		const float  up[3]  = { 0.0f, 0.0f, 1.0f };
		const float  alt[3] = { 1.0f, 0.0f, 0.0f };
		const float* ref    = fabsf(normal[2]) < 0.999f ? up : alt;
		float tangent[3] = { ref[1] * normal[2] - ref[2] * normal[1], ref[2] * normal[0] - ref[0] * normal[2],
							 ref[0] * normal[1] - ref[1] * normal[0] };
		normalize3(tangent);
		const float bitangent[3] = { normal[1] * tangent[2] - normal[2] * tangent[1],
									 normal[2] * tangent[0] - normal[0] * tangent[2],
									 normal[0] * tangent[1] - normal[1] * tangent[0] };
		double      sum[3]       = { 0.0, 0.0, 0.0 };
		double      total        = 0.0;
		for (int i = 0; i < ctx->sample_count; ++i) {
			const GgxSample* s = &ctx->samples[i];
			float            l[3];
			for (int c = 0; c < 3; ++c)
				l[c] = tangent[c] * s->l[0] + bitangent[c] * s->l[1] + normal[c] * s->l[2];
			float value[3];
			envmap_sample(ctx->src, l, s->lod, value);
			sum[0] += (double)(value[0] * s->weight);
			sum[1] += (double)(value[1] * s->weight);
			sum[2] += (double)(value[2] * s->weight);
			total += (double)s->weight;
		}
		const double inv = total > 0.0 ? 1.0 / total : 0.0;
		dst[0]           = (float)(sum[0] * inv);
		dst[1]           = (float)(sum[1] * inv);
		dst[2]           = (float)(sum[2] * inv);
	}
}

void envmap_ggx_prefilter(EnvCube* out, const EnvCube* src, int samples, int jobs) {
	GgxSample* buffer = (GgxSample*)malloc((size_t)samples * sizeof *buffer);
	if (!buffer)
		return;
	const int levels = out->level_count;
	for (int level = 0; level < levels; ++level) {
		GgxContext ctx = { out, src, level, 0.0f, buffer, 0 };
		if (level == 0) {
			ctx.base_lod = log2f((float)src->size / (float)out->size);
		} else {
			const float roughness = (float)level / (float)(levels - 1);
			ctx.sample_count = ggx_build_samples(buffer, samples, roughness, src->size, src->level_count);
		}
		env_parallel_rows(envmap_level_size(out, level), jobs, ggx_row, &ctx);
	}
	free(buffer);
}
