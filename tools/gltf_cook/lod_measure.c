/* aeron_lod_measure: compares generated levels with level 0, for
 * aeron_lod_report (lod_generate.h). */
#include "lod_generate.h"
#include "lod_texels.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Image size of the hole views, and the number of steps across a triangle's
 * side when sampling its emitted light. */
#define HOLE_VIEW_SIZE 256
#define LIGHT_STEPS 8

static float triangle_area(const float* a, const float* b, const float* c) {
	const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	const float x = e1[1] * e2[2] - e1[2] * e2[1], y = e1[2] * e2[0] - e1[0] * e2[2],
				z = e1[0] * e2[1] - e1[1] * e2[0];
	return 0.5f * sqrtf(x * x + y * y + z * z);
}

/* The light of a triangle set: each triangle's area times its emitted
 * luminance, averaged over a grid of points inside it. */
static double emitted_light(const AeronGltfModel* model, LodChannel* emissive,
							const AeronGltfVertex* vertices, const uint32_t* indices, uint32_t count) {
	double light = 0.0;
	for (uint32_t t = 0; t + 2 < count; t += 3) {
		const AeronGltfVertex*   v[3] = { &vertices[indices[t]], &vertices[indices[t + 1]],
										  &vertices[indices[t + 2]] };
		const AeronGltfMaterial* m    = lod_primitive_material(model, v[0]->prim_id);
		if (!m)
			continue;
		float sum     = 0.0f;
		int   samples = 0;
		/* The centres of the LIGHT_STEPS² small triangles of the grid. */
		for (int i = 0; i < LIGHT_STEPS; i++)
			for (int j = 0; i + j < LIGHT_STEPS; j++)
				for (int flipped = 0; flipped < 2; flipped++) {
					if (flipped && i + j + 1 >= LIGHT_STEPS)
						continue;
					const float shift = flipped ? 2.0f / 3.0f : 1.0f / 3.0f;
					const float b1 = (i + shift) / LIGHT_STEPS, b2 = (j + shift) / LIGHT_STEPS,
								b0    = 1.0f - b1 - b2;
					const float uv[2] = { b0 * v[0]->uv[0] + b1 * v[1]->uv[0] + b2 * v[2]->uv[0],
										  b0 * v[0]->uv[1] + b1 * v[1]->uv[1] + b2 * v[2]->uv[1] };
					sum += lod_emitted_luminance(emissive, m, uv);
					samples++;
				}
		light += (double)triangle_area(v[0]->pos, v[1]->pos, v[2]->pos) * sum / samples;
	}
	return light;
}

typedef struct View {
	float centre[3], x[3], y[3], scale;
} View;

/* Marks the pixels whose centres a triangle set covers. */
static void cover(uint8_t* image, const View* view, const AeronGltfVertex* vertices, const uint32_t* indices,
				  uint32_t count) {
	const int size = HOLE_VIEW_SIZE;
	for (uint32_t t = 0; t + 2 < count; t += 3) {
		float px[3], py[3];
		for (int k = 0; k < 3; k++) {
			const float* p    = vertices[indices[t + (uint32_t)k]].pos;
			const float  d[3] = { p[0] - view->centre[0], p[1] - view->centre[1], p[2] - view->centre[2] };
			px[k] = (d[0] * view->x[0] + d[1] * view->x[1] + d[2] * view->x[2]) * view->scale + size * 0.5f;
			py[k] = (d[0] * view->y[0] + d[1] * view->y[1] + d[2] * view->y[2]) * view->scale + size * 0.5f;
		}
		const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
		if (area == 0.0f)
			continue;
		const float sign = area < 0.0f ? -1.0f : 1.0f;
		int         x0   = (int)floorf(fminf(px[0], fminf(px[1], px[2])));
		int         x1   = (int)ceilf(fmaxf(px[0], fmaxf(px[1], px[2])));
		int         y0   = (int)floorf(fminf(py[0], fminf(py[1], py[2])));
		int         y1   = (int)ceilf(fmaxf(py[0], fmaxf(py[1], py[2])));
		x0               = x0 < 0 ? 0 : x0;
		y0               = y0 < 0 ? 0 : y0;
		x1               = x1 > size - 1 ? size - 1 : x1;
		y1               = y1 > size - 1 ? size - 1 : y1;
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++) {
				const float sx = (float)x + 0.5f, sy = (float)y + 0.5f;
				const float w0 = sign * ((px[1] - sx) * (py[2] - sy) - (px[2] - sx) * (py[1] - sy));
				const float w1 = sign * ((px[2] - sx) * (py[0] - sy) - (px[0] - sx) * (py[2] - sy));
				const float w2 = sign * ((px[0] - sx) * (py[1] - sy) - (px[1] - sx) * (py[0] - sy));
				if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
					image[y * size + x] = 1;
			}
	}
}

/* The share of level 0's covered pixels, at least `margin` model units
 * inside its outline, that the level leaves open, over all views. */
static float holes(const AeronGltfModel* model, const AeronGltfVertex* vertices, const AeronLodLevel* level,
				   const float* centre, float radius, uint8_t* full, uint8_t* simple) {
	static const float directions[13][3] = { { 1, 0, 0 },  { 0, 1, 0 }, { 0, 0, 1 },  { 1, 1, 0 },
											 { 1, -1, 0 }, { 1, 0, 1 }, { 1, 0, -1 }, { 0, 1, 1 },
											 { 0, 1, -1 }, { 1, 1, 1 }, { 1, 1, -1 }, { 1, -1, 1 },
											 { -1, 1, 1 } };
	const int          size              = HOLE_VIEW_SIZE;
	View               view              = { .scale = (float)size / (2.2f * radius) };
	memcpy(view.centre, centre, sizeof view.centre);
	const int margin = (int)ceilf(level->error * view.scale) + 1;
	double    inside = 0.0, open = 0.0;
	for (int d = 0; d < 13; d++) {
		float       z[3]   = { directions[d][0], directions[d][1], directions[d][2] };
		const float length = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
		for (int k = 0; k < 3; k++)
			z[k] /= length;
		const float up[3] = { 0.0f, fabsf(z[2]) > 0.9f ? 1.0f : 0.0f, fabsf(z[2]) > 0.9f ? 0.0f : 1.0f };
		view.x[0]         = up[1] * z[2] - up[2] * z[1];
		view.x[1]         = up[2] * z[0] - up[0] * z[2];
		view.x[2]         = up[0] * z[1] - up[1] * z[0];
		const float lx    = sqrtf(view.x[0] * view.x[0] + view.x[1] * view.x[1] + view.x[2] * view.x[2]);
		for (int k = 0; k < 3; k++)
			view.x[k] /= lx;
		view.y[0] = z[1] * view.x[2] - z[2] * view.x[1];
		view.y[1] = z[2] * view.x[0] - z[0] * view.x[2];
		view.y[2] = z[0] * view.x[1] - z[1] * view.x[0];
		memset(full, 0, (size_t)size * size);
		memset(simple, 0, (size_t)size * size);
		cover(full, &view, vertices, model->indices, model->index_count);
		cover(simple, &view, vertices, level->indices, level->index_count);
		for (int y = margin; y < size - margin; y++)
			for (int x = margin; x < size - margin; x++) {
				bool deep = true;
				for (int dy = -margin; dy <= margin && deep; dy++)
					for (int dx = -margin; dx <= margin && deep; dx++)
						deep = full[(y + dy) * size + x + dx] != 0;
				if (!deep)
					continue;
				inside += 1.0;
				open += simple[y * size + x] ? 0.0 : 1.0;
			}
	}
	return inside > 0.0 ? (float)(open / inside) : 0.0f;
}

bool aeron_lod_measure(const AeronGltfModel* model, const AeronLodLevel* levels, int count,
					   AeronLodMeasure* out) {
	uint32_t total = model->vertex_count;
	for (int k = 0; k < count; k++)
		total += levels[k].vertex_count;
	AeronGltfVertex* vertices = malloc(sizeof *vertices * (total ? total : 1));
	uint8_t*         full     = malloc((size_t)HOLE_VIEW_SIZE * HOLE_VIEW_SIZE);
	uint8_t*         simple   = malloc((size_t)HOLE_VIEW_SIZE * HOLE_VIEW_SIZE);
	if (!vertices || !full || !simple) {
		free(vertices);
		free(full);
		free(simple);
		return false;
	}
	/* Level 0's vertices, then those each level adds, as the loader puts
	 * them. */
	memcpy(vertices, model->vertices, sizeof *vertices * model->vertex_count);
	uint32_t next = model->vertex_count;
	for (int k = 0; k < count; k++) {
		memcpy(vertices + next, levels[k].vertices, sizeof *vertices * levels[k].vertex_count);
		next += levels[k].vertex_count;
	}
	float lo[3] = { INFINITY, INFINITY, INFINITY }, hi[3] = { -INFINITY, -INFINITY, -INFINITY };
	for (uint32_t v = 0; v < model->vertex_count; v++)
		for (int a = 0; a < 3; a++) {
			lo[a] = fminf(lo[a], model->vertices[v].pos[a]);
			hi[a] = fmaxf(hi[a], model->vertices[v].pos[a]);
		}
	const float centre[3] = { 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2]) };
	const float radius = 0.5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
									  (hi[2] - lo[2]) * (hi[2] - lo[2]));
	LodChannel  emissive;
	LodChannel* channel =
		lod_channel_open(&emissive, &model->channels[AERON_GLTF_CHANNEL_EMISSIVE]) ? &emissive : NULL;
	const double full_light = emitted_light(model, channel, vertices, model->indices, model->index_count);
	for (int k = 0; k < count; k++) {
		out[k].light =
			full_light > 0.0
				? (float)(emitted_light(model, channel, vertices, levels[k].indices, levels[k].index_count) /
						  full_light)
				: -1.0f;
		out[k].holes =
			radius > 0.0f ? holes(model, vertices, &levels[k], centre, radius, full, simple) : 0.0f;
	}
	if (channel)
		lod_channel_close(channel);
	free(vertices);
	free(full);
	free(simple);
	return true;
}
