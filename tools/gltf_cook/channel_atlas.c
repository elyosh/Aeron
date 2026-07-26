#include "channel_atlas.h"

#include "aeron/atlas_pack.h"

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
	free(ca->rects);
	free(ca->rgba);
	memset(ca, 0, sizeof *ca);
}

bool channel_atlas_add_rect(ChannelAtlas* ca, uint32_t mat_idx, int src_w, int src_h,
							const uint8_t* src_rgba) {
	if (!ca || src_w <= 0 || src_h <= 0 || !src_rgba)
		return false;
	if (ca->rect_count == ca->rect_capacity) {
		int          cap   = ca->rect_capacity ? ca->rect_capacity * 2 : 8;
		ChannelRect* grown = (ChannelRect*)realloc(ca->rects, (size_t)cap * sizeof *grown);
		if (!grown)
			return false;
		ca->rects         = grown;
		ca->rect_capacity = cap;
	}
	ChannelRect* r = &ca->rects[ca->rect_count++];
	r->mat_idx     = mat_idx;
	r->src_w       = src_w;
	r->src_h       = src_h;
	r->src_rgba    = src_rgba;
	r->x = r->y = 0;
	return true;
}

/* Round up to the next power of 2 (positive ints). */
static int next_pow2(int v) {
	int p = 1;
	while (p < v)
		p <<= 1;
	return p;
}

static int mip_count_for_pad(int pad) {
	int count = 1;
	while (pad > 1) {
		pad /= 2;
		count++;
	}
	return count;
}

/* Sort rects by max(w,h) desc, tiebreak by area desc — matches the
 * AnimImage adapter's pre-sort. The skyline packer is sensitive to
 * input order; this gives a deterministic, tight pack. */
static void sort_rects_largest_first(ChannelRect* r, int n) {
	for (int i = 1; i < n; i++) {
		ChannelRect key   = r[i];
		int         kmax  = key.src_w > key.src_h ? key.src_w : key.src_h;
		long        karea = (long)key.src_w * (long)key.src_h;
		int         j     = i - 1;
		while (j >= 0) {
			int  jmax       = r[j].src_w > r[j].src_h ? r[j].src_w : r[j].src_h;
			long jarea      = (long)r[j].src_w * (long)r[j].src_h;
			bool key_bigger = (kmax > jmax) || (kmax == jmax && karea > jarea);
			if (!key_bigger)
				break;
			r[j + 1] = r[j];
			j--;
		}
		r[j + 1] = key;
	}
}

bool channel_atlas_pack(ChannelAtlas* ca, int max_atlas_size, int pad) {
	if (!ca || max_atlas_size <= 0 || pad < 0)
		return false;
	if (ca->rect_count == 0) {
		/* Empty channel — still allocate a 1×1 placeholder atlas so the
		 * texture binding has somewhere to point. */
		ca->width     = 4; /* BC7 4×4 block minimum */
		ca->height    = 4;
		ca->mip_count = mip_count_for_pad(pad);
		ca->pad       = pad;
		return true;
	}

	/* Find the largest source rect; the atlas must fit it (plus pad).
	 * If even that exceeds max_atlas_size, fail loud — there's no
	 * point degrading silently. */
	int max_w = 0, max_h = 0;
	for (int i = 0; i < ca->rect_count; i++) {
		if (ca->rects[i].src_w > max_w)
			max_w = ca->rects[i].src_w;
		if (ca->rects[i].src_h > max_h)
			max_h = ca->rects[i].src_h;
	}
	int min_dim = (max_w > max_h ? max_w : max_h) + 2 * pad;
	if (min_dim > max_atlas_size) {
		fprintf(stderr,
				"[aeron_gltf_cook] channel %d: source rect %dx%d larger than "
				"max atlas %d (+%d pad)\n",
				ca->channel, max_w, max_h, max_atlas_size, pad);
		return false;
	}

	/* Sort once; the skyline packer is destructive to position order,
	 * so we work on a temp copy per width trial. */
	sort_rects_largest_first(ca->rects, ca->rect_count);

	AeronAtlasRect* trial = (AeronAtlasRect*)calloc((size_t)ca->rect_count, sizeof *trial);
	if (!trial)
		return false;

	int             best_w = 0, best_h = 0;
	long            best_area = 0;
	AeronAtlasRect* best      = (AeronAtlasRect*)calloc((size_t)ca->rect_count, sizeof *best);
	if (!best) {
		free(trial);
		return false;
	}

	int try_w = next_pow2(min_dim);
	if (try_w < 4)
		try_w = 4;
	for (; try_w <= max_atlas_size; try_w *= 2) {
		for (int i = 0; i < ca->rect_count; i++) {
			/* Pack the complete padded footprint with no shared gutter.
			 * Each sub-rect then owns every texel its wrap bleed writes. */
			trial[i].w   = ca->rects[i].src_w + 2 * pad;
			trial[i].h   = ca->rects[i].src_h + 2 * pad;
			trial[i].x   = 0;
			trial[i].y   = 0;
			trial[i].key = (uint32_t)i;
		}
		int h = Aeron_AtlasPackRects(trial, ca->rect_count, try_w, 0);
		if (h < 0)
			continue;
		/* Round atlas height up to next power of 2 (BC7 + mip
		 * generation are happiest on power-of-2 dims). Skip if the
		 * rounded height would exceed max. */
		int try_h = next_pow2(h);
		if (try_h > max_atlas_size)
			continue;
		/* Sanity: every rect must have a valid position (x+w within
		 * try_w, y+h within try_h). Skyline skips oversize rects
		 * silently; we'd rather catch that here. */
		bool fits = true;
		for (int i = 0; i < ca->rect_count; i++) {
			if (trial[i].x + trial[i].w > try_w || trial[i].y + trial[i].h > try_h) {
				fits = false;
				break;
			}
		}
		if (!fits)
			continue;
		long area = (long)try_w * (long)try_h;
		if (best_area == 0 || area < best_area) {
			best_area = area;
			best_w    = try_w;
			best_h    = try_h;
			memcpy(best, trial, (size_t)ca->rect_count * sizeof *trial);
			/* Square atlases are usually optimal for BC7 + power-of-2
			 * mip chains; once we've found one that fits, stop. */
			break;
		}
	}

	if (best_area == 0) {
		free(trial);
		free(best);
		fprintf(stderr,
				"[aeron_gltf_cook] channel %d: cannot fit %d sub-rects "
				"into %d×%d atlas\n",
				ca->channel, ca->rect_count, max_atlas_size, max_atlas_size);
		return false;
	}

	/* Scatter positions back into the original (sorted) rect order. */
	for (int i = 0; i < ca->rect_count; i++) {
		int idx          = (int)best[i].key;
		ca->rects[idx].x = best[i].x + pad;
		ca->rects[idx].y = best[i].y + pad;
	}
	ca->width     = best_w;
	ca->height    = best_h;
	ca->mip_count = mip_count_for_pad(pad);
	ca->pad       = pad;

	free(trial);
	free(best);
	return true;
}

static int wrap_index(int index, int size) {
	int wrapped = index % size;
	return wrapped < 0 ? wrapped + size : wrapped;
}

/* Periodically extend a sub-rect into its private gutter. OPT UVs use
 * REPEAT, so opposite edges must meet under bilinear filtering. */
static void materialize_rect(uint8_t* atlas, int aw, int pad, const ChannelRect* r) {
	for (int y = -pad; y < r->src_h + pad; y++) {
		const int      sy      = wrap_index(y, r->src_h);
		const uint8_t* src_row = r->src_rgba + (size_t)sy * (size_t)r->src_w * 4u;
		uint8_t*       dst_row = atlas + ((size_t)(r->y + y) * (size_t)aw + (size_t)(r->x - pad)) * 4u;

		for (int x = -pad; x < 0; x++) {
			memcpy(dst_row + (size_t)(x + pad) * 4u, src_row + (size_t)wrap_index(x, r->src_w) * 4u, 4u);
		}
		memcpy(dst_row + (size_t)pad * 4u, src_row, (size_t)r->src_w * 4u);
		for (int x = 0; x < pad; x++) {
			memcpy(dst_row + (size_t)(pad + r->src_w + x) * 4u,
				   src_row + (size_t)wrap_index(x, r->src_w) * 4u, 4u);
		}
	}
}

bool channel_atlas_materialize(ChannelAtlas* ca) {
	if (!ca || ca->width <= 0 || ca->height <= 0)
		return false;
	size_t n_bytes = (size_t)ca->width * (size_t)ca->height * 4u;
	ca->rgba       = (uint8_t*)calloc(1, n_bytes);
	if (!ca->rgba)
		return false;

	for (int i = 0; i < ca->rect_count; i++) {
		const ChannelRect* r = &ca->rects[i];
		materialize_rect(ca->rgba, ca->width, ca->pad, r);
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
		out_offset[0] = (float)r->x / aw;
		out_offset[1] = (float)r->y / ah;
	}
	if (out_scale) {
		out_scale[0] = (float)r->src_w / aw;
		out_scale[1] = (float)r->src_h / ah;
	}
}
