/*
 * atlas_pack — skyline rectangle packer + ANIM-frame adapter +
 * YAML-emit helper.
 *
 * The generic rectangle packer lives in Aeron core. This module supplies the
 * ANIM-frame adapter — `atlas_pack_compute` + post-dims +
 *      `atlas_emit_yaml`. Factored out of filmextract so filmview's
 *      "Regenerate YAML from source ANIM" button reproduces the same
 *      per-frame coordinates filmextract would write.
 *
 * The pack is deterministic for a given input: same rects in the same
 * order with the same widths/heights produce the same output positions.
 * Two consumers therefore agree on the final layout.
 *
 * Pixel emission (PNG / KTX2 / upscale) stays in the calling tool —
 * only the geometry primitives are shared.
 */
#ifndef FILM_ATLAS_PACK_H
#define FILM_ATLAS_PACK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "anim.h"
#include "aeron/atlas_pack.h"

#include <stddef.h>
#include <stdint.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

/* ===== ANIM-frame adapter =========================================== */

typedef struct {
	int *ax;                /* per-frame X position in the classic packed atlas */
	int *ay;                /* per-frame Y position */
	int  count;             /* == anim->count */
	int  classic_atlas_w;   /* pre-upscale dims */
	int  classic_atlas_h;
} AtlasPack;

/* Run the same skyline-with-aspect-weighted-area-trial packer that
 * filmextract's emit_atlas_strip uses. Allocates ax/ay; caller frees
 * via atlas_pack_free. Returns false on OOM or empty input. */
bool atlas_pack_compute(const AnimImage *anim, AtlasPack *out);

void atlas_pack_free(AtlasPack *p);

/* Compute the post-upscale atlas dims (with BC7-alignment padding to
 * a multiple of 4) for a given pack and scale config. Mirrors the
 * sizing filmextract applies between skyline_pack and write_png_rgba.
 * `scale=false` returns the classic dims unchanged. */
void atlas_pack_post_dims(const AtlasPack *pack,
                          bool scale, bool svga_mode,
                          int *out_w, int *out_h);

/* Atomically write a YAML layout describing the pack in the same
 * inline-flow shape filmextract emits (matched byte-for-byte against
 * a fresh extraction so diffs stay minimal).
 *
 * `post_atlas_w/h` come from atlas_pack_post_dims. `scale` and
 * `svga_mode` select the per-frame coordinate scaling. `err` is
 * optional; on failure it carries a NUL-terminated reason. */
bool atlas_emit_yaml(const char *yaml_path,
                     const AnimImage *anim,
                     const AtlasPack *pack,
                     int post_atlas_w, int post_atlas_h,
                     bool scale, bool svga_mode,
                     char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif
