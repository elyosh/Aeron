#ifndef TIE_OPT2GLTF_H
#define TIE_OPT2GLTF_H

/*
 * opt2gltf — convert one OPT file to glTF 2.0 (one .gltf JSON + one
 * .bin buffer + one .png per OPT texture, written to `out_dir`).
 *
 * Output is faithful to every OPT datum the engine reads:
 *
 *   - Geometry: one glTF node per OPT mesh, parented to a single root
 *     scene node. Each mesh node carries the mesh's MeshDescriptor.
 *     meshType in `extras.tieMeshType` (and the symbolic name in
 *     `tieMeshTypeName`). Coordinates are emitted in glTF convention
 *     (+Y up, -Z forward) — derived from the OPT convention (+Z up,
 *     -Y forward) by swapping Y/Z. The renderer applies the inverse
 *     when consuming the glTF, the same way it applies the swap when
 *     consuming OPT vertices directly.
 *
 *   - Rotation pivots: rotating-component meshes (OPT mesh types
 *     OPT_MT_ROTARY_*) carry `extras.tieRotation = {axis, pivot,
 *     animated:true}`. Static meshes omit `tieRotation`.
 *
 *   - Hardpoints: each OPT mesh's hardpoints become child empty
 *     nodes (no mesh, just translation) named "hp_<type>". Each
 *     carries `extras.tieHardpoint = {type, typeName, link,
 *     component}`.
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
 * `vertex_scale` multiplies every emitted vertex position (plus
 * hardpoint translations and rotation pivots; NOT normals or axes,
 * which are directions). Use 1.0 for the normal case. It exists to
 * reconcile OPTs authored at the full 1995 LFD coordinate scale: the
 * runtime scales OPT coords by 1/65536 while the classic path uses
 * FLIGHT_VERT_SCALE = 1/131072, so a correctly-authored OPT is at half
 * the LFD scale. A handful of ships (e.g. FRTC, TUG) ship at the full
 * LFD scale and render 2x too large — pass 0.5 for those.
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

#include "cgltf.h"
#include "opt.h"

typedef struct OptGltfBuildOptions {
    float vertex_scale;
    float smooth_angle_degrees;
    bool repair_normals;
    bool emissive;
} OptGltfBuildOptions;

typedef struct OptGltfImageView {
    const uint8_t *rgba;
    uint32_t width;
    uint32_t height;
} OptGltfImageView;

typedef struct OptGltfDocument OptGltfDocument;

bool OptGltf_BuildMemory(const opt_file_t *opt,
                         const char *basename,
                         const OptGltfBuildOptions *options,
                         OptGltfDocument **out_document,
                         opt_error_t *error);

cgltf_data *OptGltf_Data(OptGltfDocument *document);
bool OptGltf_ImageView(const OptGltfDocument *document,
                       const cgltf_image *image,
                       OptGltfImageView *out_view);
void OptGltf_Free(OptGltfDocument *document);

bool OptGltf_WriteFiles(const OptGltfDocument *document,
                        const char *out_dir,
                        const char *basename);

/* repair_normals (stored-normals mode only): reproduce the classic
 * renderer's first-normal-per-position remap within each FaceData group,
 * then substitute the face normal for a zero-length canonical normal or
 * negate a canonical normal opposing its first face plane. Pass false to
 * emit the source per-corner data verbatim for diagnostics. */
bool opt2gltf_convert(const opt_file_t *opt,
                      const char *out_dir,
                      const char *basename,
                      float vertex_scale,
                      float smooth_angle_deg,
                      bool repair_normals,
                      bool emissive);

#endif /* TIE_OPT2GLTF_H */
