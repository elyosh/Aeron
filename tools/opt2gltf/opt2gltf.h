#ifndef AERON_OPT2GLTF_H
#define AERON_OPT2GLTF_H

/*
 * opt2gltf — convert one OPT file to glTF 2.0 (one .gltf JSON + one
 * .bin buffer + one .png per OPT texture, written to `out_dir`).
 *
 * Output is faithful to every OPT datum the engine reads:
 *
 *   - Geometry: one glTF node per OPT mesh, parented to a single root
 *     scene node. Each mesh node carries the complete authored MeshDescriptor
 *     in the `AERON_flight_model` node extension. Coordinates use standard
 *     glTF axes and meters.
 *
 *   - Rotation pivots and axes are preserved in the component role.
 *
 *   - Hardpoints: each OPT mesh's hardpoints become child empty
 *     nodes (no mesh, just translation) named "hardpoint_<type>". Each
 *     carries an `AERON_flight_model` hardpoint role and raw type.
 *
 *   - Every OPT mesh becomes exactly one component, so component ordinals
 *     match the OPT's mesh numbers. A mesh with vertices but no LOD 0 faces
 *     becomes one untextured degenerate triangle over its vertices, which
 *     the flight-model topology keeps as positions without faces; a mesh
 *     without vertices or LOD 0 triangles fails the conversion.
 *
 *   - A switched face group whose state textures fall in different alpha
 *     classes (opaque, mask, blend) uses, for every state, a copy of its
 *     material promoted to the most transparent class among them: the
 *     renderer keeps each primitive in one alpha class across variants.
 *
 *   - Paint-scheme variants: OPT NodeSwitch's per-face-group
 *     state_textures[] become a KHR_materials_variants extension at
 *     asset level. The variant count equals the largest LOD 0
 *     state_count across all face groups (typically 4 in the IVFILES
 *     set), followed by any skin variants (OptGltfSkins). Each
 *     multi-state face group emits its primitive with mappings from
 *     variant index to material; single-state face groups use a
 *     single default material.
 *
 *   - Textures: one glTF image per OPT texture, written as
 *     <out_dir>/textures/<opt_basename>_TexNN.png. Materials reference
 *     them via baseColorTexture. The shaded-15 base mip is what gets
 *     written (the OPT format stores per-shade mip chains; we keep
 *     only the base — generators can re-derive mips on import).
 *
 * `emissive` enables self-illumination export (off by default). When set,
 * textures whose palette marks self-lit texels (the flat-ramp trick used by
 * both TIE98 and XWA — e.g. lit windows, cockpit lights) get a second
 * `<basename>_TexNN_emissive.png` (base color on black) wired as the
 * material's emissiveTexture with a unit emissiveFactor.
 *
 * `smooth_angle_deg` controls vertex-normal regeneration. The 1998 OPT
 * normals are full-Gouraud (every adjacent face averaged, no angle
 * threshold), which fakes roundness on the low-poly hulls. A negative
 * value (the CLI default) keeps those normals and reproduces the classic
 * renderer's per-FaceData position remap: the first face-order normal for
 * each position is reused by later corners. This preserves the rounded
 * look without exposing conflicting corner normals the original renderer
 * ignored. With smooth_angle_deg >= 0 all stored normals are ignored.
 * Geometric face normals form connected smoothing fans across shared
 * edges that meet the threshold, then are corner-angle weighted so the
 * result is not biased by triangulation density. A fan that wraps around
 * a non-manifold vertex is split where its average would point behind a
 * rendered triangle. Edges beyond the angle split the vertex, e.g. ~45°
 * for a crisp hard-surface look or ~90° for the rounded appearance of
 * the low-poly original models.
 *
 * Returns true on success. Errors emit to stderr.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cgltf.h"
#include "opt.h"

/* Adds `tag` to the component built from OPT mesh `mesh_index`. Tags match
 * [a-z0-9._]{1,63}; duplicates on one component are dropped. */
typedef struct OptGltfComponentTag {
	uint32_t    mesh_index;
	const char* tag;
} OptGltfComponentTag;

typedef struct OptGltfImageView {
	const uint8_t* rgba;
	uint32_t       width;
	uint32_t       height;
} OptGltfImageView;

/*
 * Material of one OPT texture as the X-Wing Alliance Upgrade "effects"
 * renderer (xwa_ddraw_d3d11) shades it, resolved by the caller from the XWAU
 * material files (see xwau_mat.h). With these, the converter reproduces that
 * renderer's per-texel logic in standard glTF inputs (docs:
 * xwau_model_import.md, "Per-material output"): authored colors, palette or
 * 32bpp illumination, Shadeless/Ambient, glass, the dark-texel reflectance
 * fade (approximated through roughness), Metallic/Glossiness, Intensity as
 * KHR_materials_specular, and the normal map with NMIntensity baked in. Stored
 * normals are used per corner, as that renderer does (no remap or repair).
 */
typedef struct OptGltfXwauMaterial {
	float metallic;     /* Metallic */
	float intensity;    /* Intensity: dielectric F0 = 0.16 * intensity^2 */
	float glossiness;   /* Glossiness */
	float nm_intensity; /* NMIntensity */
	float specular_val; /* SpecularVal: luma floor of the dark-texel fade */
	float ambient;      /* Ambient: partial shadeless weight */
	bool  shadeless;
	bool  alpha_isnt_glass;
	bool  alpha_to_bloom;
	/* Tangent-space normal map: R, G, B = X, Y, Z encoded 0..1 (+Y toward
	 * the first row), rows in UV order, alpha ignored; rgba NULL when the
	 * texture has none. normal_map_name labels its baked image file and must
	 * be unique per distinct map. */
	OptGltfImageView normal_map;
	const char*      normal_map_name;
} OptGltfXwauMaterial;

/*
 * Animated materials of one OPT texture in XWAU mode (docs:
 * xwau_animated_materials.md, "Model material extras"). The caller resolves
 * them from the XWAU material files:
 *   - `members`: the material's entry in the animation file
 *     (<basename>.anim.yaml: flipbooks, damage, energy), JSON members without
 *     the enclosing braces; aeron_gltf_cook --animations merges them into the
 *     material extras;
 *   - `anim_base`: write aeronAnimBaseColor, an image of the texture's colours
 *     before the illumination split, for its material and each of its skin
 *     variant materials;
 *   - `static_layer` / `static_emissive`: frame 0 of an `always` illumination
 *     flipbook, baked as library images (RGB layer_rgb and A k; RGB
 *     emission), sampled at uv·(uv_scale_x, 1) + uv_offset, clamped when
 *     uv_clamp. The material's base becomes c·k + layer_rgb and its emission
 *     the frame's, with emissive strength 1. rgba NULL for none.
 */
typedef struct OptGltfXwauAnim {
	const char*      members;
	bool             anim_base;
	OptGltfImageView static_layer;
	OptGltfImageView static_emissive;
	float            uv_scale_x;
	float            uv_offset[2];
	bool             uv_clamp;
} OptGltfXwauAnim;

/* Whether XWAU's renderer gives texture `t` an illumination layer: a 32bpp
 * texture with the illumination flag, or an 8bpp one with a lit texel. */
bool OptGltf_XwauTextureHasIllum(const opt_texture_t* t);

/*
 * Screen surfaces: OPT textures that show a game-provided texture pair (a
 * captured HUD) through rectangles, as the XWAU Dynamic Cockpit does. The
 * caller resolves them (dynamic_cockpit.h); the converter rewrites the
 * texture's material and records the rectangles in its `extras` as
 * `aeronScreen` (docs: dynamic_cockpit.md, 2.4):
 *
 *   - with a cover image: base color = the cover premultiplied by its alpha,
 *     base alpha = the cover alpha (the screen shows where it is below 1),
 *     emissive = the cover's lit texels;
 *   - without a cover, or for a hologram: no base color (black) and no
 *     emissive texture, so the surface shows only the screen;
 *   - normal and metallic-roughness textures are dropped (they follow the
 *     OPT texture's layout, not the cover's);
 *   - transparent and hologram surfaces are BLEND, others OPAQUE.
 */
#define OPT_GLTF_SCREEN_MAX_RECTS 12

/* `layers` bits. */
#define OPT_GLTF_SCREEN_LAYER_GRAPHICS 1u
#define OPT_GLTF_SCREEN_LAYER_TEXT 2u
#define OPT_GLTF_SCREEN_LAYER_BAR 4u /* filled from the bottom; no texture */

typedef struct OptGltfScreenRect {
	uint32_t source; /* source index (XWAU table order) */
	float    dst[4]; /* u0, v0, u1, v1 in TEXCOORD_0; swapped corners mirror */
	float    bg[3];  /* linear background colours */
	float    highlight[3];
	float    lock[3];
	float    gain;   /* emissive multiplier of the rectangle's content */
	uint32_t layers; /* OPT_GLTF_SCREEN_LAYER_* */
	float    bar[3]; /* linear fill colour, with OPT_GLTF_SCREEN_LAYER_BAR */
} OptGltfScreenRect;

typedef struct OptGltfScreen {
	bool              present;
	bool              transparent;
	bool              always_visible;
	bool              hologram;
	bool              noisy;
	uint32_t          hide; /* bit per HUD region the 2D overlay drops */
	uint32_t          rect_count;
	OptGltfScreenRect rects[OPT_GLTF_SCREEN_MAX_RECTS];
	/* Cover images, rows in UV order; rgba NULL without a cover. The two
	 * views of one cover share cover_name, which labels the image files and
	 * must be unique per distinct cover. */
	OptGltfImageView cover_base;
	OptGltfImageView cover_emissive;
	const char*      cover_name;
} OptGltfScreen;

/* A point light (KHR_lights_punctual) on the component built from OPT mesh
 * `mesh`: position in OPT units, linear colour, intensity in candela and
 * range in metres (0 = none). */
typedef struct OptGltfLight {
	int32_t mesh;
	float   position[3];
	float   color[3];
	float   intensity;
	float   range;
} OptGltfLight;

/*
 * XWAU skins (XWAU mode only; docs: xwau_skins.md). Each variant becomes a
 * KHR_materials_variants variant after the asset variants (which are then
 * emitted even for a single state). For every state-0 texture of LOD 0 that
 * is not a screen surface, a variant's layers with an image for the texture
 * are blended, in order and with the hook's integer straight-alpha blend,
 * onto its decoded colors; the result is baked as the texture is and gets
 * its own material. Primitives without a composite in a variant show the
 * asset material of the variant's state. Images only composites use carry
 * the extras `aeronVariants` (their variant indices); the asset carries the
 * extras `aeronSkinMarkings` (the default markings map) and
 * `aeronAssetVariants` (the number of asset variants before them).
 */
typedef struct OptGltfSkinLayer {
	uint32_t skin;    /* index into OptGltfSkins.images rows */
	uint32_t opacity; /* 0..100 */
} OptGltfSkinLayer;

typedef struct OptGltfSkinVariant {
	const char*             name;
	uint32_t                state; /* asset state of primitives without a composite */
	const OptGltfSkinLayer* layers;
	size_t                  layer_count;
} OptGltfSkinVariant;

typedef struct OptGltfSkins {
	/* images[skin * opt->texture_count + texture]: the skin's image for that
	 * OPT texture, of the texture's size, rows in OPT order; rgba NULL when
	 * the skin has none. */
	const OptGltfImageView*   images;
	size_t                    skin_count;
	const OptGltfSkinVariant* variants;
	size_t                    variant_count;
	/* Variant name per markings value; NULL for an unskinned one. */
	const char* const* default_markings;
	size_t             markings_count;
} OptGltfSkins;

typedef struct OptGltfBuildOptions {
	float smooth_angle_degrees;
	bool  repair_normals;
	bool  emissive;
	/* When positive, every material is fully self-lit: black base color
	 * (alpha kept), the base texture as emissiveTexture, and this
	 * KHR_materials_emissive_strength. Supersedes `emissive`. */
	float                              fullbright_strength;
	const struct OptGltfAlphaOverride* alpha_overrides;
	size_t                             alpha_override_count;
	const OptGltfComponentTag*         component_tags;
	size_t                             component_tag_count;
	/* Development only: split every triangle into four by its edge midpoints
	 * this many times (at most OPT_GLTF_MAX_SUBDIVIDE). */
	uint32_t subdivide_levels;
	/* XWAU mode when set: one entry per OPT texture (opt->texture_count),
	 * plus the material of untextured faces. Excludes smooth_angle_degrees
	 * >= 0, emissive and fullbright_strength. */
	const OptGltfXwauMaterial* xwau_materials;
	const OptGltfXwauMaterial* xwau_untextured;
	/* Screen surfaces when set: one entry per OPT texture
	 * (opt->texture_count); entries with `present` false are ordinary. */
	const OptGltfScreen* screens;
	/* Point lights, each on an existing OPT mesh. */
	const OptGltfLight* lights;
	size_t              light_count;
	/* Skin variants when set; needs xwau_materials. */
	const OptGltfSkins* skins;
	/* Animated materials when set: one entry per OPT texture; needs
	 * xwau_materials. */
	const OptGltfXwauAnim* xwau_anim;
} OptGltfBuildOptions;

#define OPT_GLTF_MAX_SUBDIVIDE 8u
/* OPT hardpoint type whose Y and Z the game reads as the pilot eye (31). */
#define OPT_GLTF_COCKPIT_HARDPOINT 31

typedef enum OptGltfAlphaMode {
	OPT_GLTF_ALPHA_OPAQUE = 0,
	OPT_GLTF_ALPHA_MASK   = 1,
	OPT_GLTF_ALPHA_BLEND  = 2,
} OptGltfAlphaMode;

typedef struct OptGltfAlphaOverride {
	const char*      texture_name;
	OptGltfAlphaMode alpha_mode;
	float            alpha_cutoff;
} OptGltfAlphaOverride;

/* Classify a legacy OPT base-level alpha plane. OPT stores samples but no
 * material semantic, so coverage is inferred conservatively from endpoint
 * prevalence. */
OptGltfAlphaMode OptGltf_ClassifyAlpha(const uint8_t* alpha, size_t sample_count);

typedef struct OptGltfDocument OptGltfDocument;

bool OptGltf_BuildMemory(const opt_file_t* opt, const char* basename, const OptGltfBuildOptions* options,
						 OptGltfDocument** out_document, opt_error_t* error);

cgltf_data* OptGltf_Data(OptGltfDocument* document);
bool OptGltf_ImageView(const OptGltfDocument* document, const cgltf_image* image, OptGltfImageView* out_view);
void OptGltf_Free(OptGltfDocument* document);

bool OptGltf_WriteFiles(const OptGltfDocument* document, const char* out_dir, const char* basename);

/* repair_normals (stored-normals mode only): reproduce the classic
 * renderer's first-normal-per-position remap within each FaceData group,
 * then substitute the face normal for a zero-length canonical normal or
 * negate a canonical normal opposing its first face plane. Pass false to
 * emit the source per-corner data verbatim for diagnostics. Errors are
 * reported to stderr prefixed with `label`. */
bool opt2gltf_convert(const opt_file_t* opt, const char* out_dir, const char* basename,
					  const OptGltfBuildOptions* options, const char* label);

#endif /* AERON_OPT2GLTF_H */
