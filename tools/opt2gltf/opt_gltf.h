#ifndef AERON_OPT2GLTF_OPT_GLTF_H
#define AERON_OPT2GLTF_OPT_GLTF_H

/*
 * OPT models as glTF 2.0 documents in memory (written as a .gltf, its .bin
 * and a PNG per image by opt_gltf_write.h).
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
 *     set), followed by any given variants (OptGltfVariants). Each
 *     multi-state face group emits its primitive with mappings from
 *     variant index to material; single-state face groups use a
 *     single default material.
 *
 *   - Textures: one glTF image per OPT texture, written as
 *     <out_dir>/textures/<opt_basename>_TexNN.png. Materials reference
 *     them via baseColorTexture. The shaded-15 base mip is what gets
 *     written (the OPT format stores per-shade mip chains; we keep
 *     only the base — generators can re-derive mips on import).
 *     Only LOD 0 is converted: the materials no LOD 0 face draws (in any
 *     variant), and the textures and images only they use, are left out.
 *
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

/* An AERON_flight_model articulation on the model root, for component
 * `component` (0..254, an OPT mesh index; it may be past this OPT's meshes
 * when the game draws another model, such as a cockpit, with the same
 * per-component state). `group` and `behavior` (empty for none) match
 * [a-z0-9._]{1,63}; `angle` is -128..255, `speed` and `speed_back` 0..255. */
typedef struct OptGltfComponentArticulation {
	uint32_t component;
	char     group[64];
	char     behavior[64];
	int32_t  angle;
	int32_t  speed;
	int32_t  speed_back;
} OptGltfComponentArticulation;

/* A model-root AERON_flight_model component set: `name` matches
 * [a-z0-9._]{1,63} and is unique; bit n % 32 of members[n / 32] is
 * component n (0..254, which may be past this OPT's meshes). */
typedef struct OptGltfComponentSet {
	char     name[64];
	uint32_t members[8];
} OptGltfComponentSet;

/* A model-root AERON_flight_model articulation setting; `name` matches
 * [a-z0-9._]{1,63} and is unique. */
typedef struct OptGltfArticulationSetting {
	char    name[64];
	int32_t value;
} OptGltfArticulationSetting;

typedef struct OptGltfImageView {
	const uint8_t* rgba;
	uint32_t       width;
	uint32_t       height;
} OptGltfImageView;

/*
 * Materials given by the caller instead of the OPT textures' stock ones
 * (OptGltfBuildOptions.materials), in glTF's PBR inputs.
 *
 * An image is RGBA pixels, rows in OPT order, with its glTF name and its
 * file name under textures/. The document refers to the pixels, which must
 * outlive it; an image given more than once (the same pixels) is written
 * once.
 */
typedef struct OptGltfImage {
	OptGltfImageView view; /* rgba NULL: none */
	const char*      name;
	const char*      file;
} OptGltfImage;

typedef struct OptGltfMaterial {
	const char* name; /* NULL: the stock name */
	/* The base color image, of the texture's size. A texture's base image
	 * takes the stock name and file; without one (rgba NULL), the texture
	 * has no pixels and its material is the stock one (the rest is
	 * ignored). */
	OptGltfImage base;
	/* The alpha mode is classified from the base image's alpha, else the
	 * material is opaque; alpha overrides apply either way. */
	bool         alpha_from_base;
	float        base_color[3];
	OptGltfImage emissive;
	float        emissive_factor[3];
	float        emissive_strength; /* KHR_materials_emissive_strength when > 0 */
	OptGltfImage metallic_roughness;
	float        metallic;
	float        roughness;
	OptGltfImage transmission; /* KHR_materials_transmission, factor 1, when set */
	/* A tangent-space normal map; the meshes drawing the material get
	 * tangents. */
	OptGltfImage normal;
	bool         specular; /* KHR_materials_specular: factor 1, color factor specular_color */
	float        specular_color;
	const char*  json; /* its AERON_flight_model object (animations), or NULL */
} OptGltfMaterial;

/*
 * Material variants beyond the switch states (KHR_materials_variants): each
 * comes after the state variants (`State 1` ... `State N-1`; state 0 is the
 * primitives' own materials), shows, for the face groups of a texture, a
 * material of its own when it has one, else the material of its state. The
 * model root's markings list gives, per markings value, the variant
 * `default_markings` names, else the state the OPT clamps it to.
 */
typedef struct OptGltfVariant {
	const char* name;
	uint32_t    state; /* the state shown where the variant has no material */
} OptGltfVariant;

/* A variant's material for the face groups of texture `texture`: it takes
 * that texture's material's alpha mode, and its name followed by `suffix`.
 * Its base image is set. */
typedef struct OptGltfVariantMaterial {
	int32_t         texture;
	const char*     suffix;
	OptGltfMaterial material;
} OptGltfVariantMaterial;

typedef struct OptGltfVariants {
	const OptGltfVariant*         variants;
	size_t                        count;
	const OptGltfVariantMaterial* materials;
	size_t                        material_count;
	/* [variant * opt->texture_count + texture]: the variant's material for
	 * the texture (an index into `materials`), or -1. */
	const int32_t* material_of;
	/* Variant name per markings value; NULL for one showing a state. */
	const char* const* default_markings;
	size_t             markings_count;
} OptGltfVariants;

/*
 * Screen surfaces: OPT textures that show a game-provided texture pair (a
 * captured HUD) through rectangles, as the XWAU Dynamic Cockpit does. The
 * caller resolves them (xwau_dynamic_cockpit.h); the converter rewrites the
 * texture's material and records the rectangles in its AERON_flight_model
 * object as `screen`:
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
	float            cover_emissive_strength; /* KHR_materials_emissive_strength of the cover's lit texels */
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

/* How vertex normals are made. */
typedef enum OptGltfNormals {
	/* The stored normals as the classic renderer draws them (the default).
	 * The 1998 normals are full-Gouraud (every adjacent face averaged, no
	 * angle threshold), which fakes roundness on the low-poly hulls. The
	 * renderer's per-FaceData position remap is reproduced: the first
	 * face-order normal for each position is reused by later corners, which
	 * keeps the rounded look without the conflicting corner normals it
	 * ignored; then a zero-length canonical normal is replaced by the face
	 * normal, and one opposing its first face plane negated. */
	OPT_GLTF_NORMALS_CLASSIC = 0,
	/* Each corner's stored normal as is (XWAU's renderer). */
	OPT_GLTF_NORMALS_STORED,
	/* Regenerated, the stored normals ignored: geometric face normals form
	 * connected smoothing fans across shared edges that meet
	 * smooth_angle_degrees, then are corner-angle weighted so that the
	 * triangulation's density does not bias them. A fan that wraps around a
	 * non-manifold vertex is split where its average would point behind a
	 * rendered triangle. Edges beyond the angle split the vertex, e.g. 45
	 * for a crisp hard-surface look, or 90 for the rounded appearance of
	 * the low-poly original models. */
	OPT_GLTF_NORMALS_SMOOTHED,
} OptGltfNormals;

typedef struct OptGltfGeometryOptions {
	OptGltfNormals normals;
	float          smooth_angle_degrees; /* OPT_GLTF_NORMALS_SMOOTHED: 0 to 180 */
} OptGltfGeometryOptions;

/* Where the materials come from. */
typedef enum OptGltfMaterialSource {
	/* The textures' colors (the default). With `emissive`, the textures
	 * whose palette marks self-lit texels (the flat-ramp trick of TIE98 and
	 * XWA: lit windows, cockpit lights) get a second <basename>_TexNN_
	 * emissive.png (their glow colour on black) as the material's
	 * emissiveTexture with a unit emissiveFactor, and their base colour is
	 * black there: the classic renderer drew the glow over the surface in
	 * their place. */
	OPT_GLTF_MATERIALS_STOCK = 0,
	/* Every material fully self-lit, as the classic renderer draws
	 * projectiles: black base color (alpha kept), the base texture as
	 * emissiveTexture, and fullbright_strength (> 0) as
	 * KHR_materials_emissive_strength. */
	OPT_GLTF_MATERIALS_FULLBRIGHT,
	/* The caller's: `given`, one per OPT texture (opt->texture_count), the
	 * material of untextured faces (its factors), and `variants` when set. */
	OPT_GLTF_MATERIALS_GIVEN,
} OptGltfMaterialSource;

typedef struct OptGltfMaterialOptions {
	OptGltfMaterialSource  source;
	bool                   emissive;            /* OPT_GLTF_MATERIALS_STOCK */
	float                  fullbright_strength; /* OPT_GLTF_MATERIALS_FULLBRIGHT */
	const OptGltfMaterial* given;               /* OPT_GLTF_MATERIALS_GIVEN */
	const OptGltfMaterial* untextured;
	const OptGltfVariants* variants;
	/* Screen surfaces when set: one entry per OPT texture
	 * (opt->texture_count); entries with `present` false are ordinary. */
	const OptGltfScreen*               screens;
	const struct OptGltfAlphaOverride* alpha_overrides;
	size_t                             alpha_override_count;
} OptGltfMaterialOptions;

/* The model's AERON_flight_model data and lights. */
typedef struct OptGltfModelOptions {
	const OptGltfComponentTag*          tags;
	size_t                              tag_count;
	const OptGltfComponentArticulation* articulations;
	size_t                              articulation_count;
	const OptGltfArticulationSetting*   settings;
	size_t                              setting_count;
	const OptGltfComponentSet*          sets;
	size_t                              set_count;
	const OptGltfLight*                 lights; /* each on an existing OPT mesh */
	size_t                              light_count;
} OptGltfModelOptions;

/* The conversion's options; zero-initialized, the defaults. */
typedef struct OptGltfBuildOptions {
	OptGltfGeometryOptions geometry;
	OptGltfMaterialOptions material;
	OptGltfModelOptions    model;
	/* The conversion's notes (a texture's alpha class) and warnings (an
	 * unused alpha override) when set; without it, warnings go to stderr. */
	void (*log)(bool warning, const char* message);
} OptGltfBuildOptions;

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

/* The base mip of an OPT texture as RGBA8, at palette shade row `shade`
 * (the file's base-color row, opt_palette_base_shade); NULL without
 * pixels, or when out of memory. */
uint8_t* OptGltf_DecodeTexture(const opt_texture_t* t, int shade);

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

#endif /* AERON_OPT2GLTF_OPT_GLTF_H */
