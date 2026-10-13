#ifndef AERON_OPT2GLTF_OPT_GLTF_H
#define AERON_OPT2GLTF_OPT_GLTF_H

/*
 * Converts OPT models to glTF 2.0 documents in memory. opt_gltf_write.h
 * writes a document as a .gltf file, its .bin file and one PNG per image.
 *
 * The output keeps every OPT value that the engine reads:
 *
 *   - Geometry: each OPT mesh becomes one glTF node, and all of them are
 *     children of a single root node in the scene. Each mesh node carries the
 *     full MeshDescriptor in the `AERON_flight_model` node extension.
 *     Coordinates use standard glTF axes and meters.
 *
 *   - Rotation pivots and axes are kept in the component role.
 *
 *   - Hardpoints: the hardpoints of each OPT mesh become empty child nodes
 *     (a translation and no mesh) named "hardpoint_<type>". Each one carries
 *     an `AERON_flight_model` hardpoint role and the raw OPT type.
 *
 *   - Each OPT mesh becomes exactly one component, so the component numbers
 *     match the OPT mesh numbers. A mesh with vertices but no LOD 0 faces
 *     becomes one untextured degenerate triangle over its vertices. The
 *     flight-model topology keeps its positions and drops the face. A mesh
 *     with no vertices and no LOD 0 triangles makes the conversion fail.
 *
 *   - When the state textures of a switched face group have different alpha
 *     classes (opaque, mask, blend), every state uses a copy of its material
 *     promoted to the most transparent of these classes. The renderer needs
 *     each primitive to keep one alpha class in all variants.
 *
 *   - Paint-scheme variants: the state_textures[] of each face group (OPT
 *     NodeSwitch) become a KHR_materials_variants extension at asset level.
 *     There is one variant for each state after state 0, up to the largest
 *     LOD 0 state_count of any face group (typically 4 in the IVFILES set).
 *     The given variants (OptGltfVariants) come after them. The primitive of
 *     a face group with several states maps variant indices to materials.
 *     Other face groups use a single default material, unless a given
 *     variant has a material for their texture.
 *
 *   - Textures: each OPT texture becomes one glTF image, written as
 *     <out_dir>/textures/<opt_basename>_TexNN.png. Materials use them as
 *     baseColorTexture. Only the base mip is written, at the base-color row
 *     of the palette (opt_palette_base_shade). The OPT format stores a mip
 *     chain for each shade, but tools can generate mips again on import.
 *     Only LOD 0 is converted. Materials that no LOD 0 face draws in any
 *     variant are left out, with the textures and images that only they use.
 *
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cgltf.h"
#include "opt.h"

/* Adds `tag` to the component built from OPT mesh `mesh_index`. Tags match
 * [a-z0-9._]{1,63}. A tag repeated on one component is written once. */
typedef struct OptGltfComponentTag {
	uint32_t    mesh_index;
	const char* tag;
} OptGltfComponentTag;

/* An AERON_flight_model articulation on the model root, for component
 * `component`. The component is an OPT mesh index, 0..254. It may be past
 * the meshes of this OPT when the game draws another model with the same
 * per-component state, such as a cockpit. `group` and `behavior` match
 * [a-z0-9._]{1,63}, and `behavior` may be empty. `angle` is -128..255,
 * `speed` and `speed_back` are 0..255. */
typedef struct OptGltfComponentArticulation {
	uint32_t component;
	char     group[64];
	char     behavior[64];
	int32_t  angle;
	int32_t  speed;
	int32_t  speed_back;
} OptGltfComponentArticulation;

/* An AERON_flight_model component set on the model root. `name` matches
 * [a-z0-9._]{1,63} and is unique. Bit n % 32 of members[n / 32] stands for
 * component n (0..254), which may be past the meshes of this OPT. */
typedef struct OptGltfComponentSet {
	char     name[64];
	uint32_t members[8];
} OptGltfComponentSet;

/* An AERON_flight_model articulation setting on the model root. `name`
 * matches [a-z0-9._]{1,63} and is unique. */
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
 * Materials that the caller gives in place of the stock materials of the
 * OPT textures (OptGltfMaterialOptions.given), as glTF PBR inputs.
 *
 * An image is RGBA pixels with rows in OPT order, a glTF name, and a file
 * name under textures/. The document points to the pixels, so they must
 * outlive it. An image given more than once (the same pixel pointer) is
 * written once.
 */
typedef struct OptGltfImage {
	OptGltfImageView view; /* rgba is NULL when there is no image */
	const char*      name;
	const char*      file;
} OptGltfImage;

typedef struct OptGltfMaterial {
	const char* name; /* NULL for the stock name */
	/* The base color image, with the size of the texture. It uses the stock
	 * name and file name. When its rgba is NULL, the texture keeps its stock
	 * material and the other fields are ignored. */
	OptGltfImage base;
	/* When set, the alpha mode is classified from the alpha of the base
	 * image. Otherwise the material is opaque. Alpha overrides apply in both
	 * cases. */
	bool         alpha_from_base;
	float        base_color[3];
	OptGltfImage emissive;
	float        emissive_factor[3];
	float        emissive_strength; /* KHR_materials_emissive_strength when > 0 */
	OptGltfImage metallic_roughness;
	float        metallic;
	float        roughness;
	OptGltfImage transmission; /* KHR_materials_transmission with factor 1, when set */
	/* A tangent-space normal map. Meshes that draw the material get
	 * tangents. */
	OptGltfImage normal;
	bool         specular; /* KHR_materials_specular with factor 1 and color factor specular_color */
	float        specular_color;
	const char*  json; /* the material's AERON_flight_model object (animations), or NULL */
} OptGltfMaterial;

/*
 * Material variants added after the switch-state variants
 * (KHR_materials_variants). The state variants are `State 1` ... `State N-1`,
 * and state 0 uses the primitives' own materials. On the face groups of a
 * texture, a given variant shows its own material for that texture if it
 * has one. Otherwise it shows the material of its `state`. For each markings
 * value, the markings list on the model root names the variant from
 * `default_markings`. If there is none, it names the state that the OPT
 * clamps the value to.
 */
typedef struct OptGltfVariant {
	const char* name;
	uint32_t    state; /* the state shown where the variant has no material */
} OptGltfVariant;

/* The material that a variant shows on the face groups of texture
 * `texture`. It uses the alpha mode of that texture's material, and its name
 * is the name of that material followed by `suffix`. Its base image must be
 * set. */
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
	/* Indexed by [variant * opt->texture_count + texture]. Each entry is the
	 * index into `materials` of the variant's material for the texture, or
	 * -1. */
	const int32_t* material_of;
	/* The variant name for each markings value, or NULL when the value shows
	 * a state. */
	const char* const* default_markings;
	size_t             markings_count;
} OptGltfVariants;

/*
 * Screen surfaces are OPT textures that show a texture pair from the game
 * (a captured HUD) through rectangles, like the XWAU Dynamic Cockpit. The
 * caller resolves them (xwau_dynamic_cockpit.h). The converter rewrites the
 * material of the texture and writes the rectangles as `screen` in its
 * AERON_flight_model object:
 *
 *   - With a cover image, the base color is the cover premultiplied by its
 *     alpha, and the base alpha is the cover alpha. The screen shows where
 *     this alpha is below 1. The emissive texture holds the lit texels of
 *     the cover.
 *   - Without a cover, or for a hologram, there is no base color (black) and
 *     no emissive texture, so the surface shows only the screen.
 *   - Normal and metallic-roughness textures are dropped, because they
 *     follow the layout of the OPT texture and not the layout of the cover.
 *   - Transparent and hologram surfaces are BLEND, the others are OPAQUE.
 */
#define OPT_GLTF_SCREEN_MAX_RECTS 12

/* `layers` bits. */
#define OPT_GLTF_SCREEN_LAYER_GRAPHICS 1u
#define OPT_GLTF_SCREEN_LAYER_TEXT 2u
#define OPT_GLTF_SCREEN_LAYER_BAR 4u /* filled from the bottom, without a texture */

typedef struct OptGltfScreenRect {
	uint32_t source; /* source index (XWAU table order) */
	float    dst[4]; /* u0, v0, u1, v1 in TEXCOORD_0. Swapped corners mirror the content. */
	float    bg[3];  /* linear background colours */
	float    highlight[3];
	float    lock[3];
	float    gain;   /* emissive multiplier of the rectangle's content */
	uint32_t layers; /* OPT_GLTF_SCREEN_LAYER_* */
	float    bar[3]; /* linear fill colour for OPT_GLTF_SCREEN_LAYER_BAR */
} OptGltfScreenRect;

typedef struct OptGltfScreen {
	bool              present;
	bool              transparent;
	bool              always_visible;
	bool              hologram;
	bool              noisy;
	uint32_t          hide; /* one bit per HUD region that the 2D overlay drops */
	uint32_t          rect_count;
	OptGltfScreenRect rects[OPT_GLTF_SCREEN_MAX_RECTS];
	/* Cover images, with rows in UV order. rgba is NULL when there is no
	 * cover. Both views of a cover use cover_name, which names the image
	 * files. Each distinct cover needs its own cover_name. */
	OptGltfImageView cover_base;
	OptGltfImageView cover_emissive;
	const char*      cover_name;
	float            cover_emissive_strength; /* KHR_materials_emissive_strength of the lit cover texels */
} OptGltfScreen;

/* A point light (KHR_lights_punctual) on the component built from OPT mesh
 * `mesh`. The position is in OPT units, the colour is linear, the intensity
 * is in candela and the range is in metres (0 means no range). */
typedef struct OptGltfLight {
	int32_t mesh;
	float   position[3];
	float   color[3];
	float   intensity;
	float   range;
} OptGltfLight;

/* How vertex normals are made. */
typedef enum OptGltfNormals {
	/* The stored normals, as the classic renderer draws them (the default).
	 * The 1998 normals are full Gouraud: they average every adjacent face,
	 * with no angle threshold, which makes the low-poly hulls look round.
	 * The position remap that the renderer does for each FaceData is
	 * reproduced. For each position, the first normal in face order is
	 * reused by the later corners. This keeps the rounded look and drops the
	 * conflicting corner normals that the renderer ignored. Then a
	 * zero-length canonical normal is replaced by the face normal, and a
	 * canonical normal that points against the plane of its first face is
	 * negated. */
	OPT_GLTF_NORMALS_CLASSIC = 0,
	/* The stored normal of each corner, unchanged, as XWAU's renderer uses
	 * it. */
	OPT_GLTF_NORMALS_STORED,
	/* New normals that ignore the stored ones. Face normals are grouped into
	 * connected smoothing fans across shared edges whose angle is within
	 * smooth_angle_degrees. Each face normal is weighted by its corner
	 * angle, so that the density of the triangulation does not bias the
	 * result. A fan that wraps around a non-manifold vertex is split where
	 * its average would point behind a rendered triangle. Edges with a
	 * larger angle split the vertex. Use 45 for a crisp hard-surface look,
	 * or 90 for the rounded look of the low-poly original models. */
	OPT_GLTF_NORMALS_SMOOTHED,
} OptGltfNormals;

typedef struct OptGltfGeometryOptions {
	OptGltfNormals normals;
	float          smooth_angle_degrees; /* OPT_GLTF_NORMALS_SMOOTHED: 0 to 180 */
} OptGltfGeometryOptions;

/* Where the materials come from. */
typedef enum OptGltfMaterialSource {
	/* The colors of the textures (the default). With `emissive`, a texture
	 * whose palette marks self-lit texels (lit windows and cockpit lights,
	 * drawn with the flat-ramp trick of TIE98 and XWA) gets a second image,
	 * <basename>_TexNN_emissive.png. It holds the glow colour on black and
	 * is the emissiveTexture of the material, with an emissiveFactor of 1.
	 * The base colour of those texels is black, because the classic renderer
	 * drew the glow instead of the surface there. */
	OPT_GLTF_MATERIALS_STOCK = 0,
	/* Every material is fully self-lit, the way the classic renderer draws
	 * projectiles. The base color is black (alpha is kept), the base texture
	 * becomes the emissiveTexture, and fullbright_strength (> 0) becomes
	 * KHR_materials_emissive_strength. */
	OPT_GLTF_MATERIALS_FULLBRIGHT,
	/* Materials from the caller. `given` holds one material per OPT texture
	 * (opt->texture_count). `untextured` is the material of untextured faces,
	 * and only its factors are used. `variants` is optional. */
	OPT_GLTF_MATERIALS_GIVEN,
} OptGltfMaterialSource;

typedef struct OptGltfMaterialOptions {
	OptGltfMaterialSource  source;
	bool                   emissive;            /* OPT_GLTF_MATERIALS_STOCK */
	float                  fullbright_strength; /* OPT_GLTF_MATERIALS_FULLBRIGHT */
	const OptGltfMaterial* given;               /* OPT_GLTF_MATERIALS_GIVEN */
	const OptGltfMaterial* untextured;
	const OptGltfVariants* variants;
	/* Optional screen surfaces, with one entry per OPT texture
	 * (opt->texture_count). Entries with `present` false are ordinary. */
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

/* Conversion options. Zero-initialized options give the defaults. */
typedef struct OptGltfBuildOptions {
	OptGltfGeometryOptions geometry;
	OptGltfMaterialOptions material;
	OptGltfModelOptions    model;
	/* Receives the notes of the conversion (such as the alpha class of a
	 * texture) and its warnings (such as an unused alpha override or
	 * repaired normals), with `log_context`. The messages do not name the
	 * model. When `log` is NULL, nothing is reported. Errors go to the
	 * `error` argument of OptGltf_BuildMemory. */
	void (*log)(void* context, bool warning, const char* message);
	void* log_context;
} OptGltfBuildOptions;

/* The OPT hardpoint type (31) whose Y and Z the game reads as the position
 * of the pilot's eye. */
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

/* Decodes the base mip of an OPT texture as RGBA8, with palette shade row
 * `shade`. opt_palette_base_shade gives the base-color row of the file.
 * Returns NULL when the texture has no pixels or when out of memory. */
uint8_t* OptGltf_DecodeTexture(const opt_texture_t* t, int shade);

/* Classifies the alpha plane of the base level of an OPT texture. OPT
 * stores alpha samples but not how a material should use them, so the mode
 * is guessed conservatively from how many samples are fully transparent or
 * fully opaque. */
OptGltfAlphaMode OptGltf_ClassifyAlpha(const uint8_t* alpha, size_t sample_count);

typedef struct OptGltfDocument OptGltfDocument;

bool OptGltf_BuildMemory(const opt_file_t* opt, const char* basename, const OptGltfBuildOptions* options,
						 OptGltfDocument** out_document, opt_error_t* error);

cgltf_data* OptGltf_Data(OptGltfDocument* document);
bool OptGltf_ImageView(const OptGltfDocument* document, const cgltf_image* image, OptGltfImageView* out_view);
void OptGltf_Free(OptGltfDocument* document);

#endif /* AERON_OPT2GLTF_OPT_GLTF_H */
