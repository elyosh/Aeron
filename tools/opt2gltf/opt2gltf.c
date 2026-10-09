/*
 * opt2gltf — OPT → glTF 2.0 conversion. See opt2gltf.h for the
 * data-preservation contract.
 *
 * OPT (+X right, -Y forward, +Z up) maps to standard meter-space glTF:
 * glTF = (-OPT.x, OPT.z, -OPT.y). The reflection converts OPT clockwise
 * face order to glTF counterclockwise order without changing indices.
 */

#include "opt2gltf.h"
#include "aeron/log.h"
#include "aeron/mesh_normals.h"
#include "opt.h"

#include "mikktspace.h"

#include <math.h>
#include <stdarg.h>
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
 * result in standard glTF inputs (xwau_model_import.md, "Per-material
 * output"). The math runs on gamma-encoded values, as that renderer does.
 *   - illumination: 8bpp texels whose palette row 4 equals row 8 and are not
 *     near-black (XwaD3dRendererHook.cpp D3dOptCreateTextureColorLight), or
 *     the 32bpp illumination plane; XwaD3dPixelShader.hlsl's illum branch
 *     makes them unlit at 1.25x their color;
 *   - Shadeless/Ambient: output = lerp(lit, color, s), stored as base color
 *     color * (1 - s) plus emissive color * s;
 *   - glass (transparent, not shadeless, no illumination): below alpha 0.95
 *     a texel gets alpha a^2, gloss 0.93, metallic 0 and shadeless 0.8;
 *   - dark-texel reflectance fade (PBRAddCommon.h "blackish"): approximated
 *     by raising roughness to 1;
 *   - Metallic, Glossiness: metallic and roughness = 1 - min(0.93, G)^2.
 * Colors are authored ones (palette row 8, raw 32bpp): XWA's brightness
 * setting is a display option, not material color. */

#define XWAU_GLASS_GLOSS 0.93f
#define XWAU_GLASS_SHADELESS 0.8f
#define XWAU_ILLUM_GAIN 1.25f

typedef struct XwauBaked {
	uint8_t* base;           /* RGBA */
	uint8_t* emissive;       /* RGBA / XWAU_ILLUM_GAIN; NULL when no texel emits */
	uint8_t* metal_rough;    /* RGBA, G roughness, B metallic; NULL when uniform */
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
	memset(baked, 0, sizeof *baked);
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

static bool xwau_bake_rgba(const opt_texture_t* t, const OptGltfXwauMaterial* xm, uint8_t* rgba,
						   XwauBaked* out);

static bool xwau_bake_texture(const opt_texture_t* t, int base_shade, const OptGltfXwauMaterial* xm,
							  XwauBaked* out) {
	memset(out, 0, sizeof *out);
	uint8_t* rgba = decode_texture_rgba8(t, base_shade);
	if (!rgba)
		return true; /* no pixels: the caller emits the missing-image material */
	return xwau_bake_rgba(t, xm, rgba, out);
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
	bool         has_illum   = bpp32 && t->flag_illum;
	if (!bpp32) {
		/* The renderer creates the light texture when any stored texel,
		 * mips included, is lit. */
		for (int32_t k = 0; k < t->mip_chain_bytes && !has_illum; ++k)
			has_illum = xwau_palette_lit(t->palette, t->pixels[k]);
	}
	const bool glass =
		transparent && !xm->shadeless && !xm->alpha_isnt_glass && !xm->alpha_to_bloom && !has_illum;
	const float metallic    = xwau_metallic(xm->metallic);
	const float roughness   = xwau_roughness(xm->glossiness);
	const float glass_rough = 1.0f - XWAU_GLASS_GLOSS * XWAU_GLASS_GLOSS;
	const float shadeless   = (xm->shadeless || xm->alpha_to_bloom) ? 1.0f : clamp01f(xm->ambient);

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
		const float s  = lerpf(lerpf(shadeless, XWAU_GLASS_SHADELESS, gl), 1.0f, w);

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
	out->specular_color = (glass && any_glass) ? 4.0f : xwau_specular_color(xm->intensity);
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

	cgltf_buffer            buffer;
	cgltf_buffer_view*      views;
	size_t                  views_count, views_cap;
	cgltf_accessor*         accessors;
	size_t                  accessors_count, accessors_cap;
	cgltf_image*            images;
	size_t                  images_count;
	cgltf_texture*          textures;
	size_t                  textures_count;
	cgltf_sampler           sampler; /* single repeat/linear sampler shared by all */
	cgltf_material*         materials;
	size_t                  materials_count;
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

static bool set_flight_extension(GltfBuild* build, cgltf_node* node, const char* json) {
	cgltf_extension* extension = (cgltf_extension*)calloc(1, sizeof *extension);
	char*            data      = xstrdup(json);
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
 * BLEND. Copies are shared per (base material, class). */

static int alpha_class_rank(const cgltf_material* m) {
	return m->alpha_mode == cgltf_alpha_mode_blend ? 2 : m->alpha_mode == cgltf_alpha_mode_mask ? 1 : 0;
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
	copy->name =
		gb_keep_string(build, xprintf_dup("%s_%s", base->name ? base->name : "Mat", rank == 2 ? "Blend" : "Mask"));
	if (rank == 2) {
		copy->alpha_mode = cgltf_alpha_mode_blend;
	} else {
		copy->alpha_mode   = cgltf_alpha_mode_mask;
		copy->alpha_cutoff = 0.5f;
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

	float* corner_normals = regen ? built_mesh_build_corner_normals(m, lod, smooth_angle_degrees) : NULL;
	if (regen && !corner_normals) {
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
	 * closest stable representation elsewhere. Keep --no-normal-repair as
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
					built_mesh_emit_index(bm, v[0]);
					built_mesh_emit_index(bm, v[1]);
					built_mesh_emit_index(bm, v[2]);
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
			built_mesh_emit_index(bm, v[0]);
			built_mesh_emit_index(bm, v[1]);
			built_mesh_emit_index(bm, v[2]);
			if (corners == 4) {
				built_mesh_emit_index(bm, v[0]);
				built_mesh_emit_index(bm, v[2]);
				built_mesh_emit_index(bm, v[3]);
			}
		}
		bm->fg_index_start[g]     = start;
		bm->fg_index_count_arr[g] = bm->index_count - start;
		if (bm->fg_index_count_arr[g] > 0)
			bm->fg_count++;
	}
	free(corner_normals);
	free(classic_normals);
	free(classic_normal_set);
	return bm->index_count > 0;
}

/* ===== Development subdivision ======================================
 *
 * Splits every triangle into four by its edge midpoints. A midpoint is
 * shared between the triangles of an edge through a table keyed by the
 * sorted vertex pair. Vertices split by a UV or normal seam produce separate
 * midpoints, but those are computed in double from the same positions, so
 * they are bitwise equal and the mesh stays watertight. Children keep the
 * parent's winding; face groups stay contiguous and keep their order. */

typedef struct EdgeSlot {
	uint64_t key; /* UINT64_MAX = empty */
	uint32_t vertex;
} EdgeSlot;

static void midpoint_vertex(const GltfVertex* a, const GltfVertex* b, GltfVertex* out) {
	for (int k = 0; k < 3; ++k)
		out->pos[k] = (float)(((double)a->pos[k] + (double)b->pos[k]) * 0.5);
	for (int k = 0; k < 2; ++k)
		out->uv[k] = (float)(((double)a->uv[k] + (double)b->uv[k]) * 0.5);
	double normal[3];
	for (int k = 0; k < 3; ++k)
		normal[k] = (double)a->nrm[k] + (double)b->nrm[k];
	const double length = sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
	for (int k = 0; k < 3; ++k)
		out->nrm[k] = length > 1.0e-12 ? (float)(normal[k] / length) : a->nrm[k];
	for (int k = 0; k < 4; ++k)
		out->tan[k] = 0.0f;
}

static uint32_t edge_midpoint(BuiltMesh* bm, EdgeSlot* slots, uint64_t mask, uint32_t a, uint32_t b) {
	if (a == b)
		return a;
	const uint64_t key = a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a;
	uint64_t       at  = (key * 0x9E3779B97F4A7C15ull) >> 32;
	for (;; ++at) {
		EdgeSlot* slot = &slots[at & mask];
		if (slot->key == key)
			return slot->vertex;
		if (slot->key == UINT64_MAX) {
			slot->key    = key;
			slot->vertex = bm->vertex_count;
			midpoint_vertex(&bm->vertices[a], &bm->vertices[b], &bm->vertices[bm->vertex_count]);
			return bm->vertex_count++;
		}
	}
}

static bool built_mesh_subdivide_once(BuiltMesh* bm) {
	/* Each triangle adds at most three midpoints; the index count grows by 4. */
	if (bm->index_count > UINT32_MAX / 4u || bm->index_count > UINT32_MAX - bm->vertex_count)
		return false;
	const uint32_t vertex_cap = bm->vertex_count + bm->index_count;
	uint64_t       slot_count = 16;
	while (slot_count < (uint64_t)bm->index_count * 2u)
		slot_count *= 2u;
	GltfVertex* vertices = (GltfVertex*)realloc(bm->vertices, (size_t)vertex_cap * sizeof *vertices);
	if (!vertices)
		return false;
	bm->vertices      = vertices;
	bm->vertex_cap    = vertex_cap;
	uint32_t* indices = (uint32_t*)malloc((size_t)bm->index_count * 4u * sizeof *indices);
	EdgeSlot* slots   = (EdgeSlot*)malloc((size_t)slot_count * sizeof *slots);
	if (!indices || !slots) {
		free(indices);
		free(slots);
		return false;
	}
	for (uint64_t i = 0; i < slot_count; ++i)
		slots[i].key = UINT64_MAX;

	uint32_t out = 0;
	for (uint32_t i = 0; i + 2 < bm->index_count; i += 3) {
		const uint32_t a            = bm->indices[i + 0];
		const uint32_t b            = bm->indices[i + 1];
		const uint32_t c            = bm->indices[i + 2];
		const uint32_t ab           = edge_midpoint(bm, slots, slot_count - 1, a, b);
		const uint32_t bc           = edge_midpoint(bm, slots, slot_count - 1, b, c);
		const uint32_t ca           = edge_midpoint(bm, slots, slot_count - 1, c, a);
		const uint32_t children[12] = { a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca };
		memcpy(indices + out, children, sizeof children);
		out += 12;
	}
	free(slots);
	free(bm->indices);
	bm->indices     = indices;
	bm->index_count = out;
	bm->index_cap   = out;
	return true;
}

static bool built_mesh_subdivide(BuiltMesh* bm, int32_t group_count, uint32_t levels) {
	for (uint32_t level = 0; level < levels; ++level) {
		if (!built_mesh_subdivide_once(bm))
			return false;
		for (int32_t g = 0; g < group_count; ++g) {
			bm->fg_index_start[g] *= 4u;
			bm->fg_index_count_arr[g] *= 4u;
		}
	}
	return true;
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

/* Appends formatted text to the string *out (length *len, capacity *cap);
 * on allocation failure *out becomes NULL and later calls do nothing. */
static void json_append(char** out, size_t* len, size_t* cap, const char* fmt, ...) {
	if (!*out)
		return;
	va_list args;
	va_start(args, fmt);
	const int n = vsnprintf(NULL, 0, fmt, args);
	va_end(args);
	if (n < 0) {
		free(*out);
		*out = NULL;
		return;
	}
	if (*len + (size_t)n + 1 > *cap) {
		size_t ncap = *cap * 2;
		while (ncap < *len + (size_t)n + 1)
			ncap *= 2;
		char* grown = (char*)realloc(*out, ncap);
		if (!grown) {
			free(*out);
			*out = NULL;
			return;
		}
		*out = grown;
		*cap = ncap;
	}
	va_start(args, fmt);
	vsnprintf(*out + *len, *cap - *len, fmt, args);
	va_end(args);
	*len += (size_t)n;
}

static void json_append_vec3(char** out, size_t* len, size_t* cap, const char* key, const float v[3]) {
	json_append(out, len, cap, ",\"%s\":[%.9g,%.9g,%.9g]", key, (double)v[0], (double)v[1], (double)v[2]);
}

/* The material's extras with `aeronScreen` added: a new object, or the
 * existing one with the member appended. */
static char* screen_extras_json(const OptGltfScreen* s, const char* existing) {
	size_t len = 0, cap = 1024;
	char*  out = (char*)malloc(cap);
	if (!out)
		return NULL;
	out[0]            = 0;
	const char* close = existing ? strrchr(existing, '}') : NULL;
	if (close) {
		json_append(&out, &len, &cap, "%.*s", (int)(close - existing), existing);
		/* An empty object needs no separator. */
		const char* p = close;
		while (p > existing && (p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\t'))
			--p;
		json_append(&out, &len, &cap, p > existing && p[-1] == '{' ? "" : ",");
	} else {
		json_append(&out, &len, &cap, "{");
	}
	json_append(&out, &len, &cap,
				"\"aeronScreen\":{\"cover\":%s,\"transparent\":%s,\"alwaysVisible\":%s,\"hologram\":%s,"
				"\"noisy\":%s,\"hide\":%u,\"rects\":[",
				s->cover_base.rgba && !s->hologram ? "true" : "false", s->transparent ? "true" : "false",
				s->always_visible ? "true" : "false", s->hologram ? "true" : "false", s->noisy ? "true" : "false",
				s->hide);
	for (uint32_t i = 0; i < s->rect_count; ++i) {
		const OptGltfScreenRect* r = &s->rects[i];
		json_append(&out, &len, &cap, "%s{\"source\":%u,\"dst\":[%.9g,%.9g,%.9g,%.9g]", i ? "," : "", r->source,
					(double)r->dst[0], (double)r->dst[1], (double)r->dst[2], (double)r->dst[3]);
		json_append_vec3(&out, &len, &cap, "bg", r->bg);
		json_append_vec3(&out, &len, &cap, "highlight", r->highlight);
		json_append_vec3(&out, &len, &cap, "lock", r->lock);
		json_append(&out, &len, &cap, ",\"gain\":%.9g,\"layers\":%u", (double)r->gain, r->layers);
		if (r->layers & OPT_GLTF_SCREEN_LAYER_BAR)
			json_append_vec3(&out, &len, &cap, "bar", r->bar);
		json_append(&out, &len, &cap, "}");
	}
	json_append(&out, &len, &cap, "]}}");
	return out;
}

/* Rewrite the material of a screen texture (opt2gltf.h, "Screen surfaces").
 * `cover_base` and `cover_emissive` are the cover's textures, or NULL. */
static bool screen_apply(GltfBuild* build, cgltf_material* mat, const OptGltfScreen* s, cgltf_texture* cover_base,
						 cgltf_texture* cover_emissive) {
	char* extras = screen_extras_json(s, mat->extras.data);
	if (!extras)
		return false;
	mat->extras.data = gb_keep_string(build, extras);

	mat->normal_texture.texture                                    = NULL;
	mat->pbr_metallic_roughness.metallic_roughness_texture.texture = NULL;
	mat->pbr_metallic_roughness.metallic_factor                    = 0.0f;
	mat->pbr_metallic_roughness.roughness_factor                   = 1.0f;
	const bool cover = cover_base && !s->hologram;
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

/* One composite: texture `texture` with the effective chain `chain` (the
 * layers of a variant that have an image for it), shared by `variants`. */
typedef struct SkinComposite {
	int32_t           texture;
	OptGltfSkinLayer* chain;
	size_t            chain_count;
	XwauBaked         baked;
	/* The bake's emissive / metallic-roughness equal the texture's own: the
	 * composite material keeps the texture's (baked copies freed). */
	bool      same_emissive;
	bool      same_metal_rough;
	uint32_t* variants; /* skin variant indices, ascending */
	size_t    variant_count;
	int32_t   images[3]; /* image slots only this composite uses; -1 */
	size_t    image_count;
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
							 int base_shade, const OptGltfXwauMaterial* xwau, const XwauBaked* baked,
							 SkinState* state) {
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
				for (int k = 0; k < 3; ++k)
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
		if (!xwau_bake_rgba(t, &xwau[composite->texture], rgba, &composite->baked))
			goto fail;
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
		composite->image_count = 1u + (result->emissive ? 1u : 0u) + (result->metal_rough ? 1u : 0u);
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

/* Appends `text` as a JSON string. */
static void json_append_string(char** out, size_t* len, size_t* cap, const char* text) {
	json_append(out, len, cap, "\"");
	for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
		if (*p == '"' || *p == '\\')
			json_append(out, len, cap, "\\%c", *p);
		else if (*p < 0x20)
			json_append(out, len, cap, "\\u%04x", *p);
		else
			json_append(out, len, cap, "%c", *p);
	}
	json_append(out, len, cap, "\"");
}

static bool build_memory(const opt_file_t* opt, const char* basename, const OptGltfBuildOptions* options,
						 OptGltfDocument** out_document, opt_error_t* error);

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

static bool build_memory(const opt_file_t* opt, const char* basename, const OptGltfBuildOptions* options,
						 OptGltfDocument** out_document, opt_error_t* error) {
	if (out_document)
		*out_document = NULL;
	if (error)
		error->msg[0] = '\0';
	if (!opt || !basename || !out_document)
		return false;
	if (!validate_alpha_overrides(options, error))
		return false;
	const uint32_t subdivide_levels = options ? options->subdivide_levels : 0;
	if (subdivide_levels > OPT_GLTF_MAX_SUBDIVIDE) {
		if (error)
			snprintf(error->msg, sizeof error->msg, "subdivision level %u exceeds %u", subdivide_levels,
					 OPT_GLTF_MAX_SUBDIVIDE);
		return false;
	}
	const size_t tag_count = options ? options->component_tag_count : 0;
	for (size_t i = 0; i < tag_count; ++i) {
		const OptGltfComponentTag* tag = &options->component_tags[i];
		if (!valid_component_tag(tag->tag) || tag->mesh_index >= (uint32_t)opt->mesh_count) {
			if (error)
				snprintf(error->msg, sizeof error->msg, "invalid tag %u=%s (%d meshes)", tag->mesh_index,
						 tag->tag ? tag->tag : "", (int)opt->mesh_count);
			return false;
		}
	}
	const float                smooth_angle_deg = options ? options->smooth_angle_degrees : -1.0f;
	const OptGltfXwauMaterial* xwau             = options ? options->xwau_materials : NULL;
	if (xwau && (smooth_angle_deg >= 0.0f || options->emissive || options->fullbright_strength > 0.0f ||
				 !options->xwau_untextured)) {
		if (error)
			snprintf(error->msg, sizeof error->msg,
					 "XWAU materials exclude --smooth-angle, --emissive and --fullbright");
		return false;
	}
	const OptGltfSkins* skins = options ? options->skins : NULL;
	if (skins && !xwau) {
		if (error)
			snprintf(error->msg, sizeof error->msg, "skins need XWAU materials");
		return false;
	}
	if (skins && !skin_validate(skins, error))
		return false;
	/* XWAU's renderer uses each corner's stored normal: no remap, no repair. */
	const bool  repair_normals      = xwau ? false : (options ? options->repair_normals : true);
	const float fullbright_strength = options ? options->fullbright_strength : 0.0f;
	const bool  fullbright          = fullbright_strength > 0.0f;
	const bool  emissive            = options && options->emissive && !fullbright;
	const float meters_per_opt_unit = OPT_METERS_PER_UNIT;

	OptGltfDocument* document = (OptGltfDocument*)calloc(1, sizeof *document);
	if (!document) {
		if (error)
			snprintf(error->msg, sizeof error->msg, "out of memory");
		return false;
	}
	GltfBuild* build        = &document->gb;
	int*       tex_emissive = NULL;
	BuiltMesh* built        = NULL;
	/* promoted_variant_material's copies: [base material * 2 + rank - 1]. */
	cgltf_material** promoted_materials  = NULL;
	size_t           base_material_count = 0;
	/* XWAU mode: per-texture bake results, and per texture the first
	 * texture sharing its baked normal map (same source and NMIntensity)
	 * plus the image slot of that map once emitted. */
	XwauBaked* xwau_baked        = NULL;
	int32_t*   xwau_normal_owner = NULL;
	int32_t*   xwau_normal_image = NULL;
	/* Screen surfaces: per texture, the image slot of its cover's base image
	 * (the emissive image follows it), or -1. Textures sharing a cover share
	 * its images. */
	const OptGltfScreen* screens            = options ? options->screens : NULL;
	int32_t*             screen_cover_image = NULL;
	SkinState            skin_state         = { 0 };

	/* Negative angle → keep the original OPT normals; otherwise
	 * regenerate with an angle-based smoothing threshold. */
	const bool regen   = (smooth_angle_deg >= 0.0f);
	build->buffer.size = 0; /* filled at end */
	build->buffer.uri  = xprintf_dup("%s.bin", basename);
	gb_keep_string(build, build->buffer.uri);

	/* Sampler — single, repeat + trilinear. */
	build->sampler.mag_filter = 9729;  /* LINEAR */
	build->sampler.min_filter = 9987;  /* LINEAR_MIPMAP_LINEAR */
	build->sampler.wrap_s     = 10497; /* REPEAT */
	build->sampler.wrap_t     = 10497; /* REPEAT */

	/* ---- Textures + images + materials --------------------------- */
	/* Base-color palette row depends on the authoring game (XWA = 8,
	 * TIE98/XvT = 15); sampling the wrong row washes XWA textures out
	 * toward a faintly magenta white. */
	const int base_shade = opt_palette_base_shade(opt);

	/* Self-illuminated texels (flat palette ramp) become a second, emissive
	 * texture/image per affected base texture (opt-in via --emissive). Count
	 * them first so the image/texture arrays — which cgltf indexes by
	 * pointer — never realloc. */
	tex_emissive      = (int*)calloc(opt->texture_count ? opt->texture_count : 1, sizeof *tex_emissive);
	size_t n_emissive = 0;
	if (!tex_emissive) {
		fprintf(stderr, "opt2gltf: oom\n");
		goto fail;
	}
	if (emissive) {
		for (int32_t i = 0; i < opt->texture_count; ++i) {
			uint8_t* em =
				decode_texture_emissive_rgba8(opt, &opt->textures[i], base_shade, OPT_EMISSIVE_SHADE_TOL);
			if (em) {
				tex_emissive[i] = 1;
				n_emissive++;
				free(em);
			}
		}
	}

	size_t n_xwau_images = 0;
	if (xwau) {
		const size_t count = opt->texture_count ? (size_t)opt->texture_count : 1;
		xwau_baked         = (XwauBaked*)calloc(count, sizeof *xwau_baked);
		xwau_normal_owner  = (int32_t*)malloc(count * sizeof *xwau_normal_owner);
		xwau_normal_image  = (int32_t*)malloc(count * sizeof *xwau_normal_image);
		if (!xwau_baked || !xwau_normal_owner || !xwau_normal_image) {
			fprintf(stderr, "opt2gltf: oom (xwau)\n");
			goto fail;
		}
		for (int32_t i = 0; i < opt->texture_count; ++i) {
			xwau_normal_owner[i] = -1;
			xwau_normal_image[i] = -1;
			if (!xwau_bake_texture(&opt->textures[i], base_shade, &xwau[i], &xwau_baked[i])) {
				fprintf(stderr, "opt2gltf: oom (xwau bake)\n");
				goto fail;
			}
			n_xwau_images += (xwau_baked[i].emissive ? 1u : 0u) + (xwau_baked[i].metal_rough ? 1u : 0u);
			if (!xwau[i].normal_map.rgba || !xwau_baked[i].base)
				continue;
			xwau_normal_owner[i] = i;
			for (int32_t j = 0; j < i; ++j) {
				if (xwau_normal_owner[j] == j && xwau[j].normal_map.rgba == xwau[i].normal_map.rgba &&
					xwau[j].nm_intensity == xwau[i].nm_intensity) {
					xwau_normal_owner[i] = j;
					break;
				}
			}
			if (xwau_normal_owner[i] == i)
				n_xwau_images++;
		}
	}

	size_t n_screen_images = 0;
	if (screens) {
		screen_cover_image = (int32_t*)malloc((opt->texture_count ? (size_t)opt->texture_count : 1) *
											  sizeof *screen_cover_image);
		if (!screen_cover_image) {
			fprintf(stderr, "opt2gltf: oom (screens)\n");
			goto fail;
		}
		for (int32_t i = 0; i < opt->texture_count; ++i) {
			screen_cover_image[i] = -1;
			const OptGltfScreen* s = &screens[i];
			if (!s->present || s->hologram || !s->cover_base.rgba)
				continue;
			bool shared = false;
			for (int32_t j = 0; j < i && !shared; ++j)
				shared = screens[j].present && !screens[j].hologram &&
						 screens[j].cover_base.rgba == s->cover_base.rgba;
			if (!shared)
				n_screen_images += s->cover_emissive.rgba ? 2u : 1u;
		}
	}

	size_t n_skin_images = 0;
	if (skins) {
		if (!skin_state_build(opt, skins, screens, base_shade, xwau, xwau_baked, &skin_state)) {
			fprintf(stderr, "opt2gltf: oom (skins)\n");
			goto fail;
		}
		for (size_t c = 0; c < skin_state.count; ++c)
			n_skin_images += skin_state.composites[c].image_count;
	}

	const size_t image_total =
		(size_t)opt->texture_count + n_emissive + n_xwau_images + n_screen_images + n_skin_images;
	build->images            = (cgltf_image*)calloc(image_total ? image_total : 1, sizeof *build->images);
	build->textures          = (cgltf_texture*)calloc(image_total ? image_total : 1, sizeof *build->textures);
	/* Room for the base materials (one per texture, the untextured one, then
	 * one per skin composite) and, after them, up to two alpha-promoted
	 * copies of each (see promoted_variant_material). Primitives point into
	 * this array, so it is never reallocated. */
	build->materials = (cgltf_material*)calloc(((size_t)opt->texture_count + 1 + skin_state.count) * 3,
											   sizeof *build->materials);
	document->image_pixels =
		(OptGltfOwnedImage*)calloc(image_total ? image_total : 1, sizeof *document->image_pixels);
	document->image_pixels_count = image_total;
	if (!build->images || !build->textures || !build->materials || !document->image_pixels) {
		fprintf(stderr, "opt2gltf: oom (textures)\n");
		goto fail;
	}
	size_t emi = (size_t)opt->texture_count; /* next emissive image/texture slot */
	for (int32_t i = 0; i < opt->texture_count; ++i) {
		const opt_texture_t* t    = &opt->textures[i];
		uint8_t*             rgba = xwau ? xwau_baked[i].base : decode_texture_rgba8(t, base_shade);
		if (xwau)
			xwau_baked[i].base = NULL; /* owned by the document from here */
		if (rgba) {
			char png_rel[256];
			snprintf(png_rel, sizeof png_rel, "textures/%s_Tex%02d.png", basename, (int)i);
			document->image_pixels[i].rgba   = rgba;
			document->image_pixels[i].width  = (uint32_t)t->width;
			document->image_pixels[i].height = (uint32_t)t->height;
			build->images[i].name            = gb_keep_string(build, xprintf_dup("Tex%02d", (int)i));
			build->images[i].uri             = gb_keep_string(build, xstrdup(png_rel));
			build->images[i].mime_type       = "image/png";
		} else {
			build->images[i].name = gb_keep_string(build, xprintf_dup("Tex%02d (missing pixels)", (int)i));
		}
		build->textures[i].image   = &build->images[i];
		build->textures[i].sampler = &build->sampler;
		build->textures[i].name    = build->images[i].name;

		/* PBR material: baseColorTexture only; metallic 0, roughness
		 * 1 by default — author can refine. */
		cgltf_material* mat             = &build->materials[i];
		mat->name                       = gb_keep_string(build, xprintf_dup("Mat_Tex%02d", (int)i));
		mat->has_pbr_metallic_roughness = 1;
		mat->pbr_metallic_roughness.base_color_factor[0]       = 1.0f;
		mat->pbr_metallic_roughness.base_color_factor[1]       = 1.0f;
		mat->pbr_metallic_roughness.base_color_factor[2]       = 1.0f;
		mat->pbr_metallic_roughness.base_color_factor[3]       = 1.0f;
		mat->pbr_metallic_roughness.metallic_factor            = 0.0f;
		mat->pbr_metallic_roughness.roughness_factor           = 1.0f;
		mat->pbr_metallic_roughness.base_color_texture.texture = &build->textures[i];
		AlphaHistogram alpha_histogram                         = { 0, 0, 0 };
		/* XWAU mode classifies the baked alpha (glass remaps it). */
		uint8_t* xwau_alpha = NULL;
		if (xwau && rgba && xwau_baked[i].transparent) {
			const size_t n = (size_t)t->width * (size_t)t->height;
			xwau_alpha     = (uint8_t*)malloc(n);
			if (!xwau_alpha) {
				fprintf(stderr, "opt2gltf: oom (xwau alpha)\n");
				goto fail;
			}
			for (size_t k = 0; k < n; ++k)
				xwau_alpha[k] = rgba[k * 4 + 3];
		}
		OptGltfAlphaMode alpha_mode = classify_alpha(xwau ? xwau_alpha : t->alpha,
													 (size_t)t->width * (size_t)t->height, &alpha_histogram);
		free(xwau_alpha);
		float                       alpha_cutoff   = 0.5f;
		const OptGltfAlphaOverride* alpha_override = find_alpha_override(options, t, i);
		if (alpha_override_match_count(options, t, i) > 1) {
			if (error) {
				snprintf(error->msg, sizeof error->msg, "multiple alpha overrides resolve to texture '%s'",
						 t->name);
			}
			goto fail;
		}
		if (alpha_override) {
			if (alpha_override->alpha_mode < OPT_GLTF_ALPHA_OPAQUE ||
				alpha_override->alpha_mode > OPT_GLTF_ALPHA_BLEND ||
				!isfinite(alpha_override->alpha_cutoff) || alpha_override->alpha_cutoff < 0.0f ||
				alpha_override->alpha_cutoff > 1.0f ||
				(alpha_override->alpha_mode != OPT_GLTF_ALPHA_OPAQUE && !t->alpha)) {
				if (error) {
					snprintf(error->msg, sizeof error->msg, "invalid alpha override for texture '%s'",
							 t->name);
				}
				goto fail;
			}
			alpha_mode   = alpha_override->alpha_mode;
			alpha_cutoff = alpha_override->alpha_cutoff;
		}
		if (t->alpha) {
			Aeron_LogDebug("aeron.opt",
						   "[opt_alpha] %s %s: mode=%s source=%s zero=%llu full=%llu "
						   "intermediate=%llu cutoff=%.3g",
						   basename, t->name[0] ? t->name : build->images[i].name,
						   alpha_mode_name(alpha_mode), alpha_override ? "override" : "heuristic",
						   (unsigned long long)alpha_histogram.zero_count,
						   (unsigned long long)alpha_histogram.full_count,
						   (unsigned long long)alpha_histogram.intermediate_count, (double)alpha_cutoff);
		}
		if (alpha_mode == OPT_GLTF_ALPHA_MASK) {
			mat->alpha_mode   = cgltf_alpha_mode_mask;
			mat->alpha_cutoff = alpha_cutoff;
		} else if (alpha_mode == OPT_GLTF_ALPHA_BLEND) {
			mat->alpha_mode = cgltf_alpha_mode_blend;
		}

		/* Fullbright: the texture lights itself and the scene adds nothing
		 * diffuse, as the classic renderer draws projectiles. Alpha still
		 * comes from the base texture. */
		if (fullbright) {
			mat->pbr_metallic_roughness.base_color_factor[0] = 0.0f;
			mat->pbr_metallic_roughness.base_color_factor[1] = 0.0f;
			mat->pbr_metallic_roughness.base_color_factor[2] = 0.0f;
			mat->emissive_texture.texture                    = &build->textures[i];
			mat->emissive_factor[0]                          = 1.0f;
			mat->emissive_factor[1]                          = 1.0f;
			mat->emissive_factor[2]                          = 1.0f;
			mat->has_emissive_strength                       = 1;
			mat->emissive_strength.emissive_strength         = fullbright_strength;
		}

		/* Emissive: self-illuminated texels (e.g. lit windows) get a second
		 * texture holding their base color on black, wired as emissiveTexture
		 * with a unit emissiveFactor. */
		if (tex_emissive[i]) {
			uint8_t* em = decode_texture_emissive_rgba8(opt, t, base_shade, OPT_EMISSIVE_SHADE_TOL);
			if (em) {
				char png_rel[256];
				snprintf(png_rel, sizeof png_rel, "textures/%s_Tex%02d_emissive.png", basename, (int)i);
				document->image_pixels[emi].rgba   = em;
				document->image_pixels[emi].width  = (uint32_t)t->width;
				document->image_pixels[emi].height = (uint32_t)t->height;
				build->images[emi].name      = gb_keep_string(build, xprintf_dup("Tex%02d_emissive", (int)i));
				build->images[emi].uri       = gb_keep_string(build, xstrdup(png_rel));
				build->images[emi].mime_type = "image/png";
				build->textures[emi].image   = &build->images[emi];
				build->textures[emi].sampler = &build->sampler;
				build->textures[emi].name    = build->images[emi].name;
				mat->emissive_texture.texture = &build->textures[emi];
				mat->emissive_factor[0]       = 1.0f;
				mat->emissive_factor[1]       = 1.0f;
				mat->emissive_factor[2]       = 1.0f;
				/* Generic Aeron material mode: emissive alpha carries
				 * coverage for a legacy fixed-function sRGB/SRCALPHA pass. */
				mat->extras.data =
					gb_keep_string(build, xstrdup("{\"aeronEmissiveMode\":\"legacy_srgb_srcalpha\"}"));
				emi++;
			}
		}

		/* XWAU: baked emissive, metallic-roughness and normal maps, plus
		 * the specular F0. */
		if (xwau && rgba) {
			XwauBaked* baked = &xwau_baked[i];
			/* XWAU materials are keyed by texture name: keep it. */
			if (t->name[0])
				mat->name = gb_keep_string(build, xstrdup(t->name));
			if (baked->emissive) {
				mat->emissive_texture.texture =
					document_set_image(document, emi, baked->emissive, (uint32_t)t->width,
									   (uint32_t)t->height, xprintf_dup("Tex%02d_emissive", (int)i),
									   xprintf_dup("%s_Tex%02d_emissive.png", basename, (int)i));
				baked->emissive         = NULL;
				mat->emissive_factor[0] = mat->emissive_factor[1] = mat->emissive_factor[2] = 1.0f;
				mat->has_emissive_strength                                                  = 1;
				mat->emissive_strength.emissive_strength                                    = XWAU_ILLUM_GAIN;
				emi++;
			}
			if (baked->metal_rough) {
				mat->pbr_metallic_roughness.metallic_roughness_texture.texture = document_set_image(
					document, emi, baked->metal_rough, (uint32_t)t->width, (uint32_t)t->height,
					xprintf_dup("Tex%02d_mr", (int)i), xprintf_dup("%s_Tex%02d_mr.png", basename, (int)i));
				baked->metal_rough                           = NULL;
				mat->pbr_metallic_roughness.metallic_factor  = 1.0f;
				mat->pbr_metallic_roughness.roughness_factor = 1.0f;
				emi++;
			} else {
				mat->pbr_metallic_roughness.metallic_factor  = baked->metallic;
				mat->pbr_metallic_roughness.roughness_factor = baked->roughness;
			}
			const int32_t owner = xwau_normal_owner[i];
			if (owner == i) {
				uint8_t* normal = xwau_bake_normal(&xwau[i].normal_map, xwau[i].nm_intensity);
				if (!normal) {
					fprintf(stderr, "opt2gltf: oom (xwau normal)\n");
					goto fail;
				}
				document_set_image(document, emi, normal, xwau[i].normal_map.width, xwau[i].normal_map.height,
								   xprintf_dup("Tex%02d_normal", (int)i),
								   xprintf_dup("%s_nm_%s_k%d.png", basename,
											   xwau[i].normal_map_name ? xwau[i].normal_map_name : "map",
											   (int)lroundf(xwau[i].nm_intensity * 1000.0f)));
				xwau_normal_image[i] = (int32_t)emi++;
			}
			if (owner >= 0)
				mat->normal_texture.texture = &build->textures[xwau_normal_image[owner]];
			mat->normal_texture.scale = 1.0f;
			xwau_set_specular(mat, baked->specular_color);
		}

		/* Screen surface: replaces the material's textures (opt2gltf.h). */
		if (screens && screens[i].present) {
			const OptGltfScreen* s       = &screens[i];
			cgltf_texture*       base    = NULL;
			cgltf_texture*       glow    = NULL;
			int32_t              sharing = -1;
			for (int32_t j = 0; j < i && sharing < 0 && s->cover_base.rgba && !s->hologram; ++j)
				if (screen_cover_image[j] >= 0 && screens[j].cover_base.rgba == s->cover_base.rgba)
					sharing = j;
			if (sharing >= 0) {
				screen_cover_image[i] = screen_cover_image[sharing];
			} else if (s->cover_base.rgba && !s->hologram) {
				const OptGltfImageView* views[2] = { &s->cover_base, &s->cover_emissive };
				const char*             kinds[2] = { "base", "emissive" };
				screen_cover_image[i]            = (int32_t)emi;
				for (int k = 0; k < 2; ++k) {
					if (!views[k]->rgba)
						continue;
					const size_t bytes = (size_t)views[k]->width * views[k]->height * 4u;
					uint8_t*     copy  = (uint8_t*)malloc(bytes);
					if (!copy) {
						fprintf(stderr, "opt2gltf: oom (screen cover)\n");
						goto fail;
					}
					memcpy(copy, views[k]->rgba, bytes);
					document_set_image(document, emi, copy, views[k]->width, views[k]->height,
									   xprintf_dup("Cover_%s_%s", s->cover_name ? s->cover_name : "cover", kinds[k]),
									   xprintf_dup("%s_cover_%s_%s.png", basename,
												   s->cover_name ? s->cover_name : "cover", kinds[k]));
					emi++;
				}
			}
			if (screen_cover_image[i] >= 0) {
				base = &build->textures[screen_cover_image[i]];
				glow = s->cover_emissive.rgba ? &build->textures[screen_cover_image[i] + 1] : NULL;
			}
			if (!screen_apply(build, mat, s, base, glow)) {
				fprintf(stderr, "opt2gltf: oom (screen extras)\n");
				goto fail;
			}
		}
	}
	if (options && options->alpha_overrides) {
		for (size_t override_index = 0; override_index < options->alpha_override_count; ++override_index) {
			bool matched = false;
			for (int32_t texture_index = 0; texture_index < opt->texture_count; ++texture_index) {
				const opt_texture_t* texture = &opt->textures[texture_index];
				char                 generated_name[32];
				snprintf(generated_name, sizeof generated_name, "Tex%02d", (int)texture_index);
				if (alpha_name_equal(options->alpha_overrides[override_index].texture_name, texture->name) ||
					alpha_name_equal(options->alpha_overrides[override_index].texture_name, generated_name)) {
					matched = true;
					break;
				}
			}
			if (!matched) {
				Aeron_LogWarn("aeron.opt", "%s: unused alpha override '%s'", basename,
							  options->alpha_overrides[override_index].texture_name);
			}
		}
	}
	/* One extra material for untextured face groups (texture_index = -1). */
	cgltf_material* untex                              = &build->materials[opt->texture_count];
	untex->name                                        = gb_keep_string(build, xstrdup("Mat_Untextured"));
	untex->has_pbr_metallic_roughness                  = 1;
	untex->pbr_metallic_roughness.base_color_factor[0] = 0.6f;
	untex->pbr_metallic_roughness.base_color_factor[1] = 0.6f;
	untex->pbr_metallic_roughness.base_color_factor[2] = 0.6f;
	untex->pbr_metallic_roughness.base_color_factor[3] = 1.0f;
	untex->pbr_metallic_roughness.metallic_factor      = 0.0f;
	untex->pbr_metallic_roughness.roughness_factor     = 1.0f;
	if (fullbright) {
		for (int channel = 0; channel < 3; ++channel) {
			untex->emissive_factor[channel] = untex->pbr_metallic_roughness.base_color_factor[channel];
			untex->pbr_metallic_roughness.base_color_factor[channel] = 0.0f;
		}
		untex->has_emissive_strength               = 1;
		untex->emissive_strength.emissive_strength = fullbright_strength;
	}
	if (xwau) {
		const OptGltfXwauMaterial* um = options->xwau_untextured;
		const float                s  = (um->shadeless || um->alpha_to_bloom) ? 1.0f : clamp01f(um->ambient);
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
	build->materials_count                             = (size_t)opt->texture_count + 1;
	const size_t untex_idx                             = (size_t)opt->texture_count;

	/* Skin composites: a copy of the texture's material with the composite's
	 * base color, and its own emissive or metallic-roughness where the bake
	 * differs. Material untex_idx + 1 + c. */
	for (size_t c = 0; c < skin_state.count; ++c) {
		SkinComposite*       composite = &skin_state.composites[c];
		const int32_t        ti        = composite->texture;
		const opt_texture_t* t         = &opt->textures[ti];
		XwauBaked*           baked     = &composite->baked;
		cgltf_material*      mat       = &build->materials[build->materials_count++];
		*mat                           = build->materials[ti];
		mat->name = gb_keep_string(build, xprintf_dup("%s_skin%03zu", build->materials[ti].name, c));
		mat->pbr_metallic_roughness.base_color_texture.texture =
			document_set_image(document, emi, baked->base, (uint32_t)t->width, (uint32_t)t->height,
							   xprintf_dup("Tex%02d_skin%03zu", (int)ti, c),
							   xprintf_dup("%s_Tex%02d_skin%03zu.png", basename, (int)ti, c));
		baked->base          = NULL;
		composite->images[0] = (int32_t)emi++;
		if (!composite->same_emissive) {
			if (baked->emissive) {
				mat->emissive_texture.texture = document_set_image(
					document, emi, baked->emissive, (uint32_t)t->width, (uint32_t)t->height,
					xprintf_dup("Tex%02d_skin%03zu_emissive", (int)ti, c),
					xprintf_dup("%s_Tex%02d_skin%03zu_emissive.png", basename, (int)ti, c));
				baked->emissive         = NULL;
				composite->images[1]    = (int32_t)emi++;
				mat->emissive_factor[0] = mat->emissive_factor[1] = mat->emissive_factor[2] = 1.0f;
				mat->has_emissive_strength                                                  = 1;
				mat->emissive_strength.emissive_strength                                    = XWAU_ILLUM_GAIN;
			} else {
				mat->emissive_texture.texture = NULL;
				mat->emissive_factor[0] = mat->emissive_factor[1] = mat->emissive_factor[2] = 0.0f;
				mat->has_emissive_strength                                                  = 0;
			}
		}
		if (!composite->same_metal_rough) {
			if (baked->metal_rough) {
				mat->pbr_metallic_roughness.metallic_roughness_texture.texture =
					document_set_image(document, emi, baked->metal_rough, (uint32_t)t->width,
									   (uint32_t)t->height, xprintf_dup("Tex%02d_skin%03zu_mr", (int)ti, c),
									   xprintf_dup("%s_Tex%02d_skin%03zu_mr.png", basename, (int)ti, c));
				baked->metal_rough                           = NULL;
				composite->images[2]                         = (int32_t)emi++;
				mat->pbr_metallic_roughness.metallic_factor  = 1.0f;
				mat->pbr_metallic_roughness.roughness_factor = 1.0f;
			} else {
				mat->pbr_metallic_roughness.metallic_roughness_texture.texture = NULL;
				mat->pbr_metallic_roughness.metallic_factor                    = baked->metallic;
				mat->pbr_metallic_roughness.roughness_factor                   = baked->roughness;
			}
		}
		xwau_set_specular(mat, baked->specular_color);
	}

	build->images_count   = emi;
	build->textures_count = emi;
	free(tex_emissive);
	tex_emissive = NULL;

	/* ---- Pass 1: build per-mesh geometry into BuiltMesh[] -------- */
	built = (BuiltMesh*)calloc(opt->mesh_count ? opt->mesh_count : 1, sizeof *built);
	if (!built) {
		fprintf(stderr, "opt2gltf: oom (meshes)\n");
		goto fail;
	}
	size_t   total_meshes    = 0;
	int      max_state_count = 1; /* drives KHR_materials_variants */
	uint32_t fixed_zero = 0, fixed_flip = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m = &opt->meshes[mi];
		if (!built_mesh_from_opt(&built[mi], m, meters_per_opt_unit, regen, smooth_angle_deg,
								 repair_normals)) {
			/* Components stay one-to-one with the OPT meshes. */
			if (error)
				snprintf(error->msg, sizeof error->msg, "mesh %d has no LOD 0 triangles", (int)mi);
			goto fail;
		}
		if (!built_mesh_subdivide(&built[mi], m->lods[0].group_count, subdivide_levels)) {
			if (error)
				snprintf(error->msg, sizeof error->msg, "mesh %d is too large to subdivide %u times", (int)mi,
						 subdivide_levels);
			goto fail;
		}
		/* XWAU: tangents for meshes drawing any normal-mapped material,
		 * in any switch state. */
		bool needs_tangents = false;
		for (int32_t g = 0; xwau && !needs_tangents && g < m->lods[0].group_count; ++g) {
			const opt_face_group_t* grp = &m->lods[0].groups[g];
			if (built[mi].fg_index_count_arr[g] == 0)
				continue;
			const int states = (grp->state_count > 1 && grp->state_textures) ? grp->state_count : 1;
			for (int st = 0; st < states && !needs_tangents; ++st) {
				const int32_t ti = st == 0 ? grp->texture_index : grp->state_textures[st];
				needs_tangents   = ti >= 0 && ti < opt->texture_count && xwau_normal_owner[ti] >= 0;
			}
		}
		if (needs_tangents && !built_mesh_generate_tangents(&built[mi])) {
			fprintf(stderr, "opt2gltf: oom (tangents)\n");
			goto fail;
		}
		total_meshes++;
		fixed_zero += built[mi].fixed_zero_normals;
		fixed_flip += built[mi].fixed_flipped_normals;
		const opt_lod_t* lod = &m->lods[0];
		for (int32_t g = 0; g < lod->group_count; ++g) {
			int sc = lod->groups[g].state_count;
			if (sc > max_state_count)
				max_state_count = sc;
		}
	}
	if (fixed_zero || fixed_flip) {
		fprintf(stderr, "opt2gltf: %s: repaired %u zero + %u face-opposed canonical normals\n", basename,
				fixed_zero, fixed_flip);
	}

	/* ---- Variants (asset-level) ---------------------------------- */
	/* Asset states first, then skin variants; with skin variants the asset
	 * states are emitted even for a single state. */
	const size_t skin_variant_count = skins ? skins->variant_count : 0;
	if (max_state_count > 1 || skin_variant_count > 0) {
		const size_t count = (size_t)max_state_count + skin_variant_count;
		build->variants    = (cgltf_material_variant*)calloc(count, sizeof *build->variants);
		if (!build->variants) {
			fprintf(stderr, "opt2gltf: oom\n");
			goto fail;
		}
		for (int v = 0; v < max_state_count; ++v) {
			build->variants[v].name = gb_keep_string(build, xprintf_dup("Variant%d", v));
		}
		for (size_t v = 0; v < skin_variant_count; ++v)
			build->variants[(size_t)max_state_count + v].name =
				gb_keep_string(build, xstrdup(skins->variants[v].name));
		build->variants_count = count;
	}
	/* Images only a composite uses list its variants. */
	for (size_t c = 0; c < skin_state.count; ++c) {
		const SkinComposite* composite = &skin_state.composites[c];
		size_t               len = 0, cap = 64;
		char*                json = (char*)malloc(cap);
		if (json)
			json[0] = '\0';
		json_append(&json, &len, &cap, "{\"aeronVariants\":[");
		for (size_t v = 0; v < composite->variant_count; ++v)
			json_append(&json, &len, &cap, "%s%zu", v ? "," : "",
						(size_t)max_state_count + composite->variants[v]);
		json_append(&json, &len, &cap, "]}");
		if (!json) {
			fprintf(stderr, "opt2gltf: oom (skin extras)\n");
			goto fail;
		}
		gb_keep_string(build, json);
		for (int k = 0; k < 3; ++k)
			if (composite->images[k] >= 0)
				build->images[composite->images[k]].extras.data = json;
	}

	/* ---- Pass 2: write geometry to .bin + accessors + meshes ----- */
	build->meshes = (cgltf_mesh*)calloc(total_meshes, sizeof *build->meshes);
	if (!build->meshes) {
		fprintf(stderr, "opt2gltf: oom\n");
		goto fail;
	}
	build->meshes_count = total_meshes;

	/* Exact pre-sized allocations for every array cgltf indexes by
	 * pointer arithmetic at write time. ANY realloc mid-build would
	 * dangle the cgltf_* pointers stored in dependent records (e.g.
	 * primitive.indices points into accessors[]; attribute.data points
	 * into accessors[]; node.mesh points into meshes[]). pre-counting
	 * is one extra walk; reallocs are forbidden. */
	size_t total_prims = 0, total_attrs = 0, total_maps = 0;
	size_t total_views = 0, total_accs = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m   = &opt->meshes[mi];
		const opt_lod_t*  lod = &m->lods[0];
		/* 3 vertex-attribute streams (pos/nrm/uv), plus TANGENT when
		 * generated: one view + one accessor each. */
		const size_t streams = built[mi].has_tangents ? 4u : 3u;
		total_views += streams;
		total_accs += streams;
		for (int32_t g = 0; g < lod->group_count; ++g) {
			if (built[mi].fg_index_count_arr[g] == 0)
				continue;
			total_prims++;
			total_attrs += streams; /* POSITION, NORMAL, TEXCOORD_0 [, TANGENT] */
			total_views += 1; /* index buffer view */
			total_accs += 1;  /* index accessor */
			const int32_t t0 = lod->groups[g].texture_index;
			if ((lod->groups[g].state_count > 1 && lod->groups[g].state_textures) ||
				skin_texture_has_composite(&skin_state, skin_variant_count, opt->texture_count, t0))
				total_maps += build->variants_count;
		}
	}
	build->views      = (cgltf_buffer_view*)calloc(total_views ? total_views : 1, sizeof *build->views);
	build->accessors  = (cgltf_accessor*)calloc(total_accs ? total_accs : 1, sizeof *build->accessors);
	build->primitives = (cgltf_primitive*)calloc(total_prims ? total_prims : 1, sizeof *build->primitives);
	build->attributes = (cgltf_attribute*)calloc(total_attrs ? total_attrs : 1, sizeof *build->attributes);
	build->mappings   = (cgltf_material_mapping*)calloc(total_maps ? total_maps : 1, sizeof *build->mappings);
	build->views_cap  = total_views;
	build->accessors_cap  = total_accs;
	build->primitives_cap = total_prims;
	build->attributes_cap = total_attrs;
	build->mappings_cap   = total_maps;
	if (!build->views || !build->accessors || !build->primitives || !build->attributes || !build->mappings) {
		fprintf(stderr, "opt2gltf: oom (geometry arrays)\n");
		goto fail;
	}

	base_material_count = build->materials_count;
	promoted_materials  = (cgltf_material**)calloc(base_material_count * 2, sizeof *promoted_materials);
	if (!promoted_materials) {
		fprintf(stderr, "opt2gltf: oom (promoted materials)\n");
		goto fail;
	}

	size_t mesh_out = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		BuiltMesh*        bm   = &built[mi];
		const opt_mesh_t* m    = &opt->meshes[mi];
		const opt_lod_t*  lod  = &m->lods[0];
		cgltf_mesh*       mesh = &build->meshes[mesh_out++];
		mesh->name             = gb_keep_string(
			build, xprintf_dup("Mesh%02d_%s", (int)mi, opt_mesh_type_name(m->descriptor.mesh_type)));

		/* Vertex stream: positions / normals / uvs. */
		size_t pos_off = bin_append(build, NULL, 0);
		/* compute min/max for POSITION accessor */
		float pmin[3] = { +INFINITY, +INFINITY, +INFINITY };
		float pmax[3] = { -INFINITY, -INFINITY, -INFINITY };
		for (uint32_t i = 0; i < bm->vertex_count; ++i) {
			const float* p = bm->vertices[i].pos;
			for (int k = 0; k < 3; ++k) {
				if (p[k] < pmin[k])
					pmin[k] = p[k];
				if (p[k] > pmax[k])
					pmax[k] = p[k];
			}
		}
		for (uint32_t i = 0; i < bm->vertex_count; ++i)
			bin_append(build, bm->vertices[i].pos, sizeof bm->vertices[i].pos);
		size_t             pos_bytes = bm->vertex_count * sizeof bm->vertices[0].pos;
		cgltf_buffer_view* pos_view = gb_new_view(build, pos_off, pos_bytes, cgltf_buffer_view_type_vertices);
		cgltf_accessor*    pos_acc =
			gb_new_accessor(build, pos_view, bm->vertex_count, cgltf_type_vec3, cgltf_component_type_r_32f);
		pos_acc->has_min = 1;
		pos_acc->has_max = 1;
		memcpy(pos_acc->min, pmin, sizeof pmin);
		memcpy(pos_acc->max, pmax, sizeof pmax);

		size_t nrm_off = bin_append(build, NULL, 0);
		for (uint32_t i = 0; i < bm->vertex_count; ++i)
			bin_append(build, bm->vertices[i].nrm, sizeof bm->vertices[i].nrm);
		size_t             nrm_bytes = bm->vertex_count * sizeof bm->vertices[0].nrm;
		cgltf_buffer_view* nrm_view = gb_new_view(build, nrm_off, nrm_bytes, cgltf_buffer_view_type_vertices);
		cgltf_accessor*    nrm_acc =
			gb_new_accessor(build, nrm_view, bm->vertex_count, cgltf_type_vec3, cgltf_component_type_r_32f);

		size_t uv_off = bin_append(build, NULL, 0);
		for (uint32_t i = 0; i < bm->vertex_count; ++i)
			bin_append(build, bm->vertices[i].uv, sizeof bm->vertices[i].uv);
		size_t             uv_bytes = bm->vertex_count * sizeof bm->vertices[0].uv;
		cgltf_buffer_view* uv_view  = gb_new_view(build, uv_off, uv_bytes, cgltf_buffer_view_type_vertices);
		cgltf_accessor*    uv_acc =
			gb_new_accessor(build, uv_view, bm->vertex_count, cgltf_type_vec2, cgltf_component_type_r_32f);

		cgltf_accessor* tan_acc = NULL;
		if (bm->has_tangents) {
			size_t tan_off = bin_append(build, NULL, 0);
			for (uint32_t i = 0; i < bm->vertex_count; ++i)
				bin_append(build, bm->vertices[i].tan, sizeof bm->vertices[i].tan);
			size_t             tan_bytes = bm->vertex_count * sizeof bm->vertices[0].tan;
			cgltf_buffer_view* tan_view =
				gb_new_view(build, tan_off, tan_bytes, cgltf_buffer_view_type_vertices);
			tan_acc = gb_new_accessor(build, tan_view, bm->vertex_count, cgltf_type_vec4,
									  cgltf_component_type_r_32f);
		}

		/* One primitive per face group with indices. */
		size_t           mesh_prim_count  = 0;
		cgltf_primitive* mesh_prims_start = build->primitives + build->primitives_count;
		for (int32_t g = 0; g < lod->group_count; ++g) {
			uint32_t ic = bm->fg_index_count_arr[g];
			if (ic == 0)
				continue;
			const opt_face_group_t* grp = &lod->groups[g];

			/* Indices for this face group: 16-bit while every vertex fits. */
			const uint32_t*      group_indices = bm->indices + bm->fg_index_start[g];
			size_t               idx_off, idx_bytes;
			cgltf_component_type idx_type;
			if (bm->vertex_count <= 65536u) {
				uint16_t* narrow = (uint16_t*)malloc((size_t)ic * sizeof *narrow);
				if (!narrow)
					goto fail;
				for (uint32_t i = 0; i < ic; ++i)
					narrow[i] = (uint16_t)group_indices[i];
				idx_bytes = (size_t)ic * sizeof *narrow;
				idx_off   = bin_append(build, narrow, idx_bytes);
				idx_type  = cgltf_component_type_r_16u;
				free(narrow);
			} else {
				idx_bytes = (size_t)ic * sizeof *group_indices;
				idx_off   = bin_append(build, group_indices, idx_bytes);
				idx_type  = cgltf_component_type_r_32u;
			}
			cgltf_buffer_view* idx_view =
				gb_new_view(build, idx_off, idx_bytes, cgltf_buffer_view_type_indices);
			cgltf_accessor* idx_acc = gb_new_accessor(build, idx_view, ic, cgltf_type_scalar, idx_type);

			/* Build the primitive. */
			cgltf_primitive* prim =
				(cgltf_primitive*)push_array((void**)&build->primitives, &build->primitives_count,
											 &build->primitives_cap, sizeof *prim, 1);
			prim->type    = cgltf_primitive_type_triangles;
			prim->indices = idx_acc;

			/* Attributes: POSITION, NORMAL, TEXCOORD_0 [, TANGENT]. */
			const size_t     attr_count = tan_acc ? 4u : 3u;
			cgltf_attribute* attrs =
				(cgltf_attribute*)push_array((void**)&build->attributes, &build->attributes_count,
											 &build->attributes_cap, sizeof *attrs, attr_count);
			attrs[0].name          = gb_keep_string(build, xstrdup("POSITION"));
			attrs[0].type          = cgltf_attribute_type_position;
			attrs[0].index         = 0;
			attrs[0].data          = pos_acc;
			attrs[1].name          = gb_keep_string(build, xstrdup("NORMAL"));
			attrs[1].type          = cgltf_attribute_type_normal;
			attrs[1].index         = 0;
			attrs[1].data          = nrm_acc;
			attrs[2].name          = gb_keep_string(build, xstrdup("TEXCOORD_0"));
			attrs[2].type          = cgltf_attribute_type_texcoord;
			attrs[2].index         = 0;
			attrs[2].data          = uv_acc;
			if (tan_acc) {
				attrs[3].name  = gb_keep_string(build, xstrdup("TANGENT"));
				attrs[3].type  = cgltf_attribute_type_tangent;
				attrs[3].index = 0;
				attrs[3].data  = tan_acc;
			}
			prim->attributes       = attrs;
			prim->attributes_count = attr_count;

			/* Default material = state 0 (or untextured fallback). */
			int t0 = grp->texture_index;
			prim->material =
				(t0 >= 0 && t0 < opt->texture_count) ? &build->materials[t0] : &build->materials[untex_idx];

			/* Variant mappings: for a face group with more than one state or
			 * a skin composite, one per variant. */
			const int  sc       = grp->state_count;
			const bool switched = sc > 1 && grp->state_textures;
			if (switched ||
				skin_texture_has_composite(&skin_state, skin_variant_count, opt->texture_count, t0)) {
				const size_t            vc   = build->variants_count;
				cgltf_material_mapping* maps = (cgltf_material_mapping*)push_array(
					(void**)&build->mappings, &build->mappings_count, &build->mappings_cap, sizeof *maps, vc);
				int rank = alpha_class_rank(prim->material);
				for (size_t v = 0; v < vc; ++v) {
					/* Asset variant v shows state v; a skin variant its
					 * composite, else its state. */
					size_t  state     = v;
					int32_t composite = -1;
					if (v >= (size_t)max_state_count) {
						const size_t sv = v - (size_t)max_state_count;
						state           = skins->variants[sv].state;
						if (t0 >= 0 && t0 < opt->texture_count)
							composite = skin_state.composite_of[sv * (size_t)opt->texture_count + (size_t)t0];
					}
					cgltf_material* m_ref;
					if (composite >= 0) {
						m_ref = &build->materials[untex_idx + 1 + (size_t)composite];
					} else {
						/* The original OPT walker clamps each switch to its last child. */
						const int ti =
							switched ? grp->state_textures[state < (size_t)sc ? state : (size_t)sc - 1] : t0;
						m_ref = (ti >= 0 && ti < opt->texture_count) ? &build->materials[ti]
																	 : &build->materials[untex_idx];
					}
					maps[v].variant  = (cgltf_size)v;
					maps[v].material = m_ref;
					if (alpha_class_rank(m_ref) > rank)
						rank = alpha_class_rank(m_ref);
				}
				int mixed = alpha_class_rank(prim->material) != rank;
				for (size_t v = 0; v < vc; ++v)
					mixed |= alpha_class_rank(maps[v].material) != rank;
				if (mixed) {
					prim->material = promoted_variant_material(build, prim->material, rank, base_material_count,
															   promoted_materials);
					for (size_t v = 0; v < vc; ++v)
						maps[v].material = promoted_variant_material(build, maps[v].material, rank,
																	 base_material_count, promoted_materials);
					fprintf(stderr, "opt2gltf: %s: mesh %d face group %d switch states mix alpha classes; all use %s\n",
							basename, (int)mi, (int)g, rank == 2 ? "BLEND" : "MASK");
				}
				prim->mappings       = maps;
				prim->mappings_count = (cgltf_size)vc;
			}
			mesh_prim_count++;
		}
		mesh->primitives       = mesh_prims_start;
		mesh->primitives_count = mesh_prim_count;
	}

	/* ---- Nodes: one root + one per mesh (with hardpoint children) -- */
	/* Count nodes: 1 root + (visible meshes) + (hardpoint + engine-glow
	 * child nodes across all visible meshes). */
	size_t total_hardpoints = 0;
	size_t total_glows      = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		total_hardpoints += opt->meshes[mi].hardpoint_count;
		total_glows += opt->meshes[mi].engine_glow_count;
	}
	const size_t total_lights = options ? options->light_count : 0;
	for (size_t li = 0; li < total_lights; ++li) {
		if (options->lights[li].mesh < 0 || options->lights[li].mesh >= opt->mesh_count) {
			if (error)
				snprintf(error->msg, sizeof error->msg, "light %zu names OPT mesh %d of %d", li,
						 (int)options->lights[li].mesh, (int)opt->mesh_count);
			goto fail;
		}
	}
	if (total_lights) {
		build->lights = (cgltf_light*)calloc(total_lights, sizeof *build->lights);
		if (!build->lights)
			goto fail;
		build->lights_count = total_lights;
	}
	size_t total_nodes = 1 + total_meshes + total_hardpoints + total_glows + total_lights;
	build->nodes       = (cgltf_node*)calloc(total_nodes, sizeof *build->nodes);
	if (!build->nodes) {
		fprintf(stderr, "opt2gltf: oom\n");
		goto fail;
	}
	build->nodes_count = total_nodes;
	/* cgltf transform helpers read the implicit glTF defaults from these arrays. */
	for (size_t i = 0; i < total_nodes; ++i) {
		build->nodes[i].rotation[3] = 1.0f;
		build->nodes[i].scale[0]    = 1.0f;
		build->nodes[i].scale[1]    = 1.0f;
		build->nodes[i].scale[2]    = 1.0f;
	}

	/* Layout in build->nodes[]:
	 *   [0]                   = scene root
	 *   [1 .. total_meshes]   = per-mesh nodes
	 *   [1+total_meshes ..]   = hardpoint + engine-glow + light child
	 *                           nodes, contiguous per parent mesh */
	cgltf_node* root_node = &build->nodes[0];
	root_node->name       = gb_keep_string(build, xprintf_dup("OPT_%s", basename));
	if (!set_flight_extension(build, root_node, "{\"role\":\"model\"}"))
		goto fail;

	cgltf_node** root_children = (cgltf_node**)calloc(total_meshes, sizeof *root_children);
	if (!root_children) {
		fprintf(stderr, "opt2gltf: oom\n");
		goto fail;
	}
	root_node->children       = root_children;
	root_node->children_count = total_meshes;

	cgltf_node* next_mesh_node  = &build->nodes[1];
	cgltf_node* next_child_node = &build->nodes[1 + total_meshes];

	mesh_out = 0;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_mesh_t* m     = &opt->meshes[mi];
		cgltf_node*       node  = next_mesh_node++;
		root_children[mesh_out] = node;
		node->parent            = root_node;
		node->mesh              = &build->meshes[mesh_out];
		/* Alias the mesh's already-kept name; do NOT re-push into
		 * owned_strings or the cleanup loop will double-free. */
		node->name = build->meshes[mesh_out].name;

		/* Fixed fields fit in 2048 bytes; each tag needs at most 66. */
		const size_t extension_size = 2048 + tag_count * 66;
		char*        extension      = (char*)malloc(extension_size);
		if (!extension)
			goto fail;
		size_t off = 0;
		off += snprintf(extension + off, extension_size - off,
						"{\"role\":\"component\",\"meshType\":%d,"
						"\"explosionFlags\":%u,\"targetId\":%d",
						(int)m->descriptor.mesh_type, (unsigned)m->descriptor.explosion_type,
						(int)m->descriptor.target_id);
		if (m->has_descriptor) {
			float center[3];
			swap_axis_v3(&m->descriptor.center, center);
			const float span[3] = {
				m->descriptor.span.x,
				m->descriptor.span.z,
				m->descriptor.span.y,
			};
			const float bounds_min[3] = {
				-m->descriptor.bbox_max.x,
				m->descriptor.bbox_min.z,
				-m->descriptor.bbox_max.y,
			};
			const float bounds_max[3] = {
				-m->descriptor.bbox_min.x,
				m->descriptor.bbox_max.z,
				-m->descriptor.bbox_min.y,
			};
			off += snprintf(extension + off, extension_size - off,
							",\"span\":[%.9g,%.9g,%.9g],"
							"\"center\":[%.9g,%.9g,%.9g],"
							"\"boundsMin\":[%.9g,%.9g,%.9g],"
							"\"boundsMax\":[%.9g,%.9g,%.9g]",
							span[0] * meters_per_opt_unit, span[1] * meters_per_opt_unit,
							span[2] * meters_per_opt_unit, center[0] * meters_per_opt_unit,
							center[1] * meters_per_opt_unit, center[2] * meters_per_opt_unit,
							bounds_min[0] * meters_per_opt_unit, bounds_min[1] * meters_per_opt_unit,
							bounds_min[2] * meters_per_opt_unit, bounds_max[0] * meters_per_opt_unit,
							bounds_max[1] * meters_per_opt_unit, bounds_max[2] * meters_per_opt_unit);
		}
		if (m->descriptor.target_id != 0) {
			float target[3];
			swap_axis_v3(&m->descriptor.target, target);
			off += snprintf(extension + off, extension_size - off, ",\"target\":[%.9g,%.9g,%.9g]",
							target[0] * meters_per_opt_unit, target[1] * meters_per_opt_unit,
							target[2] * meters_per_opt_unit);
		}

		/* Emit for every mesh carrying the node — the default axis
		 * frame is NOT "static": XWA rotates default-frame meshes
		 * about their pivot whenever their runtime angle is nonzero
		 * (hangar droid antennas spin about the default vertical axis). */
		if (m->has_rotation_scale) {
			float pivot[3], rax[3], dax[3], uax[3];
			swap_axis_v3(&m->rotation_scale.pivot, pivot);
			/* pivot is a position — scale it; axes are directions — don't. */
			pivot[0] *= meters_per_opt_unit;
			pivot[1] *= meters_per_opt_unit;
			pivot[2] *= meters_per_opt_unit;
			swap_axis_v3(&m->rotation_scale.rotation_axis, rax);
			swap_axis_v3(&m->rotation_scale.direction_axis, dax);
			swap_axis_v3(&m->rotation_scale.up_axis, uax);
			for (int axis = 0; axis < 3; ++axis) {
				rax[axis] /= OPT_Q15_TO_UNIT;
				dax[axis] /= OPT_Q15_TO_UNIT;
				uax[axis] /= OPT_Q15_TO_UNIT;
			}
			off += snprintf(extension + off, extension_size - off,
							",\"rotation\":{"
							"\"pivot\":[%.9g,%.9g,%.9g],"
							"\"rotationAxis\":[%.9g,%.9g,%.9g],"
							"\"directionAxis\":[%.9g,%.9g,%.9g],"
							"\"upAxis\":[%.9g,%.9g,%.9g]}",
							pivot[0], pivot[1], pivot[2], rax[0], rax[1], rax[2], dax[0], dax[1], dax[2],
							uax[0], uax[1], uax[2]);
		}
		/* Tags in option order, duplicates dropped. */
		bool tags_open = false;
		for (size_t t = 0; t < tag_count; ++t) {
			const OptGltfComponentTag* tag = &options->component_tags[t];
			if (tag->mesh_index != (uint32_t)mi)
				continue;
			bool duplicate = false;
			for (size_t previous = 0; previous < t && !duplicate; ++previous)
				duplicate = options->component_tags[previous].mesh_index == (uint32_t)mi &&
							strcmp(options->component_tags[previous].tag, tag->tag) == 0;
			if (duplicate)
				continue;
			off += snprintf(extension + off, extension_size - off, "%s\"%s\"",
							tags_open ? "," : ",\"tags\":[", tag->tag);
			tags_open = true;
		}
		snprintf(extension + off, extension_size - off, tags_open ? "]}" : "}");
		const bool extension_set = set_flight_extension(build, node, extension);
		free(extension);
		if (!extension_set)
			goto fail;

		/* Child nodes: hardpoints, engine glows, then lights (emitted as
		 * empties so Blender and other tools surface them). */
		int mesh_lights = 0;
		for (size_t li = 0; li < total_lights; ++li)
			if (options->lights[li].mesh == mi)
				++mesh_lights;
		int child_total = m->hardpoint_count + m->engine_glow_count + mesh_lights;
		if (child_total > 0) {
			cgltf_node** children = (cgltf_node**)calloc(child_total, sizeof *children);
			if (!children) {
				fprintf(stderr, "opt2gltf: oom\n");
				goto fail;
			}
			int ci = 0;
			for (int h = 0; h < m->hardpoint_count; ++h) {
				const opt_hardpoint_t* hp  = &m->hardpoints[h];
				cgltf_node*            hpn = next_child_node++;
				hpn->parent                = node;
				hpn->name = gb_keep_string(build, xprintf_dup("hardpoint_%d", (int)hp->type));
				float pos[3];
				swap_axis_v3(&hp->pos, pos);
				hpn->has_translation = 1;
				hpn->translation[0]  = pos[0] * meters_per_opt_unit;
				hpn->translation[1]  = pos[1] * meters_per_opt_unit;
				hpn->translation[2]  = pos[2] * meters_per_opt_unit;
				char hardpoint_extension[96];
				snprintf(hardpoint_extension, sizeof hardpoint_extension,
						 "{\"role\":\"hardpoint\",\"type\":%d}", (int)hp->type);
				if (!set_flight_extension(build, hpn, hardpoint_extension))
					goto fail;
				children[ci++] = hpn;
			}
			for (int e = 0; e < m->engine_glow_count; ++e) {
				const opt_engine_glow_t* eg  = &m->engine_glows[e];
				cgltf_node*              egn = next_child_node++;
				egn->parent                  = node;
				egn->name                    = gb_keep_string(build, xprintf_dup("engine_glow_%d", e));
				float pos[3], look[3], up[3], right[3], rotation[4];
				swap_axis_v3(&eg->position, pos);
				swap_axis_v3(&eg->look_axis, look);
				swap_axis_v3(&eg->up_axis, up);
				swap_axis_v3(&eg->right_axis, right);
				egn->has_translation = 1;
				egn->translation[0]  = pos[0] * meters_per_opt_unit;
				egn->translation[1]  = pos[1] * meters_per_opt_unit;
				egn->translation[2]  = pos[2] * meters_per_opt_unit;
				quaternion_from_axes(right, up, look, rotation);
				egn->has_rotation = 1;
				memcpy(egn->rotation, rotation, sizeof rotation);
				egn->has_scale           = 1;
				egn->scale[0]            = eg->dimensions.x * meters_per_opt_unit;
				egn->scale[1]            = eg->dimensions.y * meters_per_opt_unit;
				egn->scale[2]            = eg->dimensions.z * meters_per_opt_unit;
				const uint32_t colors[2] = { eg->core_color, eg->outer_color };
				float          rgba[2][4];
				for (int color = 0; color < 2; ++color) {
					rgba[color][0] = (float)((colors[color] >> 16) & 0xff) / 255.0f;
					rgba[color][1] = (float)((colors[color] >> 8) & 0xff) / 255.0f;
					rgba[color][2] = (float)(colors[color] & 0xff) / 255.0f;
					rgba[color][3] = (float)((colors[color] >> 24) & 0xff) / 255.0f;
				}
				char glow_extension[512];
				snprintf(glow_extension, sizeof glow_extension,
						 "{\"role\":\"engineGlow\",\"enabled\":%s,"
						 "\"coreColor\":[%.9g,%.9g,%.9g,%.9g],"
						 "\"outerColor\":[%.9g,%.9g,%.9g,%.9g]}",
						 eg->is_disabled ? "false" : "true", rgba[0][0], rgba[0][1], rgba[0][2], rgba[0][3],
						 rgba[1][0], rgba[1][1], rgba[1][2], rgba[1][3]);
				if (!set_flight_extension(build, egn, glow_extension))
					goto fail;
				children[ci++] = egn;
			}
			for (size_t li = 0; li < total_lights; ++li) {
				const OptGltfLight* source = &options->lights[li];
				if (source->mesh != mi)
					continue;
				cgltf_light* light = &build->lights[li];
				light->name        = gb_keep_string(build, xprintf_dup("light_%zu", li));
				light->type        = cgltf_light_type_point;
				memcpy(light->color, source->color, sizeof light->color);
				light->intensity = source->intensity;
				light->range     = source->range;
				cgltf_node* ln   = next_child_node++;
				ln->parent       = node;
				ln->name         = light->name;
				ln->light        = light;
				const opt_vec3_t position = { source->position[0], source->position[1], source->position[2] };
				float            pos[3];
				swap_axis_v3(&position, pos);
				ln->has_translation = 1;
				ln->translation[0]  = pos[0] * meters_per_opt_unit;
				ln->translation[1]  = pos[1] * meters_per_opt_unit;
				ln->translation[2]  = pos[2] * meters_per_opt_unit;
				if (!set_flight_extension(build, ln, "{\"role\":\"light\"}"))
					goto fail;
				children[ci++] = ln;
			}
			node->children       = children;
			node->children_count = child_total;
		}

		mesh_out++;
	}

	/* ---- Scene + asset ------------------------------------------- */
	document->scene.name        = gb_keep_string(build, xstrdup("Scene"));
	document->scene.nodes_count = 1;
	/* The scene has exactly one root: build->nodes[0]. */
	cgltf_node** scene_roots = (cgltf_node**)calloc(1, sizeof *scene_roots);
	if (!scene_roots)
		goto fail;
	scene_roots[0]        = root_node;
	document->scene.nodes = scene_roots;
	document->scene_roots = scene_roots;

	/* Buffer size = total .bin bytes. */
	build->buffer.size = build->bin_size;
	build->buffer.data = build->bin;

	/* Final cgltf_data wiring. */
	document->data.asset.version   = "2.0";
	document->data.asset.generator = "opt2gltf";
	if (skins) {
		size_t len = 0, cap = 256;
		char*  json = (char*)malloc(cap);
		if (json)
			json[0] = '\0';
		json_append(&json, &len, &cap, "{\"aeronSkinMarkings\":[");
		for (size_t m = 0; m < skins->markings_count; ++m) {
			json_append(&json, &len, &cap, m ? "," : "");
			if (skins->default_markings[m])
				json_append_string(&json, &len, &cap, skins->default_markings[m]);
			else
				json_append(&json, &len, &cap, "null");
		}
		json_append(&json, &len, &cap, "]}");
		if (!json)
			goto fail;
		document->data.asset.extras.data = gb_keep_string(build, json);
		size_t own_emissive = 0, own_metal_rough = 0;
		for (size_t c = 0; c < skin_state.count; ++c) {
			own_emissive += skin_state.composites[c].images[1] >= 0;
			own_metal_rough += skin_state.composites[c].images[2] >= 0;
		}
		fprintf(
			stderr,
			"opt2gltf: %s: %zu skin variants, %zu composites (%zu own emissive, %zu own metallic-roughness), "
			"%zu markings\n",
			basename, skins->variant_count, skin_state.count, own_emissive, own_metal_rough,
			skins->markings_count);
	}

	document->data.buffers            = &build->buffer;
	document->data.buffers_count      = 1;
	document->data.buffer_views       = build->views;
	document->data.buffer_views_count = build->views_count;
	document->data.accessors          = build->accessors;
	document->data.accessors_count    = build->accessors_count;
	document->data.images             = build->images;
	document->data.images_count       = build->images_count;
	document->data.textures           = build->textures;
	document->data.textures_count     = build->textures_count;
	document->data.samplers           = &build->sampler;
	document->data.samplers_count     = 1;
	document->data.materials          = build->materials;
	document->data.materials_count    = build->materials_count;
	document->data.meshes             = build->meshes;
	document->data.meshes_count       = build->meshes_count;
	document->data.nodes              = build->nodes;
	document->data.nodes_count        = build->nodes_count;
	document->data.scenes             = &document->scene;
	document->data.scenes_count       = 1;
	document->data.scene              = &document->scene;
	document->data.variants           = build->variants;
	document->data.variants_count     = build->variants_count;
	document->data.lights             = build->lights;
	document->data.lights_count       = build->lights_count;

	/* Declare the flight contract and optional material variants. */
	const char*  ext_flight        = "AERON_flight_model";
	const char*  ext_variants      = "KHR_materials_variants";
	const size_t extension_count   = build->variants_count > 0 ? 2u : 1u;
	document->data.extensions_used = (char**)calloc(extension_count, sizeof(char*));
	if (!document->data.extensions_used)
		goto fail;
	document->data.extensions_used[0] = (char*)ext_flight;
	if (build->variants_count > 0)
		document->data.extensions_used[1] = (char*)ext_variants;
	document->data.extensions_used_count = extension_count;

	free(promoted_materials);
	promoted_materials = NULL;

	/* The geometry build is temporary; the cgltf accessors now reference
	 * copies in build->bin. */
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
		built_mesh_free(&built[mi]);
	free(built);
	built = NULL;
	xwau_state_free(xwau_baked, opt->texture_count, xwau_normal_owner, xwau_normal_image);
	free(screen_cover_image);
	skin_state_free(&skin_state);

	*out_document = document;
	return true;

fail:
	free(promoted_materials);
	free(tex_emissive);
	free(screen_cover_image);
	skin_state_free(&skin_state);
	xwau_state_free(xwau_baked, opt->texture_count, xwau_normal_owner, xwau_normal_image);
	if (built) {
		for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
			built_mesh_free(&built[mi]);
		free(built);
	}
	if (error && !error->msg[0])
		snprintf(error->msg, sizeof error->msg, "out of memory while converting %s", basename);
	OptGltf_Free(document);
	return false;
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
	free(gb->materials);
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
