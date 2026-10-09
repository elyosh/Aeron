#include "channel_atlas.h"

#include "aeron/atlas_pack.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void channel_atlas_init(ChannelAtlas* ca, int channel) {
	if (!ca)
		return;
	memset(ca, 0, sizeof *ca);
	ca->channel = channel;
}

void channel_atlas_free(ChannelAtlas* ca) {
	if (!ca)
		return;
	for (int i = 0; i < ca->rect_count; i++)
		free(ca->rects[i].owned);
	free(ca->rects);
	free(ca->rgba);
	memset(ca, 0, sizeof *ca);
}

/* Taps of output sample `i` when resampling `s` samples to `d` with REPEAT
 * addressing: the covered source area when shrinking, linear interpolation
 * otherwise. `index` and `weight` hold at least s / d + 2 entries. */
static int resample_taps(int s, int d, int i, int* index, float* weight) {
	const double ratio = (double)s / (double)d;
	if (d < s) {
		const double a = i * ratio, b = (i + 1) * ratio;
		int          n = 0;
		for (int k = (int)a; k < s && k < b; k++) {
			const double w = (b < k + 1 ? b : k + 1) - (a > k ? a : k);
			if (w > 0.0) {
				index[n]    = k;
				weight[n++] = (float)(w / ratio);
			}
		}
		return n;
	}
	const double c = (i + 0.5) * ratio - 0.5;
	const double f = floor(c);
	index[0]       = ((int)f % s + s) % s;
	index[1]       = (index[0] + 1) % s;
	weight[1]      = (float)(c - f);
	weight[0]      = 1.0f - weight[1];
	return 2;
}

/* Separable RGBA8 resample with REPEAT addressing, so a resampled texture
 * still tiles. Returns a fresh buffer, or NULL on OOM. */
static uint8_t* resample_rgba8(const uint8_t* src, int sw, int sh, int dw, int dh) {
	const int x_taps = sw / dw + 2, y_taps = sh / dh + 2;
	int*      x_count  = (int*)malloc((size_t)dw * sizeof *x_count);
	int*      x_index  = (int*)malloc((size_t)dw * (size_t)x_taps * sizeof *x_index);
	float*    x_weight = (float*)malloc((size_t)dw * (size_t)x_taps * sizeof *x_weight);
	int*      y_index  = (int*)malloc((size_t)y_taps * sizeof *y_index);
	float*    y_weight = (float*)malloc((size_t)y_taps * sizeof *y_weight);
	float*    row      = (float*)malloc((size_t)dw * 4u * sizeof *row);
	uint8_t*  out      = (uint8_t*)malloc((size_t)dw * (size_t)dh * 4u);
	if (!x_count || !x_index || !x_weight || !y_index || !y_weight || !row || !out) {
		free(out);
		out = NULL;
		goto done;
	}
	for (int x = 0; x < dw; x++)
		x_count[x] = resample_taps(sw, dw, x, x_index + (size_t)x * x_taps, x_weight + (size_t)x * x_taps);
	for (int y = 0; y < dh; y++) {
		memset(row, 0, (size_t)dw * 4u * sizeof *row);
		const int y_count = resample_taps(sh, dh, y, y_index, y_weight);
		for (int t = 0; t < y_count; t++) {
			const uint8_t* line = src + (size_t)y_index[t] * (size_t)sw * 4u;
			for (int x = 0; x < dw; x++) {
				const int*   index  = x_index + (size_t)x * x_taps;
				const float* weight = x_weight + (size_t)x * x_taps;
				for (int k = 0; k < x_count[x]; k++) {
					const float    w = y_weight[t] * weight[k];
					const uint8_t* p = line + (size_t)index[k] * 4u;
					for (int c = 0; c < 4; c++)
						row[x * 4 + c] += w * p[c];
				}
			}
		}
		uint8_t* dst = out + (size_t)y * (size_t)dw * 4u;
		for (int i = 0; i < dw * 4; i++) {
			const float v = row[i] + 0.5f;
			dst[i]        = (uint8_t)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v);
		}
	}
done:
	free(x_count);
	free(x_index);
	free(x_weight);
	free(y_index);
	free(y_weight);
	free(row);
	return out;
}

int channel_atlas_add_rect(ChannelAtlas* ca, const void* key, int src_w, int src_h, const uint8_t* src_rgba,
						   int max_texture) {
	if (!ca || src_w <= 0 || src_h <= 0 || !src_rgba)
		return -1;
	if (ca->rect_count == ca->rect_capacity) {
		int          cap   = ca->rect_capacity ? ca->rect_capacity * 2 : 8;
		ChannelRect* grown = (ChannelRect*)realloc(ca->rects, (size_t)cap * sizeof *grown);
		if (!grown)
			return -1;
		ca->rects         = grown;
		ca->rect_capacity = cap;
	}
	uint8_t* owned = NULL;
	if (max_texture > 0 && (src_w > max_texture || src_h > max_texture)) {
		const double scale = (double)max_texture / (double)(src_w > src_h ? src_w : src_h);
		const int    w = (int)(src_w * scale + 0.5), h = (int)(src_h * scale + 0.5);
		owned = resample_rgba8(src_rgba, src_w, src_h, w > 0 ? w : 1, h > 0 ? h : 1);
		if (!owned)
			return -1;
		src_w    = w > 0 ? w : 1;
		src_h    = h > 0 ? h : 1;
		src_rgba = owned;
	}
	ChannelRect* r = &ca->rects[ca->rect_count];
	memset(r, 0, sizeof *r);
	r->key      = key;
	r->src_w    = src_w;
	r->src_h    = src_h;
	r->src_rgba = src_rgba;
	r->owned    = owned;
	return ca->rect_count++;
}

static bool is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

static int floor_log2(int v) {
	int l = 0;
	while (v > 1) {
		v >>= 1;
		l++;
	}
	return l;
}

/* Footprint of a source extent, in gutter-sized cells: the extent plus a
 * gutter on both sides, rounded up to the gutter. */
static int footprint_cells(int extent, int gutter) { return (extent + 3 * gutter - 1) / gutter; }

/* Sort rect indices by max(w,h) desc, tiebreak by area desc — matches the
 * AnimImage adapter's pre-sort. The skyline packer is sensitive to
 * input order; this gives a deterministic, tight pack. */
static void sort_rects_largest_first(const ChannelRect* rects, int* order, int n) {
	for (int i = 1; i < n; i++) {
		const int          key_index = order[i];
		const ChannelRect* key       = &rects[key_index];
		int                kmax      = key->src_w > key->src_h ? key->src_w : key->src_h;
		long               karea     = (long)key->src_w * (long)key->src_h;
		int                j         = i - 1;
		while (j >= 0) {
			const ChannelRect* r          = &rects[order[j]];
			int                jmax       = r->src_w > r->src_h ? r->src_w : r->src_h;
			long               jarea      = (long)r->src_w * (long)r->src_h;
			bool               key_bigger = (kmax > jmax) || (kmax == jmax && karea > jarea);
			if (!key_bigger)
				break;
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = key_index;
	}
}

/* Skyline-pack `count` rects of `order` on the gutter-aligned grid, at a
 * width of `grid_width` cells. Returns the height in cells, or -1. */
static int pack_cells(const ChannelAtlas* ca, const int* order, int count, AeronAtlasRect* cells,
					  int grid_width) {
	for (int i = 0; i < count; i++) {
		const ChannelRect* r = &ca->rects[order[i]];
		cells[i].w           = footprint_cells(r->src_w, ca->gutter);
		cells[i].h           = footprint_cells(r->src_h, ca->gutter);
		cells[i].x = cells[i].y = 0;
		cells[i].key            = (uint32_t)order[i];
	}
	return Aeron_AtlasPackRects(cells, count, grid_width, 0);
}

static void place_rect(ChannelAtlas* ca, const AeronAtlasRect* cell, int page) {
	ChannelRect* r = &ca->rects[cell->key];
	r->page        = page;
	r->x           = cell->x * ca->gutter + ca->gutter;
	r->y           = cell->y * ca->gutter + ca->gutter;
}

/* First-fit pages from page `first`: pack the `count` rects of `order` at
 * the full page width and move the rects that do not fit to the next page.
 * The first rect always lands at the bottom of an empty page. Returns the
 * page count after them, or -1 when a rect fits no page; `order` is
 * reused as scratch. */
static int first_fit_pages(ChannelAtlas* ca, int* order, int count, AeronAtlasRect* cells, int grid_w,
						   int grid_h, int first) {
	int pages = first, remaining = count;
	while (remaining) {
		if (pack_cells(ca, order, remaining, cells, grid_w) < 0)
			return -1;
		int kept = 0;
		for (int i = 0; i < remaining; i++) {
			if (cells[i].y + cells[i].h <= grid_h)
				place_rect(ca, &cells[i], pages);
			else
				order[kept++] = (int)cells[i].key;
		}
		if (kept == remaining)
			return -1;
		remaining = kept;
		pages++;
	}
	return pages;
}

bool channel_atlas_pack(ChannelAtlas* ca, int page_size, int gutter) {
	if (!ca || !is_pow2(page_size) || page_size < 128 || !is_pow2(gutter) || gutter < 4 || gutter > 32 ||
		page_size < 8 * gutter)
		return false;
	ca->gutter = gutter;
	if (ca->rect_count == 0) {
		/* Empty channel — still allocate a 4×4 placeholder page so the
		 * texture binding has somewhere to point. */
		ca->width      = 4; /* BC7 4×4 block minimum */
		ca->height     = 4;
		ca->page_count = 1;
		ca->mip_count  = 0;
		return true;
	}

	int largest = 0;
	for (int i = 0; i < ca->rect_count; i++) {
		const ChannelRect* r = &ca->rects[i];
		if (r->src_w > largest)
			largest = r->src_w;
		if (r->src_h > largest)
			largest = r->src_h;
	}
	int page = page_size;
	while (page < largest)
		page *= 2;

	int*            order = (int*)malloc((size_t)ca->rect_count * sizeof *order);
	AeronAtlasRect* cells = (AeronAtlasRect*)calloc((size_t)ca->rect_count, sizeof *cells);
	if (!order || !cells) {
		free(order);
		free(cells);
		return false;
	}

	/* Sources over half a page get a dedicated layer each, ahead of the
	 * shared pages. A shared source samples at most log2(gutter) levels:
	 * its footprint is gutter-aligned, so no texel of those levels mixes
	 * two footprints. */
	const int lod_limit = floor_log2(gutter);
	int       packed = 0, dedicated = 0, deepest = 0;
	for (int i = 0; i < ca->rect_count; i++) {
		ChannelRect* r    = &ca->rects[i];
		const int    side = r->src_w > r->src_h ? r->src_w : r->src_h;
		if (side > page / 2) {
			r->dedicated = true;
			r->page      = dedicated++;
			r->x = r->y = 0;
			r->max_lod  = CHANNEL_ATLAS_UNCLAMPED_LOD;
			deepest     = floor_log2(page);
		} else {
			const int short_side = r->src_w < r->src_h ? r->src_w : r->src_h;
			r->max_lod           = floor_log2(short_side) < lod_limit ? floor_log2(short_side) : lod_limit;
			if (r->max_lod > deepest)
				deepest = r->max_lod;
			order[packed++] = i;
		}
	}
	sort_rects_largest_first(ca->rects, order, packed);

	const bool fixed = ca->fixed_width > 0 && ca->fixed_height > 0;
	if (fixed && (ca->fixed_width % gutter || ca->fixed_height % gutter ||
				  (dedicated && (ca->fixed_width != page || ca->fixed_height != page)))) {
		fprintf(stderr, "[aeron_gltf_cook] channel %d: %d×%d pages do not fit its sources\n", ca->channel,
				ca->fixed_width, ca->fixed_height);
		free(order);
		free(cells);
		return false;
	}
	const int grid_w    = fixed ? ca->fixed_width / gutter : page / gutter;
	const int grid_h    = fixed ? ca->fixed_height / gutter : page / gutter;
	int       width     = fixed ? ca->fixed_width : page;
	int       height    = fixed ? ca->fixed_height : page;
	int       pages     = dedicated;
	int       remaining = packed;
	if (packed && !dedicated && !fixed) {
		const int grid = grid_w;
		/* A single shared page: the first power-of-two trial width that
		 * fits, trimmed to its occupied bounds. */
		int widest = 0;
		for (int i = 0; i < packed; i++) {
			const int w = footprint_cells(ca->rects[order[i]].src_w, gutter);
			if (w > widest)
				widest = w;
		}
		int trial = 1;
		while (trial < widest)
			trial *= 2;
		for (; trial <= grid && remaining; trial *= 2) {
			const int used_height = pack_cells(ca, order, packed, cells, trial);
			if (used_height < 0 || used_height > grid)
				continue;
			int used_width = 0;
			for (int i = 0; i < packed; i++) {
				place_rect(ca, &cells[i], 0);
				if (cells[i].x + cells[i].w > used_width)
					used_width = cells[i].x + cells[i].w;
			}
			width     = used_width * gutter;
			height    = used_height * gutter;
			pages     = 1;
			remaining = 0;
		}
	}
	if (remaining) {
		/* The gutters keep power-of-two sources from tiling power-of-two
		 * pages (two 1024-wide footprints need 2112 texels), so a multi-page
		 * atlas without dedicated layers also tries pages of P + P/16 and
		 * keeps the size that needs fewer texels. */
		const int  alternative     = page + page / 16;
		const bool try_alternative = !fixed && !dedicated && (page / 16) % gutter == 0;
		int*       sorted = try_alternative ? (int*)malloc((size_t)remaining * sizeof *sorted) : NULL;
		if (sorted)
			memcpy(sorted, order, (size_t)remaining * sizeof *sorted);
		const int count = remaining;
		pages           = first_fit_pages(ca, order, count, cells, grid_w, grid_h, pages);
		if (sorted && pages > 1) {
			const int alt_grid = alternative / gutter;
			memcpy(order, sorted, (size_t)count * sizeof *order);
			const int alt_pages = first_fit_pages(ca, order, count, cells, alt_grid, alt_grid, 0);
			if (alt_pages > 0 &&
				(double)alt_pages * alternative * alternative < (double)pages * page * page) {
				pages  = alt_pages;
				width  = alternative;
				height = alternative;
			} else {
				memcpy(order, sorted, (size_t)count * sizeof *order);
				pages = first_fit_pages(ca, order, count, cells, grid_w, grid_h, 0);
			}
		}
		free(sorted);
		remaining = pages < 0 ? count : 0;
	}
	free(order);
	free(cells);
	if (remaining) {
		fprintf(stderr, "[aeron_gltf_cook] channel %d: cannot pack %d sub-rects into %d×%d pages\n",
				ca->channel, remaining, width, height);
		return false;
	}
	ca->width      = width;
	ca->height     = height;
	ca->page       = page;
	ca->page_count = pages;
	ca->mip_count  = deepest + 1;
	return true;
}

/* Fill a rect's whole footprint by REPEAT around the source. */
static void fill_footprint(uint8_t* page, int page_width, const ChannelRect* r, int gutter) {
	const int fw = footprint_cells(r->src_w, gutter) * gutter;
	const int fh = footprint_cells(r->src_h, gutter) * gutter;
	for (int fy = 0; fy < fh; fy++) {
		const int      sy  = ((fy - gutter) % r->src_h + r->src_h) % r->src_h;
		const uint8_t* src = r->src_rgba + (size_t)sy * (size_t)r->src_w * 4u;
		uint8_t*       dst =
			page + ((size_t)(r->y - gutter + fy) * (size_t)page_width + (size_t)(r->x - gutter)) * 4u;
		for (int fx = 0; fx < fw; fx++) {
			const int sx = ((fx - gutter) % r->src_w + r->src_w) % r->src_w;
			memcpy(dst + (size_t)fx * 4u, src + (size_t)sx * 4u, 4u);
		}
	}
}

bool channel_atlas_materialize(ChannelAtlas* ca) {
	if (!ca || ca->width <= 0 || ca->height <= 0 || ca->page_count <= 0)
		return false;
	const size_t page_bytes = (size_t)ca->width * (size_t)ca->height * 4u;
	ca->rgba                = (uint8_t*)calloc((size_t)ca->page_count, page_bytes);
	if (!ca->rgba)
		return false;

	for (int i = 0; i < ca->rect_count; i++) {
		const ChannelRect* r    = &ca->rects[i];
		uint8_t*           page = ca->rgba + (size_t)r->page * page_bytes;
		if (!r->dedicated) {
			fill_footprint(page, ca->width, r, ca->gutter);
		} else if (r->src_w == ca->width && r->src_h == ca->height) {
			memcpy(page, r->src_rgba, page_bytes);
		} else {
			uint8_t* scaled = resample_rgba8(r->src_rgba, r->src_w, r->src_h, ca->width, ca->height);
			if (!scaled) {
				free(ca->rgba);
				ca->rgba = NULL;
				return false;
			}
			memcpy(page, scaled, page_bytes);
			free(scaled);
		}
	}
	return true;
}

void channel_atlas_rect_uv_transform(const ChannelAtlas* ca, const ChannelRect* r, float out_offset[2],
									 float out_scale[2]) {
	if (!ca || !r || ca->width <= 0 || ca->height <= 0) {
		if (out_offset)
			out_offset[0] = out_offset[1] = 0.0f;
		if (out_scale)
			out_scale[0] = out_scale[1] = 0.0f;
		return;
	}
	float aw = (float)ca->width;
	float ah = (float)ca->height;
	if (out_offset) {
		out_offset[0] = r->dedicated ? 0.0f : (float)r->x / aw;
		out_offset[1] = r->dedicated ? 0.0f : (float)r->y / ah;
	}
	if (out_scale) {
		out_scale[0] = r->dedicated ? 1.0f : (float)r->src_w / aw;
		out_scale[1] = r->dedicated ? 1.0f : (float)r->src_h / ah;
	}
}
