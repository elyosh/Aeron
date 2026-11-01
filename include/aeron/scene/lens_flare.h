#ifndef AERON_SCENE_LENS_FLARE_H
#define AERON_SCENE_LENS_FLARE_H

/*
 * The procedural lens flare: one quad over the whole output viewport, drawn
 * in the LENS pass after the LENS billboards, with additive blending. Its
 * shape is a list of elements that the scene keeps between frames, and its
 * source is set each frame.
 *
 * A compute pass measures how much of the source's disc is visible once per
 * frame, from 64 depth samples spread over the disc. The flare is scaled by
 * that fraction, and draws no pixels when the source is hidden.
 *
 * Flare units: the output viewport's centre is 0, half its shorter side is
 * 1, and y points down. S is the source's position in these units. Each
 * element reads the pixel's position p, stretched toward the edges by
 * `stretch`: q = p + stretch * (p * |p| - p). The element is centred at
 * `position * S + offset`, so position 1 puts it on the source and -1 on
 * the far side of the centre. d is q minus the centre, turned by -rotation
 * for hexagons and streaks.
 *
 * The elements add up to a value for a display image: the sum times the
 * desc's intensity, clipped at 1. It is turned into linear light and shown
 * at the scene's image radiance, as additive billboards are.
 */

#include <stdbool.h>
#include <stdint.h>

#include "aeron/config_file.h"
#include "aeron/scene/scene3d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AERON_SCENE_LENS_FLARE_MAX_ELEMENTS 32u

typedef enum AeronSceneLensFlareShape {
	AERON_SCENE_LENS_FLARE_HALO = 0, /* 1 / (1 + size * |d|^2): size is the falloff */
	AERON_SCENE_LENS_FLARE_DISC,     /* max(1 - (|d| / size)^power, 0): size is the radius */
	AERON_SCENE_LENS_FLARE_HEXAGON,  /* 1 inside a hexagon whose sides are `size` from its centre */
	AERON_SCENE_LENS_FLARE_STREAK,   /* 1 / max(|d.x| + power * |d.y|, size): a line along x */
} AeronSceneLensFlareShape;

typedef struct AeronSceneLensFlareElement {
	AeronSceneLensFlareShape shape;
	float                    position;  /* along the line from the centre to S */
	float                    offset[2]; /* added to the centre, in flare units */
	float                    stretch;
	float                    size;     /* by shape, see AeronSceneLensFlareShape */
	float                    power;    /* disc: the edge's sharpness; streak: how fast it falls across */
	float                    rotation; /* hexagon and streak, radians, toward +y */
	float                    color[3]; /* display value at the shape's 1, before the intensity */
	bool                     tinted;   /* times the desc's tint */
} AeronSceneLensFlareElement;

/* Sets the elements of the scene's flare, kept until the next call. Copies
 * at most AERON_SCENE_LENS_FLARE_MAX_ELEMENTS. With no elements, no flare is
 * drawn. */
void AeronScene_SetLensFlareElements(AeronScene3D* scene, const AeronSceneLensFlareElement* elements,
									 uint32_t count);

typedef struct AeronSceneLensFlareDesc {
	float anchor_world[3]; /* the source, tested against the depth buffer */
	float radius_world;    /* the source's disc around the anchor, across the view */
	float tint[3];         /* sRGB, 0 to 1 */
	float intensity;
} AeronSceneLensFlareDesc;

/* Sets this frame's flare; a scene draws at most one, and NULL removes it.
 * AeronScene_Begin resets it, so a frame without a call draws none. An
 * anchor behind the camera draws no flare. Depth samples outside the
 * viewport count as hidden, so the flare fades as the disc leaves the
 * screen. */
void AeronScene_SetLensFlare(AeronScene3D* scene, const AeronSceneLensFlareDesc* desc);

/* Builds the flare's pipelines and buffers now, so the first frame with a
 * flare doesn't wait for them. Returns false when they can't be built; the
 * scene then draws no flare. */
bool AeronScene_PrepareLensFlares(AeronScene3D* scene);

/* Reads a sequence of elements. Each entry is a map with `shape` (halo,
 * disc, hexagon or streak) and `color` ([r, g, b], 0 or more), and
 * optionally `position`, `offset` ([x, y]), `stretch`, `size`, `power`,
 * `rotation` and `tinted`. `size` is required, above 0, except for halos,
 * where it may be 0. `power` defaults to 1 and must be above 0 for discs
 * and 0 or more for streaks. Returns false and fills `error` on a malformed
 * entry or more than `capacity` entries. */
bool AeronSceneLensFlare_ParseElements(const AeronConfigNode* list, AeronSceneLensFlareElement* out,
									   uint32_t capacity, uint32_t* count, AeronConfigError* error);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_LENS_FLARE_H */
