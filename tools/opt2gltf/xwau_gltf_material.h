#ifndef AERON_OPT2GLTF_XWAU_GLTF_MATERIAL_H
#define AERON_OPT2GLTF_XWAU_GLTF_MATERIAL_H

/*
 * XWAU materials as glTF materials: how the X-Wing Alliance Upgrade
 * "effects" renderer (xwa_ddraw_d3d11) shades an OPT's textures, baked into
 * the conversion's given materials (opt_gltf.h, OptGltfMaterial), with the
 * skins as material variants (xwau_gltf_skins.h).
 *
 * The bake reproduces that renderer's per-texel logic in standard glTF
 * inputs: authored colors, palette or 32bpp illumination, Shadeless/Ambient,
 * glass, the dark-texel reflectance fade (approximated through roughness),
 * Metallic/Glossiness, Intensity as KHR_materials_specular, and the normal
 * map with NMIntensity baked in. That renderer uses each corner's stored
 * normal, so XWAU models are converted with the stored normals, neither
 * remapped nor repaired, and without a smoothing angle.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "opt.h"
#include "opt_gltf.h"

struct XwauSkins;

/* The illumination's gain: illuminated texels are unlit at 1.25x their
 * color, stored as an emissive image of colors / gain at this strength. */
#define XWAU_GLTF_ILLUM_GAIN 1.25f

/* The material of one OPT texture, from the XWAU material files
 * (xwau_mat.h). */
typedef struct XwauGltfMaterial {
	float metallic;     /* Metallic */
	float intensity;    /* Intensity: dielectric F0 = 0.16 * intensity^2 */
	float glossiness;   /* Glossiness */
	float nm_intensity; /* NMIntensity */
	float specular_val; /* SpecularVal: luma floor of the dark-texel fade */
	float ambient;      /* Ambient: partial shadeless weight */
	bool  shadeless;
	bool  alpha_isnt_glass;
	bool  alpha_to_bloom;
	/* Baked as a plain lit surface: no unlit weight and no glass, whatever
	 * Shadeless, Ambient or AlphaToBloom say. */
	bool lit;
	/* Tangent-space normal map: R, G, B = X, Y, Z encoded 0..1 (+Y toward
	 * the first row), rows in UV order, alpha ignored; rgba NULL when the
	 * texture has none. normal_map_name labels its baked image file and must
	 * be unique per distinct map. */
	OptGltfImageView normal_map;
	const char*      normal_map_name;
} XwauGltfMaterial;

/*
 * Animated materials of one OPT texture (xwau_anim.h):
 *   - `json`: the material's flipbooks, damage and energy, as its
 *     AERON_flight_model object (the energy most materials share becomes
 *     the model root's default), or NULL;
 *   - `anim_base`: it has illumination flipbooks. Its base colour (and each
 *     of its skin variant materials') is then the texture's colours before
 *     the illumination split, which the frames apply over;
 *   - `always_illumination`: one of them always plays. The material then
 *     has no emission of its own (the frames are its glow).
 */
typedef struct XwauGltfAnim {
	const char* json;
	bool        anim_base;
	bool        always_illumination;
} XwauGltfAnim;

/* Whether XWAU's renderer gives texture `t` an illumination layer: a 32bpp
 * texture with the illumination flag, or an 8bpp one with a lit texel. */
bool XwauGltf_TextureHasIllum(const opt_texture_t* t);

/* The material's unlit weight s, 0..1: 1 for Shadeless (or AlphaToBloom),
 * else Ambient; 0 for a `lit` material. The bake stores lerp(lit, color, s)
 * as base color color·(1 − s) plus emission color·s; colour frames take the
 * same split. */
float XwauGltf_Unlit(const XwauGltfMaterial* xm);

/* The baked materials of an OPT: one per texture, the untextured one, and
 * the skins' variants; the images they name are owned here, and must
 * outlive the documents built with them. */
typedef struct XwauGltfMaterials {
	OptGltfMaterial*          textures; /* opt->texture_count */
	OptGltfMaterial           untextured;
	OptGltfVariants           variants; /* the skins; count 0 without */
	struct XwauGltfBakeState* state;
} XwauGltfMaterials;

/* Bakes the materials of `opt`: `materials` and `anims` have one entry per
 * texture (`anims` may be NULL), `screens` one per texture or NULL (screen
 * surfaces are not skinned), `skins` may be NULL. Image files are named
 * after `basename`. False when out of memory. */
bool XwauGltf_Bake(const opt_file_t* opt, const char* basename, const XwauGltfMaterial* materials,
				   const XwauGltfMaterial* untextured, const XwauGltfAnim* anims,
				   const OptGltfScreen* screens, const struct XwauSkins* skins, XwauGltfMaterials* out);

/* Reports the skins' variants and composites to stderr. */
void XwauGltf_ReportSkins(const XwauGltfMaterials* materials, const char* basename);

void XwauGltf_Free(XwauGltfMaterials* materials, int32_t texture_count);

/* ---- The bake, shared with the skins (xwau_gltf_skins.c) ----------------- */

typedef struct XwauBaked {
	uint8_t* base;           /* RGBA */
	uint8_t* emissive;       /* RGBA / XWAU_GLTF_ILLUM_GAIN; NULL when no texel emits */
	uint8_t* metal_rough;    /* RGBA, G roughness, B metallic; NULL when uniform */
	uint8_t* transmission;   /* RGBA, R = 1 - glass coverage; NULL without glass texels */
	float    metallic;       /* factors when metal_rough is NULL */
	float    roughness;      /* factors when metal_rough is NULL */
	float    specular_color; /* KHR_materials_specular specularColorFactor */
	bool     transparent;
} XwauBaked;

/* Bakes `rgba`, the decoded colors of texture `t` (owned from here, baked
 * in place into out->base); `t` gives the alpha, illumination and format.
 * False when out of memory. */
bool XwauGltf_BakeRgba(const opt_texture_t* t, const XwauGltfMaterial* xm, uint8_t* rgba, XwauBaked* out);

/* The base colour of a material with illumination flipbooks
 * (XwauGltfAnim.anim_base): the colours `*colour` (before the bake), which
 * its frames apply over. XWAU shows them, lit, where a frame turns the
 * texture's own glow off; at rest the glowing texels then also receive the
 * scene's light. With an `always` flipbook the material has no emission of
 * its own (the frames are its glow). Frees `*colour`. */
void XwauGltf_AnimBase(uint8_t** colour, uint32_t w, uint32_t h, const XwauGltfAnim* a, XwauBaked* baked);

void XwauGltf_BakedFree(XwauBaked* baked);

/* A copy of `n` RGBA texels; NULL when out of memory. */
uint8_t* XwauGltf_RgbaDup(const uint8_t* rgba, size_t n);

#endif /* AERON_OPT2GLTF_XWAU_GLTF_MATERIAL_H */
