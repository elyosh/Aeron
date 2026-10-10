#include "xwau_effect_bake.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

/* `v` with the fewest significant digits, from 6, that read back as `v`. */
static void exact_float(char* out, size_t cap, float v) {
	for (int digits = 6; digits <= 9; ++digits) {
		snprintf(out, cap, "%.*g", digits, (double)v);
		if (strtof(out, NULL) == v)
			return;
	}
}

void XwauFx_Key(char* out, size_t cap, int group, int image, XwauFxKind kind, const float tint[3],
				int alpha_mode) {
	int n = snprintf(out, cap, "%d-%d:%s", group, image, XwauFx_KindName(kind));
	if (alpha_mode == 1 && n > 0 && (size_t)n < cap)
		n += snprintf(out + n, cap - (size_t)n, ":a1");
	if (tint && (tint[0] != 1.0f || tint[1] != 1.0f || tint[2] != 1.0f) && n > 0 && (size_t)n < cap) {
		char c[3][32];
		for (int k = 0; k < 3; ++k)
			exact_float(c[k], sizeof c[k], tint[k]);
		snprintf(out + n, cap - (size_t)n, ":t%s_%s_%s", c[0], c[1], c[2]);
	}
}

bool XwauFx_ParseKey(const char* key, int* group, int* image, XwauFxKind* kind, float tint[3],
					 int* alpha_mode) {
	char*      end;
	const long g = strtol(key, &end, 10);
	if (end == key || *end != '-')
		return false;
	const char* p = end + 1;
	const long  i = strtol(p, &end, 10);
	if (end == p || *end != ':')
		return false;
	p                  = end + 1;
	const size_t len   = strcspn(p, ":");
	bool         found = false;
	for (int k = XWAU_FX_ILLUMINATION; k <= XWAU_FX_MARK && !found; ++k) {
		const char* name = XwauFx_KindName((XwauFxKind)k);
		if (strlen(name) == len && strncmp(p, name, len) == 0) {
			*kind = (XwauFxKind)k;
			found = true;
		}
	}
	if (!found)
		return false;
	p += len;
	*alpha_mode = 0;
	tint[0] = tint[1] = tint[2] = 1.0f;
	if (strncmp(p, ":a1", 3) == 0) {
		*alpha_mode = 1;
		p += 3;
	}
	if (strncmp(p, ":t", 2) == 0) {
		p += 2;
		for (int k = 0; k < 3; ++k) {
			tint[k] = strtof(p, &end);
			if (end == p || (k < 2 && *end != '_'))
				return false;
			p = k < 2 ? end + 1 : end;
		}
	}
	if (*p)
		return false;
	*group = (int)g;
	*image = (int)i;
	/* Only keys XwauFx_Key writes: one spelling per image. */
	char again[96];
	XwauFx_Key(again, sizeof again, *group, *image, *kind, tint, *alpha_mode);
	return strcmp(again, key) == 0;
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
