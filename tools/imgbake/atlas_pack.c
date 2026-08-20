/*
 * atlas_pack — implementation.
 *
 * The skyline packer and width-trial loop are deterministic so every
 * caller derives identical coordinates for identical frame sequences.
 * Changes must preserve agreement between atlas pixels and emitted YAML.
 */

#include "atlas_pack.h"

#include "upscale.h"      /* scale_y_4k, scale_svga_xy_to_4k, SCALE_X_4K */

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== ANIM-frame adapter =========================================== */

/* Adapter shim: drive the generic packer from an AnimImage + caller-
 * supplied frame-index order. Builds a contiguous AeronAtlasRect[] in pack
 * order, then scatters positions back into the
 * sparse `out_ax[anim_index]` / `out_ay[anim_index]` arrays the legacy
 * AtlasPack contract exposes. */
static int skyline_pack_anim(const AnimImage *anim, const int *order, int n,
                             int atlas_w, int *out_ax, int *out_ay) {
	AeronAtlasRect *rects = (AeronAtlasRect *)calloc((size_t)n, sizeof *rects);
	if (!rects) return -1;
	for (int k = 0; k < n; k++) {
		const Image8 *img = &anim->frames[order[k]];
		rects[k].w   = img->width;
		rects[k].h   = img->height;
		rects[k].key = (uint32_t)order[k];
	}
	int atlas_h = Aeron_AtlasPackRects(rects, n, atlas_w, IMGBAKE_ATLAS_GUTTER);
	if (atlas_h < 0) { free(rects); return atlas_h; }
	for (int k = 0; k < n; k++) {
		int i = (int)rects[k].key;
		out_ax[i] = rects[k].x;
		out_ay[i] = rects[k].y;
	}
	free(rects);
	return atlas_h;
}

bool atlas_pack_compute(const AnimImage *anim, AtlasPack *out) {
	if (!anim || !out) return false;
	memset(out, 0, sizeof *out);

	int max_w = 0, valid = 0;
	long total_area = 0;
	for (int i = 0; i < anim->count; i++) {
		const Image8 *img = &anim->frames[i];
		if (!img->pixels) continue;
		if (img->width > max_w) max_w = img->width;
		total_area += (long)(img->width + 2 * IMGBAKE_ATLAS_GUTTER) *
		              (long)(img->height + 2 * IMGBAKE_ATLAS_GUTTER);
		valid++;
	}
	if (valid == 0 || max_w <= 0)
		return false;

	int *order    = (int *)calloc((size_t)valid,       sizeof(int));
	int *frame_ax = (int *)calloc((size_t)anim->count, sizeof(int));
	int *frame_ay = (int *)calloc((size_t)anim->count, sizeof(int));
	int *try_ax   = (int *)calloc((size_t)anim->count, sizeof(int));
	int *try_ay   = (int *)calloc((size_t)anim->count, sizeof(int));
	if (!order || !frame_ax || !frame_ay || !try_ax || !try_ay) {
		free(order); free(frame_ax); free(frame_ay);
		free(try_ax); free(try_ay);
		return false;
	}

	int n_order = 0;
	for (int i = 0; i < anim->count; i++)
		if (anim->frames[i].pixels)
			order[n_order++] = i;

	/* Sort by max(w,h) desc; tiebreak by area desc. Insertion sort
	 * — N typically <64 frames per ANIM. */
	for (int i = 1; i < n_order; i++) {
		int key = order[i];
		const Image8 *ki = &anim->frames[key];
		int  kkey = ki->width > ki->height ? ki->width : ki->height;
		long akey = (long)ki->width * (long)ki->height;
		int j = i - 1;
		while (j >= 0) {
			const Image8 *kj = &anim->frames[order[j]];
			int  kj_key = kj->width > kj->height ? kj->width : kj->height;
			long aj     = (long)kj->width * (long)kj->height;
			bool key_bigger = (kkey > kj_key) ||
			                  (kkey == kj_key && akey > aj);
			if (!key_bigger) break;
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = key;
	}

	int min_w = max_w + 2 * IMGBAKE_ATLAS_GUTTER;
	int sqrt_area = (int)ceil(sqrt((double)total_area));
	int upper_w = sqrt_area * 2;
	if (upper_w < min_w * 2) upper_w = min_w * 2;
	int atlas_w = min_w;
	int atlas_h = IMGBAKE_ATLAS_GUTTER;
	double best_score = HUGE_VAL;
	const int n_widths = 8;
	for (int wi = 0; wi < n_widths; wi++) {
		int try_w = min_w + (upper_w - min_w) * wi / (n_widths - 1);
		if (try_w < min_w) try_w = min_w;
		int h = skyline_pack_anim(anim, order, n_order, try_w, try_ax, try_ay);
		if (h < 0) continue;
		double a = (double)try_w * (double)h;
		double aspect = (try_w >= h)
		    ? (double)try_w / (double)h
		    : (double)h / (double)try_w;
		double score = a * aspect;
		if (score < best_score) {
			best_score = score;
			atlas_w = try_w;
			atlas_h = h;
			memcpy(frame_ax, try_ax, sizeof(int) * (size_t)anim->count);
			memcpy(frame_ay, try_ay, sizeof(int) * (size_t)anim->count);
		}
	}
	free(order);
	free(try_ax);
	free(try_ay);

	out->ax = frame_ax;
	out->ay = frame_ay;
	out->count = anim->count;
	out->classic_atlas_w = atlas_w;
	out->classic_atlas_h = atlas_h;
	return true;
}

void atlas_pack_free(AtlasPack *p) {
	if (!p) return;
	free(p->ax);
	free(p->ay);
	memset(p, 0, sizeof *p);
}

void atlas_pack_post_dims(const AtlasPack *pack,
                          bool scale, bool svga_mode,
                          int *out_w, int *out_h) {
	if (!pack || !out_w || !out_h) return;
	int w = pack->classic_atlas_w;
	int h = pack->classic_atlas_h;
	if (scale) {
		if (svga_mode) {
			w = scale_svga_xy_to_4k(w);
			h = scale_svga_xy_to_4k(h);
		} else {
			w = w * SCALE_X_4K;
			h = scale_y_4k(h);
		}
	}
	/* BC formats require dimensions rounded to complete 4×4 blocks. */
	w = (w + 3) & ~3;
	h = (h + 3) & ~3;
	*out_w = w;
	*out_h = h;
}

bool atlas_emit_yaml(const char *yaml_path,
                     const AnimImage *anim,
                     const AtlasPack *pack,
                     int post_atlas_w, int post_atlas_h,
                     bool scale, bool svga_mode,
                     char *err, size_t errsz) {
	if (!yaml_path || !anim || !pack || pack->count != anim->count) {
		if (err && errsz) snprintf(err, errsz, "invalid args");
		return false;
	}
	char tmp_path[2048];
	int n = snprintf(tmp_path, sizeof tmp_path, "%s.tmp", yaml_path);
	if (n < 0 || n >= (int)sizeof tmp_path) {
		if (err && errsz) snprintf(err, errsz, "path too long");
		return false;
	}
	FILE *yf = fopen(tmp_path, "w");
	if (!yf) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", tmp_path, strerror(errno));
		return false;
	}

	if (fprintf(yf,
	            "atlas:\n"
	            "  w: %d\n"
	            "  h: %d\n",
	            post_atlas_w, post_atlas_h) < 0) goto io_err;
	if (scale) {
		if (fprintf(yf,
		            "  classic_w: %d\n"
		            "  classic_h: %d\n",
		            pack->classic_atlas_w, pack->classic_atlas_h) < 0)
			goto io_err;
	}
	if (fprintf(yf, "frames:\n") < 0) goto io_err;

	for (int i = 0; i < anim->count; i++) {
		const Image8 *img = &anim->frames[i];
		int fw = img->pixels ? img->width  : 0;
		int fh = img->pixels ? img->height : 0;
		int ox = pack->ax[i];
		int oy = pack->ay[i];
		int yaml_x, yaml_y, yaml_w, yaml_h;
		if (scale) {
			if (svga_mode) {
				int left  = scale_svga_xy_to_4k(ox);
				int right = scale_svga_xy_to_4k(ox + fw);
				int top   = scale_svga_xy_to_4k(oy);
				int bot   = scale_svga_xy_to_4k(oy + fh);
				yaml_x = left;  yaml_w = right - left;
				yaml_y = top;   yaml_h = bot - top;
			} else {
				yaml_x = ox * SCALE_X_4K;
				yaml_w = fw * SCALE_X_4K;
				int top = scale_y_4k(oy);
				int bot = scale_y_4k(oy + fh);
				yaml_y = top;
				yaml_h = bot - top;
			}
		} else {
			yaml_x = ox; yaml_w = fw;
			yaml_y = oy; yaml_h = fh;
		}
		if (fprintf(yf,
		            "  - { x: %d, y: %d, w: %d, h: %d, "
		            "origin_x: %d, origin_y: %d }\n",
		            yaml_x, yaml_y, yaml_w, yaml_h,
		            img->pixels ? img->left : 0,
		            img->pixels ? img->top  : 0) < 0)
			goto io_err;
	}

	if (fflush(yf) != 0) goto io_err;
	if (fclose(yf) != 0) {
		yf = NULL;
		if (err && errsz)
			snprintf(err, errsz, "close %s: %s", tmp_path, strerror(errno));
		SDL_RemovePath(tmp_path);
		return false;
	}
	yf = NULL;
	if (!SDL_RenamePath(tmp_path, yaml_path)) {
		if (err && errsz)
			snprintf(err, errsz, "rename %s -> %s: %s",
			         tmp_path, yaml_path, SDL_GetError());
		SDL_RemovePath(tmp_path);
		return false;
	}
	return true;

io_err:
	if (err && errsz)
		snprintf(err, errsz, "write %s: %s", tmp_path, strerror(errno));
	if (yf) fclose(yf);
	SDL_RemovePath(tmp_path);
	return false;
}
