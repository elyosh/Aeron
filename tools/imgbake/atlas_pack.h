/*
 * atlas_pack — skyline rectangle packer + ANIM-frame adapter +
 * YAML-emit helper.
 *
 * The generic rectangle packer lives in Aeron core. This module supplies
 * deterministic ANIM-frame adaptation, output scaling, and YAML emission.
 *
 * The pack is deterministic for a given input: same rects in the same
 * order with the same widths/heights produce the same output positions.
 * Two consumers therefore agree on the final layout.
 *
 * Pixel emission (PNG / KTX2 / upscale) stays in the calling tool —
 * only the geometry primitives are shared.
 */
#ifndef IMGBAKE_ATLAS_PACK_H
#define IMGBAKE_ATLAS_PACK_H

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

/* Run the skyline packer's aspect-weighted area trials. Allocates ax/ay;
 * caller frees via atlas_pack_free. Returns false on OOM or empty input. */
bool atlas_pack_compute(const AnimImage *anim, AtlasPack *out);

void atlas_pack_free(AtlasPack *p);

/* Compute post-upscale atlas dimensions with BC block alignment.
 * `scale=false` returns the source dimensions unchanged. */
void atlas_pack_post_dims(const AtlasPack *pack,
                          bool scale, bool svga_mode,
                          int *out_w, int *out_h);

/* Atomically write the pack as a compact inline-flow YAML layout.
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
