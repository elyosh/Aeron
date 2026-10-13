/*
 * opt2gltf — OPT → glTF 2.0 conversion. See opt2gltf.h for the
 * data-preservation contract.
 *
 * OPT (+X right, -Y forward, +Z up) maps to standard meter-space glTF:
 * glTF = (-OPT.x, OPT.z, -OPT.y). The reflection converts OPT clockwise
 * face order to glTF counterclockwise order without changing indices.
 */

#include "opt2gltf.h"
#include "aeron/mesh_normals.h"
#include "cJSON.h"
#include "opt.h"
#include "opt_json.h"

#include "mikktspace.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "cgltf_write.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* ===== Helpers ====================================================== */

static void swap_axis_v3(const opt_vec3_t* in, float out[3]) {
	out[0] = -in->x;
	out[1] = in->z;
	out[2] = -in->y;
}

#define OPT_METERS_PER_UNIT (1600.0f / 65536.0f)
#define OPT_Q15_TO_UNIT 32768.0f

static char* xstrdup(const char* s) {
	size_t n = strlen(s);
	char*  r = (char*)malloc(n + 1);
	if (r)
		memcpy(r, s, n + 1);
	return r;
}

static void normalize3(float vector[3]) {
	const float length = sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
	if (length > 1.0e-9f) {
		vector[0] /= length;
		vector[1] /= length;
		vector[2] /= length;
	}
}

static void quaternion_from_axes(float right[3], float up[3], float look[3], float out[4]) {
	normalize3(right);
	normalize3(up);
	normalize3(look);
	const float trace = right[0] + up[1] + look[2];
	if (trace > 0.0f) {
		const float s = sqrtf(trace + 1.0f) * 2.0f;
		out[3]        = 0.25f * s;
		out[0]        = (up[2] - look[1]) / s;
		out[1]        = (look[0] - right[2]) / s;
		out[2]        = (right[1] - up[0]) / s;
	} else if (right[0] > up[1] && right[0] > look[2]) {
		const float s = sqrtf(1.0f + right[0] - up[1] - look[2]) * 2.0f;
		out[3]        = (up[2] - look[1]) / s;
		out[0]        = 0.25f * s;
		out[1]        = (up[0] + right[1]) / s;
		out[2]        = (look[0] + right[2]) / s;
	} else if (up[1] > look[2]) {
		const float s = sqrtf(1.0f + up[1] - right[0] - look[2]) * 2.0f;
		out[3]        = (look[0] - right[2]) / s;
		out[0]        = (up[0] + right[1]) / s;
		out[1]        = 0.25f * s;
		out[2]        = (look[1] + up[2]) / s;
	} else {
		const float s = sqrtf(1.0f + look[2] - right[0] - up[1]) * 2.0f;
		out[3]        = (right[1] - up[0]) / s;
		out[0]        = (look[0] + right[2]) / s;
		out[1]        = (look[1] + up[2]) / s;
		out[2]        = 0.25f * s;
	}
	const float length = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
	if (length > 1.0e-9f) {
		for (int index = 0; index < 4; ++index)
			out[index] /= length;
	}
}

static char* xprintf_dup(const char* fmt, ...) {
	char    buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return xstrdup(buf);
}

static void path_join(char* out, size_t cap, const char* a, const char* b) {
	size_t la       = strlen(a);
	int    need_sep = (la > 0 && a[la - 1] != '/');
	snprintf(out, cap, "%s%s%s", a, need_sep ? "/" : "", b);
}

/* RGB565 word → 8-bit triple. */
static inline void rgb565_to_rgb8(uint16_t w, uint8_t* r, uint8_t* g, uint8_t* b) {
	uint8_t r5 = (w >> 11) & 0x1F;
	uint8_t g6 = (w >> 5) & 0x3F;
	uint8_t b5 = w & 0x1F;
	*r         = (uint8_t)((r5 << 3) | (r5 >> 2));
	*g         = (uint8_t)((g6 << 2) | (g6 >> 4));
	*b         = (uint8_t)((b5 << 3) | (b5 >> 2));
}

/* Decode the base mip of an OPT texture into RGBA8 at the given palette
 * shade row (the file's base-color row; see opt_palette_base_shade).
 * 32bpp textures ignore the shade and keep their authored B,G,R,A color;
 * their alpha is t->alpha (set only with the texture's alpha flag), else
 * opaque. Caller owns the returned buffer. Returns NULL on bad inputs. */
static uint8_t* decode_texture_rgba8(const opt_texture_t* t, int shade) {
	if (!t || !t->pixels || t->width <= 0 || t->height <= 0)
		return NULL;
	const size_t base_bytes = (size_t)t->width * (size_t)t->height;
	uint8_t*     out        = (uint8_t*)malloc(base_bytes * 4);
	if (!out)
		return NULL;
	if (t->bytes_per_texel == 4) {
		for (size_t i = 0; i < base_bytes; ++i) {
			out[i * 4 + 0] = t->pixels[i * 4 + 2];
			out[i * 4 + 1] = t->pixels[i * 4 + 1];
			out[i * 4 + 2] = t->pixels[i * 4 + 0];
			out[i * 4 + 3] = t->alpha ? t->alpha[i] : 255;
		}
		return out;
	}
	const uint8_t* src = t->pixels;
	const uint8_t* pal = t->palette + ((size_t)shade * OPT_PALETTE_COLORS * OPT_PALETTE_BPP);
	for (size_t i = 0; i < base_bytes; ++i) {
		uint8_t  idx = src[i];
		uint16_t w   = (uint16_t)pal[idx * 2] | (uint16_t)((uint16_t)pal[idx * 2 + 1] << 8);
		uint8_t  r, g, b;
		rgb565_to_rgb8(w, &r, &g, &b);
		out[i * 4 + 0] = r;
		out[i * 4 + 1] = g;
		out[i * 4 + 2] = b;
		/* Alpha from an XWA TextureAlpha node (base mip), else opaque. */
		out[i * 4 + 3] = t->alpha ? t->alpha[i] : 255;
	}
	return out;
}

/* Decode the base mip into an emissive RGBA8 map: glow color + alpha 255
 * on self-illuminated texels, transparent black elsewhere. The alpha is
 * the classic lightmap coverage mask; gltf_cook premultiplies it before
 * mip generation so the runtime can reconstruct the original gamma-space
 * filtering and SRCALPHA pass in the opaque PBR shader. Returns NULL if
 * the texture builds no emissive layer (so no emissive PNG/material is
 * emitted for it). Caller owns the returned buffer.
 *
 * XWA (version 5) files replicate the engine EXACTLY
 * (ModelTexture_FilterHardwarePalette + CreateD3DfromTexture): entries
 * classified by opt_palette_classic_lit, glow color from palette row 10,
 * no lightmap for textures named "_*" or when all/none of the 256
 * entries classify lit. Legacy files keep the flat-ramp heuristic with
 * `tol`, sampling the base shade. */
static uint8_t* decode_texture_emissive_rgba8(const opt_file_t* opt, const opt_texture_t* t, int shade,
											  int tol) {
	if (!t || !t->pixels || t->width <= 0 || t->height <= 0)
		return NULL;
	/* The classifiers read the palette; a 32bpp texture has none (its
	 * illumination is a separate plane, t->illum). */
	if (t->bytes_per_texel == 4)
		return NULL;
	const int xwa = opt && opt->version >= 5;
	if (xwa && t->name[0] == '_') {
		return NULL; /* engine gate: underscore textures never glow */
	}

	int lit[OPT_PALETTE_COLORS];
	int lit_count = 0;
	for (int i = 0; i < OPT_PALETTE_COLORS; ++i) {
		lit[i] = xwa ? opt_palette_classic_lit(t->palette, i)
					 : opt_palette_index_emissive(opt, t->palette, i, tol);
		lit_count += lit[i];
	}
	/* Engine gate (clearedCount at 0x44A600): a lightmap builds only
	 * when SOME but not ALL palette entries classify lit. */
	if (xwa && (lit_count == 0 || lit_count == OPT_PALETTE_COLORS)) {
		return NULL;
	}

	const int      glow_shade = xwa ? OPT_XWA_GLOW_SHADE : shade;
	const size_t   base_bytes = (size_t)t->width * (size_t)t->height;
	const uint8_t* src        = t->pixels;
	const uint8_t* pal        = t->palette + ((size_t)glow_shade * OPT_PALETTE_COLORS * OPT_PALETTE_BPP);
	uint8_t*       out        = NULL;
	for (size_t i = 0; i < base_bytes; ++i) {
		uint8_t idx = src[i];
		if (!lit[idx]) {
			if (out) {
				out[i * 4 + 0] = 0;
				out[i * 4 + 1] = 0;
				out[i * 4 + 2] = 0;
				out[i * 4 + 3] = 0;
			}
			continue;
		}
		if (!out) {
			out = (uint8_t*)calloc(base_bytes, 4);
			if (!out)
				return NULL;
		}
		uint16_t w = (uint16_t)pal[idx * 2] | (uint16_t)((uint16_t)pal[idx * 2 + 1] << 8);
		uint8_t  r, g, b;
		rgb565_to_rgb8(w, &r, &g, &b);
		out[i * 4 + 0] = r;
		out[i * 4 + 1] = g;
		out[i * 4 + 2] = b;
		out[i * 4 + 3] = 255;
	}
	return out; /* NULL when no texel qualified */
}

/* ===== XWAU material bake ============================================
 *
 * Reproduces, per texel, how the X-Wing Alliance Upgrade "effects" renderer
 * (xwa_ddraw_d3d11, PBR deferred path) shades an OPT texture, and stores the
 * result in standard glTF inputs. The math runs on gamma-encoded values, as
 * that renderer does.
 *   - illumination: 8bpp texels whose palette row 4 equals row 8 and are not
 *     near-black (XwaD3dRendererHook.cpp D3dOptCreateTextureColorLight), or
 *     the 32bpp illumination plane; XwaD3dPixelShader.hlsl's illum branch
 *     makes them unlit at 1.25x their color;
 *   - Shadeless/Ambient: output = lerp(lit, color, s), stored as base color
 *     color * (1 - s) plus emissive color * s;
 *   - glass (transparent, not shadeless, no illumination): below alpha 0.95
 *     a texel gets coverage a^2, gloss 0.93 and metallic 0. XWAU also makes
 *     it 0.8 shadeless, to see into cockpits; as emission that reads as
 *     glowing glass, so the glass is lit. The material is opaque with
 *     KHR_materials_transmission and F0 0.04, its base colour and
 *     transmission giving that blend under the spec's tint (glass bake).
 *     Its roughness is XWAU_GLASS_ROUGHNESS, not gloss 0.93's 0.135: XWAU
 *     blended glass, so its gloss only shaped highlights, while the spec's
 *     roughness also blurs what shows through, which would frost it;
 *   - dark-texel reflectance fade (PBRAddCommon.h "blackish"): approximated
 *     by raising roughness to 1;
 *   - Metallic, Glossiness: metallic and roughness = 1 - min(0.93, G)^2.
 * Colors are authored ones (palette row 8, raw 32bpp): XWA's brightness
 * setting is a display option, not material color. */

#define XWAU_GLASS_GLOSS 0.93f
#define XWAU_GLASS_ROUGHNESS 0.05f
#define XWAU_ILLUM_GAIN 1.25f

typedef struct XwauBaked {
	uint8_t* base;           /* RGBA */
	uint8_t* emissive;       /* RGBA / XWAU_ILLUM_GAIN; NULL when no texel emits */
	uint8_t* metal_rough;    /* RGBA, G roughness, B metallic; NULL when uniform */
	uint8_t* transmission;   /* RGBA, R = 1 - glass coverage; NULL without glass texels */
	float    metallic;       /* factors when metal_rough is NULL */
	float    roughness;      /* factors when metal_rough is NULL */
	float    specular_color; /* KHR_materials_specular specularColorFactor */
	bool     transparent;
} XwauBaked;

static float clamp01f(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* HLSL smoothstep, also valid with edge0 > edge1. */
static float smoothstepf(float edge0, float edge1, float x) {
	const float t = clamp01f((x - edge0) / (edge1 - edge0));
	return t * t * (3.0f - 2.0f * t);
}

static uint8_t unorm8(float x) { return (uint8_t)(clamp01f(x) * 255.0f + 0.5f); }

/* Metallic as the renderer decodes it: stored as Metallic * 0.5 in an 8-bit
 * channel, read back as metallic unless the stored value reaches METAL_HI
 * (0.55), which reads as 0.25. */
static float xwau_metallic(float metallic) {
	const float stored = clamp01f(metallic * 0.5f);
	return stored < 0.55f ? clamp01f(stored / 0.5f) : 0.25f;
}

/* Glossiness: stored in 8 bits, capped at 0.93; roughness = 1 - g^2. */
static float xwau_roughness(float glossiness) {
	const float g = fminf(XWAU_GLASS_GLOSS, clamp01f(glossiness));
	return 1.0f - g * g;
}

/* F0 = 0.16 * intensity^2 = 0.04 * specularColorFactor (specularFactor 1
 * keeps F90 = 1, as the renderer's Schlick term). */
static float xwau_specular_color(float intensity) { return 4.0f * intensity * intensity; }

/* 8bpp illumination: row 4 (dim) equals row 8 (base) and is not near-black. */
static bool xwau_palette_lit(const uint8_t* palette, uint8_t index) {
	const uint8_t* dim = palette + ((size_t)4 * OPT_PALETTE_COLORS + index) * OPT_PALETTE_BPP;
	const uint8_t* base =
		palette + ((size_t)OPT_XWA_BASE_SHADE * OPT_PALETTE_COLORS + index) * OPT_PALETTE_BPP;
	const uint16_t c4 = (uint16_t)(dim[0] | (dim[1] << 8));
	const uint16_t c8 = (uint16_t)(base[0] | (base[1] << 8));
	if (((c4 >> 11) & 0x1F) <= 8 && ((c4 >> 5) & 0x3F) <= 16 && (c4 & 0x1F) <= 8)
		return false;
	return c4 == c8;
}

static void xwau_baked_free(XwauBaked* baked) {
	free(baked->base);
	free(baked->emissive);
	free(baked->metal_rough);
	free(baked->transmission);
	memset(baked, 0, sizeof *baked);
}

static void xwau_colour_free(uint8_t** colour, int32_t count) {
	if (!colour)
		return;
	for (int32_t i = 0; i < count; ++i)
		free(colour[i]);
	free(colour);
}

static void xwau_state_free(XwauBaked* baked, int32_t count, int32_t* normal_owner, int32_t* normal_image) {
	if (baked) {
		for (int32_t i = 0; i < count; ++i)
			xwau_baked_free(&baked[i]);
		free(baked);
	}
	free(normal_owner);
	free(normal_image);
}

float OptGltf_XwauUnlit(const OptGltfXwauMaterial* xm) {
	if (xm->lit)
		return 0.0f;
	return (xm->shadeless || xm->alpha_to_bloom) ? 1.0f : clamp01f(xm->ambient);
}

bool OptGltf_XwauTextureHasIllum(const opt_texture_t* t) {
	if (t->bytes_per_texel == 4)
		return t->flag_illum;
	/* The renderer creates the light texture when any stored texel, mips
	 * included, is lit. */
	for (int32_t k = 0; k < t->mip_chain_bytes; ++k)
		if (xwau_palette_lit(t->palette, t->pixels[k]))
			return true;
	return false;
}

static bool xwau_bake_rgba(const opt_texture_t* t, const OptGltfXwauMaterial* xm, uint8_t* rgba,
						   XwauBaked* out);

/* Copies `n` RGBA texels; NULL on OOM. */
static uint8_t* rgba_dup(const uint8_t* rgba, size_t n) {
	uint8_t* copy = (uint8_t*)malloc(n * 4);
	if (copy)
		memcpy(copy, rgba, n * 4);
	return copy;
}

/* `colour`, when set, receives a copy of the decoded colours before the bake
 * (NULL without pixels). */
static bool xwau_bake_texture(const opt_texture_t* t, int base_shade, const OptGltfXwauMaterial* xm,
							  XwauBaked* out, uint8_t** colour) {
	memset(out, 0, sizeof *out);
	if (colour)
		*colour = NULL;
	uint8_t* rgba = decode_texture_rgba8(t, base_shade);
	if (!rgba)
		return true; /* no pixels: the caller emits the missing-image material */
	if (colour) {
		*colour = rgba_dup(rgba, (size_t)t->width * (size_t)t->height);
		if (!*colour) {
			free(rgba);
			return false;
		}
	}
	return xwau_bake_rgba(t, xm, rgba, out);
}

static float srgb_to_linear(float c) {
	c = clamp01f(c);
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float c) {
	c = clamp01f(c);
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* The base colour of a material with illumination flipbooks
 * (OptGltfXwauAnim.anim_base): the colours `*colour` (before the bake),
 * which its frames apply over. XWAU shows them, lit, where a frame turns
 * the texture's own glow off; at rest the glowing texels then also receive
 * the scene's light. With an `always` flipbook the material has no
 * emission of its own (the frames are its glow). Frees `*colour`. */
static void xwau_anim_base(uint8_t** colour, uint32_t w, uint32_t h, const OptGltfXwauAnim* a,
						   XwauBaked* baked) {
	if (!a || !*colour || !baked->base)
		return;
	const size_t n = (size_t)w * h;
	for (size_t i = 0; i < n; ++i)
		for (int k = 0; k < 3; ++k)
			baked->base[i * 4 + k] = (*colour)[i * 4 + k];
	if (a->always_illumination) {
		free(baked->emissive);
		baked->emissive = NULL;
	}
	free(*colour);
	*colour = NULL;
}

/* Bakes `rgba`, the decoded colors of texture `t` (owned from here, baked in
 * place into out->base); `t` gives the alpha, illumination and format. */
static bool xwau_bake_rgba(const opt_texture_t* t, const OptGltfXwauMaterial* xm, uint8_t* rgba,
						   XwauBaked* out) {
	memset(out, 0, sizeof *out);
	out->base                = rgba;
	const size_t n           = (size_t)t->width * (size_t)t->height;
	const bool   bpp32       = t->bytes_per_texel == 4;
	const bool   transparent = t->alpha != NULL;
	/* A shadeless transparent texture (cockpit glass) returns before the
	   illumination branch (XwaD3dPixelShader.hlsl:80); so does a `lit` one,
	   whose Shadeless the override cleared. */
	const bool  has_illum   = OptGltf_XwauTextureHasIllum(t) && !(transparent && (xm->shadeless || xm->lit));
	const bool  glass       = transparent && !xm->lit && !xm->shadeless && !xm->alpha_isnt_glass &&
							  !xm->alpha_to_bloom && !has_illum;
	const float metallic    = xwau_metallic(xm->metallic);
	const float roughness   = xwau_roughness(xm->glossiness);
	const float glass_rough = XWAU_GLASS_ROUGHNESS;
	const float shadeless   = OptGltf_XwauUnlit(xm);

	out->transparent = transparent;
	out->emissive    = (uint8_t*)malloc(n * 4);
	out->metal_rough = (uint8_t*)malloc(n * 4);
	if (!out->emissive || !out->metal_rough) {
		xwau_baked_free(out);
		return false;
	}
	bool  any_emissive = false, any_glass = false, uniform = true;
	float first_rough = 0.0f, first_metal = 0.0f;
	for (size_t i = 0; i < n; ++i) {
		uint8_t*    px   = &out->base[i * 4];
		const float c[3] = { px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f };
		const float a    = px[3] / 255.0f;

		uint8_t illum = 0;
		if (has_illum)
			illum = bpp32 ? (t->illum ? t->illum[i] : 0)
						  : (xwau_palette_lit(t->palette, t->pixels[i]) ? 0x7F : 0);
		const float v  = fmaxf(c[0], fmaxf(c[1], c[2]));
		const float w  = illum ? (transparent ? 1.0f : smoothstepf(0.75f, 0.85f, v)) *
									 smoothstepf(0.2f, 0.9f, 5.0f * illum / 255.0f)
							   : 0.0f;
		const float gl = glass ? 1.0f - smoothstepf(0.95f, 1.0f, a) : 0.0f;
		const float s  = lerpf(shadeless * (1.0f - gl), 1.0f, w);

		const float luma =
			0.299f * c[0] * c[0] + 0.587f * c[1] * c[1] + 0.114f * c[2] * c[2]; /* on the decoded color */
		const float blackish = smoothstepf(0.1f, 0.0f, fmaxf(luma, xm->specular_val));
		const float rough    = lerpf(lerpf(roughness, 1.0f, blackish), glass_rough, gl);
		const float metal    = metallic * (1.0f - fmaxf(gl, w));

		uint8_t* e = &out->emissive[i * 4];
		for (int k = 0; k < 3; ++k) {
			e[k]  = unorm8(c[k] * (1.0f + (XWAU_ILLUM_GAIN - 1.0f) * w) * s / XWAU_ILLUM_GAIN);
			px[k] = unorm8(c[k] * (1.0f - s));
		}
		e[3]  = 255;
		px[3] = transparent ? unorm8(glass ? lerpf(a * a, a, 1.0f - gl) : a) : 255;
		any_emissive |= s > 0.0f;
		any_glass |= gl > 0.0f;

		uint8_t* mr = &out->metal_rough[i * 4];
		mr[0]       = 255;
		mr[1]       = unorm8(rough);
		mr[2]       = unorm8(metal);
		mr[3]       = 255;
		if (i == 0) {
			first_rough = rough;
			first_metal = metal;
		} else if (uniform && (mr[1] != out->metal_rough[1] || mr[2] != out->metal_rough[2])) {
			uniform = false;
		}
	}
	if (!any_emissive) {
		free(out->emissive);
		out->emissive = NULL;
	}
	out->metallic  = first_metal;
	out->roughness = first_rough;
	if (uniform) {
		free(out->metal_rough);
		out->metal_rough = NULL;
	}
	out->specular_color = xwau_specular_color(xm->intensity);
	if (glass && any_glass) {
		/* Glass (KHR_materials_transmission): an opaque, thin material whose
		 * base colour B tints the light it transmits, with the dielectric F0
		 * 0.04 of real glass (XWAU's reflectance 1, F0 0.16, was for glass
		 * mostly drawn unlit). XWAU blends the colour C over the background
		 * with the coverage c the bake gave: bg·(1 - c) + C·c. With
		 * B = (1 - c) + c·C (linear) and transmission T = (1 - c) / Y(B),
		 * the spec's bg·T·B + (1 - T)·B gives both terms back for neutral
		 * glass, and the background's luminance otherwise (the runtime tints
		 * by Y(B), the Rec. 709 luminance). */
		out->transmission = (uint8_t*)malloc(n * 4);
		if (!out->transmission) {
			xwau_baked_free(out);
			return false;
		}
		for (size_t i = 0; i < n; ++i) {
			uint8_t*    px    = &out->base[i * 4];
			const float c     = px[3] / 255.0f;
			uint8_t     clear = 0;
			if (px[3] < 255) {
				float colour[3];
				for (int k = 0; k < 3; ++k)
					colour[k] = srgb_to_linear(px[k] / 255.0f);
				const float tint =
					(1.0f - c) + c * (0.2126f * colour[0] + 0.7152f * colour[1] + 0.0722f * colour[2]);
				clear = unorm8((1.0f - c) / tint);
				for (int k = 0; k < 3; ++k)
					px[k] = unorm8(linear_to_srgb((1.0f - c) + c * colour[k]));
			}
			out->transmission[i * 4 + 0] = out->transmission[i * 4 + 1] = out->transmission[i * 4 + 2] =
				clear;
			out->transmission[i * 4 + 3] = 255;
			px[3]                        = 255;
		}
		out->specular_color = 1.0f;
	}
	return true;
}

/* Normal map with NMIntensity baked in: XWAU blends N = lerp(N_vertex,
 * N_mapped, k) in tangent space, i.e. n' = normalize(lerp((0,0,1), n, k)).
 * Output is 0..1-encoded RGB with A = 255. */
static uint8_t* xwau_bake_normal(const OptGltfImageView* map, float k) {
	const size_t n   = (size_t)map->width * (size_t)map->height;
	uint8_t*     out = (uint8_t*)malloc(n * 4);
	if (!out)
		return NULL;
	for (size_t i = 0; i < n; ++i) {
		const uint8_t* s     = &map->rgba[i * 4];
		double         v[3]  = { s[0] / 127.5 - 1.0, s[1] / 127.5 - 1.0, s[2] / 127.5 - 1.0 };
		double         len   = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		double         nv[3] = { 0.0, 0.0, 1.0 };
		if (len > 1e-6) {
			const double b[3] = { k * v[0] / len, k * v[1] / len, (1.0 - k) + k * v[2] / len };
			const double bl   = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
			if (bl > 1e-6) {
				for (int c = 0; c < 3; ++c)
					nv[c] = b[c] / bl;
			}
		}
		for (int c = 0; c < 3; ++c)
			out[i * 4 + c] = unorm8((float)(nv[c] * 0.5 + 0.5));
		out[i * 4 + 3] = 255;
	}
	return out;
}

static int ensure_dir(const char* path) {
	/* mkdir -p semantics for one level; assumes the parent exists. */
	struct stat st;
	if (stat(path, &st) == 0)
		return S_ISDIR(st.st_mode) ? 0 : -1;
#if defined(_WIN32)
	return _mkdir(path);
#else
	return mkdir(path, 0755);
#endif
}

/* ===== Build context ================================================ */

/* Linear allocator for arrays of cgltf structs — we know all the
 * sizes after a counting pass, so one big malloc per array. */
typedef struct {
	/* Buffers (one .bin file with vertices + indices interleaved per
	 * mesh). Byte stream + buffer_views into it + accessors. */
	uint8_t* bin;
	size_t   bin_size;
	size_t   bin_cap;

	cgltf_buffer       buffer;
	cgltf_buffer_view* views;
	size_t             views_count, views_cap;
	cgltf_accessor*    accessors;
	size_t             accessors_count, accessors_cap;
	cgltf_image*       images;
	size_t             images_count;
	cgltf_texture*     textures;
	size_t             textures_count;
	cgltf_sampler      sampler; /* single repeat/linear sampler shared by all */
	cgltf_material*    materials;
	size_t             materials_count;
	/* Per material: its animation (its AERON_flight_model object) and its
	 * screen (a JSON object), or NULL; a material's copies share them. */
	const char**            material_anim;
	const char**            material_screen;
	cgltf_mesh*             meshes;
	size_t                  meshes_count;
	cgltf_primitive*        primitives;
	size_t                  primitives_count, primitives_cap;
	cgltf_attribute*        attributes;
	size_t                  attributes_count, attributes_cap;
	cgltf_material_mapping* mappings;
	size_t                  mappings_count, mappings_cap;
	cgltf_material_variant* variants;
	size_t                  variants_count;
	cgltf_node*             nodes;
	size_t                  nodes_count, nodes_cap;
	cgltf_node**            root_children;
	size_t                  root_children_count;
	cgltf_light*            lights;
	size_t                  lights_count;

	/* Stable strings emitted into the JSON (cgltf doesn't deep-copy on
	 * write — we own the pointer lifetimes). */
	char** owned_strings;
	size_t owned_strings_count, owned_strings_cap;
} GltfBuild;

typedef struct OptGltfOwnedImage {
	uint8_t* rgba;
	uint32_t width;
	uint32_t height;
} OptGltfOwnedImage;

struct OptGltfDocument {
	GltfBuild          gb;
	cgltf_data         data;
	cgltf_scene        scene;
	cgltf_node**       scene_roots;
	OptGltfOwnedImage* image_pixels;
	size_t             image_pixels_count;
};

static void* push_array(void** arr, size_t* count, size_t* cap, size_t elem_size, size_t want) {
	if (*count + want > *cap) {
		/* Reaching this branch means the pre-counting pass missed an
		 * entry. cgltf records pointers (not indices) into these arrays
		 * and writes them by pointer arithmetic at serialization time,
		 * so a realloc here would dangle every pointer captured before
		 * this call. Abort with a diagnostic instead of silently
		 * corrupting the output. */
		fprintf(stderr,
				"opt2gltf: push_array would realloc "
				"(count=%zu + want=%zu > cap=%zu); pre-count missed.\n",
				*count, want, *cap);
		abort();
	}
	void* slot = (char*)*arr + *count * elem_size;
	*count += want;
	return slot;
}

/* owned_strings: realloc-safe pointer vector (cgltf indexes nothing
 * into this; it only references the strings themselves, which we
 * allocate separately and never move). */
static char* gb_keep_string(GltfBuild* gb, char* s) {
	if (gb->owned_strings_count + 1 > gb->owned_strings_cap) {
		size_t ncap  = gb->owned_strings_cap ? gb->owned_strings_cap * 2 : 64;
		char** grown = (char**)realloc(gb->owned_strings, ncap * sizeof(char*));
		if (!grown) {
			fprintf(stderr, "opt2gltf: oom\n");
			abort();
		}
		gb->owned_strings     = grown;
		gb->owned_strings_cap = ncap;
	}
	gb->owned_strings[gb->owned_strings_count++] = s;
	return s;
}

/* Sets the node's AERON_flight_model object, printed; deletes `object`. */
static bool set_flight_extension(GltfBuild* build, cgltf_node* node, cJSON* object) {
	cgltf_extension* extension = (cgltf_extension*)calloc(1, sizeof *extension);
	char*            data      = OptJson_Print(object);
	if (!extension || !data) {
		free(extension);
		free(data);
		return false;
	}
	extension->name        = (char*)"AERON_flight_model";
	extension->data        = gb_keep_string(build, data);
	node->extensions       = extension;
	node->extensions_count = 1;
	return true;
}

/* Append bytes to the .bin buffer with 4-byte alignment as required by
 * the glTF accessor spec for component bounds. Returns the byte offset
 * where the data starts. */
static size_t bin_append(GltfBuild* gb, const void* data, size_t bytes) {
	if (gb->bin_size + bytes + 4 > gb->bin_cap) {
		size_t ncap = gb->bin_cap ? gb->bin_cap * 2 : 4096;
		while (ncap < gb->bin_size + bytes + 4)
			ncap *= 2;
		uint8_t* grown = (uint8_t*)realloc(gb->bin, ncap);
		if (!grown) {
			fprintf(stderr, "opt2gltf: oom\n");
			abort();
		}
		gb->bin     = grown;
		gb->bin_cap = ncap;
	}
	/* 4-byte align (cgltf writes accessor.componentType-specific offsets,
	 * float and uint16 alignments fit inside 4-byte alignment). */
	size_t off = (gb->bin_size + 3) & ~(size_t)3;
	while (gb->bin_size < off)
		gb->bin[gb->bin_size++] = 0;
	memcpy(gb->bin + gb->bin_size, data, bytes);
	gb->bin_size += bytes;
	return off;
}

static cgltf_buffer_view* gb_new_view(GltfBuild* gb, size_t offset, size_t bytes,
									  cgltf_buffer_view_type type) {
	cgltf_buffer_view* v =
		(cgltf_buffer_view*)push_array((void**)&gb->views, &gb->views_count, &gb->views_cap, sizeof *v, 1);
	v->buffer = &gb->buffer;
	v->offset = offset;
	v->size   = bytes;
	v->type   = type;
	return v;
}

static cgltf_accessor* gb_new_accessor(GltfBuild* gb, cgltf_buffer_view* view, cgltf_size count,
									   cgltf_type type, cgltf_component_type comp) {
	cgltf_accessor* a = (cgltf_accessor*)push_array((void**)&gb->accessors, &gb->accessors_count,
													&gb->accessors_cap, sizeof *a, 1);
	a->buffer_view    = view;
	a->offset         = 0;
	a->count          = count;
	a->type           = type;
	a->component_type = comp;
	/* Parsed cgltf graphs derive this during pointer fixup. Our graph is
	 * constructed directly, so populate it explicitly for accessor reads. */
	a->stride = cgltf_calc_size(type, comp);
	return a;
}

/* ===== Switch-state alpha classes =================================== */

/* The renderer partitions a model's indices once into OPAQUE, MASK and BLEND
 * ranges, so every material variant of a primitive must share one alpha
 * class. A switched face group whose state textures differ (an opaque
 * texture beside one with translucent texels) uses, for every state, a copy
 * of its material promoted to the most transparent class among them; an
 * opaque texture draws the same under MASK (alpha 1 passes any cutoff) or
 * BLEND. Copies are shared per (base material, class). A transmissive
 * material draws in the BLEND range (aeron/scene/gltf_mesh.h). */

static int alpha_class_rank(const cgltf_material* m) {
	if (m->alpha_mode == cgltf_alpha_mode_blend ||
		(m->has_transmission && m->transmission.transmission_factor > 0.0f))
		return 2;
	return m->alpha_mode == cgltf_alpha_mode_mask ? 1 : 0;
}

static cgltf_material* promoted_variant_material(GltfBuild* build, cgltf_material* base, int rank,
												 size_t base_count, cgltf_material** cache) {
	const size_t index = (size_t)(base - build->materials);
	if (index >= base_count || alpha_class_rank(base) >= rank)
		return base;
	cgltf_material** slot = &cache[index * 2 + (size_t)(rank - 1)];
	if (*slot)
		return *slot;
	cgltf_material* copy = &build->materials[build->materials_count++];
	*copy                = *base;
	copy->name           = gb_keep_string(
		build, xprintf_dup("%s_%s", base->name ? base->name : "Mat", rank == 2 ? "Blend" : "Mask"));
	if (rank == 2) {
		copy->alpha_mode = cgltf_alpha_mode_blend;
	} else {
		copy->alpha_mode   = cgltf_alpha_mode_mask;
		copy->alpha_cutoff = 0.5f;
	}
	if (build->material_anim) {
		build->material_anim[copy - build->materials]   = build->material_anim[index];
		build->material_screen[copy - build->materials] = build->material_screen[index];
	}
	*slot = copy;
	return copy;
}

/* ===== Per-OPT-mesh emit ============================================ */

/* OPT face data is per-corner-indexed (separate verts/uvs/normals
 * arrays per face vertex). glTF needs (position, normal, uv) tuples
 * per vertex. In stored-normal mode we first reproduce the classic
 * renderer's position-index remap (see built_mesh_from_opt), then
 * dedup-by-tuple within a mesh so UV seams still split vertices while
 * carrying the same effective lighting normal.
 *
 * For the IVFILES set, faces are small (3-4 corners) and dedup keys
 * fit in a small hash. We use an unsorted linear scan — meshes are
 * under a couple thousand vertices, so it's fast enough.
 */

typedef struct {
	float pos[3];
	float nrm[3];
	float uv[2];
	float tan[4]; /* glTF TANGENT (xyz, w); zero until tangents are generated */
} GltfVertex;

typedef struct {
	GltfVertex* vertices;
	uint32_t*   indices; /* per face group, contiguous */
	uint32_t    vertex_count;
	uint32_t    vertex_cap;
	uint32_t    index_count;
	uint32_t    index_cap;
	uint32_t*   fg_index_start;
	uint32_t*   fg_index_count_arr;
	uint32_t    fg_count; /* face groups with non-empty indices */
	/* Defective canonical source normals repaired at emit (see the
	 * classic-remap block in built_mesh_from_opt). */
	uint32_t fixed_zero_normals;
	uint32_t fixed_flipped_normals;
	/* Triangles the OPT repeats (see built_mesh_emit_triangle): left out,
	 * or kept because the repeat has another texture. */
	uint32_t skipped_repeats;
	uint32_t kept_repeats;
	bool     has_tangents; /* vertices carry TANGENT (built_mesh_generate_tangents) */
} BuiltMesh;

static void built_mesh_free(BuiltMesh* bm) {
	free(bm->vertices);
	free(bm->indices);
	free(bm->fg_index_start);
	free(bm->fg_index_count_arr);
	memset(bm, 0, sizeof *bm);
}

static uint32_t built_mesh_intern_vertex(BuiltMesh* bm, const GltfVertex* v) {
	/* Linear dedup. fine for OPT mesh sizes. */
	for (uint32_t i = 0; i < bm->vertex_count; ++i) {
		const GltfVertex* e = &bm->vertices[i];
		if (e->pos[0] == v->pos[0] && e->pos[1] == v->pos[1] && e->pos[2] == v->pos[2] &&
			e->nrm[0] == v->nrm[0] && e->nrm[1] == v->nrm[1] && e->nrm[2] == v->nrm[2] &&
			e->uv[0] == v->uv[0] && e->uv[1] == v->uv[1] && e->tan[0] == v->tan[0] &&
			e->tan[1] == v->tan[1] && e->tan[2] == v->tan[2] && e->tan[3] == v->tan[3])
			return i;
	}
	if (bm->vertex_count >= bm->vertex_cap) {
		uint32_t    ncap  = bm->vertex_cap ? bm->vertex_cap * 2 : 64;
		GltfVertex* grown = (GltfVertex*)realloc(bm->vertices, ncap * sizeof *grown);
		if (!grown) {
			fprintf(stderr, "opt2gltf: oom\n");
			abort();
		}
		bm->vertices   = grown;
		bm->vertex_cap = ncap;
	}
	bm->vertices[bm->vertex_count] = *v;
	return bm->vertex_count++;
}

static void built_mesh_emit_index(BuiltMesh* bm, uint32_t v) {
	if (bm->index_count >= bm->index_cap) {
		uint32_t  ncap  = bm->index_cap ? bm->index_cap * 2 : 128;
		uint32_t* grown = (uint32_t*)realloc(bm->indices, ncap * sizeof *grown);
		if (!grown) {
			fprintf(stderr, "opt2gltf: oom\n");
			abort();
		}
		bm->indices   = grown;
		bm->index_cap = ncap;
	}
	bm->indices[bm->index_count++] = v;
}

/* ===== Angle-based normal regeneration =============================
 *
 * This path deliberately ignores all normals stored in the OPT. Some
 * shipped models contain mutually incompatible per-corner normals, and
 * even their stored face normals need not be derived consistently from
 * the polygon geometry.
 *
 * A smoothing angle describes EDGES, not an arbitrary set of faces that
 * happen to reference the same position. Build the connected smoothing
 * fan containing the current corner by walking shared edges whose
 * geometric face normals meet the threshold. Faces that touch only at a
 * point, or belong to a different fan separated by a hard edge, cannot
 * contaminate the result.
 *
 * Within the fan, weight each geometric face normal by its corner angle.
 * This is invariant under ordinary triangulation of a flat polygon: the
 * diagonal of a two-triangle panel no longer gives one side extra weight.
 * It also avoids making a dense patch of small triangles dominate a
 * neighbouring coarse patch. */
static float* built_mesh_build_corner_normals(const opt_mesh_t* mesh, const opt_lod_t* lod,
											  float smooth_angle_degrees) {
	uint32_t triangle_count = 0;
	for (int32_t group = 0; group < lod->group_count; ++group) {
		const opt_face_group_t* face_group = &lod->groups[group];
		for (int32_t face = 0; face < face_group->face_count; ++face)
			triangle_count += face_group->faces[face].verts[3] >= 0 ? 2u : 1u;
	}
	if (!triangle_count || mesh->vertex_count <= 0)
		return NULL;

	uint32_t* indices = malloc((size_t)triangle_count * 3 * sizeof *indices);
	float*    normals = malloc((size_t)triangle_count * 9 * sizeof *normals);
	if (!indices || !normals) {
		free(indices);
		free(normals);
		return NULL;
	}
	uint32_t triangle = 0;
	for (int32_t group = 0; group < lod->group_count; ++group) {
		const opt_face_group_t* face_group = &lod->groups[group];
		for (int32_t face = 0; face < face_group->face_count; ++face) {
			const opt_face_t* source              = &face_group->faces[face];
			const int         source_corner[2][3] = { { 0, 2, 1 }, { 0, 3, 2 } };
			const int         count               = source->verts[3] >= 0 ? 2 : 1;
			for (int item = 0; item < count; ++item, ++triangle) {
				for (int corner = 0; corner < 3; ++corner) {
					const int32_t vertex = source->verts[source_corner[item][corner]];
					if (vertex < 0 || vertex >= mesh->vertex_count) {
						free(indices);
						free(normals);
						return NULL;
					}
					indices[(size_t)triangle * 3 + corner] = (uint32_t)vertex;
				}
			}
		}
	}
	AeronMeshNormalsError       error = { 0 };
	const AeronMeshNormalsInput input = {
		.positions                 = &mesh->vertices[0].x,
		.position_stride           = sizeof mesh->vertices[0],
		.position_count            = (uint32_t)mesh->vertex_count,
		.triangle_position_indices = indices,
		.triangle_count            = triangle_count,
		.smooth_angle_degrees      = smooth_angle_degrees,
	};
	const bool built = Aeron_MeshNormalsBuildCorners(&input, normals, &error);
	free(indices);
	if (!built) {
		fprintf(stderr, "opt2gltf: normal build failed: %s\n", error.message);
		free(normals);
		return NULL;
	}
	return normals;
}

static uint32_t built_mesh_emit_corner(BuiltMesh* bm, const opt_mesh_t* m, int32_t vi, int32_t ui,
									   const opt_vec3_t* normal, float meters_per_opt_unit) {
	const opt_vec3_t* p  = &m->vertices[vi];
	const opt_vec2_t* uv = (ui >= 0 && ui < m->uv_count) ? &m->uvs[ui] : NULL;
	GltfVertex        gv = { 0 };
	swap_axis_v3(p, gv.pos);
	gv.pos[0] *= meters_per_opt_unit;
	gv.pos[1] *= meters_per_opt_unit;
	gv.pos[2] *= meters_per_opt_unit;
	swap_axis_v3(normal, gv.nrm);
	/* Stored OPT normals are decoded from Q15 and may be slightly off
	 * unit length; regenerated normals are already unit length, but use
	 * the same final normalization for both paths. */
	float nl = sqrtf(gv.nrm[0] * gv.nrm[0] + gv.nrm[1] * gv.nrm[1] + gv.nrm[2] * gv.nrm[2]);
	if (nl > 1e-9f) {
		gv.nrm[0] /= nl;
		gv.nrm[1] /= nl;
		gv.nrm[2] /= nl;
	}
	if (uv) {
		gv.uv[0] = uv->u;
		gv.uv[1] = uv->v;
	}
	return built_mesh_intern_vertex(bm, &gv);
}

/* The triangles a mesh has emitted, by their corners (rotated to start at
 * the smallest index, so a reversed triangle, the back of a two-sided
 * surface, is another key), with the face group that emitted each. */
typedef struct {
	uint32_t v[3];
	int32_t  group; /* -1: empty slot */
} SeenTriangle;

typedef struct {
	SeenTriangle* slots;
	uint32_t      cap; /* a power of 2 */
} TriangleSet;

static bool triangle_set_init(TriangleSet* set, const opt_lod_t* lod) {
	uint32_t triangles = 0;
	for (int32_t g = 0; g < lod->group_count; ++g)
		for (int32_t f = 0; f < lod->groups[g].face_count; ++f)
			triangles += lod->groups[g].faces[f].verts[3] >= 0 ? 2u : 1u;
	set->cap = 16;
	while (set->cap < triangles * 2u)
		set->cap *= 2;
	set->slots = (SeenTriangle*)malloc(set->cap * sizeof *set->slots);
	for (uint32_t i = 0; set->slots && i < set->cap; ++i)
		set->slots[i].group = -1;
	return set->slots != NULL;
}

/* The slot of triangle `v` (rotated), found or empty. */
static SeenTriangle* triangle_set_slot(TriangleSet* set, const uint32_t v[3]) {
	uint32_t hash = (v[0] * 73856093u) ^ (v[1] * 19349663u) ^ (v[2] * 83492791u);
	for (uint32_t i = hash & (set->cap - 1);; i = (i + 1) & (set->cap - 1)) {
		SeenTriangle* slot = &set->slots[i];
		if (slot->group < 0 || (slot->v[0] == v[0] && slot->v[1] == v[1] && slot->v[2] == v[2]))
			return slot;
	}
}

static bool same_textures(const opt_face_group_t* a, const opt_face_group_t* b) {
	if (a->texture_index != b->texture_index || a->state_count != b->state_count)
		return false;
	for (int32_t s = 0; a->state_textures && b->state_textures && s < a->state_count; ++s)
		if (a->state_textures[s] != b->state_textures[s])
			return false;
	return true;
}

/* Emits triangle (a, b, c) of face group `g`, unless the mesh already has
 * it with the same textures: some shipped OPTs list a face twice, which
 * draws the same pixels, and which Blender cannot hold (two faces on the
 * same vertices). A repeat with another texture is kept: which one shows
 * is not data to drop. */
static void built_mesh_emit_triangle(BuiltMesh* bm, TriangleSet* seen, const opt_lod_t* lod, int32_t g,
									 uint32_t a, uint32_t b, uint32_t c) {
	uint32_t v[3] = { a, b, c };
	while (v[0] > v[1] || v[0] > v[2]) {
		const uint32_t first = v[0];
		v[0]                 = v[1];
		v[1]                 = v[2];
		v[2]                 = first;
	}
	SeenTriangle* slot = triangle_set_slot(seen, v);
	if (slot->group >= 0) {
		if (same_textures(&lod->groups[slot->group], &lod->groups[g])) {
			bm->skipped_repeats++;
			return;
		}
		bm->kept_repeats++;
	} else {
		memcpy(slot->v, v, sizeof v);
		slot->group = g;
	}
	built_mesh_emit_index(bm, a);
	built_mesh_emit_index(bm, b);
	built_mesh_emit_index(bm, c);
}

/* Walk LOD 0 of an OPT mesh and emit deduplicated vertices + indices
 * grouped by face group. Skips degenerate faces and empty groups.
 * `regen` enables angle-based normal regeneration; when false the stored
 * OPT normals are used. */
static bool built_mesh_from_opt(BuiltMesh* bm, const opt_mesh_t* m, float meters_per_opt_unit, bool regen,
								float smooth_angle_degrees, bool repair_normals) {
	memset(bm, 0, sizeof *bm);
	if (m->lod_count <= 0)
		return false;
	const opt_lod_t* lod = &m->lods[0];
	if (lod->group_count <= 0)
		return false;

	bm->fg_index_start     = (uint32_t*)calloc(lod->group_count, sizeof(uint32_t));
	bm->fg_index_count_arr = (uint32_t*)calloc(lod->group_count, sizeof(uint32_t));
	if (!bm->fg_index_start || !bm->fg_index_count_arr)
		return false;

	TriangleSet seen = { 0 };
	if (!triangle_set_init(&seen, lod)) {
		built_mesh_free(bm);
		return false;
	}
	float* corner_normals = regen ? built_mesh_build_corner_normals(m, lod, smooth_angle_degrees) : NULL;
	if (regen && !corner_normals) {
		free(seen.slots);
		built_mesh_free(bm);
		return false;
	}

	/* The classic renderer resets g_vertexRemap for every FaceData group,
	 * then computes lighting only when a POSITION index is first seen in
	 * the visible, file-ordered face list. Later corners with the same
	 * position reuse that already-lit vertex even when their normal index
	 * differs. Many shipped OPTs rely on this: the ignored corner normals
	 * can disagree by more than 90 degrees across the diagonal of an
	 * otherwise coplanar two-triangle panel.
	 *
	 * A static glTF cannot reproduce the view-dependent "first visible"
	 * choice at a non-coplanar junction. File order is exact for coplanar
	 * pairs, however, because both faces have identical culling, and is the
	 * closest stable representation elsewhere. Keep repair_normals false as
	 * the raw per-corner diagnostic path, so this compatibility remap is
	 * enabled together with the normal sanitization default. */
	opt_vec3_t* classic_normals    = NULL;
	uint8_t*    classic_normal_set = NULL;
	if (!regen && repair_normals && m->vertex_count > 0) {
		classic_normals    = (opt_vec3_t*)calloc((size_t)m->vertex_count, sizeof *classic_normals);
		classic_normal_set = (uint8_t*)calloc((size_t)m->vertex_count, sizeof *classic_normal_set);
		if (!classic_normals || !classic_normal_set) {
			free(classic_normals);
			free(classic_normal_set);
			free(corner_normals);
			free(seen.slots);
			return false;
		}
	}

	int32_t smooth_face_index = 0;
	for (int32_t g = 0; g < lod->group_count; ++g) {
		const opt_face_group_t* grp = &lod->groups[g];
		if (grp->face_count <= 0)
			continue;
		if (classic_normal_set) {
			memset(classic_normal_set, 0, (size_t)m->vertex_count);
		}
		uint32_t start = bm->index_count;
		for (int32_t fi = 0; fi < grp->face_count; ++fi) {
			const opt_face_t* f       = &grp->faces[fi];
			int               corners = (f->verts[3] >= 0) ? 4 : 3;

			if (regen) {
				int source_corner[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
				int triangle_count      = (corners == 4) ? 2 : 1;
				for (int t = 0; t < triangle_count; ++t, ++smooth_face_index) {
					uint32_t v[3] = { 0 };
					int      ok   = 1;
					for (int q = 0; q < 3; ++q) {
						int     k  = source_corner[t][q];
						int32_t vi = f->verts[k];
						if (vi < 0 || vi >= m->vertex_count) {
							ok = 0;
							break;
						}
						static const int normal_corner[3] = { 0, 2, 1 };
						const float*     normal =
							&corner_normals[((size_t)smooth_face_index * 3 + normal_corner[q]) * 3];
						opt_vec3_t n = { normal[0], normal[1], normal[2] };
						v[q]         = built_mesh_emit_corner(bm, m, vi, f->uvs[k], &n, meters_per_opt_unit);
					}
					if (!ok)
						continue;
					built_mesh_emit_triangle(bm, &seen, lod, g, v[0], v[1], v[2]);
				}
				continue;
			}

			uint32_t v[4] = { 0 };
			int      ok   = 1;
			for (int k = 0; k < corners; ++k) {
				int32_t vi = f->verts[k];
				int32_t ni = f->normals[k];
				if (vi < 0 || vi >= m->vertex_count) {
					ok = 0;
					break;
				}
				const opt_vec3_t* n = (ni >= 0 && ni < m->normal_count) ? &m->normals[ni] : &f->face_normal;
				if (classic_normal_set) {
					if (!classic_normal_set[vi]) {
						/* Sanitize only the canonical, first-use normal.
						 * Later per-corner normals are ignored by the
						 * classic renderer and must not be repaired
						 * independently: doing so turns them into real
						 * glTF seams. */
						double nlen2 = (double)n->x * n->x + (double)n->y * n->y + (double)n->z * n->z;
						if (nlen2 < 1e-12) {
							classic_normals[vi] = f->face_normal;
							bm->fixed_zero_normals++;
						} else {
							double d = (double)n->x * f->face_normal.x + (double)n->y * f->face_normal.y +
									   (double)n->z * f->face_normal.z;
							if (d < 0.0) {
								classic_normals[vi] = (opt_vec3_t) { -n->x, -n->y, -n->z };
								bm->fixed_flipped_normals++;
							} else {
								classic_normals[vi] = *n;
							}
						}
						classic_normal_set[vi] = 1;
					}
					n = &classic_normals[vi];
				}
				v[k] = built_mesh_emit_corner(bm, m, vi, f->uvs[k], n, meters_per_opt_unit);
			}
			if (!ok)
				continue;
			built_mesh_emit_triangle(bm, &seen, lod, g, v[0], v[1], v[2]);
			if (corners == 4)
				built_mesh_emit_triangle(bm, &seen, lod, g, v[0], v[2], v[3]);
		}
		bm->fg_index_start[g]     = start;
		bm->fg_index_count_arr[g] = bm->index_count - start;
		if (bm->fg_index_count_arr[g] > 0)
			bm->fg_count++;
	}
	free(corner_normals);
	free(classic_normals);
	free(classic_normal_set);
	free(seen.slots);
	return bm->index_count > 0;
}

/* ===== Tangents ======================================================
 *
 * MikkTSpace over the mesh's final triangle list, in the glTF frame. glTF
 * puts the texture's first row at v = 0 and wants the bitangent
 * cross(normal, tangent) * w to point toward that row (+Y of a normal map is
 * "up" in the image), so MikkTSpace sees (u, 1 - v): its bitangent then
 * follows decreasing v and its sign is the glTF w. Vertices are re-interned
 * with their tangent, splitting where corner frames differ. */

typedef struct TangentBuild {
	const BuiltMesh* bm;
	float*           corner_tangents; /* index_count * 4 */
} TangentBuild;

static const GltfVertex* tangent_corner(const SMikkTSpaceContext* ctx, int face, int vert) {
	const TangentBuild* tb = (const TangentBuild*)ctx->m_pUserData;
	return &tb->bm->vertices[tb->bm->indices[(size_t)face * 3 + (size_t)vert]];
}

static int tangent_face_count(const SMikkTSpaceContext* ctx) {
	return (int)(((const TangentBuild*)ctx->m_pUserData)->bm->index_count / 3);
}

static int tangent_face_vertices(const SMikkTSpaceContext* ctx, const int face) {
	(void)ctx;
	(void)face;
	return 3;
}

static void tangent_position(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
	memcpy(out, tangent_corner(ctx, face, vert)->pos, 3 * sizeof(float));
}

static void tangent_normal(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
	memcpy(out, tangent_corner(ctx, face, vert)->nrm, 3 * sizeof(float));
}

static void tangent_texcoord(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
	const GltfVertex* v = tangent_corner(ctx, face, vert);
	out[0]              = v->uv[0];
	out[1]              = 1.0f - v->uv[1];
}

static void tangent_store(const SMikkTSpaceContext* ctx, const float tangent[], const float sign,
						  const int face, const int vert) {
	float* out = &((TangentBuild*)ctx->m_pUserData)->corner_tangents[((size_t)face * 3 + (size_t)vert) * 4];
	if (isfinite(tangent[0]) && isfinite(tangent[1]) && isfinite(tangent[2])) {
		memcpy(out, tangent, 3 * sizeof(float));
	} else {
		out[0] = 1.0f;
		out[1] = out[2] = 0.0f;
	}
	out[3] = sign < 0.0f ? -1.0f : 1.0f;
}

static bool built_mesh_generate_tangents(BuiltMesh* bm) {
	if (bm->index_count == 0)
		return true;
	TangentBuild tb = { bm, (float*)calloc((size_t)bm->index_count * 4, sizeof(float)) };
	if (!tb.corner_tangents)
		return false;
	SMikkTSpaceInterface iface = {
		.m_getNumFaces          = tangent_face_count,
		.m_getNumVerticesOfFace = tangent_face_vertices,
		.m_getPosition          = tangent_position,
		.m_getNormal            = tangent_normal,
		.m_getTexCoord          = tangent_texcoord,
		.m_setTSpaceBasic       = tangent_store,
	};
	SMikkTSpaceContext ctx = { &iface, &tb };
	if (!genTangSpaceDefault(&ctx)) {
		free(tb.corner_tangents);
		return false;
	}
	BuiltMesh rebuilt = { 0 };
	for (uint32_t k = 0; k < bm->index_count; ++k) {
		GltfVertex v = bm->vertices[bm->indices[k]];
		memcpy(v.tan, &tb.corner_tangents[(size_t)k * 4], sizeof v.tan);
		bm->indices[k] = built_mesh_intern_vertex(&rebuilt, &v);
	}
	free(tb.corner_tangents);
	free(bm->vertices);
	bm->vertices     = rebuilt.vertices;
	bm->vertex_count = rebuilt.vertex_count;
	bm->vertex_cap   = rebuilt.vertex_cap;
	bm->has_tangents = true;
	return true;
}

static bool valid_component_tag(const char* tag) {
	const size_t length = tag ? strlen(tag) : 0;
	if (length == 0 || length > 63)
		return false;
	for (size_t i = 0; i < length; ++i) {
		const char c = tag[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_'))
			return false;
	}
	return true;
}

/* ===== Conversion ==================================================== */

typedef struct AlphaHistogram {
	uint64_t zero_count;
	uint64_t full_count;
	uint64_t intermediate_count;
} AlphaHistogram;

static OptGltfAlphaMode classify_alpha(const uint8_t* alpha, size_t sample_count, AlphaHistogram* histogram) {
	AlphaHistogram result = { 0, 0, 0 };
	if (!alpha || sample_count == 0) {
		if (histogram)
			*histogram = result;
		return OPT_GLTF_ALPHA_OPAQUE;
	}

	for (size_t i = 0; i < sample_count; ++i) {
		result.zero_count += alpha[i] == 0u;
		result.full_count += alpha[i] == 255u;
	}
	result.intermediate_count = (uint64_t)sample_count - result.zero_count - result.full_count;
	if (histogram)
		*histogram = result;
	if (result.full_count == sample_count)
		return OPT_GLTF_ALPHA_OPAQUE;

	const uint64_t endpoint_count = result.zero_count + result.full_count;
	if (result.zero_count != 0 && result.full_count != 0 &&
		result.intermediate_count <= endpoint_count / 9u) {
		return OPT_GLTF_ALPHA_MASK;
	}
	return OPT_GLTF_ALPHA_BLEND;
}

OptGltfAlphaMode OptGltf_ClassifyAlpha(const uint8_t* alpha, size_t sample_count) {
	return classify_alpha(alpha, sample_count, NULL);
}

static int alpha_name_equal(const char* a, const char* b) {
	if (!a || !b)
		return 0;
	while (*a && *b) {
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if (ca >= 'a' && ca <= 'z')
			ca = (unsigned char)(ca - ('a' - 'A'));
		if (cb >= 'a' && cb <= 'z')
			cb = (unsigned char)(cb - ('a' - 'A'));
		if (ca != cb)
			return 0;
	}
	return *a == *b;
}

static const OptGltfAlphaOverride* find_alpha_override(const OptGltfBuildOptions* options,
													   const opt_texture_t* texture, int32_t texture_index) {
	if (!options || !options->alpha_overrides || options->alpha_override_count == 0)
		return NULL;
	char generated_name[32];
	snprintf(generated_name, sizeof generated_name, "Tex%02d", (int)texture_index);
	for (size_t i = 0; i < options->alpha_override_count; ++i) {
		const OptGltfAlphaOverride* override = &options->alpha_overrides[i];
		if (alpha_name_equal(override->texture_name, texture->name) ||
			alpha_name_equal(override->texture_name, generated_name)) {
			return override;
		}
	}
	return NULL;
}

static size_t alpha_override_match_count(const OptGltfBuildOptions* options, const opt_texture_t* texture,
										 int32_t texture_index) {
	if (!options || !options->alpha_overrides)
		return 0;
	char generated_name[32];
	snprintf(generated_name, sizeof generated_name, "Tex%02d", (int)texture_index);
	size_t count = 0;
	for (size_t i = 0; i < options->alpha_override_count; ++i) {
		const char* name = options->alpha_overrides[i].texture_name;
		count += alpha_name_equal(name, texture->name) || alpha_name_equal(name, generated_name);
	}
	return count;
}

static const char* alpha_mode_name(OptGltfAlphaMode mode) {
	switch (mode) {
		case OPT_GLTF_ALPHA_OPAQUE:
			return "opaque";
		case OPT_GLTF_ALPHA_MASK:
			return "mask";
		case OPT_GLTF_ALPHA_BLEND:
			return "blend";
		default:
			return "invalid";
	}
}

static bool validate_alpha_overrides(const OptGltfBuildOptions* options, opt_error_t* error) {
	if (!options || options->alpha_override_count == 0)
		return true;
	if (!options->alpha_overrides) {
		if (error)
			snprintf(error->msg, sizeof error->msg, "alpha override count has no records");
		return false;
	}
	for (size_t i = 0; i < options->alpha_override_count; ++i) {
		const OptGltfAlphaOverride* item = &options->alpha_overrides[i];
		if (!item->texture_name || !item->texture_name[0] || item->alpha_mode < OPT_GLTF_ALPHA_OPAQUE ||
			item->alpha_mode > OPT_GLTF_ALPHA_BLEND || !isfinite(item->alpha_cutoff) ||
			item->alpha_cutoff < 0.0f || item->alpha_cutoff > 1.0f) {
			if (error)
				snprintf(error->msg, sizeof error->msg, "invalid alpha override %zu", i);
			return false;
		}
		for (size_t previous = 0; previous < i; ++previous) {
			if (alpha_name_equal(item->texture_name, options->alpha_overrides[previous].texture_name)) {
				if (error)
					snprintf(error->msg, sizeof error->msg, "duplicate alpha override '%s'",
							 item->texture_name);
				return false;
			}
		}
	}
	return true;
}

/* Register an owned RGBA image as image and texture `slot`, written to
 * textures/<file>. Takes ownership of `rgba`, `name` and `file`. */
static cgltf_texture* document_set_image(OptGltfDocument* document, size_t slot, uint8_t* rgba,
										 uint32_t width, uint32_t height, char* name, char* file) {
	GltfBuild* build                    = &document->gb;
	document->image_pixels[slot].rgba   = rgba;
	document->image_pixels[slot].width  = width;
	document->image_pixels[slot].height = height;
	build->images[slot].name            = gb_keep_string(build, name);
	build->images[slot].uri             = gb_keep_string(build, xprintf_dup("textures/%s", file));
	free(file);
	build->images[slot].mime_type = "image/png";
	build->textures[slot].image   = &build->images[slot];
	build->textures[slot].sampler = &build->sampler;
	build->textures[slot].name    = build->images[slot].name;
	return &build->textures[slot];
}

/* KHR_materials_specular for an XWAU dielectric F0 = 0.04 * color. */
static void xwau_set_specular(cgltf_material* mat, float specular_color) {
	mat->has_specular                      = 1;
	mat->specular.specular_factor          = 1.0f;
	mat->specular.specular_color_factor[0] = specular_color;
	mat->specular.specular_color_factor[1] = specular_color;
	mat->specular.specular_color_factor[2] = specular_color;
}

/* ===== Screen surfaces ============================================== */

/* A material's animation. */
static void anim_apply(OptGltfDocument* document, cgltf_material* mat, const OptGltfXwauAnim* anim) {
	document->gb.material_anim[mat - document->gb.materials] = anim->json;
}

/* The material's AERON_flight_model `screen` (owned), or NULL. */
static char* screen_json(const OptGltfScreen* s) {
	cJSON* object = cJSON_CreateObject();
	bool   ok     = OptJson_Add(object, "cover", cJSON_CreateBool(s->cover_base.rgba && !s->hologram)) &&
					OptJson_Add(object, "transparent", cJSON_CreateBool(s->transparent)) &&
					OptJson_Add(object, "alwaysVisible", cJSON_CreateBool(s->always_visible)) &&
					OptJson_Add(object, "hologram", cJSON_CreateBool(s->hologram)) &&
					OptJson_Add(object, "noisy", cJSON_CreateBool(s->noisy)) &&
					OptJson_Add(object, "hide", cJSON_CreateNumber(s->hide));
	cJSON* rects  = ok ? OptJson_AddArray(object, "rects") : NULL;
	ok            = rects != NULL;
	for (uint32_t i = 0; ok && i < s->rect_count; ++i) {
		const OptGltfScreenRect* r    = &s->rects[i];
		cJSON*                   rect = OptJson_AddObject(rects, NULL);
		ok                            = OptJson_Add(rect, "source", cJSON_CreateNumber(r->source)) &&
										OptJson_Add(rect, "dst", OptJson_Floats(r->dst, 4)) &&
										OptJson_Add(rect, "bg", OptJson_Floats(r->bg, 3)) &&
										OptJson_Add(rect, "highlight", OptJson_Floats(r->highlight, 3)) &&
										OptJson_Add(rect, "lock", OptJson_Floats(r->lock, 3)) &&
										OptJson_Add(rect, "gain", OptJson_Float(r->gain)) &&
										OptJson_Add(rect, "layers", cJSON_CreateNumber(r->layers));
		if (ok && (r->layers & OPT_GLTF_SCREEN_LAYER_BAR))
			ok = OptJson_Add(rect, "bar", OptJson_Floats(r->bar, 3));
	}
	if (!ok) {
		cJSON_Delete(object);
		return NULL;
	}
	return OptJson_Print(object);
}

/* Rewrite the material of a screen texture (opt2gltf.h, "Screen surfaces").
 * `cover_base` and `cover_emissive` are the cover's textures, or NULL. */
static bool screen_apply(GltfBuild* build, cgltf_material* mat, const OptGltfScreen* s,
						 cgltf_texture* cover_base, cgltf_texture* cover_emissive) {
	char* screen = screen_json(s);
	if (!screen)
		return false;
	build->material_screen[mat - build->materials] = gb_keep_string(build, screen);

	mat->normal_texture.texture                                    = NULL;
	mat->pbr_metallic_roughness.metallic_roughness_texture.texture = NULL;
	mat->pbr_metallic_roughness.metallic_factor                    = 0.0f;
	mat->pbr_metallic_roughness.roughness_factor                   = 1.0f;
	const bool cover                                               = cover_base && !s->hologram;
	for (int channel = 0; channel < 3; ++channel) {
		mat->pbr_metallic_roughness.base_color_factor[channel] = cover ? 1.0f : 0.0f;
		mat->emissive_factor[channel]                          = cover && cover_emissive ? 1.0f : 0.0f;
	}
	mat->pbr_metallic_roughness.base_color_factor[3]       = 1.0f;
	mat->pbr_metallic_roughness.base_color_texture.texture = cover ? cover_base : NULL;
	mat->emissive_texture.texture                          = cover ? cover_emissive : NULL;
	mat->has_emissive_strength                             = cover && cover_emissive;
	mat->emissive_strength.emissive_strength               = cover && cover_emissive ? XWAU_ILLUM_GAIN : 1.0f;
	mat->alpha_mode   = s->transparent || s->hologram ? cgltf_alpha_mode_blend : cgltf_alpha_mode_opaque;
	mat->alpha_cutoff = 0.5f;
	return true;
}

/* ===== Faceless meshes ============================================== */

static int32_t opt_mesh_lod0_face_count(const opt_mesh_t* m) {
	int32_t faces = 0;
	if (m->lod_count > 0) {
		for (int32_t g = 0; g < m->lods[0].group_count; ++g)
			faces += m->lods[0].groups[g].face_count;
	}
	return faces;
}

/* Some shipped OPTs carry meshes with vertices but no LOD 0 faces
 * (Interdictor2Hangar's meshes 2 and 3: two vertices, no LOD), which the
 * game loads as components without geometry. Each GLB component needs a
 * mesh, and the flight-model topology keeps the positions of a component
 * whose triangles are all degenerate while dropping the triangles, so such
 * a mesh is converted as one untextured degenerate triangle over its own
 * vertices: the same positions and no faces, and nothing drawn. */
typedef struct FacelessPatch {
	opt_file_t        file;
	opt_mesh_t*       meshes;
	opt_lod_t*        lods;
	opt_face_group_t* groups;
	opt_face_t*       faces;
} FacelessPatch;

static void faceless_patch_free(FacelessPatch* patch) {
	free(patch->meshes);
	free(patch->lods);
	free(patch->groups);
	free(patch->faces);
}

/* Returns `opt` when nothing needs patching, the patched copy otherwise, or
 * NULL when out of memory. */
static const opt_file_t* faceless_patch_apply(const opt_file_t* opt, FacelessPatch* patch) {
	memset(patch, 0, sizeof *patch);
	int32_t count = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
		count += opt->meshes[mi].vertex_count > 0 && opt_mesh_lod0_face_count(&opt->meshes[mi]) == 0;
	if (count == 0)
		return opt;
	patch->file   = *opt;
	patch->meshes = (opt_mesh_t*)malloc((size_t)opt->mesh_count * sizeof *patch->meshes);
	patch->lods   = (opt_lod_t*)calloc((size_t)count, sizeof *patch->lods);
	patch->groups = (opt_face_group_t*)calloc((size_t)count, sizeof *patch->groups);
	patch->faces  = (opt_face_t*)calloc((size_t)count, sizeof *patch->faces);
	if (!patch->meshes || !patch->lods || !patch->groups || !patch->faces) {
		faceless_patch_free(patch);
		return NULL;
	}
	memcpy(patch->meshes, opt->meshes, (size_t)opt->mesh_count * sizeof *patch->meshes);
	int32_t next = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		opt_mesh_t* m = &patch->meshes[mi];
		if (m->vertex_count <= 0 || opt_mesh_lod0_face_count(m) != 0)
			continue;
		const int32_t     last  = m->vertex_count > 1 ? 1 : 0;
		opt_face_t*       face  = &patch->faces[next];
		opt_face_group_t* group = &patch->groups[next];
		opt_lod_t*        lod   = &patch->lods[next];
		for (int k = 0; k < 4; ++k) {
			face->verts[k]   = k == 0 ? 0 : k < 3 ? last : -1;
			face->edges[k]   = -1;
			face->uvs[k]     = -1;
			face->normals[k] = -1;
		}
		face->face_normal    = (opt_vec3_t) { 0.0f, 0.0f, 1.0f };
		group->texture_index = -1;
		group->state_count   = 1;
		group->face_count    = 1;
		group->faces         = face;
		lod->group_count     = 1;
		lod->groups          = group;
		m->lod_count         = 1;
		m->lods              = lod;
		fprintf(stderr, "opt2gltf: mesh %d has %d vertices and no LOD 0 faces; converted without faces\n",
				(int)mi, (int)m->vertex_count);
		++next;
	}
	patch->file.meshes = patch->meshes;
	return &patch->file;
}

/* ===== XWAU skins ==================================================== */

enum { SKIN_COMPOSITE_IMAGES = 5 };

/* One composite: texture `texture` with the effective chain `chain` (the
 * layers of a variant that have an image for it), shared by `variants`. */
typedef struct SkinComposite {
	int32_t           texture;
	OptGltfSkinLayer* chain;
	size_t            chain_count;
	XwauBaked         baked;
	/* The bake's emissive / metallic-roughness / transmission equal the
	 * texture's own: the composite material keeps the texture's (baked
	 * copies freed). */
	bool      same_emissive;
	bool      same_metal_rough;
	bool      same_transmission;
	uint32_t* variants; /* skin variant indices, ascending */
	size_t    variant_count;
	/* Image slots only this composite uses (base, emissive,
	 * metallic-roughness, unused, transmission); -1. */
	int32_t  images[SKIN_COMPOSITE_IMAGES];
	uint8_t* colour; /* colours before the bake, for xwau_anim_base */
	size_t   image_count;
} SkinComposite;

typedef struct SkinState {
	SkinComposite* composites;
	size_t         count;
	/* [skin variant * texture_count + texture]: composite index or -1. */
	int32_t* composite_of;
} SkinState;

static void skin_state_free(SkinState* state) {
	for (size_t c = 0; c < state->count; ++c) {
		free(state->composites[c].chain);
		free(state->composites[c].variants);
		free(state->composites[c].colour);
		xwau_baked_free(&state->composites[c].baked);
	}
	free(state->composites);
	free(state->composite_of);
	memset(state, 0, sizeof *state);
}

/* The hook's CombineTextures: straight alpha in integer arithmetic onto the
 * color channels; the destination alpha is kept. */
static void skin_blend(uint8_t* dst, const OptGltfImageView* src, uint32_t opacity) {
	const size_t n = (size_t)src->width * src->height;
	for (size_t i = 0; i < n; ++i) {
		const int a = ((int)src->rgba[i * 4 + 3] * (int)opacity + 50) / 100;
		for (int k = 0; k < 3; ++k)
			dst[i * 4 + k] = (uint8_t)(dst[i * 4 + k] * (255 - a) / 255 + src->rgba[i * 4 + k] * a / 255);
	}
}

static bool skin_chain_equal(const SkinComposite* c, int32_t texture, const OptGltfSkinLayer* chain,
							 size_t count) {
	if (c->texture != texture || c->chain_count != count)
		return false;
	for (size_t i = 0; i < count; ++i)
		if (c->chain[i].skin != chain[i].skin || c->chain[i].opacity != chain[i].opacity)
			return false;
	return true;
}

/* Whether some skin variant has a composite of texture `texture`. */
static bool skin_texture_has_composite(const SkinState* state, size_t variant_count, int32_t texture_count,
									   int32_t texture) {
	if (texture < 0 || texture >= texture_count)
		return false;
	for (size_t v = 0; v < variant_count; ++v)
		if (state->composite_of[v * (size_t)texture_count + (size_t)texture] >= 0)
			return true;
	return false;
}

static bool skin_validate(const OptGltfSkins* skins, opt_error_t* error) {
	for (size_t v = 0; v < skins->variant_count; ++v) {
		const OptGltfSkinVariant* variant = &skins->variants[v];
		bool                      ok      = variant->name && variant->name[0];
		for (size_t w = 0; ok && w < v; ++w)
			ok = strcmp(skins->variants[w].name, variant->name) != 0;
		for (size_t l = 0; ok && l < variant->layer_count; ++l)
			ok = variant->layers[l].skin < skins->skin_count && variant->layers[l].opacity <= 100;
		if (!ok) {
			if (error)
				snprintf(error->msg, sizeof error->msg, "invalid or duplicate skin variant %zu '%s'", v,
						 variant->name ? variant->name : "");
			return false;
		}
	}
	for (size_t m = 0; m < skins->markings_count; ++m) {
		const char* name  = skins->default_markings[m];
		bool        found = name == NULL;
		for (size_t v = 0; !found && v < skins->variant_count; ++v)
			found = strcmp(skins->variants[v].name, name) == 0;
		if (!found) {
			if (error)
				snprintf(error->msg, sizeof error->msg, "markings %zu names unknown skin variant '%s'", m,
						 name);
			return false;
		}
	}
	return true;
}

/* Finds and bakes the composites. `baked` holds each texture's own bake;
 * textures without pixels and screen surfaces are not skinned. */
static bool skin_state_build(const opt_file_t* opt, const OptGltfSkins* skins, const OptGltfScreen* screens,
							 int base_shade, const OptGltfXwauMaterial* xwau, const OptGltfXwauAnim* anim,
							 const XwauBaked* baked, SkinState* state) {
	memset(state, 0, sizeof *state);
	const size_t texture_count = (size_t)opt->texture_count;
	bool*        skinnable     = (bool*)calloc(texture_count ? texture_count : 1, sizeof *skinnable);
	state->composite_of =
		(int32_t*)malloc((skins->variant_count * texture_count ? skins->variant_count * texture_count : 1) *
						 sizeof *state->composite_of);
	size_t max_layers = 1;
	for (size_t v = 0; v < skins->variant_count; ++v)
		if (skins->variants[v].layer_count > max_layers)
			max_layers = skins->variants[v].layer_count;
	OptGltfSkinLayer* chain = (OptGltfSkinLayer*)malloc(max_layers * sizeof *chain);
	if (!skinnable || !state->composite_of || !chain)
		goto fail;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_lod_t* lod = &opt->meshes[mi].lods[0];
		for (int32_t g = 0; g < lod->group_count; ++g) {
			const int32_t t = lod->groups[g].texture_index;
			if (lod->groups[g].face_count > 0 && t >= 0 && t < opt->texture_count && baked[t].base &&
				!(screens && screens[t].present))
				skinnable[t] = true;
		}
	}
	for (size_t v = 0; v < skins->variant_count; ++v) {
		const OptGltfSkinVariant* variant = &skins->variants[v];
		for (size_t t = 0; t < texture_count; ++t) {
			int32_t* slot = &state->composite_of[v * texture_count + t];
			*slot         = -1;
			if (!skinnable[t])
				continue;
			size_t count = 0;
			for (size_t l = 0; l < variant->layer_count; ++l)
				if (skins->images[variant->layers[l].skin * texture_count + t].rgba)
					chain[count++] = variant->layers[l];
			if (count == 0)
				continue;
			size_t c = 0;
			while (c < state->count && !skin_chain_equal(&state->composites[c], (int32_t)t, chain, count))
				++c;
			if (c == state->count) {
				SkinComposite* grown =
					(SkinComposite*)realloc(state->composites, (state->count + 1) * sizeof *grown);
				if (!grown)
					goto fail;
				state->composites    = grown;
				SkinComposite* added = &grown[state->count++];
				memset(added, 0, sizeof *added);
				added->texture     = (int32_t)t;
				added->chain       = (OptGltfSkinLayer*)malloc(count * sizeof *added->chain);
				added->chain_count = count;
				added->variants    = (uint32_t*)malloc(skins->variant_count * sizeof *added->variants);
				for (int k = 0; k < SKIN_COMPOSITE_IMAGES; ++k)
					added->images[k] = -1;
				if (!added->chain || !added->variants)
					goto fail;
				memcpy(added->chain, chain, count * sizeof *chain);
			}
			state->composites[c].variants[state->composites[c].variant_count++] = (uint32_t)v;
			*slot                                                               = (int32_t)c;
		}
	}
	for (size_t c = 0; c < state->count; ++c) {
		SkinComposite*       composite = &state->composites[c];
		const opt_texture_t* t         = &opt->textures[composite->texture];
		const XwauBaked*     own       = &baked[composite->texture];
		uint8_t*             rgba      = decode_texture_rgba8(t, base_shade);
		if (!rgba)
			goto fail;
		for (size_t l = 0; l < composite->chain_count; ++l)
			skin_blend(rgba,
					   &skins->images[composite->chain[l].skin * texture_count + (size_t)composite->texture],
					   composite->chain[l].opacity);
		const OptGltfXwauAnim* a = anim ? &anim[composite->texture] : NULL;
		if (a && a->anim_base) {
			composite->colour = rgba_dup(rgba, (size_t)t->width * (size_t)t->height);
			if (!composite->colour) {
				free(rgba);
				goto fail;
			}
		}
		if (!xwau_bake_rgba(t, &xwau[composite->texture], rgba, &composite->baked))
			goto fail;
		xwau_anim_base(&composite->colour, (uint32_t)t->width, (uint32_t)t->height, a, &composite->baked);
		const size_t bytes  = (size_t)t->width * (size_t)t->height * 4u;
		XwauBaked*   result = &composite->baked;
		composite->same_emissive =
			(!result->emissive && !own->emissive) ||
			(result->emissive && own->emissive && memcmp(result->emissive, own->emissive, bytes) == 0);
		composite->same_metal_rough =
			(!result->metal_rough && !own->metal_rough && result->metallic == own->metallic &&
			 result->roughness == own->roughness) ||
			(result->metal_rough && own->metal_rough &&
			 memcmp(result->metal_rough, own->metal_rough, bytes) == 0);
		if (composite->same_emissive) {
			free(result->emissive);
			result->emissive = NULL;
		}
		if (composite->same_metal_rough) {
			free(result->metal_rough);
			result->metal_rough = NULL;
		}
		composite->same_transmission = (!result->transmission && !own->transmission) ||
									   (result->transmission && own->transmission &&
										memcmp(result->transmission, own->transmission, bytes) == 0);
		if (composite->same_transmission) {
			free(result->transmission);
			result->transmission = NULL;
		}
		composite->image_count = 1u + (result->emissive ? 1u : 0u) + (result->metal_rough ? 1u : 0u) +
								 (result->transmission ? 1u : 0u);
	}
	free(skinnable);
	free(chain);
	return true;

fail:
	free(skinnable);
	free(chain);
	skin_state_free(state);
	return false;
}

/* Adds the model root's markings list to `root`: entry m is the skin
 * `default_markings` names for m, else state min(m, N - 1) (null for state
 * 0), up to the longer of the two lists; after a last skin, the state of
 * the values past it, which the list's last entry then repeats. Nothing
 * when every markings value shows the own materials. False when out of
 * memory. */
static bool markings_json(cJSON* root, const OptGltfSkins* skins, int state_count) {
	const size_t named = skins ? skins->markings_count : 0;
	size_t       count = named > (size_t)state_count ? named : (size_t)state_count;
	const char*  skin[257];
	int          state[257];
	for (size_t m = 0; m < count && m < 256; ++m) {
		skin[m]  = m < named ? skins->default_markings[m] : NULL;
		state[m] = m < (size_t)state_count ? (int)m : state_count - 1;
	}
	count = count < 256 ? count : 256;
	if (count && skin[count - 1] && count < 256) {
		skin[count]  = NULL;
		state[count] = count < (size_t)state_count ? (int)count : state_count - 1;
		count++;
	}
	while (count > 1 && !skin[count - 1] && !skin[count - 2] && state[count - 1] == state[count - 2])
		count--;
	bool shown = false;
	for (size_t m = 0; m < count; ++m)
		shown |= skin[m] != NULL || state[m] != 0;
	if (!shown)
		return true;
	cJSON* list = OptJson_AddArray(root, "markings");
	bool   ok   = list != NULL;
	for (size_t m = 0; ok && m < count; ++m) {
		char name[32];
		snprintf(name, sizeof name, "State %d", state[m]);
		ok = OptJson_Add(list, NULL,
						 skin[m]    ? cJSON_CreateString(skin[m])
						 : state[m] ? cJSON_CreateString(name)
									: cJSON_CreateNull());
	}
	return ok;
}

/* The texture views of material `m` (kViewCount of them). */
enum { kViewCount = 21 };

static void material_views(cgltf_material* m, cgltf_texture_view* views[kViewCount]) {
	cgltf_texture_view* const all[kViewCount] = {
		&m->pbr_metallic_roughness.base_color_texture,
		&m->pbr_metallic_roughness.metallic_roughness_texture,
		&m->pbr_specular_glossiness.diffuse_texture,
		&m->pbr_specular_glossiness.specular_glossiness_texture,
		&m->clearcoat.clearcoat_texture,
		&m->clearcoat.clearcoat_roughness_texture,
		&m->clearcoat.clearcoat_normal_texture,
		&m->transmission.transmission_texture,
		&m->specular.specular_texture,
		&m->specular.specular_color_texture,
		&m->volume.thickness_texture,
		&m->sheen.sheen_color_texture,
		&m->sheen.sheen_roughness_texture,
		&m->iridescence.iridescence_texture,
		&m->iridescence.iridescence_thickness_texture,
		&m->diffuse_transmission.diffuse_transmission_texture,
		&m->diffuse_transmission.diffuse_transmission_color_texture,
		&m->anisotropy.anisotropy_texture,
		&m->normal_texture,
		&m->occlusion_texture,
		&m->emissive_texture,
	};
	memcpy(views, all, sizeof all);
}

/* Leaves out the materials no primitive draws, directly or through a
 * variant (those only the OPT's lower LODs use: the conversion keeps LOD 0),
 * then the textures and images only they used. cgltf references them by
 * pointer, so the arrays are compacted and the pointers moved. */
static bool prune_unused(OptGltfDocument* document) {
	GltfBuild*   gb = &document->gb;
	const size_t largest =
		gb->materials_count > gb->textures_count ? gb->materials_count : gb->textures_count;
	const size_t count = largest > gb->images_count ? largest : gb->images_count;
	size_t*      to    = (size_t*)malloc((count ? count : 1) * sizeof *to);
	if (!to)
		return false;

	/* Materials. */
	for (size_t m = 0; m < gb->materials_count; ++m)
		to[m] = SIZE_MAX;
	for (size_t p = 0; p < gb->primitives_count; ++p) {
		const cgltf_primitive* prim = &gb->primitives[p];
		if (prim->material)
			to[prim->material - gb->materials] = 0;
		for (size_t k = 0; k < prim->mappings_count; ++k)
			if (prim->mappings[k].material)
				to[prim->mappings[k].material - gb->materials] = 0;
	}
	size_t kept = 0;
	for (size_t m = 0; m < gb->materials_count; ++m) {
		if (to[m] == SIZE_MAX)
			continue;
		gb->materials[kept]       = gb->materials[m];
		gb->material_anim[kept]   = gb->material_anim[m];
		gb->material_screen[kept] = gb->material_screen[m];
		to[m]                     = kept++;
	}
	for (size_t p = 0; p < gb->primitives_count; ++p) {
		cgltf_primitive* prim = &gb->primitives[p];
		if (prim->material)
			prim->material = &gb->materials[to[prim->material - gb->materials]];
		for (size_t k = 0; k < prim->mappings_count; ++k)
			if (prim->mappings[k].material)
				prim->mappings[k].material = &gb->materials[to[prim->mappings[k].material - gb->materials]];
	}
	gb->materials_count = kept;

	/* Textures. */
	cgltf_texture_view* views[kViewCount];
	for (size_t t = 0; t < gb->textures_count; ++t)
		to[t] = SIZE_MAX;
	for (size_t m = 0; m < gb->materials_count; ++m) {
		material_views(&gb->materials[m], views);
		for (int v = 0; v < kViewCount; ++v)
			if (views[v]->texture)
				to[views[v]->texture - gb->textures] = 0;
	}
	kept = 0;
	for (size_t t = 0; t < gb->textures_count; ++t) {
		if (to[t] == SIZE_MAX)
			continue;
		gb->textures[kept] = gb->textures[t];
		to[t]              = kept++;
	}
	for (size_t m = 0; m < gb->materials_count; ++m) {
		material_views(&gb->materials[m], views);
		for (int v = 0; v < kViewCount; ++v)
			if (views[v]->texture)
				views[v]->texture = &gb->textures[to[views[v]->texture - gb->textures]];
	}
	gb->textures_count = kept;

	/* Images, with their pixels. */
	for (size_t i = 0; i < gb->images_count; ++i)
		to[i] = SIZE_MAX;
	for (size_t t = 0; t < gb->textures_count; ++t)
		if (gb->textures[t].image)
			to[gb->textures[t].image - gb->images] = 0;
	kept = 0;
	for (size_t i = 0; i < gb->images_count; ++i) {
		if (to[i] == SIZE_MAX) {
			free(document->image_pixels[i].rgba);
			continue;
		}
		gb->images[kept]             = gb->images[i];
		document->image_pixels[kept] = document->image_pixels[i];
		to[i]                        = kept++;
	}
	for (size_t i = gb->images_count; i < document->image_pixels_count; ++i)
		free(document->image_pixels[i].rgba);
	for (size_t t = 0; t < gb->textures_count; ++t)
		if (gb->textures[t].image)
			gb->textures[t].image = &gb->images[to[gb->textures[t].image - gb->images]];
	gb->images_count             = kept;
	document->image_pixels_count = kept;
	free(to);
	return true;
}

/* The energy effects an AERON_flight_model entry names, as bits of
 * kAnimEnergies. */
static const char* const kAnimEnergies[] = { "shields", "tractor", "jamming" };

static unsigned energy_bits(const cJSON* entry) {
	unsigned     bits = 0;
	const cJSON* name;
	cJSON_ArrayForEach(name, cJSON_GetObjectItemCaseSensitive(entry, "energy")) {
		for (unsigned e = 0; e < 3; ++e)
			if (cJSON_IsString(name) && strcmp(name->valuestring, kAnimEnergies[e]) == 0)
				bits |= 1u << e;
	}
	return bits;
}

/* Adds `energy`, the effects of `bits`, to `object`; false when out of
 * memory. */
static bool add_energy(cJSON* object, unsigned bits) {
	cJSON* list = OptJson_AddArray(object, "energy");
	bool   ok   = list != NULL;
	for (unsigned e = 0; ok && e < 3; ++e)
		if (bits & 1u << e)
			ok = OptJson_Add(list, NULL, cJSON_CreateString(kAnimEnergies[e]));
	return ok;
}

/* Each material's AERON_flight_model object: its animation and its screen.
 * The energy effects most of the materials have (ties: the smaller set;
 * every material is drawn, see prune_unused) become the model root's
 * `energy` (*common, bits of kAnimEnergies); only the materials with other
 * effects keep their own, `[]` for none (a material XWAU's texture-name
 * sections never reached, such as the faces without a texture). */
static bool material_extensions(OptGltfDocument* document, unsigned* common_energy) {
	GltfBuild*   gb       = &document->gb;
	const size_t count    = gb->materials_count;
	cJSON**      entries  = (cJSON**)calloc(count ? count : 1, sizeof *entries);
	unsigned     tally[8] = { 0 };
	bool         ok       = entries != NULL;
	for (size_t m = 0; ok && m < count; ++m) {
		const char* anim = gb->material_anim ? gb->material_anim[m] : NULL;
		if (anim) {
			entries[m] = cJSON_Parse(anim);
			ok         = entries[m] != NULL;
		}
		tally[energy_bits(entries[m])]++;
	}
	unsigned common = 0;
	for (unsigned bits = 1; bits < 8; ++bits)
		if (tally[bits] > tally[common])
			common = bits;
	for (size_t m = 0; ok && m < count; ++m) {
		cJSON* entry = entries[m] ? entries[m] : cJSON_CreateObject();
		entries[m]   = entry;
		ok           = entry != NULL;
		if (ok && common) {
			const unsigned bits = energy_bits(entry);
			cJSON_DeleteItemFromObjectCaseSensitive(entry, "energy");
			if (bits != common)
				ok = add_energy(entry, bits);
		}
		const char* screen = gb->material_screen ? gb->material_screen[m] : NULL;
		if (ok && screen)
			ok = OptJson_Add(entry, "screen", cJSON_Parse(screen));
		if (ok && entry->child) {
			char*            printed   = cJSON_PrintUnformatted(entry);
			cgltf_extension* extension = (cgltf_extension*)calloc(1, sizeof *extension);
			ok                         = printed && extension;
			if (ok) {
				extension->name                   = (char*)"AERON_flight_model";
				extension->data                   = gb_keep_string(gb, printed);
				gb->materials[m].extensions       = extension;
				gb->materials[m].extensions_count = 1;
			} else {
				free(printed);
				free(extension);
			}
		}
	}
	*common_energy = common;
	for (size_t m = 0; entries && m < count; ++m)
		cJSON_Delete(entries[m]);
	free(entries);
	return ok;
}

/* ===== The build ====================================================== */

/* What the build's phases share. */
typedef struct Build {
	const opt_file_t*          opt;
	const char*                basename;
	const OptGltfBuildOptions* options;
	opt_error_t*               error;
	const OptGltfXwauMaterial* xwau; /* XWAU mode when set */
	const OptGltfXwauAnim*     anim;
	const OptGltfSkins*        skins;
	const OptGltfScreen*       screens;
	float                      smooth_angle; /* negative: the OPT's own normals */
	bool                       repair_normals;
	bool                       emissive;
	bool                       fullbright;
	float                      fullbright_strength;
	/* The palette row of base colours: it depends on the authoring game
	 * (XWA = 8, TIE98/XvT = 15); sampling the wrong row washes XWA textures
	 * out toward a faintly magenta white. */
	int base_shade;

	OptGltfDocument* document;
	GltfBuild*       gb;
	size_t           image_slot; /* the next image and texture slot */

	/* Per texture: whether it has self-lit texels (stock emissive). */
	int* tex_emissive;
	/* XWAU mode, per texture: its bake results; the first texture sharing
	 * its baked normal map (same source and NMIntensity) and the image slot
	 * of that map once written; its colours before the bake when its
	 * illumination frames need them (OptGltfXwauAnim.anim_base). */
	XwauBaked* xwau_baked;
	int32_t*   xwau_normal_owner;
	int32_t*   xwau_normal_image;
	uint8_t**  xwau_colour;
	/* Screen surfaces, per texture: the image slot of its cover's base image
	 * (the emissive image follows it), or -1. Textures sharing a cover share
	 * its images. */
	int32_t*  screen_cover_image;
	SkinState skin_state;
	/* The untextured material: the texture materials come before it, the
	 * skin composites after it. */
	size_t untextured;

	BuiltMesh* built;
	int        max_state_count; /* drives KHR_materials_variants */
	size_t     state_variant_count;
	size_t     skin_variant_count;
	/* promoted_variant_material's copies: [base material * 2 + rank - 1]. */
	cgltf_material** promoted;
	size_t           base_material_count;
} Build;

/* The options' defaults, when none are given. */
static const OptGltfBuildOptions kDefaultOptions = { .smooth_angle_degrees = -1.0f, .repair_normals = true };

/* Sets the build's error message; returns false. */
static bool build_fail(Build* b, const char* format, ...) {
	if (b->error) {
		va_list args;
		va_start(args, format);
		vsnprintf(b->error->msg, sizeof b->error->msg, format, args);
		va_end(args);
	}
	return false;
}

static void build_free(Build* b) {
	free(b->promoted);
	free(b->tex_emissive);
	free(b->screen_cover_image);
	skin_state_free(&b->skin_state);
	xwau_state_free(b->xwau_baked, b->opt->texture_count, b->xwau_normal_owner, b->xwau_normal_image);
	xwau_colour_free(b->xwau_colour, b->opt->texture_count);
	if (b->built) {
		for (int32_t mi = 0; mi < b->opt->mesh_count; ++mi)
			built_mesh_free(&b->built[mi]);
		free(b->built);
	}
}

/* ---- Options ------------------------------------------------------------ */

static bool validate_options(Build* b) {
	const OptGltfBuildOptions* o   = b->options;
	const opt_file_t*          opt = b->opt;
	if (!validate_alpha_overrides(o, b->error))
		return false;
	for (size_t i = 0; i < o->component_tag_count; ++i) {
		const OptGltfComponentTag* tag = &o->component_tags[i];
		if (!valid_component_tag(tag->tag) || tag->mesh_index >= (uint32_t)opt->mesh_count)
			return build_fail(b, "invalid tag %u=%s (%d meshes)", tag->mesh_index, tag->tag ? tag->tag : "",
							  (int)opt->mesh_count);
	}
	for (size_t i = 0; i < o->component_articulation_count; ++i) {
		const OptGltfComponentArticulation* a = &o->component_articulations[i];
		if (!valid_component_tag(a->group) || (a->behavior[0] && !valid_component_tag(a->behavior)) ||
			a->angle < -128 || a->angle > 255 || a->speed < 0 || a->speed > 255 || a->speed_back < 0 ||
			a->speed_back > 255 || a->component > 254)
			return build_fail(b, "invalid articulation %u=%s:%d:%d:%d:%s", a->component, a->group,
							  (int)a->angle, (int)a->speed, (int)a->speed_back, a->behavior);
	}
	for (size_t i = 0; i < o->component_set_count; ++i) {
		bool duplicate = false;
		for (size_t previous = 0; previous < i; ++previous)
			duplicate |= strcmp(o->component_sets[previous].name, o->component_sets[i].name) == 0;
		if (!valid_component_tag(o->component_sets[i].name) || duplicate ||
			(o->component_sets[i].members[7] & 0x80000000u))
			return build_fail(b, "invalid or repeated component set %s", o->component_sets[i].name);
	}
	for (size_t i = 0; i < o->articulation_setting_count; ++i) {
		bool duplicate = false;
		for (size_t previous = 0; previous < i; ++previous)
			duplicate |=
				strcmp(o->articulation_settings[previous].name, o->articulation_settings[i].name) == 0;
		if (!valid_component_tag(o->articulation_settings[i].name) || duplicate)
			return build_fail(b, "invalid or repeated articulation setting %s",
							  o->articulation_settings[i].name);
	}
	if (b->xwau && (o->smooth_angle_degrees >= 0.0f || o->emissive || o->fullbright_strength > 0.0f ||
					!o->xwau_untextured))
		return build_fail(b, "XWAU materials exclude a smoothing angle, emissive and fullbright");
	if (b->skins && !b->xwau)
		return build_fail(b, "skins need XWAU materials");
	if (b->anim && !b->xwau)
		return build_fail(b, "animated materials need XWAU materials");
	return !b->skins || skin_validate(b->skins, b->error);
}

/* ---- Texture preparation ------------------------------------------------ */

/* Self-illuminated texels (flat palette ramp) become a second, emissive
 * image per affected texture (with `emissive`). Counted first, as every
 * image: cgltf points into the image and texture arrays, which are never
 * reallocated. */
static bool find_stock_emissive(Build* b, size_t* images) {
	const opt_file_t* opt = b->opt;
	b->tex_emissive = (int*)calloc(opt->texture_count ? opt->texture_count : 1, sizeof *b->tex_emissive);
	if (!b->tex_emissive) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	for (int32_t i = 0; b->emissive && i < opt->texture_count; ++i) {
		uint8_t* em =
			decode_texture_emissive_rgba8(opt, &opt->textures[i], b->base_shade, OPT_EMISSIVE_SHADE_TOL);
		if (em) {
			b->tex_emissive[i] = 1;
			++*images;
			free(em);
		}
	}
	return true;
}

/* XWAU mode: bakes every texture, and finds the textures sharing a normal
 * map. */
static bool bake_xwau_textures(Build* b, size_t* images) {
	const opt_file_t*          opt   = b->opt;
	const OptGltfXwauMaterial* xwau  = b->xwau;
	const size_t               count = opt->texture_count ? (size_t)opt->texture_count : 1;
	b->xwau_baked                    = (XwauBaked*)calloc(count, sizeof *b->xwau_baked);
	b->xwau_normal_owner             = (int32_t*)malloc(count * sizeof *b->xwau_normal_owner);
	b->xwau_normal_image             = (int32_t*)malloc(count * sizeof *b->xwau_normal_image);
	b->xwau_colour                   = (uint8_t**)calloc(count, sizeof *b->xwau_colour);
	if (!b->xwau_baked || !b->xwau_normal_owner || !b->xwau_normal_image || !b->xwau_colour) {
		fprintf(stderr, "opt2gltf: oom (xwau)\n");
		return false;
	}
	for (int32_t i = 0; i < opt->texture_count; ++i) {
		XwauBaked* baked         = &b->xwau_baked[i];
		b->xwau_normal_owner[i]  = -1;
		b->xwau_normal_image[i]  = -1;
		const OptGltfXwauAnim* a = b->anim ? &b->anim[i] : NULL;
		if (!xwau_bake_texture(&opt->textures[i], b->base_shade, &xwau[i], baked,
							   a && a->anim_base ? &b->xwau_colour[i] : NULL)) {
			fprintf(stderr, "opt2gltf: oom (xwau bake)\n");
			return false;
		}
		xwau_anim_base(&b->xwau_colour[i], (uint32_t)opt->textures[i].width,
					   (uint32_t)opt->textures[i].height, a, baked);
		*images +=
			(baked->emissive ? 1u : 0u) + (baked->metal_rough ? 1u : 0u) + (baked->transmission ? 1u : 0u);
		if (!xwau[i].normal_map.rgba || !baked->base)
			continue;
		b->xwau_normal_owner[i] = i;
		for (int32_t j = 0; j < i; ++j) {
			if (b->xwau_normal_owner[j] == j && xwau[j].normal_map.rgba == xwau[i].normal_map.rgba &&
				xwau[j].nm_intensity == xwau[i].nm_intensity) {
				b->xwau_normal_owner[i] = j;
				break;
			}
		}
		if (b->xwau_normal_owner[i] == i)
			++*images;
	}
	return true;
}

/* Screen surfaces: the cover images, one set per cover. */
static bool find_screen_covers(Build* b, size_t* images) {
	const opt_file_t*    opt     = b->opt;
	const OptGltfScreen* screens = b->screens;
	b->screen_cover_image        = (int32_t*)malloc((opt->texture_count ? (size_t)opt->texture_count : 1) *
													sizeof *b->screen_cover_image);
	if (!b->screen_cover_image) {
		fprintf(stderr, "opt2gltf: oom (screens)\n");
		return false;
	}
	for (int32_t i = 0; i < opt->texture_count; ++i) {
		b->screen_cover_image[i] = -1;
		const OptGltfScreen* s   = &screens[i];
		if (!s->present || s->hologram || !s->cover_base.rgba)
			continue;
		bool shared = false;
		for (int32_t j = 0; j < i && !shared; ++j)
			shared = screens[j].present && !screens[j].hologram &&
					 screens[j].cover_base.rgba == s->cover_base.rgba;
		if (!shared)
			*images += s->cover_emissive.rgba ? 2u : 1u;
	}
	return true;
}

static bool prepare_textures(Build* b) {
	const opt_file_t* opt    = b->opt;
	GltfBuild*        gb     = b->gb;
	size_t            images = (size_t)opt->texture_count;
	b->base_shade            = opt_palette_base_shade(opt);
	if (!find_stock_emissive(b, &images) || (b->xwau && !bake_xwau_textures(b, &images)) ||
		(b->screens && !find_screen_covers(b, &images)))
		return false;
	if (b->skins) {
		if (!skin_state_build(opt, b->skins, b->screens, b->base_shade, b->xwau, b->anim, b->xwau_baked,
							  &b->skin_state)) {
			fprintf(stderr, "opt2gltf: oom (skins)\n");
			return false;
		}
		for (size_t c = 0; c < b->skin_state.count; ++c)
			images += b->skin_state.composites[c].image_count;
	}

	gb->images   = (cgltf_image*)calloc(images ? images : 1, sizeof *gb->images);
	gb->textures = (cgltf_texture*)calloc(images ? images : 1, sizeof *gb->textures);
	/* Room for the base materials (one per texture, the untextured one, then
	 * one per skin composite) and, after them, up to two alpha-promoted
	 * copies of each (see promoted_variant_material). Primitives point into
	 * this array, so it is never reallocated. */
	const size_t materials = ((size_t)opt->texture_count + 1 + b->skin_state.count) * 3;
	gb->materials          = (cgltf_material*)calloc(materials, sizeof *gb->materials);
	gb->material_anim      = (const char**)calloc(materials, sizeof *gb->material_anim);
	gb->material_screen    = (const char**)calloc(materials, sizeof *gb->material_screen);
	b->document->image_pixels =
		(OptGltfOwnedImage*)calloc(images ? images : 1, sizeof *b->document->image_pixels);
	b->document->image_pixels_count = images;
	if (!gb->images || !gb->textures || !gb->materials || !gb->material_anim || !gb->material_screen ||
		!b->document->image_pixels) {
		fprintf(stderr, "opt2gltf: oom (textures)\n");
		return false;
	}
	b->image_slot = (size_t)opt->texture_count;
	return true;
}

/* ---- Texture materials -------------------------------------------------- */

static void set_color(float out[], float r, float g, float b) {
	out[0] = r;
	out[1] = g;
	out[2] = b;
}

/* The material's alpha mode: classified from the texture's alpha (in XWAU
 * mode the baked one: glass remaps it), or an alpha override. */
static bool material_alpha(Build* b, int32_t i, cgltf_material* mat, const uint8_t* rgba) {
	const opt_texture_t* t               = &b->opt->textures[i];
	AlphaHistogram       alpha_histogram = { 0, 0, 0 };
	uint8_t*             xwau_alpha      = NULL;
	if (b->xwau && rgba && b->xwau_baked[i].transparent) {
		const size_t n = (size_t)t->width * (size_t)t->height;
		xwau_alpha     = (uint8_t*)malloc(n);
		if (!xwau_alpha) {
			fprintf(stderr, "opt2gltf: oom (xwau alpha)\n");
			return false;
		}
		for (size_t k = 0; k < n; ++k)
			xwau_alpha[k] = rgba[k * 4 + 3];
	}
	OptGltfAlphaMode alpha_mode = classify_alpha(b->xwau ? xwau_alpha : t->alpha,
												 (size_t)t->width * (size_t)t->height, &alpha_histogram);
	free(xwau_alpha);
	float                       alpha_cutoff   = 0.5f;
	const OptGltfAlphaOverride* alpha_override = find_alpha_override(b->options, t, i);
	if (alpha_override_match_count(b->options, t, i) > 1)
		return build_fail(b, "multiple alpha overrides resolve to texture '%s'", t->name);
	if (alpha_override) {
		if (alpha_override->alpha_mode < OPT_GLTF_ALPHA_OPAQUE ||
			alpha_override->alpha_mode > OPT_GLTF_ALPHA_BLEND || !isfinite(alpha_override->alpha_cutoff) ||
			alpha_override->alpha_cutoff < 0.0f || alpha_override->alpha_cutoff > 1.0f ||
			(alpha_override->alpha_mode != OPT_GLTF_ALPHA_OPAQUE && !t->alpha))
			return build_fail(b, "invalid alpha override for texture '%s'", t->name);
		alpha_mode   = alpha_override->alpha_mode;
		alpha_cutoff = alpha_override->alpha_cutoff;
	}
	if (t->alpha && b->options->log) {
		char note[512];
		snprintf(note, sizeof note,
				 "[opt_alpha] %s %s: mode=%s source=%s zero=%llu full=%llu intermediate=%llu cutoff=%.3g",
				 b->basename, t->name[0] ? t->name : b->gb->images[i].name, alpha_mode_name(alpha_mode),
				 alpha_override ? "override" : "heuristic", (unsigned long long)alpha_histogram.zero_count,
				 (unsigned long long)alpha_histogram.full_count,
				 (unsigned long long)alpha_histogram.intermediate_count, (double)alpha_cutoff);
		b->options->log(false, note);
	}
	if (alpha_mode == OPT_GLTF_ALPHA_MASK) {
		mat->alpha_mode   = cgltf_alpha_mode_mask;
		mat->alpha_cutoff = alpha_cutoff;
	} else if (alpha_mode == OPT_GLTF_ALPHA_BLEND) {
		mat->alpha_mode = cgltf_alpha_mode_blend;
	}
	return true;
}

/* Emissive: self-illuminated texels (e.g. lit windows) get a second texture
 * holding their glow colour on black, wired as emissiveTexture with a unit
 * emissiveFactor. The classic renderer drew them over the lit surface,
 * glow · c + lit · (1 - c) with c their coverage, 1 on a lit texel: the
 * base colour is black under them, so that the standard lit + emission
 * draws the same. */
static void stock_emissive(Build* b, int32_t i, cgltf_material* mat) {
	const opt_texture_t* t  = &b->opt->textures[i];
	uint8_t*             em = decode_texture_emissive_rgba8(b->opt, t, b->base_shade, OPT_EMISSIVE_SHADE_TOL);
	uint8_t*             base_rgba = b->document->image_pixels[i].rgba;
	if (em && base_rgba) {
		for (size_t k = 0; k < (size_t)t->width * (size_t)t->height; ++k)
			if (em[k * 4 + 3])
				base_rgba[k * 4 + 0] = base_rgba[k * 4 + 1] = base_rgba[k * 4 + 2] = 0;
	}
	if (!em)
		return;
	mat->emissive_texture.texture = document_set_image(
		b->document, b->image_slot++, em, (uint32_t)t->width, (uint32_t)t->height,
		xprintf_dup("Tex%02d_emissive", (int)i), xprintf_dup("%s_Tex%02d_emissive.png", b->basename, (int)i));
	set_color(mat->emissive_factor, 1.0f, 1.0f, 1.0f);
}

/* XWAU: baked emissive, metallic-roughness, transmission and normal maps,
 * the specular F0 and the animation. */
static bool xwau_material(Build* b, int32_t i, cgltf_material* mat) {
	const opt_texture_t*       t     = &b->opt->textures[i];
	const OptGltfXwauMaterial* xwau  = &b->xwau[i];
	XwauBaked*                 baked = &b->xwau_baked[i];
	const uint32_t             w = (uint32_t)t->width, h = (uint32_t)t->height;
	/* XWAU materials are keyed by texture name: keep it. */
	if (t->name[0])
		mat->name = gb_keep_string(b->gb, xstrdup(t->name));
	if (baked->emissive) {
		mat->emissive_texture.texture = document_set_image(
			b->document, b->image_slot++, baked->emissive, w, h, xprintf_dup("Tex%02d_emissive", (int)i),
			xprintf_dup("%s_Tex%02d_emissive.png", b->basename, (int)i));
		baked->emissive = NULL;
		set_color(mat->emissive_factor, 1.0f, 1.0f, 1.0f);
		mat->has_emissive_strength               = 1;
		mat->emissive_strength.emissive_strength = XWAU_ILLUM_GAIN;
	}
	if (baked->metal_rough) {
		mat->pbr_metallic_roughness.metallic_roughness_texture.texture = document_set_image(
			b->document, b->image_slot++, baked->metal_rough, w, h, xprintf_dup("Tex%02d_mr", (int)i),
			xprintf_dup("%s_Tex%02d_mr.png", b->basename, (int)i));
		baked->metal_rough                           = NULL;
		mat->pbr_metallic_roughness.metallic_factor  = 1.0f;
		mat->pbr_metallic_roughness.roughness_factor = 1.0f;
	} else {
		mat->pbr_metallic_roughness.metallic_factor  = baked->metallic;
		mat->pbr_metallic_roughness.roughness_factor = baked->roughness;
	}
	if (baked->transmission) {
		mat->has_transmission                 = 1;
		mat->transmission.transmission_factor = 1.0f;
		mat->transmission.transmission_texture.texture =
			document_set_image(b->document, b->image_slot++, baked->transmission, w, h,
							   xprintf_dup("Tex%02d_transmission", (int)i),
							   xprintf_dup("%s_Tex%02d_transmission.png", b->basename, (int)i));
		baked->transmission = NULL;
	}
	const int32_t owner = b->xwau_normal_owner[i];
	if (owner == i) {
		uint8_t* normal = xwau_bake_normal(&xwau->normal_map, xwau->nm_intensity);
		if (!normal) {
			fprintf(stderr, "opt2gltf: oom (xwau normal)\n");
			return false;
		}
		b->xwau_normal_image[i] = (int32_t)b->image_slot;
		document_set_image(b->document, b->image_slot++, normal, xwau->normal_map.width,
						   xwau->normal_map.height, xprintf_dup("Tex%02d_normal", (int)i),
						   xprintf_dup("%s_nm_%s_k%d.png", b->basename,
									   xwau->normal_map_name ? xwau->normal_map_name : "map",
									   (int)lroundf(xwau->nm_intensity * 1000.0f)));
	}
	if (owner >= 0)
		mat->normal_texture.texture = &b->gb->textures[b->xwau_normal_image[owner]];
	mat->normal_texture.scale = 1.0f;
	xwau_set_specular(mat, baked->specular_color);
	if (b->anim)
		anim_apply(b->document, mat, &b->anim[i]);
	return true;
}

/* Screen surface: replaces the material's textures (opt2gltf.h), with its
 * cover's images (written once per cover). */
static bool screen_material(Build* b, int32_t i, cgltf_material* mat) {
	const OptGltfScreen* s       = &b->screens[i];
	cgltf_texture*       base    = NULL;
	cgltf_texture*       glow    = NULL;
	int32_t              sharing = -1;
	for (int32_t j = 0; j < i && sharing < 0 && s->cover_base.rgba && !s->hologram; ++j)
		if (b->screen_cover_image[j] >= 0 && b->screens[j].cover_base.rgba == s->cover_base.rgba)
			sharing = j;
	if (sharing >= 0) {
		b->screen_cover_image[i] = b->screen_cover_image[sharing];
	} else if (s->cover_base.rgba && !s->hologram) {
		const OptGltfImageView* views[2] = { &s->cover_base, &s->cover_emissive };
		const char*             kinds[2] = { "base", "emissive" };
		const char*             cover    = s->cover_name ? s->cover_name : "cover";
		b->screen_cover_image[i]         = (int32_t)b->image_slot;
		for (int k = 0; k < 2; ++k) {
			if (!views[k]->rgba)
				continue;
			const size_t bytes = (size_t)views[k]->width * views[k]->height * 4u;
			uint8_t*     copy  = (uint8_t*)malloc(bytes);
			if (!copy) {
				fprintf(stderr, "opt2gltf: oom (screen cover)\n");
				return false;
			}
			memcpy(copy, views[k]->rgba, bytes);
			document_set_image(b->document, b->image_slot++, copy, views[k]->width, views[k]->height,
							   xprintf_dup("Cover_%s_%s", cover, kinds[k]),
							   xprintf_dup("%s_cover_%s_%s.png", b->basename, cover, kinds[k]));
		}
	}
	if (b->screen_cover_image[i] >= 0) {
		base = &b->gb->textures[b->screen_cover_image[i]];
		glow = s->cover_emissive.rgba ? &b->gb->textures[b->screen_cover_image[i] + 1] : NULL;
	}
	if (!screen_apply(b->gb, mat, s, base, glow)) {
		fprintf(stderr, "opt2gltf: oom (screen)\n");
		return false;
	}
	return true;
}

/* Texture `i`'s image (slot i) and material (material i). */
static bool texture_material(Build* b, int32_t i) {
	const opt_texture_t* t    = &b->opt->textures[i];
	GltfBuild*           gb   = b->gb;
	uint8_t*             rgba = b->xwau ? b->xwau_baked[i].base : decode_texture_rgba8(t, b->base_shade);
	if (b->xwau)
		b->xwau_baked[i].base = NULL; /* owned by the document from here */
	if (rgba) {
		document_set_image(b->document, (size_t)i, rgba, (uint32_t)t->width, (uint32_t)t->height,
						   xprintf_dup("Tex%02d", (int)i),
						   xprintf_dup("%s_Tex%02d.png", b->basename, (int)i));
	} else {
		gb->images[i].name      = gb_keep_string(gb, xprintf_dup("Tex%02d (missing pixels)", (int)i));
		gb->textures[i].image   = &gb->images[i];
		gb->textures[i].sampler = &gb->sampler;
		gb->textures[i].name    = gb->images[i].name;
	}

	/* PBR material: baseColorTexture only; metallic 0, roughness 1 by
	 * default. */
	cgltf_material* mat             = &gb->materials[i];
	mat->name                       = gb_keep_string(gb, xprintf_dup("Mat_Tex%02d", (int)i));
	mat->has_pbr_metallic_roughness = 1;
	set_color(mat->pbr_metallic_roughness.base_color_factor, 1.0f, 1.0f, 1.0f);
	mat->pbr_metallic_roughness.base_color_factor[3]       = 1.0f;
	mat->pbr_metallic_roughness.metallic_factor            = 0.0f;
	mat->pbr_metallic_roughness.roughness_factor           = 1.0f;
	mat->pbr_metallic_roughness.base_color_texture.texture = &gb->textures[i];
	if (!material_alpha(b, i, mat, rgba))
		return false;

	/* Fullbright: the texture lights itself and the scene adds nothing
	 * diffuse, as the classic renderer draws projectiles. Alpha still comes
	 * from the base texture. */
	if (b->fullbright) {
		set_color(mat->pbr_metallic_roughness.base_color_factor, 0.0f, 0.0f, 0.0f);
		mat->emissive_texture.texture = &gb->textures[i];
		set_color(mat->emissive_factor, 1.0f, 1.0f, 1.0f);
		mat->has_emissive_strength               = 1;
		mat->emissive_strength.emissive_strength = b->fullbright_strength;
	}
	if (b->tex_emissive[i])
		stock_emissive(b, i, mat);
	if (b->xwau && rgba && !xwau_material(b, i, mat))
		return false;
	return !(b->screens && b->screens[i].present) || screen_material(b, i, mat);
}

static void report_unused_alpha_overrides(const Build* b) {
	const OptGltfBuildOptions* o = b->options;
	for (size_t override_index = 0; o->alpha_overrides && override_index < o->alpha_override_count;
		 ++override_index) {
		bool matched = false;
		for (int32_t texture_index = 0; !matched && texture_index < b->opt->texture_count; ++texture_index) {
			char generated_name[32];
			snprintf(generated_name, sizeof generated_name, "Tex%02d", (int)texture_index);
			matched = alpha_name_equal(o->alpha_overrides[override_index].texture_name,
									   b->opt->textures[texture_index].name) ||
					  alpha_name_equal(o->alpha_overrides[override_index].texture_name, generated_name);
		}
		if (!matched) {
			char warning[512];
			snprintf(warning, sizeof warning, "%s: unused alpha override '%s'", b->basename,
					 o->alpha_overrides[override_index].texture_name);
			if (o->log)
				o->log(true, warning);
			else
				fprintf(stderr, "opt2gltf: %s\n", warning);
		}
	}
}

/* The material of untextured face groups (texture_index -1). */
static void untextured_material(Build* b) {
	cgltf_material* untex             = &b->gb->materials[b->opt->texture_count];
	untex->name                       = gb_keep_string(b->gb, xstrdup("Mat_Untextured"));
	untex->has_pbr_metallic_roughness = 1;
	set_color(untex->pbr_metallic_roughness.base_color_factor, 0.6f, 0.6f, 0.6f);
	untex->pbr_metallic_roughness.base_color_factor[3] = 1.0f;
	untex->pbr_metallic_roughness.metallic_factor      = 0.0f;
	untex->pbr_metallic_roughness.roughness_factor     = 1.0f;
	if (b->fullbright) {
		for (int channel = 0; channel < 3; ++channel) {
			untex->emissive_factor[channel] = untex->pbr_metallic_roughness.base_color_factor[channel];
			untex->pbr_metallic_roughness.base_color_factor[channel] = 0.0f;
		}
		untex->has_emissive_strength               = 1;
		untex->emissive_strength.emissive_strength = b->fullbright_strength;
	}
	if (b->xwau) {
		const OptGltfXwauMaterial* um = b->options->xwau_untextured;
		const float                s  = OptGltf_XwauUnlit(um);
		for (int channel = 0; channel < 3; ++channel) {
			const float grey = untex->pbr_metallic_roughness.base_color_factor[channel];
			untex->pbr_metallic_roughness.base_color_factor[channel] = grey * (1.0f - s);
			untex->emissive_factor[channel]                          = grey * s / XWAU_ILLUM_GAIN;
		}
		if (s > 0.0f) {
			untex->has_emissive_strength               = 1;
			untex->emissive_strength.emissive_strength = XWAU_ILLUM_GAIN;
		}
		untex->pbr_metallic_roughness.metallic_factor  = xwau_metallic(um->metallic);
		untex->pbr_metallic_roughness.roughness_factor = xwau_roughness(um->glossiness);
		xwau_set_specular(untex, xwau_specular_color(um->intensity));
	}
	b->gb->materials_count = (size_t)b->opt->texture_count + 1;
	b->untextured          = (size_t)b->opt->texture_count;
}

/* Skin composite `c`: a copy of its texture's material with the composite's
 * base color, and its own emissive, metallic-roughness or transmission
 * where the bake differs (material untextured + 1 + c). */
static void skin_material(Build* b, size_t c) {
	GltfBuild*           gb        = b->gb;
	SkinComposite*       composite = &b->skin_state.composites[c];
	const int32_t        ti        = composite->texture;
	const opt_texture_t* t         = &b->opt->textures[ti];
	const uint32_t       w = (uint32_t)t->width, h = (uint32_t)t->height;
	XwauBaked*           baked               = &composite->baked;
	cgltf_material*      mat                 = &gb->materials[gb->materials_count++];
	*mat                                     = gb->materials[ti];
	gb->material_screen[mat - gb->materials] = gb->material_screen[ti];
	mat->name            = gb_keep_string(gb, xprintf_dup("%s_skin%03zu", gb->materials[ti].name, c));
	composite->images[0] = (int32_t)b->image_slot;
	mat->pbr_metallic_roughness.base_color_texture.texture = document_set_image(
		b->document, b->image_slot++, baked->base, w, h, xprintf_dup("Tex%02d_skin%03zu", (int)ti, c),
		xprintf_dup("%s_Tex%02d_skin%03zu.png", b->basename, (int)ti, c));
	baked->base = NULL;
	if (!composite->same_emissive) {
		if (baked->emissive) {
			composite->images[1] = (int32_t)b->image_slot;
			mat->emissive_texture.texture =
				document_set_image(b->document, b->image_slot++, baked->emissive, w, h,
								   xprintf_dup("Tex%02d_skin%03zu_emissive", (int)ti, c),
								   xprintf_dup("%s_Tex%02d_skin%03zu_emissive.png", b->basename, (int)ti, c));
			baked->emissive = NULL;
			set_color(mat->emissive_factor, 1.0f, 1.0f, 1.0f);
			mat->has_emissive_strength               = 1;
			mat->emissive_strength.emissive_strength = XWAU_ILLUM_GAIN;
		} else {
			mat->emissive_texture.texture = NULL;
			set_color(mat->emissive_factor, 0.0f, 0.0f, 0.0f);
			mat->has_emissive_strength = 0;
		}
	}
	if (!composite->same_metal_rough) {
		if (baked->metal_rough) {
			composite->images[2] = (int32_t)b->image_slot;
			mat->pbr_metallic_roughness.metallic_roughness_texture.texture =
				document_set_image(b->document, b->image_slot++, baked->metal_rough, w, h,
								   xprintf_dup("Tex%02d_skin%03zu_mr", (int)ti, c),
								   xprintf_dup("%s_Tex%02d_skin%03zu_mr.png", b->basename, (int)ti, c));
			baked->metal_rough                           = NULL;
			mat->pbr_metallic_roughness.metallic_factor  = 1.0f;
			mat->pbr_metallic_roughness.roughness_factor = 1.0f;
		} else {
			mat->pbr_metallic_roughness.metallic_roughness_texture.texture = NULL;
			mat->pbr_metallic_roughness.metallic_factor                    = baked->metallic;
			mat->pbr_metallic_roughness.roughness_factor                   = baked->roughness;
		}
	}
	if (!composite->same_transmission) {
		mat->has_transmission                          = baked->transmission != NULL;
		mat->transmission.transmission_factor          = baked->transmission ? 1.0f : 0.0f;
		mat->transmission.transmission_texture.texture = NULL;
		if (baked->transmission) {
			composite->images[4]                           = (int32_t)b->image_slot;
			mat->transmission.transmission_texture.texture = document_set_image(
				b->document, b->image_slot++, baked->transmission, w, h,
				xprintf_dup("Tex%02d_skin%03zu_transmission", (int)ti, c),
				xprintf_dup("%s_Tex%02d_skin%03zu_transmission.png", b->basename, (int)ti, c));
			baked->transmission = NULL;
		}
	}
	xwau_set_specular(mat, baked->specular_color);
	if (b->anim)
		anim_apply(b->document, mat, &b->anim[ti]);
}

static bool build_materials(Build* b) {
	for (int32_t i = 0; i < b->opt->texture_count; ++i)
		if (!texture_material(b, i))
			return false;
	report_unused_alpha_overrides(b);
	untextured_material(b);
	for (size_t c = 0; c < b->skin_state.count; ++c)
		skin_material(b, c);
	b->gb->images_count   = b->image_slot;
	b->gb->textures_count = b->image_slot;
	return true;
}

/* ---- Geometry ----------------------------------------------------------- */

/* Pass 1: each OPT mesh's vertices and per-face-group indices, with
 * tangents for the meshes drawing a normal-mapped material in any switch
 * state (XWAU). */
static bool build_geometry(Build* b) {
	const opt_file_t* opt = b->opt;
	b->built              = (BuiltMesh*)calloc(opt->mesh_count ? opt->mesh_count : 1, sizeof *b->built);
	if (!b->built) {
		fprintf(stderr, "opt2gltf: oom (meshes)\n");
		return false;
	}
	b->max_state_count  = 1;
	uint32_t fixed_zero = 0, fixed_flip = 0, skipped_repeats = 0, kept_repeats = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m  = &opt->meshes[mi];
		BuiltMesh*        bm = &b->built[mi];
		/* Components stay one-to-one with the OPT meshes. */
		if (!built_mesh_from_opt(bm, m, OPT_METERS_PER_UNIT, b->smooth_angle >= 0.0f, b->smooth_angle,
								 b->repair_normals))
			return build_fail(b, "mesh %d has no LOD 0 triangles", (int)mi);
		bool needs_tangents = false;
		for (int32_t g = 0; b->xwau && !needs_tangents && g < m->lods[0].group_count; ++g) {
			const opt_face_group_t* grp = &m->lods[0].groups[g];
			if (bm->fg_index_count_arr[g] == 0)
				continue;
			const int states = (grp->state_count > 1 && grp->state_textures) ? grp->state_count : 1;
			for (int st = 0; st < states && !needs_tangents; ++st) {
				const int32_t ti = st == 0 ? grp->texture_index : grp->state_textures[st];
				needs_tangents   = ti >= 0 && ti < opt->texture_count && b->xwau_normal_owner[ti] >= 0;
			}
		}
		if (needs_tangents && !built_mesh_generate_tangents(bm)) {
			fprintf(stderr, "opt2gltf: oom (tangents)\n");
			return false;
		}
		fixed_zero += bm->fixed_zero_normals;
		fixed_flip += bm->fixed_flipped_normals;
		skipped_repeats += bm->skipped_repeats;
		kept_repeats += bm->kept_repeats;
		for (int32_t g = 0; g < m->lods[0].group_count; ++g)
			if (m->lods[0].groups[g].state_count > b->max_state_count)
				b->max_state_count = m->lods[0].groups[g].state_count;
	}
	if (fixed_zero || fixed_flip)
		fprintf(stderr, "opt2gltf: %s: repaired %u zero + %u face-opposed canonical normals\n", b->basename,
				fixed_zero, fixed_flip);
	if (skipped_repeats)
		fprintf(stderr, "opt2gltf: %s: left out %u repeated triangle(s)\n", b->basename, skipped_repeats);
	if (kept_repeats)
		fprintf(stderr, "opt2gltf: %s: %u triangle(s) repeat one with another texture (kept)\n", b->basename,
				kept_repeats);
	return true;
}

/* The asset's variants: state 0 is the primitives' own materials; states 1
 * to N - 1, then the skin variants. */
static bool build_variants(Build* b) {
	GltfBuild* gb          = b->gb;
	b->skin_variant_count  = b->skins ? b->skins->variant_count : 0;
	b->state_variant_count = (size_t)b->max_state_count - 1;
	const size_t count     = b->state_variant_count + b->skin_variant_count;
	if (count == 0)
		return true;
	gb->variants = (cgltf_material_variant*)calloc(count, sizeof *gb->variants);
	if (!gb->variants) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	for (size_t v = 0; v < b->state_variant_count; ++v)
		gb->variants[v].name = gb_keep_string(gb, xprintf_dup("State %d", (int)v + 1));
	for (size_t v = 0; v < b->skin_variant_count; ++v)
		gb->variants[b->state_variant_count + v].name =
			gb_keep_string(gb, xstrdup(b->skins->variants[v].name));
	gb->variants_count = count;
	return true;
}

/* The vertex attributes of a mesh: POSITION, NORMAL, TEXCOORD_0 [, TANGENT]. */
static size_t mesh_attribute_count(const BuiltMesh* bm) { return bm->has_tangents ? 4u : 3u; }

/* Whether a face group's primitive maps variants: it switches states, or a
 * skin variant has a composite of its texture. */
static bool group_has_variants(const Build* b, const opt_face_group_t* grp) {
	return (grp->state_count > 1 && grp->state_textures) ||
		   skin_texture_has_composite(&b->skin_state, b->skin_variant_count, b->opt->texture_count,
									  grp->texture_index);
}

/* The meshes, buffer views, accessors, primitives, attributes and
 * mappings pass 2 writes. cgltf points into these arrays, so each is
 * allocated once, at its full size. */
static bool allocate_geometry(Build* b) {
	const opt_file_t* opt = b->opt;
	GltfBuild*        gb  = b->gb;
	gb->meshes            = (cgltf_mesh*)calloc((size_t)opt->mesh_count, sizeof *gb->meshes);
	if (!gb->meshes) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	gb->meshes_count = (size_t)opt->mesh_count;
	size_t prims = 0, attrs = 0, maps = 0, views = 0, accessors = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_lod_t* lod     = &opt->meshes[mi].lods[0];
		const size_t     streams = mesh_attribute_count(&b->built[mi]);
		/* One view and accessor per vertex attribute, then per face group
		 * with indices its index view and accessor. */
		views += streams;
		accessors += streams;
		for (int32_t g = 0; g < lod->group_count; ++g) {
			if (b->built[mi].fg_index_count_arr[g] == 0)
				continue;
			prims++;
			attrs += streams;
			views++;
			accessors++;
			if (group_has_variants(b, &lod->groups[g]))
				maps += gb->variants_count;
		}
	}
	gb->views          = (cgltf_buffer_view*)calloc(views ? views : 1, sizeof *gb->views);
	gb->accessors      = (cgltf_accessor*)calloc(accessors ? accessors : 1, sizeof *gb->accessors);
	gb->primitives     = (cgltf_primitive*)calloc(prims ? prims : 1, sizeof *gb->primitives);
	gb->attributes     = (cgltf_attribute*)calloc(attrs ? attrs : 1, sizeof *gb->attributes);
	gb->mappings       = (cgltf_material_mapping*)calloc(maps ? maps : 1, sizeof *gb->mappings);
	gb->views_cap      = views;
	gb->accessors_cap  = accessors;
	gb->primitives_cap = prims;
	gb->attributes_cap = attrs;
	gb->mappings_cap   = maps;
	if (!gb->views || !gb->accessors || !gb->primitives || !gb->attributes || !gb->mappings) {
		fprintf(stderr, "opt2gltf: oom (geometry arrays)\n");
		return false;
	}
	b->base_material_count = gb->materials_count;
	b->promoted            = (cgltf_material**)calloc(b->base_material_count * 2, sizeof *b->promoted);
	if (!b->promoted) {
		fprintf(stderr, "opt2gltf: oom (promoted materials)\n");
		return false;
	}
	return true;
}

/* One vertex attribute of every vertex (`size` bytes at `offset` in
 * GltfVertex), as a buffer view and its accessor. */
static cgltf_accessor* write_attribute(GltfBuild* gb, const BuiltMesh* bm, size_t offset, size_t size,
									   cgltf_type type) {
	const size_t start = bin_append(gb, NULL, 0);
	for (uint32_t i = 0; i < bm->vertex_count; ++i)
		bin_append(gb, (const char*)&bm->vertices[i] + offset, size);
	cgltf_buffer_view* view =
		gb_new_view(gb, start, bm->vertex_count * size, cgltf_buffer_view_type_vertices);
	return gb_new_accessor(gb, view, bm->vertex_count, type, cgltf_component_type_r_32f);
}

/* The material texture `ti` shows: its own, else the untextured one. */
static cgltf_material* texture_material_of(const Build* b, int32_t ti) {
	return &b->gb->materials[ti >= 0 && ti < b->opt->texture_count ? (size_t)ti : b->untextured];
}

/* The variant mappings of a primitive drawing face group `grp`: per
 * variant giving it another material, that material. A state variant shows
 * its state (the original OPT walker clamps each switch to its last child);
 * a skin variant its composite, else its state. Materials of different
 * alpha classes are promoted to the most transparent one. */
static bool primitive_mappings(Build* b, cgltf_primitive* prim, const opt_face_group_t* grp, int32_t mi,
							   int32_t g) {
	GltfBuild*       gb        = b->gb;
	const size_t     vc        = gb->variants_count;
	const int32_t    t0        = grp->texture_index;
	const int        sc        = grp->state_count;
	const bool       switched  = sc > 1 && grp->state_textures;
	cgltf_material** materials = (cgltf_material**)calloc(vc ? vc : 1, sizeof *materials);
	if (!materials) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	int rank = alpha_class_rank(prim->material);
	for (size_t v = 0; v < vc; ++v) {
		size_t  state     = v + 1;
		int32_t composite = -1;
		if (v >= b->state_variant_count) {
			const size_t sv = v - b->state_variant_count;
			state           = b->skins->variants[sv].state;
			if (t0 >= 0 && t0 < b->opt->texture_count)
				composite = b->skin_state.composite_of[sv * (size_t)b->opt->texture_count + (size_t)t0];
		}
		if (composite >= 0)
			materials[v] = &gb->materials[b->untextured + 1 + (size_t)composite];
		else
			materials[v] = texture_material_of(
				b, switched ? grp->state_textures[state < (size_t)sc ? state : (size_t)sc - 1] : t0);
		if (alpha_class_rank(materials[v]) > rank)
			rank = alpha_class_rank(materials[v]);
	}
	int mixed = alpha_class_rank(prim->material) != rank;
	for (size_t v = 0; v < vc; ++v)
		mixed |= alpha_class_rank(materials[v]) != rank;
	if (mixed) {
		prim->material =
			promoted_variant_material(gb, prim->material, rank, b->base_material_count, b->promoted);
		for (size_t v = 0; v < vc; ++v)
			materials[v] =
				promoted_variant_material(gb, materials[v], rank, b->base_material_count, b->promoted);
		fprintf(stderr, "opt2gltf: %s: mesh %d face group %d switch states mix alpha classes; all use %s\n",
				b->basename, (int)mi, (int)g, rank == 2 ? "BLEND" : "MASK");
	}
	size_t mapped = 0;
	for (size_t v = 0; v < vc; ++v)
		mapped += materials[v] != prim->material;
	if (mapped) {
		cgltf_material_mapping* maps = (cgltf_material_mapping*)push_array(
			(void**)&gb->mappings, &gb->mappings_count, &gb->mappings_cap, sizeof *maps, mapped);
		size_t k = 0;
		for (size_t v = 0; v < vc; ++v) {
			if (materials[v] == prim->material)
				continue;
			maps[k].variant    = (cgltf_size)v;
			maps[k++].material = materials[v];
		}
		prim->mappings       = maps;
		prim->mappings_count = (cgltf_size)mapped;
	}
	free(materials);
	return true;
}

/* The primitive of face group `g` of mesh `mi`: its indices (16-bit while
 * every vertex fits), the mesh's vertex attributes, its material and
 * variant mappings. */
static bool write_primitive(Build* b, int32_t mi, int32_t g, cgltf_accessor* const vertex_attributes[4]) {
	GltfBuild*              gb            = b->gb;
	const BuiltMesh*        bm            = &b->built[mi];
	const opt_face_group_t* grp           = &b->opt->meshes[mi].lods[0].groups[g];
	const uint32_t          ic            = bm->fg_index_count_arr[g];
	const uint32_t*         group_indices = bm->indices + bm->fg_index_start[g];
	size_t                  idx_off, idx_bytes;
	cgltf_component_type    idx_type;
	if (bm->vertex_count <= 65536u) {
		uint16_t* narrow = (uint16_t*)malloc((size_t)ic * sizeof *narrow);
		if (!narrow)
			return false;
		for (uint32_t i = 0; i < ic; ++i)
			narrow[i] = (uint16_t)group_indices[i];
		idx_bytes = (size_t)ic * sizeof *narrow;
		idx_off   = bin_append(gb, narrow, idx_bytes);
		idx_type  = cgltf_component_type_r_16u;
		free(narrow);
	} else {
		idx_bytes = (size_t)ic * sizeof *group_indices;
		idx_off   = bin_append(gb, group_indices, idx_bytes);
		idx_type  = cgltf_component_type_r_32u;
	}
	cgltf_buffer_view* idx_view = gb_new_view(gb, idx_off, idx_bytes, cgltf_buffer_view_type_indices);
	cgltf_accessor*    idx_acc  = gb_new_accessor(gb, idx_view, ic, cgltf_type_scalar, idx_type);

	cgltf_primitive* prim = (cgltf_primitive*)push_array((void**)&gb->primitives, &gb->primitives_count,
														 &gb->primitives_cap, sizeof *prim, 1);
	prim->type            = cgltf_primitive_type_triangles;
	prim->indices         = idx_acc;

	static const struct {
		const char*          name;
		cgltf_attribute_type type;
	} kAttributes[4] = {
		{ "POSITION", cgltf_attribute_type_position },
		{ "NORMAL", cgltf_attribute_type_normal },
		{ "TEXCOORD_0", cgltf_attribute_type_texcoord },
		{ "TANGENT", cgltf_attribute_type_tangent },
	};

	const size_t     attr_count = mesh_attribute_count(bm);
	cgltf_attribute* attrs = (cgltf_attribute*)push_array((void**)&gb->attributes, &gb->attributes_count,
														  &gb->attributes_cap, sizeof *attrs, attr_count);
	for (size_t a = 0; a < attr_count; ++a) {
		attrs[a].name  = gb_keep_string(gb, xstrdup(kAttributes[a].name));
		attrs[a].type  = kAttributes[a].type;
		attrs[a].index = 0;
		attrs[a].data  = vertex_attributes[a];
	}
	prim->attributes       = attrs;
	prim->attributes_count = attr_count;

	/* Default material: state 0 (or the untextured one). */
	prim->material = texture_material_of(b, grp->texture_index);
	return !group_has_variants(b, grp) || primitive_mappings(b, prim, grp, mi, g);
}

/* Pass 2: mesh `mi`'s vertex attributes into the .bin, then one primitive
 * per face group with indices. */
static bool write_mesh(Build* b, int32_t mi) {
	GltfBuild*        gb   = b->gb;
	const BuiltMesh*  bm   = &b->built[mi];
	const opt_mesh_t* m    = &b->opt->meshes[mi];
	cgltf_mesh*       mesh = &gb->meshes[mi];
	mesh->name =
		gb_keep_string(gb, xprintf_dup("Mesh%02d_%s", (int)mi, opt_mesh_type_name(m->descriptor.mesh_type)));

	float pmin[3] = { +INFINITY, +INFINITY, +INFINITY };
	float pmax[3] = { -INFINITY, -INFINITY, -INFINITY };
	for (uint32_t i = 0; i < bm->vertex_count; ++i) {
		for (int k = 0; k < 3; ++k) {
			pmin[k] = bm->vertices[i].pos[k] < pmin[k] ? bm->vertices[i].pos[k] : pmin[k];
			pmax[k] = bm->vertices[i].pos[k] > pmax[k] ? bm->vertices[i].pos[k] : pmax[k];
		}
	}
	cgltf_accessor* attributes[4] = { NULL };
	attributes[0] =
		write_attribute(gb, bm, offsetof(GltfVertex, pos), sizeof bm->vertices[0].pos, cgltf_type_vec3);
	attributes[0]->has_min = 1;
	attributes[0]->has_max = 1;
	memcpy(attributes[0]->min, pmin, sizeof pmin);
	memcpy(attributes[0]->max, pmax, sizeof pmax);
	attributes[1] =
		write_attribute(gb, bm, offsetof(GltfVertex, nrm), sizeof bm->vertices[0].nrm, cgltf_type_vec3);
	attributes[2] =
		write_attribute(gb, bm, offsetof(GltfVertex, uv), sizeof bm->vertices[0].uv, cgltf_type_vec2);
	if (bm->has_tangents)
		attributes[3] =
			write_attribute(gb, bm, offsetof(GltfVertex, tan), sizeof bm->vertices[0].tan, cgltf_type_vec4);

	mesh->primitives = gb->primitives + gb->primitives_count;
	for (int32_t g = 0; g < m->lods[0].group_count; ++g) {
		if (bm->fg_index_count_arr[g] == 0)
			continue;
		if (!write_primitive(b, mi, g, attributes))
			return false;
		mesh->primitives_count++;
	}
	return true;
}

/* A variant that changes no primitive's material (a skin none of whose
 * images match) still maps the first primitive to its own material, so that
 * editors writing only mapped variants (Blender's glTF exporter) keep it
 * for the markings list and missions to name. */
static bool map_every_variant(Build* b) {
	GltfBuild* gb = b->gb;
	if (!gb->variants_count || !gb->primitives_count)
		return true;
	bool* mapped = (bool*)calloc(gb->variants_count, sizeof *mapped);
	if (!mapped) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	size_t unmapped = 0;
	for (size_t k = 0; k < gb->mappings_count; ++k)
		mapped[gb->mappings[k].variant] = true;
	for (size_t v = 0; v < gb->variants_count; ++v)
		unmapped += !mapped[v];
	if (unmapped) {
		/* cgltf points primitives at their mappings: rebuild the array. */
		const size_t            count    = gb->mappings_count + unmapped;
		cgltf_material_mapping* mappings = (cgltf_material_mapping*)calloc(count, sizeof *mappings);
		if (!mappings) {
			free(mapped);
			fprintf(stderr, "opt2gltf: oom\n");
			return false;
		}
		size_t k = 0;
		for (size_t p = 0; p < gb->primitives_count; ++p) {
			cgltf_primitive*        prim  = &gb->primitives[p];
			cgltf_material_mapping* first = mappings + k;
			for (size_t m = 0; m < prim->mappings_count; ++m)
				mappings[k++] = prim->mappings[m];
			for (size_t v = 0; p == 0 && v < gb->variants_count; ++v) {
				if (!mapped[v]) {
					mappings[k].variant    = (cgltf_size)v;
					mappings[k++].material = prim->material;
				}
			}
			prim->mappings_count = (cgltf_size)(mappings + k - first);
			prim->mappings       = prim->mappings_count ? first : NULL;
		}
		free(gb->mappings);
		gb->mappings       = mappings;
		gb->mappings_count = count;
		gb->mappings_cap   = count;
	}
	free(mapped);
	return true;
}

static bool write_meshes(Build* b) {
	if (!allocate_geometry(b))
		return false;
	for (int32_t mi = 0; mi < b->opt->mesh_count; ++mi)
		if (!write_mesh(b, mi))
			return false;
	return map_every_variant(b);
}

/* ---- Nodes -------------------------------------------------------------- */

/* The model root's AERON_flight_model object: the articulations (in option
 * order; speedBack only when it differs), articulation settings, component
 * sets, markings and the materials' common energy (bits of kAnimEnergies);
 * NULL when out of memory. */
static cJSON* root_extension(const Build* b, unsigned energy) {
	const OptGltfBuildOptions* o    = b->options;
	cJSON*                     root = cJSON_CreateObject();
	bool                       ok   = OptJson_Add(root, "role", cJSON_CreateString("model"));
	cJSON* list = ok && o->component_articulation_count ? OptJson_AddArray(root, "articulations") : NULL;
	ok          = ok && (list || !o->component_articulation_count);
	for (size_t a = 0; ok && a < o->component_articulation_count; ++a) {
		const OptGltfComponentArticulation* art    = &o->component_articulations[a];
		cJSON*                              object = OptJson_AddObject(list, NULL);
		ok = OptJson_Add(object, "component", cJSON_CreateNumber(art->component)) &&
			 OptJson_Add(object, "group", cJSON_CreateString(art->group)) &&
			 OptJson_Add(object, "angle", cJSON_CreateNumber(art->angle)) &&
			 OptJson_Add(object, "speed", cJSON_CreateNumber(art->speed));
		if (ok && art->speed_back != art->speed)
			ok = OptJson_Add(object, "speedBack", cJSON_CreateNumber(art->speed_back));
		if (ok && art->behavior[0])
			ok = OptJson_Add(object, "behavior", cJSON_CreateString(art->behavior));
	}
	cJSON* settings =
		ok && o->articulation_setting_count ? OptJson_AddObject(root, "articulationSettings") : NULL;
	ok = ok && (settings || !o->articulation_setting_count);
	for (size_t s = 0; ok && s < o->articulation_setting_count; ++s)
		ok = OptJson_Add(settings, o->articulation_settings[s].name,
						 cJSON_CreateNumber(o->articulation_settings[s].value));
	cJSON* sets = ok && o->component_set_count ? OptJson_AddObject(root, "componentSets") : NULL;
	ok          = ok && (sets || !o->component_set_count);
	for (size_t s = 0; ok && s < o->component_set_count; ++s) {
		const OptGltfComponentSet* set     = &o->component_sets[s];
		cJSON*                     members = OptJson_AddArray(sets, set->name);
		ok                                 = members != NULL;
		for (int component = 0; ok && component < 255; ++component)
			if (set->members[component / 32] & (1u << (component % 32)))
				ok = OptJson_Add(members, NULL, cJSON_CreateNumber(component));
	}
	ok = ok && markings_json(root, b->skins, b->max_state_count) && (!energy || add_energy(root, energy));
	if (!ok) {
		cJSON_Delete(root);
		return NULL;
	}
	return root;
}

/* Adds `values` (OPT axes and units) to `object` as `key`, in glTF axes
 * and metres. */
static bool add_position(cJSON* object, const char* key, const opt_vec3_t* values) {
	float v[3];
	swap_axis_v3(values, v);
	for (int k = 0; k < 3; ++k)
		v[k] *= OPT_METERS_PER_UNIT;
	return OptJson_Add(object, key, OptJson_Floats(v, 3));
}

/* Mesh `mi`'s component AERON_flight_model object; NULL when out of
 * memory. */
static cJSON* component_extension(const Build* b, int32_t mi) {
	const OptGltfBuildOptions* o     = b->options;
	const opt_mesh_t*          m     = &b->opt->meshes[mi];
	const float                scale = OPT_METERS_PER_UNIT;
	cJSON*                     ext   = cJSON_CreateObject();
	bool                       ok =
		OptJson_Add(ext, "role", cJSON_CreateString("component")) &&
		OptJson_Add(ext, "meshType", cJSON_CreateNumber((int)m->descriptor.mesh_type)) &&
		OptJson_Add(ext, "explosionFlags", cJSON_CreateNumber((unsigned)m->descriptor.explosion_type)) &&
		OptJson_Add(ext, "targetId", cJSON_CreateNumber((int)m->descriptor.target_id));
	if (ok && m->has_descriptor) {
		const float span[3]       = { m->descriptor.span.x * scale, m->descriptor.span.z * scale,
									  m->descriptor.span.y * scale };
		const float bounds_min[3] = { -m->descriptor.bbox_max.x * scale, m->descriptor.bbox_min.z * scale,
									  -m->descriptor.bbox_max.y * scale };
		const float bounds_max[3] = { -m->descriptor.bbox_min.x * scale, m->descriptor.bbox_max.z * scale,
									  -m->descriptor.bbox_min.y * scale };
		ok                        = OptJson_Add(ext, "span", OptJson_Floats(span, 3)) &&
									add_position(ext, "center", &m->descriptor.center) &&
									OptJson_Add(ext, "boundsMin", OptJson_Floats(bounds_min, 3)) &&
									OptJson_Add(ext, "boundsMax", OptJson_Floats(bounds_max, 3));
	}
	if (ok && m->descriptor.target_id != 0)
		ok = add_position(ext, "target", &m->descriptor.target);
	/* Every mesh with a rotation frame has it: the default axis frame is
	 * not static, XWA rotates default-frame meshes about their pivot
	 * whenever their runtime angle is nonzero (hangar droid antennas spin
	 * about the default vertical axis). */
	if (ok && m->has_rotation_scale) {
		float rax[3], dax[3], uax[3];
		swap_axis_v3(&m->rotation_scale.rotation_axis, rax);
		swap_axis_v3(&m->rotation_scale.direction_axis, dax);
		swap_axis_v3(&m->rotation_scale.up_axis, uax);
		/* The pivot is a position, scaled; the axes are directions. */
		for (int axis = 0; axis < 3; ++axis) {
			rax[axis] /= OPT_Q15_TO_UNIT;
			dax[axis] /= OPT_Q15_TO_UNIT;
			uax[axis] /= OPT_Q15_TO_UNIT;
		}
		cJSON* rotation = OptJson_AddObject(ext, "rotation");
		ok              = add_position(rotation, "pivot", &m->rotation_scale.pivot) &&
						  OptJson_Add(rotation, "rotationAxis", OptJson_Floats(rax, 3)) &&
						  OptJson_Add(rotation, "directionAxis", OptJson_Floats(dax, 3)) &&
						  OptJson_Add(rotation, "upAxis", OptJson_Floats(uax, 3));
	}
	/* Tags in option order, duplicates dropped. */
	cJSON* tags = NULL;
	for (size_t t = 0; ok && t < o->component_tag_count; ++t) {
		const OptGltfComponentTag* tag = &o->component_tags[t];
		if (tag->mesh_index != (uint32_t)mi)
			continue;
		bool duplicate = false;
		for (size_t previous = 0; previous < t && !duplicate; ++previous)
			duplicate = o->component_tags[previous].mesh_index == (uint32_t)mi &&
						strcmp(o->component_tags[previous].tag, tag->tag) == 0;
		if (duplicate)
			continue;
		if (!tags)
			tags = OptJson_AddArray(ext, "tags");
		ok = OptJson_Add(tags, NULL, cJSON_CreateString(tag->tag));
	}
	if (!ok) {
		cJSON_Delete(ext);
		return NULL;
	}
	return ext;
}

/* A child node at `position` (OPT axes and units) under `parent`. */
static void child_node(cgltf_node* node, cgltf_node* parent, const opt_vec3_t* position) {
	float pos[3];
	swap_axis_v3(position, pos);
	node->parent          = parent;
	node->has_translation = 1;
	for (int k = 0; k < 3; ++k)
		node->translation[k] = pos[k] * OPT_METERS_PER_UNIT;
}

static bool hardpoint_node(Build* b, cgltf_node* node, cgltf_node* parent, const opt_hardpoint_t* hp) {
	node->name = gb_keep_string(b->gb, xprintf_dup("hardpoint_%d", (int)hp->type));
	child_node(node, parent, &hp->pos);
	cJSON* extension = cJSON_CreateObject();
	if (!OptJson_Add(extension, "role", cJSON_CreateString("hardpoint")) ||
		!OptJson_Add(extension, "type", cJSON_CreateNumber((int)hp->type))) {
		cJSON_Delete(extension);
		return false;
	}
	return set_flight_extension(b->gb, node, extension);
}

static bool engine_glow_node(Build* b, cgltf_node* node, cgltf_node* parent, const opt_engine_glow_t* eg,
							 int e) {
	node->name = gb_keep_string(b->gb, xprintf_dup("engine_glow_%d", e));
	child_node(node, parent, &eg->position);
	float look[3], up[3], right[3], rotation[4];
	swap_axis_v3(&eg->look_axis, look);
	swap_axis_v3(&eg->up_axis, up);
	swap_axis_v3(&eg->right_axis, right);
	quaternion_from_axes(right, up, look, rotation);
	node->has_rotation = 1;
	memcpy(node->rotation, rotation, sizeof rotation);
	node->has_scale          = 1;
	node->scale[0]           = eg->dimensions.x * OPT_METERS_PER_UNIT;
	node->scale[1]           = eg->dimensions.y * OPT_METERS_PER_UNIT;
	node->scale[2]           = eg->dimensions.z * OPT_METERS_PER_UNIT;
	const uint32_t colors[2] = { eg->core_color, eg->outer_color };
	float          rgba[2][4];
	for (int color = 0; color < 2; ++color) {
		rgba[color][0] = (float)((colors[color] >> 16) & 0xff) / 255.0f;
		rgba[color][1] = (float)((colors[color] >> 8) & 0xff) / 255.0f;
		rgba[color][2] = (float)(colors[color] & 0xff) / 255.0f;
		rgba[color][3] = (float)((colors[color] >> 24) & 0xff) / 255.0f;
	}
	cJSON* extension = cJSON_CreateObject();
	if (!OptJson_Add(extension, "role", cJSON_CreateString("engineGlow")) ||
		!OptJson_Add(extension, "enabled", cJSON_CreateBool(!eg->is_disabled)) ||
		!OptJson_Add(extension, "coreColor", OptJson_Floats(rgba[0], 4)) ||
		!OptJson_Add(extension, "outerColor", OptJson_Floats(rgba[1], 4))) {
		cJSON_Delete(extension);
		return false;
	}
	return set_flight_extension(b->gb, node, extension);
}

/* Light `li` (a point light) and its node. */
static bool light_node(Build* b, cgltf_node* node, cgltf_node* parent, size_t li) {
	const OptGltfLight* source = &b->options->lights[li];
	cgltf_light*        light  = &b->gb->lights[li];
	light->name                = gb_keep_string(b->gb, xprintf_dup("light_%zu", li));
	light->type                = cgltf_light_type_point;
	memcpy(light->color, source->color, sizeof light->color);
	light->intensity          = source->intensity;
	light->range              = source->range;
	node->name                = light->name;
	node->light               = light;
	const opt_vec3_t position = { source->position[0], source->position[1], source->position[2] };
	child_node(node, parent, &position);
	cJSON* extension = cJSON_CreateObject();
	if (!OptJson_Add(extension, "role", cJSON_CreateString("light"))) {
		cJSON_Delete(extension);
		return false;
	}
	return set_flight_extension(b->gb, node, extension);
}

/* Mesh `mi`'s node, with its child nodes from *next_child: hardpoints,
 * engine glows, then lights (empties, so that Blender and other tools show
 * them). */
static bool mesh_node(Build* b, int32_t mi, cgltf_node* node, cgltf_node** next_child) {
	const opt_mesh_t* m    = &b->opt->meshes[mi];
	cgltf_node*       root = &b->gb->nodes[0];
	root->children[mi]     = node;
	node->parent           = root;
	node->mesh             = &b->gb->meshes[mi];
	node->name             = b->gb->meshes[mi].name; /* already kept */
	if (!set_flight_extension(b->gb, node, component_extension(b, mi)))
		return false;

	const size_t light_count = b->options->light_count;
	int          mesh_lights = 0;
	for (size_t li = 0; li < light_count; ++li)
		mesh_lights += b->options->lights[li].mesh == mi;
	const int child_total = m->hardpoint_count + m->engine_glow_count + mesh_lights;
	if (child_total == 0)
		return true;
	cgltf_node** children = (cgltf_node**)calloc(child_total, sizeof *children);
	if (!children) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	node->children       = children;
	node->children_count = child_total;
	int ci               = 0;
	for (int h = 0; h < m->hardpoint_count; ++h) {
		children[ci] = (*next_child)++;
		if (!hardpoint_node(b, children[ci++], node, &m->hardpoints[h]))
			return false;
	}
	for (int e = 0; e < m->engine_glow_count; ++e) {
		children[ci] = (*next_child)++;
		if (!engine_glow_node(b, children[ci++], node, &m->engine_glows[e], e))
			return false;
	}
	for (size_t li = 0; li < light_count; ++li) {
		if (b->options->lights[li].mesh != mi)
			continue;
		children[ci] = (*next_child)++;
		if (!light_node(b, children[ci++], node, li))
			return false;
	}
	return true;
}

/* The nodes: the model root, one per mesh, then each mesh's children. */
static bool build_nodes(Build* b) {
	const opt_file_t*          opt            = b->opt;
	const OptGltfBuildOptions* o              = b->options;
	GltfBuild*                 gb             = b->gb;
	size_t                     children_total = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
		children_total += (size_t)opt->meshes[mi].hardpoint_count + (size_t)opt->meshes[mi].engine_glow_count;
	for (size_t li = 0; li < o->light_count; ++li)
		if (o->lights[li].mesh < 0 || o->lights[li].mesh >= opt->mesh_count)
			return build_fail(b, "light %zu names OPT mesh %d of %d", li, (int)o->lights[li].mesh,
							  (int)opt->mesh_count);
	if (o->light_count) {
		gb->lights = (cgltf_light*)calloc(o->light_count, sizeof *gb->lights);
		if (!gb->lights)
			return false;
		gb->lights_count = o->light_count;
	}
	const size_t total = 1 + (size_t)opt->mesh_count + children_total + o->light_count;
	gb->nodes          = (cgltf_node*)calloc(total, sizeof *gb->nodes);
	if (!gb->nodes) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	gb->nodes_count = total;
	/* cgltf transform helpers read the implicit glTF defaults from these. */
	for (size_t i = 0; i < total; ++i) {
		gb->nodes[i].rotation[3] = 1.0f;
		set_color(gb->nodes[i].scale, 1.0f, 1.0f, 1.0f);
	}

	cgltf_node* root = &gb->nodes[0];
	root->name       = gb_keep_string(gb, xprintf_dup("OPT_%s", b->basename));
	root->children   = (cgltf_node**)calloc((size_t)opt->mesh_count, sizeof *root->children);
	if (!root->children) {
		fprintf(stderr, "opt2gltf: oom\n");
		return false;
	}
	root->children_count   = (size_t)opt->mesh_count;
	cgltf_node* next_child = &gb->nodes[1 + opt->mesh_count];
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
		if (!mesh_node(b, mi, &gb->nodes[1 + mi], &next_child))
			return false;
	return true;
}

/* ---- Document ----------------------------------------------------------- */

static bool finish_document(Build* b) {
	OptGltfDocument* document = b->document;
	GltfBuild*       gb       = b->gb;
	document->scene.name      = gb_keep_string(gb, xstrdup("Scene"));
	/* The scene has exactly one root: the model root. */
	document->scene_roots = (cgltf_node**)calloc(1, sizeof *document->scene_roots);
	if (!document->scene_roots)
		return false;
	document->scene_roots[0]    = &gb->nodes[0];
	document->scene.nodes       = document->scene_roots;
	document->scene.nodes_count = 1;
	gb->buffer.size             = gb->bin_size;
	gb->buffer.data             = gb->bin;
	/* The model root's object takes the materials' common energy. */
	unsigned energy = 0;
	if (!prune_unused(document) || !material_extensions(document, &energy) ||
		!set_flight_extension(gb, &gb->nodes[0], root_extension(b, energy)))
		return false;

	if (b->skins) {
		size_t own_emissive = 0, own_metal_rough = 0;
		for (size_t c = 0; c < b->skin_state.count; ++c) {
			own_emissive += b->skin_state.composites[c].images[1] >= 0;
			own_metal_rough += b->skin_state.composites[c].images[2] >= 0;
		}
		fprintf(
			stderr,
			"opt2gltf: %s: %zu skin variants, %zu composites (%zu own emissive, %zu own metallic-roughness), "
			"%zu markings\n",
			b->basename, b->skins->variant_count, b->skin_state.count, own_emissive, own_metal_rough,
			b->skins->markings_count);
	}

	cgltf_data* data         = &document->data;
	data->asset.version      = "2.0";
	data->asset.generator    = "opt2gltf";
	data->buffers            = &gb->buffer;
	data->buffers_count      = 1;
	data->buffer_views       = gb->views;
	data->buffer_views_count = gb->views_count;
	data->accessors          = gb->accessors;
	data->accessors_count    = gb->accessors_count;
	data->images             = gb->images;
	data->images_count       = gb->images_count;
	data->textures           = gb->textures;
	data->textures_count     = gb->textures_count;
	data->samplers           = &gb->sampler;
	data->samplers_count     = 1;
	data->materials          = gb->materials;
	data->materials_count    = gb->materials_count;
	data->meshes             = gb->meshes;
	data->meshes_count       = gb->meshes_count;
	data->nodes              = gb->nodes;
	data->nodes_count        = gb->nodes_count;
	data->scenes             = &document->scene;
	data->scenes_count       = 1;
	data->scene              = &document->scene;
	data->variants           = gb->variants;
	data->variants_count     = gb->variants_count;
	data->lights             = gb->lights;
	data->lights_count       = gb->lights_count;
	/* The flight contract, and the material variants when there are any. */
	const size_t extension_count = gb->variants_count > 0 ? 2u : 1u;
	data->extensions_used        = (char**)calloc(extension_count, sizeof(char*));
	if (!data->extensions_used)
		return false;
	data->extensions_used[0] = (char*)"AERON_flight_model";
	if (gb->variants_count > 0)
		data->extensions_used[1] = (char*)"KHR_materials_variants";
	data->extensions_used_count = extension_count;
	return true;
}

static bool build_memory(const opt_file_t* opt, const char* basename, const OptGltfBuildOptions* options,
						 OptGltfDocument** out_document, opt_error_t* error) {
	if (out_document)
		*out_document = NULL;
	if (error)
		error->msg[0] = '\0';
	if (!opt || !basename || !out_document)
		return false;
	const OptGltfBuildOptions* o = options ? options : &kDefaultOptions;
	Build                      b = {
		.opt          = opt,
		.basename     = basename,
		.options      = o,
		.error        = error,
		.xwau         = o->xwau_materials,
		.anim         = o->xwau_anim,
		.skins        = o->skins,
		.screens      = o->screens,
		.smooth_angle = o->smooth_angle_degrees,
		/* XWAU's renderer uses each corner's stored normal: no remap, no repair. */
		.repair_normals      = o->xwau_materials ? false : o->repair_normals,
		.fullbright          = o->fullbright_strength > 0.0f,
		.fullbright_strength = o->fullbright_strength,
		.emissive            = o->emissive && !(o->fullbright_strength > 0.0f),
	};
	if (!validate_options(&b))
		return false;
	b.document = (OptGltfDocument*)calloc(1, sizeof *b.document);
	if (!b.document)
		return build_fail(&b, "out of memory");
	b.gb             = &b.document->gb;
	b.gb->buffer.uri = gb_keep_string(b.gb, xprintf_dup("%s.bin", basename));
	/* One sampler: repeat, trilinear. */
	b.gb->sampler.mag_filter = 9729;  /* LINEAR */
	b.gb->sampler.min_filter = 9987;  /* LINEAR_MIPMAP_LINEAR */
	b.gb->sampler.wrap_s     = 10497; /* REPEAT */
	b.gb->sampler.wrap_t     = 10497; /* REPEAT */

	const bool ok = prepare_textures(&b) && build_materials(&b) && build_geometry(&b) && build_variants(&b) &&
					write_meshes(&b) && build_nodes(&b) && finish_document(&b);
	build_free(&b);
	if (!ok) {
		if (error && !error->msg[0])
			snprintf(error->msg, sizeof error->msg, "out of memory while converting %s", basename);
		OptGltf_Free(b.document);
		return false;
	}
	*out_document = b.document;
	return true;
}

bool OptGltf_BuildMemory(const opt_file_t* opt, const char* basename, const OptGltfBuildOptions* options,
						 OptGltfDocument** out_document, opt_error_t* error) {
	if (!opt)
		return build_memory(opt, basename, options, out_document, error);
	FacelessPatch     patch;
	const opt_file_t* source = faceless_patch_apply(opt, &patch);
	if (!source) {
		if (out_document)
			*out_document = NULL;
		if (error)
			snprintf(error->msg, sizeof error->msg, "out of memory");
		return false;
	}
	const bool ok = build_memory(source, basename, options, out_document, error);
	faceless_patch_free(&patch);
	return ok;
}

cgltf_data* OptGltf_Data(OptGltfDocument* document) { return document ? &document->data : NULL; }

bool OptGltf_ImageView(const OptGltfDocument* document, const cgltf_image* image,
					   OptGltfImageView* out_view) {
	if (!document || !image || !out_view || !document->gb.images)
		return false;
	const ptrdiff_t index = image - document->gb.images;
	if (index < 0 || (size_t)index >= document->image_pixels_count)
		return false;
	const OptGltfOwnedImage* owned = &document->image_pixels[index];
	if (!owned->rgba || owned->width == 0 || owned->height == 0)
		return false;
	out_view->rgba   = owned->rgba;
	out_view->width  = owned->width;
	out_view->height = owned->height;
	return true;
}

void OptGltf_Free(OptGltfDocument* document) {
	if (!document)
		return;
	GltfBuild* gb = &document->gb;

	/* The per-node children loop already covers both root_children
	 * (gb.nodes[0].children) and per-mesh hp_children — both are
	 * stored as node->children. Freeing root_children here would be
	 * a double-free. */
	for (size_t i = 0; i < gb->nodes_count; ++i) {
		free(gb->nodes[i].children);
		free(gb->nodes[i].extensions);
	}
	free(document->scene_roots);
	for (size_t i = 0; i < document->image_pixels_count; ++i)
		free(document->image_pixels[i].rgba);
	free(document->image_pixels);
	free(gb->images);
	free(gb->textures);
	for (size_t i = 0; i < gb->materials_count; ++i)
		free(gb->materials[i].extensions);
	free(gb->materials);
	free(gb->material_anim);
	free(gb->material_screen);
	free(gb->meshes);
	free(gb->primitives);
	free(gb->attributes);
	free(gb->mappings);
	free(gb->variants);
	free(gb->views);
	free(gb->accessors);
	free(gb->nodes);
	free(gb->lights);
	free(gb->buffer.data ? gb->buffer.data : gb->bin);
	free(document->data.extensions_used);

	for (size_t i = 0; i < gb->owned_strings_count; ++i)
		free(gb->owned_strings[i]);
	free(gb->owned_strings);
	free(document);
}

bool OptGltf_WriteFiles(const OptGltfDocument* document, const char* out_dir, const char* basename) {
	if (!document || !out_dir || !basename)
		return false;
	if (ensure_dir(out_dir) != 0)
		return false;
	char tex_dir[1024];
	path_join(tex_dir, sizeof tex_dir, out_dir, "textures");
	if (ensure_dir(tex_dir) != 0)
		return false;

	for (size_t i = 0; i < document->data.images_count; ++i) {
		const cgltf_image*       image  = &document->data.images[i];
		const OptGltfOwnedImage* pixels = &document->image_pixels[i];
		if (!image->uri || !pixels->rgba)
			continue;
		char png_path[1024];
		path_join(png_path, sizeof png_path, out_dir, image->uri);
		if (!stbi_write_png(png_path, (int)pixels->width, (int)pixels->height, 4, pixels->rgba,
							(int)pixels->width * 4))
			return false;
	}

	char gltf_path[1024], bin_path[1024], filename[256];
	snprintf(filename, sizeof filename, "%s.gltf", basename);
	path_join(gltf_path, sizeof gltf_path, out_dir, filename);
	snprintf(filename, sizeof filename, "%s.bin", basename);
	path_join(bin_path, sizeof bin_path, out_dir, filename);

	cgltf_options opts = { 0 };
	if (cgltf_write_file(&opts, gltf_path, &document->data) != cgltf_result_success)
		return false;
	FILE* fp = fopen(bin_path, "wb");
	if (!fp)
		return false;
	const size_t wrote        = fwrite(document->gb.buffer.data, 1, document->gb.bin_size, fp);
	const int    close_result = fclose(fp);
	if (wrote != document->gb.bin_size || close_result != 0)
		return false;

	fprintf(stdout,
			"%s -> %s\n"
			"  meshes=%zu primitives=%zu vertices=(per-mesh) variants=%zu\n"
			"  textures=%zu materials=%zu bin=%zu bytes\n",
			basename, gltf_path, document->gb.meshes_count, document->gb.primitives_count,
			document->gb.variants_count, document->gb.textures_count, document->gb.materials_count,
			document->gb.bin_size);
	return true;
}

bool opt2gltf_convert(const opt_file_t* opt, const char* out_dir, const char* basename,
					  const OptGltfBuildOptions* options, const char* label) {
	OptGltfDocument* document = NULL;
	opt_error_t      error    = { { 0 } };
	if (!OptGltf_BuildMemory(opt, basename, options, &document, &error)) {
		if (error.msg[0])
			fprintf(stderr, "opt2gltf: %s: %s\n", label, error.msg);
		return false;
	}
	const bool succeeded = OptGltf_WriteFiles(document, out_dir, basename);
	if (!succeeded)
		fprintf(stderr, "opt2gltf: %s: could not write %s\n", label, out_dir);
	OptGltf_Free(document);
	return succeeded;
}
