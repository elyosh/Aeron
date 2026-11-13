#include "lod_generate.h"

#include "aeron/scene/ktx2_reader.h"
#include "meshoptimizer.h"
#include "srgb8.h"

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

AeronLodGenerateOptions aeron_lod_default_options(void) {
	return (AeronLodGenerateOptions) {
		.relative_errors        = { 0.005f, 0.02f, 0.08f },
		.level_count            = AERON_LOD_MAX_GENERATED,
		.min_gain               = 0.25f,
		.min_triangles          = 300,
		.permissive             = true,
		.lock_component_borders = true,
		.prune                  = true,
		.coarse_from_level      = 2,
		.attribute_weight       = 1.0f,
	};
}

void aeron_lod_level_free(AeronLodLevel* level) {
	if (level) {
		free(level->indices);
		free(level->vertices);
		free(level->vertex_sources);
		memset(level, 0, sizeof *level);
	}
}

/* ===== Each primitive's average appearance ===== */

#define LOD_ATTRIBUTES 6 /* linear base colour, then emitted light mapped to x / (1 + x) */

/* One atlas page of a channel, decoded at a small mip to linear RGB. */
typedef struct DecodedPage {
	float* rgb;
	int    width, height;
} DecodedPage;

typedef struct DecodedChannel {
	Ktx2*        ktx2;
	int          level;
	bool         srgb;
	int          pages;
	DecodedPage* page; /* decoded on first use */
} DecodedChannel;

static bool channel_open(DecodedChannel* c, const AeronGltfChannelKtx2* source) {
	memset(c, 0, sizeof *c);
	if (!source->data || !source->size)
		return false;
	c->ktx2 = ktx2_open_mem(source->data, source->size, "lod appearance");
	if (!c->ktx2)
		return false;
	const AeronTextureFormat f = ktx2_format(c->ktx2);
	if (f != AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB && f != AERON_TEXTURE_FORMAT_BC7_RGBA_UNORM &&
		f != AERON_TEXTURE_FORMAT_RGBA8_SRGB && f != AERON_TEXTURE_FORMAT_RGBA8_UNORM) {
		ktx2_close(c->ktx2);
		c->ktx2 = NULL;
		return false;
	}
	c->srgb = f == AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB || f == AERON_TEXTURE_FORMAT_RGBA8_SRGB;
	/* The first level 256 texels wide or less: enough to tell a material's
	 * parts apart, small enough to decode quickly. */
	const int levels = ktx2_level_count(c->ktx2);
	while (c->level + 1 < levels && ktx2_level(c->ktx2, c->level).width > 256)
		c->level++;
	c->pages = ktx2_layer_count(c->ktx2) > 0 ? ktx2_layer_count(c->ktx2) : 1;
	c->page  = calloc((size_t)c->pages, sizeof *c->page);
	if (!c->page) {
		ktx2_close(c->ktx2);
		c->ktx2 = NULL;
		return false;
	}
	return true;
}

static void channel_close(DecodedChannel* c) {
	for (int i = 0; c->page && i < c->pages; i++)
		free(c->page[i].rgb);
	free(c->page);
	if (c->ktx2)
		ktx2_close(c->ktx2);
}

static const DecodedPage* channel_page(DecodedChannel* c, uint32_t page) {
	if (!c->ktx2 || (int)page >= c->pages)
		return NULL;
	DecodedPage* p = &c->page[page];
	if (p->rgb)
		return p;
	const Ktx2Level level = ktx2_level_face(c->ktx2, c->level, (int)page);
	const int       w = level.width, h = level.height;
	uint8_t*        rgba = malloc((size_t)((w + 3) & ~3) * (size_t)((h + 3) & ~3) * 4);
	p->rgb               = malloc(sizeof(float) * 3 * (size_t)w * (size_t)h);
	if (!rgba || !p->rgb || !level.data) {
		free(rgba);
		free(p->rgb);
		p->rgb = NULL;
		return NULL;
	}
	const AeronTextureFormat f     = ktx2_format(c->ktx2);
	const int                pitch = ((w + 3) & ~3) * 4;
	if (f == AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB || f == AERON_TEXTURE_FORMAT_BC7_RGBA_UNORM) {
		const int bw = (w + 3) / 4, bh = (h + 3) / 4;
		for (int by = 0; by < bh; by++)
			for (int bx = 0; bx < bw; bx++)
				bcdec_bc7(level.data + 16 * ((size_t)by * (size_t)bw + (size_t)bx),
						  rgba + (size_t)(4 * by) * (size_t)pitch + 16 * (size_t)bx, pitch);
	} else {
		for (int y = 0; y < h; y++)
			memcpy(rgba + (size_t)y * (size_t)pitch, level.data + (size_t)y * (size_t)w * 4, (size_t)w * 4);
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			for (int k = 0; k < 3; k++) {
				const uint8_t code = rgba[(size_t)y * (size_t)pitch + 4 * (size_t)x + (size_t)k];
				p->rgb[3 * ((size_t)y * (size_t)w + (size_t)x) + (size_t)k] =
					c->srgb ? srgb8_to_linear(code) : (float)code / 255.0f;
			}
	free(rgba);
	p->width  = w;
	p->height = h;
	return p;
}

/* The texel that material `m` shows at UV `uv` in `channel`, found the way
 * the shader samples it (atlas_sample: offset + frac(uv) * scale). False
 * when the material doesn't use the channel. */
static bool material_texel(DecodedChannel* c, const AeronGltfMaterial* m, int channel, const float uv[2],
						   float out[3]) {
	const float* x = m->uv_xform[channel];
	if (x[2] == 0.0f || x[3] == 0.0f)
		return false;
	const DecodedPage* p = channel_page(c, m->atlas_page[channel]);
	if (!p)
		return false;
	const float u  = x[0] + (uv[0] - floorf(uv[0])) * x[2];
	const float v  = x[1] + (uv[1] - floorf(uv[1])) * x[3];
	int         tx = (int)(u * (float)p->width), ty = (int)(v * (float)p->height);
	tx = tx < 0 ? 0 : tx >= p->width ? p->width - 1 : tx;
	ty = ty < 0 ? 0 : ty >= p->height ? p->height - 1 : ty;
	memcpy(out, &p->rgb[3 * ((size_t)ty * (size_t)p->width + (size_t)tx)], sizeof(float) * 3);
	return true;
}

/* Each primitive's average base colour and emitted light, LOD_ATTRIBUTES
 * floats per primitive, over its own material's triangles, weighted by
 * area. NULL when out of memory. */
static float* primitive_appearance(const AeronGltfModel* model) {
	const uint32_t prims = model->total_prim_count;
	float*         out   = calloc((size_t)(prims ? prims : 1) * LOD_ATTRIBUTES, sizeof(float));
	double*        sum   = calloc((size_t)(prims ? prims : 1) * (LOD_ATTRIBUTES + 1), sizeof(double));
	if (!out || !sum) {
		free(out);
		free(sum);
		return NULL;
	}
	DecodedChannel base, emissive;
	const bool     has_base     = channel_open(&base, &model->channels[AERON_GLTF_CHANNEL_BASE_COLOR]);
	const bool     has_emissive = channel_open(&emissive, &model->channels[AERON_GLTF_CHANNEL_EMISSIVE]);
	for (uint32_t t = 0; t + 2 < model->index_count; t += 3) {
		const AeronGltfVertex* v[3] = { &model->vertices[model->indices[t]],
										&model->vertices[model->indices[t + 1]],
										&model->vertices[model->indices[t + 2]] };
		const uint32_t         prim = v[0]->prim_id;
		if (prim >= prims || !model->prim_variant_material || !model->variant_columns)
			continue;
		const uint32_t material = model->prim_variant_material[(size_t)prim * model->variant_columns];
		if (material >= model->material_count)
			continue;
		const AeronGltfMaterial* m = &model->materials[material];
		float                    e1[3], e2[3];
		for (int k = 0; k < 3; k++) {
			e1[k] = v[1]->pos[k] - v[0]->pos[k];
			e2[k] = v[2]->pos[k] - v[0]->pos[k];
		}
		const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2],
					cz   = e1[0] * e2[1] - e1[1] * e2[0];
		const float area = 0.5f * sqrtf(cx * cx + cy * cy + cz * cz);
		if (!(area > 0.0f))
			continue;
		/* The three corners and the centre. */
		const float uvs[4][2] = { { v[0]->uv[0], v[0]->uv[1] },
								  { v[1]->uv[0], v[1]->uv[1] },
								  { v[2]->uv[0], v[2]->uv[1] },
								  { (v[0]->uv[0] + v[1]->uv[0] + v[2]->uv[0]) / 3.0f,
									(v[0]->uv[1] + v[1]->uv[1] + v[2]->uv[1]) / 3.0f } };
		double*     s         = &sum[(size_t)prim * (LOD_ATTRIBUTES + 1)];
		for (int k = 0; k < 4; k++) {
			float texel[3] = { 1.0f, 1.0f, 1.0f }, light[3] = { 1.0f, 1.0f, 1.0f };
			if (has_base)
				material_texel(&base, m, AERON_GLTF_CHANNEL_BASE_COLOR, uvs[k], texel);
			const bool lit =
				has_emissive && material_texel(&emissive, m, AERON_GLTF_CHANNEL_EMISSIVE, uvs[k], light);
			for (int c = 0; c < 3; c++) {
				s[c] += area * texel[c] * m->base_color_factor[c];
				const float e =
					m->emissive_factor[c] * m->emissive_strength *
					(lit || m->uv_xform[AERON_GLTF_CHANNEL_EMISSIVE][2] == 0.0f ? light[c] : 0.0f);
				s[3 + c] += area * e;
			}
			s[LOD_ATTRIBUTES] += area;
		}
	}
	for (uint32_t p = 0; p < prims; p++) {
		const double* s = &sum[(size_t)p * (LOD_ATTRIBUTES + 1)];
		if (s[LOD_ATTRIBUTES] <= 0.0)
			continue;
		for (int c = 0; c < 3; c++) {
			out[(size_t)p * LOD_ATTRIBUTES + (size_t)c]     = (float)(s[c] / s[LOD_ATTRIBUTES]);
			const float e                                   = (float)(s[3 + c] / s[LOD_ATTRIBUTES]);
			out[(size_t)p * LOD_ATTRIBUTES + 3 + (size_t)c] = e / (1.0f + e);
		}
	}
	if (has_base)
		channel_close(&base);
	if (has_emissive)
		channel_close(&emissive);
	free(sum);
	return out;
}

/* Work buffers, sized for the largest class of the model. */
typedef struct LodScratch {
	uint64_t*      order; /* (component << 32) | triangle, sorted */
	uint32_t*      local; /* a component's indices into its compact vertices */
	uint32_t*      simplified;
	float*         positions;      /* compact vertex positions */
	uint32_t*      source;         /* compact vertex -> model vertex */
	uint32_t*      compact;        /* model vertex -> compact vertex */
	uint32_t*      position_remap; /* compact vertex -> first compact vertex at its position */
	uint8_t*       border;         /* per first vertex: other materials share this position */
	unsigned char* lock;
	float*         attributes; /* compact vertex appearance, LOD_ATTRIBUTES each */
} LodScratch;

static void scratch_free(LodScratch* s) {
	free(s->order);
	free(s->local);
	free(s->simplified);
	free(s->positions);
	free(s->source);
	free(s->compact);
	free(s->position_remap);
	free(s->border);
	free(s->lock);
	free(s->attributes);
}

static bool scratch_alloc(LodScratch* s, uint32_t max_class_indices, uint32_t vertex_count) {
	const size_t n = max_class_indices ? max_class_indices : 1;
	memset(s, 0, sizeof *s);
	s->order          = malloc(sizeof *s->order * (n / 3 + 1));
	s->local          = malloc(sizeof *s->local * n);
	s->simplified     = malloc(sizeof *s->simplified * n);
	s->positions      = malloc(sizeof *s->positions * 3 * n);
	s->source         = malloc(sizeof *s->source * n);
	s->compact        = malloc(sizeof *s->compact * (vertex_count ? vertex_count : 1));
	s->position_remap = malloc(sizeof *s->position_remap * n);
	s->border         = malloc(n);
	s->lock           = malloc(n);
	s->attributes     = malloc(sizeof *s->attributes * LOD_ATTRIBUTES * n);
	if (!s->order || !s->local || !s->simplified || !s->positions || !s->source || !s->compact ||
		!s->position_remap || !s->border || !s->lock || !s->attributes) {
		scratch_free(s);
		return false;
	}
	return true;
}

/* The vertices a coarse level adds: copies of model vertices with another
 * primitive. A copy is reused when the same vertex needs the same primitive
 * again. */
typedef struct NewVertices {
	const AeronGltfModel* model;
	uint32_t              first; /* number of the first new vertex */
	AeronGltfVertex*      items;
	uint32_t*             sources; /* the model vertex each item copies */
	uint32_t              count, capacity;
	uint64_t*             keys; /* (vertex << 32 | primitive) + 1, 0 for empty */
	uint32_t*             values;
	uint32_t              slots; /* power of two */
} NewVertices;

static void new_vertices_free(NewVertices* n) {
	free(n->items);
	free(n->sources);
	free(n->keys);
	free(n->values);
}

static bool new_vertices_grow(NewVertices* n) {
	const uint32_t slots  = n->slots ? n->slots * 2 : 1024;
	uint64_t*      keys   = calloc(slots, sizeof *keys);
	uint32_t*      values = malloc(sizeof *values * slots);
	if (!keys || !values) {
		free(keys);
		free(values);
		return false;
	}
	for (uint32_t i = 0; i < n->slots; i++) {
		if (!n->keys[i])
			continue;
		uint32_t h = (uint32_t)(n->keys[i] * 0x9E3779B97F4A7C15ull >> 32) & (slots - 1);
		while (keys[h])
			h = (h + 1) & (slots - 1);
		keys[h]   = n->keys[i];
		values[h] = n->values[i];
	}
	free(n->keys);
	free(n->values);
	n->keys   = keys;
	n->values = values;
	n->slots  = slots;
	return true;
}

/* Returns the number of a vertex like model vertex `v`, but with primitive
 * `prim` and the UV of the nearest vertex of that primitive among
 * `candidates`, and adds it if it doesn't exist yet. UINT32_MAX when out of
 * memory. */
static uint32_t new_vertex(NewVertices* n, uint32_t v, uint32_t prim, const uint32_t* candidates,
						   uint32_t candidate_count) {
	if ((n->count + 1) * 2 > n->slots && !new_vertices_grow(n))
		return UINT32_MAX;
	const uint64_t key = ((uint64_t)v << 32 | prim) + 1;
	uint32_t       h   = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 32) & (n->slots - 1);
	while (n->keys[h]) {
		if (n->keys[h] == key)
			return n->values[h];
		h = (h + 1) & (n->slots - 1);
	}
	if (n->count == n->capacity) {
		const uint32_t   capacity = n->capacity ? n->capacity * 2 : 256;
		AeronGltfVertex* items    = realloc(n->items, sizeof *items * capacity);
		if (items)
			n->items = items;
		uint32_t* sources = realloc(n->sources, sizeof *sources * capacity);
		if (sources)
			n->sources = sources;
		if (!items || !sources)
			return UINT32_MAX;
		n->capacity = capacity;
	}
	const AeronGltfVertex* from    = &n->model->vertices[v];
	float                  best    = INFINITY;
	uint32_t               nearest = v;
	for (uint32_t i = 0; i < candidate_count; i++) {
		const AeronGltfVertex* c = &n->model->vertices[candidates[i]];
		if (c->prim_id != prim)
			continue;
		const float dx = c->pos[0] - from->pos[0], dy = c->pos[1] - from->pos[1],
					dz = c->pos[2] - from->pos[2];
		const float d  = dx * dx + dy * dy + dz * dz;
		if (d < best) {
			best    = d;
			nearest = candidates[i];
		}
	}
	AeronGltfVertex* out = &n->items[n->count];
	n->sources[n->count] = v;
	*out                 = *from;
	out->prim_id         = prim;
	out->uv[0]           = n->model->vertices[nearest].uv[0];
	out->uv[1]           = n->model->vertices[nearest].uv[1];
	n->keys[h]           = key;
	n->values[h]         = n->first + n->count;
	return n->first + n->count++;
}

static int compare_u64(const void* a, const void* b) {
	const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
	return x < y ? -1 : x > y;
}

/* Simplifies the triangles of one class, `indices[0..count)`, one component
 * at a time so that no triangle joins two mesh-table rows, and appends them
 * to `out`. Returns the largest error, in model units. */
static float simplify_class(const AeronGltfModel* model, const uint32_t* indices, uint32_t count,
							float target_error, unsigned options, LodScratch* s, uint32_t* out,
							uint32_t* out_count, AeronLodLevel* level, NewVertices* added,
							const float* appearance, float attribute_weight) {
	const uint32_t triangles = count / 3;
	for (uint32_t t = 0; t < triangles; t++) {
		const uint32_t component = (uint32_t)model->vertices[indices[3 * t]].component;
		s->order[t]              = (uint64_t)component << 32 | t;
	}
	qsort(s->order, triangles, sizeof *s->order, compare_u64);
	float max_error = 0.0f;
	for (uint32_t first = 0; first < triangles;) {
		uint32_t end = first + 1;
		while (end < triangles && s->order[end] >> 32 == s->order[first] >> 32)
			end++;
		/* The component's triangles on compact vertices. */
		uint32_t index_count = 0, vertex_count = 0;
		for (uint32_t k = first; k < end; k++)
			for (int corner = 0; corner < 3; corner++)
				s->compact[indices[3 * (uint32_t)s->order[k] + (uint32_t)corner]] = UINT32_MAX;
		for (uint32_t k = first; k < end; k++) {
			for (int corner = 0; corner < 3; corner++) {
				const uint32_t v = indices[3 * (uint32_t)s->order[k] + (uint32_t)corner];
				if (s->compact[v] == UINT32_MAX) {
					s->compact[v]           = vertex_count;
					s->source[vertex_count] = v;
					memcpy(&s->positions[3 * vertex_count], model->vertices[v].pos, sizeof(float) * 3);
					vertex_count++;
				}
				s->local[index_count++] = s->compact[v];
			}
		}
		/* Protect the vertices whose position other materials share, unless
		 * this is a coarse level. */
		meshopt_generatePositionRemap(s->position_remap, s->positions, vertex_count, sizeof(float) * 3);
		memset(s->border, 0, vertex_count);
		for (uint32_t v = 0; v < vertex_count; v++)
			if (model->vertices[s->source[v]].prim_id !=
				model->vertices[s->source[s->position_remap[v]]].prim_id)
				s->border[s->position_remap[v]] = 1;
		for (uint32_t v = 0; v < vertex_count; v++)
			s->lock[v] = !added && s->border[s->position_remap[v]] ? meshopt_SimplifyVertex_Protect : 0;

		/* On a coarse level, a collapse that changes a primitive's average
		 * appearance counts as error too. */
		const bool weigh = added && appearance && attribute_weight > 0.0f;
		float      weights[LOD_ATTRIBUTES];
		for (int k = 0; k < LOD_ATTRIBUTES; k++)
			weights[k] = attribute_weight;
		for (uint32_t v = 0; weigh && v < vertex_count; v++) {
			const uint32_t prim = model->vertices[s->source[v]].prim_id;
			if (prim < model->total_prim_count)
				memcpy(&s->attributes[(size_t)v * LOD_ATTRIBUTES], &appearance[(size_t)prim * LOD_ATTRIBUTES],
					   sizeof(float) * LOD_ATTRIBUTES);
			else
				memset(&s->attributes[(size_t)v * LOD_ATTRIBUTES], 0, sizeof(float) * LOD_ATTRIBUTES);
		}
		float        error = 0.0f;
		const size_t kept  = meshopt_simplifyWithAttributes(
			s->simplified, s->local, index_count, s->positions, vertex_count, sizeof(float) * 3,
			weigh ? s->attributes : NULL, weigh ? sizeof(float) * LOD_ATTRIBUTES : 0, weigh ? weights : NULL,
			weigh ? LOD_ATTRIBUTES : 0, s->lock, 0, target_error, options, &error);
		max_error = error > max_error ? error : max_error;
		for (size_t k = 0; k + 2 < kept; k += 3) {
			const uint32_t a         = s->source[s->simplified[k]];
			const uint32_t b         = s->source[s->simplified[k + 1]];
			const uint32_t c         = s->source[s->simplified[k + 2]];
			uint32_t       corner[3] = { a, b, c };
			const uint32_t pa = model->vertices[a].prim_id, pb = model->vertices[b].prim_id,
						   pc = model->vertices[c].prim_id;
			if (pa != pb || pa != pc) {
				if (!added) {
					level->dropped_triangles++;
					continue;
				}
				/* The material of most corners, else the first corner's. */
				const uint32_t prim = pa == pb || pa == pc ? pa : pb == pc ? pb : pa;
				for (int k3 = 0; k3 < 3; k3++) {
					if (model->vertices[corner[k3]].prim_id != prim) {
						corner[k3] = new_vertex(added, corner[k3], prim, s->source, vertex_count);
						if (corner[k3] == UINT32_MAX)
							return -1.0f;
					}
				}
			}
			out[(*out_count)++] = corner[0];
			out[(*out_count)++] = corner[1];
			out[(*out_count)++] = corner[2];
		}
		first = end;
	}
	return max_error;
}

int aeron_lod_generate(const AeronGltfModel* model, const AeronLodGenerateOptions* options,
					   AeronLodLevel levels[AERON_LOD_MAX_GENERATED]) {
	memset(levels, 0, sizeof(AeronLodLevel) * AERON_LOD_MAX_GENERATED);
	if (!model || !options || model->index_count / 3 < options->min_triangles || model->vertex_count == 0)
		return 0;

	float lo[3] = { INFINITY, INFINITY, INFINITY }, hi[3] = { -INFINITY, -INFINITY, -INFINITY };
	for (uint32_t v = 0; v < model->vertex_count; v++)
		for (int a = 0; a < 3; a++) {
			lo[a] = fminf(lo[a], model->vertices[v].pos[a]);
			hi[a] = fmaxf(hi[a], model->vertices[v].pos[a]);
		}
	const float radius = 0.5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
									  (hi[2] - lo[2]) * (hi[2] - lo[2]));

	/* Each class's simplified part, then its screen part copied as it is. */
	const uint32_t class_offset[3] = { 0, model->mask_index_offset, model->blend_index_offset };
	const uint32_t class_count[3]  = { model->opaque_index_count, model->mask_index_count,
									   model->blend_index_count };
	const uint32_t class_screen[3] = { model->opaque_screen_index_count, model->mask_screen_index_count,
									   model->blend_screen_index_count };
	uint32_t       max_class       = 0;
	for (int c = 0; c < 3; c++)
		max_class = class_count[c] > max_class ? class_count[c] : max_class;
	LodScratch scratch;
	if (!scratch_alloc(&scratch, max_class, model->vertex_count))
		return -1;

	/* Each primitive's appearance, for the coarse levels. */
	float* appearance = NULL;
	if (options->attribute_weight > 0.0f && options->coarse_from_level < options->level_count) {
		appearance = primitive_appearance(model);
		if (!appearance) {
			scratch_free(&scratch);
			return -1;
		}
	}
	const float    attribute_weight = options->attribute_weight * radius;
	const unsigned flags            = meshopt_SimplifyErrorAbsolute |
									  (options->permissive ? meshopt_SimplifyPermissive : 0u) |
									  (options->lock_component_borders ? meshopt_SimplifyLockBorder : 0u) |
									  (options->prune ? meshopt_SimplifyPrune : 0u);
	uint32_t       previous         = model->index_count;
	uint32_t       next_new         = model->vertex_count;
	int            kept             = 0;
	for (int level = 0; level < options->level_count && level < AERON_LOD_MAX_GENERATED; level++) {
		AeronLodLevel out = { .indices =
								  malloc(sizeof(uint32_t) * (model->index_count ? model->index_count : 1)) };
		if (!out.indices) {
			for (int k = 0; k < kept; k++)
				aeron_lod_level_free(&levels[k]);
			scratch_free(&scratch);
			free(appearance);
			return -1;
		}
		const float    target      = options->relative_errors[level] * radius;
		const bool     coarse      = level >= options->coarse_from_level;
		NewVertices    added       = { .model = model, .first = next_new };
		const unsigned level_flags = coarse ? flags & ~(unsigned)meshopt_SimplifyLockBorder : flags;
		bool           failed      = false;
		for (int c = 0; c < 3; c++) {
			const uint32_t start = out.index_count;
			const uint32_t body  = class_count[c] - class_screen[c];
			const float    error = simplify_class(model, model->indices + class_offset[c], body, target,
												  level_flags, &scratch, out.indices, &out.index_count, &out,
												  coarse ? &added : NULL, appearance, attribute_weight);
			failed               = failed || error < 0.0f;
			out.error            = error > out.error ? error : out.error;
			memcpy(out.indices + out.index_count, model->indices + class_offset[c] + body,
				   sizeof(uint32_t) * class_screen[c]);
			out.index_count += class_screen[c];
			out.ranges[2 * c]     = out.index_count - start;
			out.ranges[2 * c + 1] = class_screen[c];
		}
		out.vertices       = added.items;
		out.vertex_sources = added.sources;
		out.vertex_count   = added.count;
		added.items        = NULL;
		added.sources      = NULL;
		new_vertices_free(&added);
		if (failed) {
			aeron_lod_level_free(&out);
			for (int k = 0; k < kept; k++)
				aeron_lod_level_free(&levels[k]);
			scratch_free(&scratch);
			free(appearance);
			return -1;
		}
		if (out.index_count == 0 || (float)out.index_count > (1.0f - options->min_gain) * (float)previous) {
			aeron_lod_level_free(&out);
			continue;
		}
		previous = out.index_count;
		next_new += out.vertex_count;
		levels[kept++] = out;
	}
	scratch_free(&scratch);
	free(appearance);
	return kept;
}
