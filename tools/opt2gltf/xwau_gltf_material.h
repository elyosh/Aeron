#ifndef AERON_OPT2GLTF_XWAU_GLTF_MATERIAL_H
#define AERON_OPT2GLTF_XWAU_GLTF_MATERIAL_H

/*
 * Converts XWAU materials to glTF materials. The X-Wing Alliance Upgrade
 * "effects" renderer (xwa_ddraw_d3d11) has its own way of shading the
 * textures of an OPT. This module bakes that shading into the given
 * materials of the conversion (opt_gltf.h, OptGltfMaterial), and turns the
 * skins into skin layers and skin variants (xwau_gltf_skins.h).
 *
 * The bake reproduces the per-texel logic of that renderer with standard
 * glTF inputs. It covers authored colors, palette or 32bpp illumination,
 * Shadeless/Ambient, glass, the dark-texel reflectance fade (approximated
 * with roughness), Metallic/Glossiness, Intensity as KHR_materials_specular,
 * and the normal map with NMIntensity baked in. That renderer uses the
 * stored normal of each corner, so XWAU models are converted with the
 * stored normals. They are not remapped or repaired, and no smoothing angle
 * is used.
 *
 * That renderer draws projectiles unlit, whatever their materials say, so
 * a projectile's textures are baked as emission only (XwauGltfProjectile).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "opt.h"
#include "opt_gltf.h"

struct XwauSkins;

/* Gain of the illumination. Illuminated texels are unlit and drawn at 1.25x
 * their color. They are stored as an emissive image of color / gain, with
 * this gain as the emissive strength. */
#define XWAU_GLTF_ILLUM_GAIN 1.25f

/* How XWAU's renderer draws the texture of a projectile. It draws it unlit,
 * with no specular and no glass, and blends it with the texture's alpha:
 *   - a laser shows its colour with the saturation raised 1.5 times and the
 *     value 2 times, in HSV. Its illumination isn't used;
 *   - a warhead shows its colour, with its illumination 1.25 times
 *     brighter, as a Shadeless texture.
 * The emissive image holds the linear colour divided by its largest value,
 * so that the colour above 1 isn't clipped and keeps its hue. The brightest
 * possible texel is 2 times OPT_GLTF_PROJECTILE_STRENGTH: a laser's boost
 * doubles its value, and a warhead's illumination makes it 1.25 times
 * brighter. In linear values, a laser's boost makes it 4.95 times brighter,
 * which would whiten the bolt.
 *
 * XWAU blends an 8-bit colour, so its faint edges stay faint. At these
 * strengths they would stay bright, so the emission of a transparent texture
 * is also multiplied by its alpha, and the blend applies the alpha twice. */
typedef enum XwauGltfProjectile {
	XWAU_GLTF_NOT_PROJECTILE = 0,
	XWAU_GLTF_LASER,
	XWAU_GLTF_WARHEAD,
} XwauGltfProjectile;

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
	/* Bake as a plain lit surface, with no unlit weight and no glass,
	 * regardless of Shadeless, Ambient or AlphaToBloom. */
	bool lit;
	/* Bake as a projectile, regardless of the other fields. */
	XwauGltfProjectile projectile;
	/* Tangent-space normal map. R, G, B hold X, Y, Z encoded as 0..1 (+Y
	 * toward the first row). Rows are in UV order, and alpha is ignored.
	 * rgba is NULL when the texture has no normal map. normal_map_name is
	 * used in the file name of the baked image, and must be unique for each
	 * distinct map. */
	OptGltfImageView normal_map;
	const char*      normal_map_name;
} XwauGltfMaterial;

/*
 * Animated materials of one OPT texture (xwau_anim.h):
 *   - `json`: the flipbooks, damage and energy of the material, as its
 *     AERON_flight_model object, or NULL. The energy that most materials
 *     share becomes the default on the model root.
 *   - `anim_base`: the material has illumination flipbooks. Its base colour,
 *     and the base colour of each of its skin layers, is then
 *     the colour of the texture before the illumination split. The frames
 *     are applied over it.
 *   - `always_illumination`: one of these flipbooks always plays. The
 *     material then has no emission of its own, because the frames are its
 *     glow.
 */
typedef struct XwauGltfAnim {
	const char* json;
	bool        anim_base;
	bool        always_illumination;
} XwauGltfAnim;

/* Returns whether XWAU's renderer gives texture `t` an illumination layer.
 * This is the case for a 32bpp texture with the illumination flag, and for
 * an 8bpp texture with at least one lit texel. */
bool XwauGltf_TextureHasIllum(const opt_texture_t* t);

/* Returns the unlit weight s of the material, 0..1. It is 1 for a
 * projectile, 0 for a `lit` material, 1 for Shadeless or AlphaToBloom, and
 * Ambient otherwise. The bake stores lerp(lit, color, s) as base color
 * color·(1 − s) plus emission color·s. Colour frames are split the same
 * way. */
float XwauGltf_Unlit(const XwauGltfMaterial* xm);

/* Returns the emissive strength of the material's baked emissive images.
 * They hold the colour divided by XWAU_GLTF_ILLUM_GAIN, or, for a
 * projectile, its linear colour divided by its largest value. */
float XwauGltf_EmissiveStrength(const XwauGltfMaterial* xm);

/* The baked materials of an OPT: one per texture, the untextured one, and
 * the skin variants. This struct owns the images they use, so it must
 * outlive the documents built from it. */
typedef struct XwauGltfMaterials {
	OptGltfMaterial*          textures; /* opt->texture_count */
	OptGltfMaterial           untextured;
	OptGltfVariants           variants; /* the skin variants; none when there are no skins */
	struct XwauGltfBakeState* state;
} XwauGltfMaterials;

/* Bakes the materials of `opt`. `materials` and `anims` have one entry per
 * texture, and `anims` may be NULL. `screens` has one entry per texture or
 * is NULL. Screen surfaces get no skins. `skins` may be NULL. The skins
 * become skin layers and skin variants (xwau_gltf_skins.h). Image files are
 * named after `basename`.
 * Returns false when out of memory or when the skins are invalid. */
bool XwauGltf_Bake(const opt_file_t* opt, const char* basename, const XwauGltfMaterial* materials,
				   const XwauGltfMaterial* untextured, const XwauGltfAnim* anims,
				   const OptGltfScreen* screens, const struct XwauSkins* skins, XwauGltfMaterials* out);

/* Reports the skin variants and layers to stderr. */
void XwauGltf_ReportSkins(const XwauGltfMaterials* materials, const char* basename);

void XwauGltf_Free(XwauGltfMaterials* materials, int32_t texture_count);

/* ---- The bake, shared with the skins (xwau_gltf_skins.c) ----------------- */

typedef struct XwauBaked {
	uint8_t* base;           /* RGBA */
	uint8_t* emissive;       /* RGBA / XWAU_GLTF_ILLUM_GAIN; NULL when no texel emits */
	uint8_t* metal_rough;    /* RGBA, G roughness, B metallic; NULL when uniform */
	uint8_t* transmission;   /* RGBA, R = transmission; NULL without glass texels */
	float    metallic;       /* factors when metal_rough is NULL */
	float    roughness;      /* factors when metal_rough is NULL */
	float    specular_color; /* KHR_materials_specular specularColorFactor */
	bool     transparent;
} XwauBaked;

/* Bakes `rgba`, the decoded colors of texture `t`. The function takes
 * ownership of `rgba` and bakes it in place into out->base. `t` gives the
 * alpha, the illumination and the format. Returns false when out of
 * memory. */
bool XwauGltf_BakeRgba(const opt_texture_t* t, const XwauGltfMaterial* xm, uint8_t* rgba, XwauBaked* out);

/* Sets the base colour of a material with illumination flipbooks
 * (XwauGltfAnim.anim_base) to `*colour`, the colours before the bake. The
 * frames are applied over this base. XWAU shows these colours, lit, where
 * a frame turns off the glow of the texture. Because the base keeps them,
 * the glowing texels also receive the light of the scene when no frame
 * plays. With an `always` flipbook, the material has no emission of its
 * own, because the frames are its glow. Frees `*colour`. */
void XwauGltf_AnimBase(uint8_t** colour, uint32_t w, uint32_t h, const XwauGltfAnim* a, XwauBaked* baked);

void XwauGltf_BakedFree(XwauBaked* baked);

/* Returns a copy of `n` RGBA texels, or NULL when out of memory. */
uint8_t* XwauGltf_RgbaDup(const uint8_t* rgba, size_t n);

#endif /* AERON_OPT2GLTF_XWAU_GLTF_MATERIAL_H */
