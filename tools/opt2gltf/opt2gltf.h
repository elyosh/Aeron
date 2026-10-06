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
 *   - Every OPT mesh becomes exactly one component; a mesh without LOD 0
 *     triangles fails the conversion, so component ordinals match the
 *     OPT's mesh numbers.
 *
 *   - Paint-scheme variants: OPT NodeSwitch's per-face-group
 *     state_textures[] become a KHR_materials_variants extension at
 *     asset level. The variant count equals the largest state_count
 *     across all face groups (typically 4 in the IVFILES set). Each
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
