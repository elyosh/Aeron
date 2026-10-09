#include "xwau_effect_bake.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

/* HLSL smoothstep. */
static float smoothstep(float e0, float e1, float x) {
	const float t = clamp01((x - e0) / (e1 - e0));
	return t * t * (3.0f - 2.0f * t);
}

static uint8_t unorm8(float x) { return (uint8_t)(clamp01(x) * 255.0f + 0.5f); }

float XwauFx_ToLinear(float c) {
	c = clamp01(c);
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

float XwauFx_ToSrgb(float c) {
	c = clamp01(c);
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

const char* XwauFx_KindName(XwauFxKind kind) {
	switch (kind) {
		case XWAU_FX_ILLUMINATION:
			return "illumination";
		case XWAU_FX_COLOUR:
			return "colour";
		case XWAU_FX_MULTIPLY:
			return "multiply";
		case XWAU_FX_ENERGY:
			return "energy";
		case XWAU_FX_MARK:
			return "mark";
	}
	return NULL;
}

bool XwauFx_HasLayer(XwauFxKind kind) { return kind != XWAU_FX_ENERGY; }

bool XwauFx_HasEmissive(XwauFxKind kind) { return kind == XWAU_FX_ILLUMINATION || kind == XWAU_FX_ENERGY; }

void XwauFx_Key(char* out, size_t cap, int group, int image, XwauFxKind kind, const float tint[3],
				int alpha_mode) {
	int n = snprintf(out, cap, "%d-%d:%s", group, image, XwauFx_KindName(kind));
	if (alpha_mode == 1 && n > 0 && (size_t)n < cap)
		n += snprintf(out + n, cap - (size_t)n, ":a1");
	if (tint && (tint[0] != 1.0f || tint[1] != 1.0f || tint[2] != 1.0f) && n > 0 && (size_t)n < cap)
		snprintf(out + n, cap - (size_t)n, ":t%g_%g_%g", (double)tint[0], (double)tint[1], (double)tint[2]);
}

void XwauFx_Bake(XwauFxKind kind, const uint8_t* src, size_t n, const float tint[3], int alpha_mode,
				 uint8_t* layer, uint8_t* emissive) {
	static const float white[3] = { 1.0f, 1.0f, 1.0f };
	if (!tint)
		tint = white;
	for (size_t i = 0; i < n; ++i) {
		const uint8_t* p = &src[i * 4];
		/* The renderer's shader math runs on the stored (sRGB) values. */
		float c[3];
		for (int k = 0; k < 3; ++k)
			c[k] = p[k] / 255.0f * tint[k];
		float       a = p[3] / 255.0f;
		const float v = fmaxf(c[0], fmaxf(c[1], c[2]));
		uint8_t*    l = layer ? &layer[i * 4] : NULL;
		uint8_t*    e = emissive ? &emissive[i * 4] : NULL;
		switch (kind) {
			case XWAU_FX_ILLUMINATION: {
				if (alpha_mode == 1)
					a = clamp01(v);
				const float s = smoothstep(0.75f, 0.85f, v) * smoothstep(0.45f, 0.55f, a);
				for (int k = 0; k < 3; ++k) {
					const float lin = XwauFx_ToLinear(clamp01(1.25f * c[k]));
					l[k]            = unorm8(XwauFx_ToSrgb(lin * a * (1.0f - s)));
					e[k]            = unorm8(XwauFx_ToSrgb(lin * a * s));
				}
				l[3] = unorm8((1.0f - a) * (1.0f - s));
				e[3] = 255;
				break;
			}
			case XWAU_FX_COLOUR:
				if (alpha_mode == 1)
					a = clamp01(0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]);
				for (int k = 0; k < 3; ++k)
					l[k] = unorm8(c[k]);
				l[3] = unorm8(a);
				break;
			case XWAU_FX_MULTIPLY:
				/* sRGB-encoded m sampled through an sRGB view gives the linear
				 * factor of the renderer's sRGB-space multiply. */
				for (int k = 0; k < 3; ++k)
					l[k] = unorm8(c[k]);
				l[3] = unorm8(c[0]);
				break;
			case XWAU_FX_ENERGY:
				for (int k = 0; k < 3; ++k)
					e[k] = unorm8(XwauFx_ToSrgb(XwauFx_ToLinear(c[k]) * a));
				e[3] = 255;
				break;
			case XWAU_FX_MARK:
				for (int k = 0; k < 3; ++k)
					l[k] = unorm8(XwauFx_ToSrgb(XwauFx_ToLinear(c[k]) * a));
				l[3] = unorm8(a);
				break;
		}
	}
}
