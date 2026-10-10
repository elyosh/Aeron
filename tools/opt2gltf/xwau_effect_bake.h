#ifndef AERON_XWAU_EFFECT_BAKE_H
#define AERON_XWAU_EFFECT_BAKE_H

/*
 * Bakes of XWAU animation frames into effect-library images (docs:
 * xwau_animated_materials.md, "Image libraries"). A source is a decoded DAT
 * image (sRGB-encoded R, G, B and linear A, rows in file order); `tint` and
 * `alpha_mode` are the sequence's. Each kind is computed in linear light and
 * stored sRGB-encoded, with linear alpha.
 *
 *   illumination  layer:    RGB layer_rgb = L'·a·(1 − s), A k = (1 − a)(1 − s)
 *                 emissive: RGB L'·a·s, A 255
 *   colour        layer:    RGB the tinted colour, A its coverage
 *   multiply      layer:    RGB the damage factor m, A m.r (linear, for gloss,
 *                           metallic and specular)
 *   energy        emissive: RGB rgb·a, A 255
 *   mark          layer:    RGB premultiplied rgb·a, A a
 *
 * L' and s follow PixelShaderAnim.hlsl on the tinted sRGB values: L' =
 * saturate(1.25·L) (the HSV value gain), s = smoothstep(0.75, 0.85, V)·
 * smoothstep(0.45, 0.55, a) with V = max(L); with alpha mode 1, a = V.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum XwauFxKind {
	XWAU_FX_ILLUMINATION = 0,
	XWAU_FX_COLOUR,
	XWAU_FX_MULTIPLY,
	XWAU_FX_ENERGY,
	XWAU_FX_MARK,
} XwauFxKind;

/* Kind name as used in library keys ("illumination", ...); NULL if invalid. */
const char* XwauFx_KindName(XwauFxKind kind);

/* Whether `kind` writes the layer and the emissive image. */
bool XwauFx_HasLayer(XwauFxKind kind);
bool XwauFx_HasEmissive(XwauFxKind kind);

/* Bakes `n` texels of `src` (RGBA). `layer` and `emissive` receive n RGBA
 * texels each when the kind writes them (otherwise they may be NULL). */
void XwauFx_Bake(XwauFxKind kind, const uint8_t* src, size_t n, const float tint[3], int alpha_mode,
				 uint8_t* layer, uint8_t* emissive);

/* The library key of an image: "G-I:kind", then ":a1" for alpha mode 1 and
 * ":t<r>_<g>_<b>" (%g) for a tint other than white. */
void XwauFx_Key(char* out, size_t cap, int group, int image, XwauFxKind kind, const float tint[3],
				int alpha_mode);

/* sRGB transfer, 0..1. */
float XwauFx_ToLinear(float srgb);
float XwauFx_ToSrgb(float linear);

#endif /* AERON_XWAU_EFFECT_BAKE_H */
