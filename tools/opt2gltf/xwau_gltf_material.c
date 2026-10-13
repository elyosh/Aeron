#include "xwau_gltf_material.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xwau_config.h"
#include "xwau_gltf_skins.h"
#include "xwau_skins.h"

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

void XwauGltf_BakedFree(XwauBaked* baked) {
	free(baked->base);
	free(baked->emissive);
	free(baked->metal_rough);
	free(baked->transmission);
	memset(baked, 0, sizeof *baked);
}

float XwauGltf_Unlit(const XwauGltfMaterial* xm) {
	if (xm->lit)
		return 0.0f;
	return (xm->shadeless || xm->alpha_to_bloom) ? 1.0f : clamp01f(xm->ambient);
}

bool XwauGltf_TextureHasIllum(const opt_texture_t* t) {
	if (t->bytes_per_texel == 4)
		return t->flag_illum;
	/* The renderer creates the light texture when any stored texel, mips
	 * included, is lit. */
	for (int32_t k = 0; k < t->mip_chain_bytes; ++k)
		if (xwau_palette_lit(t->palette, t->pixels[k]))
			return true;
	return false;
}

/* Copies `n` RGBA texels; NULL on OOM. */
uint8_t* XwauGltf_RgbaDup(const uint8_t* rgba, size_t n) {
	uint8_t* copy = (uint8_t*)malloc(n * 4);
	if (copy)
		memcpy(copy, rgba, n * 4);
	return copy;
}

/* `colour`, when set, receives a copy of the decoded colours before the bake
 * (NULL without pixels). */
static bool xwau_bake_texture(const opt_texture_t* t, int base_shade, const XwauGltfMaterial* xm,
							  XwauBaked* out, uint8_t** colour) {
	memset(out, 0, sizeof *out);
	if (colour)
		*colour = NULL;
	uint8_t* rgba = OptGltf_DecodeTexture(t, base_shade);
	if (!rgba)
		return true; /* no pixels: the caller emits the missing-image material */
	if (colour) {
		*colour = XwauGltf_RgbaDup(rgba, (size_t)t->width * (size_t)t->height);
		if (!*colour) {
			free(rgba);
			return false;
		}
	}
	return XwauGltf_BakeRgba(t, xm, rgba, out);
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
 * (XwauGltfAnim.anim_base): the colours `*colour` (before the bake),
 * which its frames apply over. XWAU shows them, lit, where a frame turns
 * the texture's own glow off; at rest the glowing texels then also receive
 * the scene's light. With an `always` flipbook the material has no
 * emission of its own (the frames are its glow). Frees `*colour`. */
void XwauGltf_AnimBase(uint8_t** colour, uint32_t w, uint32_t h, const XwauGltfAnim* a, XwauBaked* baked) {
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
bool XwauGltf_BakeRgba(const opt_texture_t* t, const XwauGltfMaterial* xm, uint8_t* rgba, XwauBaked* out) {
	memset(out, 0, sizeof *out);
	out->base                = rgba;
	const size_t n           = (size_t)t->width * (size_t)t->height;
	const bool   bpp32       = t->bytes_per_texel == 4;
	const bool   transparent = t->alpha != NULL;
	/* A shadeless transparent texture (cockpit glass) returns before the
	   illumination branch (XwaD3dPixelShader.hlsl:80); so does a `lit` one,
	   whose Shadeless the override cleared. */
	const bool  has_illum   = XwauGltf_TextureHasIllum(t) && !(transparent && (xm->shadeless || xm->lit));
	const bool  glass       = transparent && !xm->lit && !xm->shadeless && !xm->alpha_isnt_glass &&
							  !xm->alpha_to_bloom && !has_illum;
	const float metallic    = xwau_metallic(xm->metallic);
	const float roughness   = xwau_roughness(xm->glossiness);
	const float glass_rough = XWAU_GLASS_ROUGHNESS;
	const float shadeless   = XwauGltf_Unlit(xm);

	out->transparent = transparent;
	out->emissive    = (uint8_t*)malloc(n * 4);
	out->metal_rough = (uint8_t*)malloc(n * 4);
	if (!out->emissive || !out->metal_rough) {
		XwauGltf_BakedFree(out);
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
			e[k]  = unorm8(c[k] * (1.0f + (XWAU_GLTF_ILLUM_GAIN - 1.0f) * w) * s / XWAU_GLTF_ILLUM_GAIN);
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
			XwauGltf_BakedFree(out);
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

/* ===== The materials ================================================== */

struct XwauGltfBakeState {
	XwauBaked*     baked;   /* per texture */
	uint8_t**      normals; /* per texture: the normal map it bakes, or NULL */
	char**         strings; /* image names and files */
	size_t         string_count;
	XwauGltfSkins* skins;
};

/* `format` printed into a new string kept by `s`; NULL when out of memory. */
static const char* keep(struct XwauGltfBakeState* s, const char* format, ...) {
	char    text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof text, format, args);
	va_end(args);
	char** grown = (char**)realloc(s->strings, (s->string_count + 1) * sizeof *grown);
	if (!grown)
		return NULL;
	s->strings = grown;
	return (grown[s->string_count] = Xwau_Strdup(text)) ? grown[s->string_count++] : NULL;
}

/* Texture `i`'s image `kind` (Tex<i>_<kind>, <basename>_Tex<i>_<kind>.png). */
static bool texture_image(struct XwauGltfBakeState* s, const char* basename, int32_t i, const char* kind,
						  uint8_t* rgba, const opt_texture_t* t, OptGltfImage* out) {
	out->view = (OptGltfImageView) { rgba, (uint32_t)t->width, (uint32_t)t->height };
	out->name = keep(s, "Tex%02d_%s", (int)i, kind);
	out->file = keep(s, "%s_Tex%02d_%s.png", basename, (int)i, kind);
	return out->name && out->file;
}

/* Texture `i`'s given material, from its bake. */
static bool texture_material(struct XwauGltfBakeState* s, const opt_file_t* opt, const char* basename,
							 int32_t i, const XwauGltfMaterial* materials, const int32_t* normal_owner,
							 const XwauGltfAnim* anims, OptGltfMaterial* m) {
	const opt_texture_t* t     = &opt->textures[i];
	const XwauBaked*     baked = &s->baked[i];
	if (!baked->base)
		return true; /* no pixels: the stock material */
	/* XWAU materials are keyed by texture name: keep it. */
	m->name            = t->name[0] ? t->name : NULL;
	m->base.view       = (OptGltfImageView) { baked->base, (uint32_t)t->width, (uint32_t)t->height };
	m->alpha_from_base = baked->transparent;
	m->base_color[0] = m->base_color[1] = m->base_color[2] = 1.0f;
	bool ok                                                = true;
	if (baked->emissive) {
		ok &= texture_image(s, basename, i, "emissive", baked->emissive, t, &m->emissive);
		m->emissive_factor[0] = m->emissive_factor[1] = m->emissive_factor[2] = 1.0f;
		m->emissive_strength                                                  = XWAU_GLTF_ILLUM_GAIN;
	}
	if (baked->metal_rough) {
		ok &= texture_image(s, basename, i, "mr", baked->metal_rough, t, &m->metallic_roughness);
		m->metallic = m->roughness = 1.0f;
	} else {
		m->metallic  = baked->metallic;
		m->roughness = baked->roughness;
	}
	if (baked->transmission)
		ok &= texture_image(s, basename, i, "transmission", baked->transmission, t, &m->transmission);
	const int32_t owner = normal_owner[i];
	if (owner >= 0) {
		const XwauGltfMaterial* o = &materials[owner];
		m->normal.view = (OptGltfImageView) { s->normals[owner], o->normal_map.width, o->normal_map.height };
		m->normal.name = keep(s, "Tex%02d_normal", (int)owner);
		m->normal.file =
			keep(s, "%s_nm_%s_k%d.png", basename, o->normal_map_name ? o->normal_map_name : "map",
				 (int)lroundf(o->nm_intensity * 1000.0f));
		ok &= m->normal.name && m->normal.file;
	}
	m->specular       = true;
	m->specular_color = baked->specular_color;
	m->json           = anims ? anims[i].json : NULL;
	return ok;
}

bool XwauGltf_Bake(const opt_file_t* opt, const char* basename, const XwauGltfMaterial* materials,
				   const XwauGltfMaterial* untextured, const XwauGltfAnim* anims,
				   const OptGltfScreen* screens, const struct XwauSkins* skins, XwauGltfMaterials* out) {
	memset(out, 0, sizeof *out);
	const size_t              count = opt->texture_count ? (size_t)opt->texture_count : 1;
	struct XwauGltfBakeState* s     = (struct XwauGltfBakeState*)calloc(1, sizeof *s);
	int32_t*                  owner = (int32_t*)malloc(count * sizeof *owner);
	out->state                      = s;
	out->textures                   = (OptGltfMaterial*)calloc(count, sizeof *out->textures);
	if (s) {
		s->baked   = (XwauBaked*)calloc(count, sizeof *s->baked);
		s->normals = (uint8_t**)calloc(count, sizeof *s->normals);
	}
	bool ok = s && owner && out->textures && s->baked && s->normals;
	if (!ok)
		fprintf(stderr, "opt2gltf: oom (xwau)\n");

	/* Each texture's bake, and the first texture sharing its baked normal
	 * map (same source and NMIntensity), which bakes it. */
	const int base_shade = opt_palette_base_shade(opt);
	for (int32_t i = 0; ok && i < opt->texture_count; ++i) {
		const opt_texture_t* t      = &opt->textures[i];
		const XwauGltfAnim*  a      = anims ? &anims[i] : NULL;
		uint8_t*             colour = NULL;
		if (!xwau_bake_texture(t, base_shade, &materials[i], &s->baked[i],
							   a && a->anim_base ? &colour : NULL)) {
			fprintf(stderr, "opt2gltf: oom (xwau bake)\n");
			ok = false;
			break;
		}
		XwauGltf_AnimBase(&colour, (uint32_t)t->width, (uint32_t)t->height, a, &s->baked[i]);
		free(colour);
		owner[i] = -1;
		if (!materials[i].normal_map.rgba || !s->baked[i].base)
			continue;
		owner[i] = i;
		for (int32_t j = 0; j < i; ++j) {
			if (owner[j] == j && materials[j].normal_map.rgba == materials[i].normal_map.rgba &&
				materials[j].nm_intensity == materials[i].nm_intensity) {
				owner[i] = j;
				break;
			}
		}
	}
	for (int32_t i = 0; ok && i < opt->texture_count; ++i) {
		if (owner[i] != i)
			continue;
		if (!(s->normals[i] = xwau_bake_normal(&materials[i].normal_map, materials[i].nm_intensity))) {
			fprintf(stderr, "opt2gltf: oom (xwau normal)\n");
			ok = false;
		}
	}
	for (int32_t i = 0; ok && i < opt->texture_count; ++i)
		ok = texture_material(s, opt, basename, i, materials, owner, anims, &out->textures[i]);

	/* Untextured faces: grey, with the material's unlit split. */
	const float unlit = XwauGltf_Unlit(untextured);
	for (int channel = 0; ok && channel < 3; ++channel) {
		const float grey                         = 0.6f;
		out->untextured.base_color[channel]      = grey * (1.0f - unlit);
		out->untextured.emissive_factor[channel] = grey * unlit / XWAU_GLTF_ILLUM_GAIN;
	}
	out->untextured.emissive_strength = unlit > 0.0f ? XWAU_GLTF_ILLUM_GAIN : 0.0f;
	out->untextured.metallic          = xwau_metallic(untextured->metallic);
	out->untextured.roughness         = xwau_roughness(untextured->glossiness);
	out->untextured.specular          = true;
	out->untextured.specular_color    = xwau_specular_color(untextured->intensity);

	if (ok && skins && skins->skins.variant_count) {
		s->skins = XwauGltfSkins_Build(opt, basename, &skins->skins, screens, base_shade, materials, anims,
									   s->baked, out->textures);
		ok       = s->skins != NULL;
		if (ok)
			out->variants = *XwauGltfSkins_Variants(s->skins);
	}
	free(owner);
	if (!ok)
		XwauGltf_Free(out, opt->texture_count);
	return ok;
}

void XwauGltf_ReportSkins(const XwauGltfMaterials* materials, const char* basename) {
	if (materials->state && materials->state->skins)
		XwauGltfSkins_Report(materials->state->skins, basename);
}

void XwauGltf_Free(XwauGltfMaterials* materials, int32_t texture_count) {
	struct XwauGltfBakeState* s = materials->state;
	if (s) {
		for (int32_t i = 0; s->baked && i < texture_count; ++i)
			XwauGltf_BakedFree(&s->baked[i]);
		free(s->baked);
		for (int32_t i = 0; s->normals && i < texture_count; ++i)
			free(s->normals[i]);
		free(s->normals);
		for (size_t i = 0; i < s->string_count; ++i)
			free(s->strings[i]);
		free(s->strings);
		XwauGltfSkins_Free(s->skins);
		free(s);
	}
	free(materials->textures);
	memset(materials, 0, sizeof *materials);
}
