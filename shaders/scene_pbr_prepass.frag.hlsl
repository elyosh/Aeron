/*
 * Depth/normal(/velocity) pre-pass fragment shader for the glTF (PBR)
 * mesh path.
 *
 * The paired prepass VS shares its precise raster-position calculation with
 * the forward VS so the later depth-EQUAL draw receives identical depth.
 *
 * Outputs:
 *   SV_Target0 — octahedral-encoded geometric world normal (R16G16_SNORM),
 *                consumed by the SSAO compute pass. This is the true face
 *                normal from screen-space derivatives of world position —
 *                NOT the interpolated (Gouraud) vertex normal, which on the
 *                legacy meshes tilts up to ~60° off the face and mis-orients
 *                the SSAO hemisphere. Derived here at full res, so it avoids
 *                the grid that per-pixel half-res reconstruction in the SSAO
 *                pass produced.
 *   SV_Target1 — screen-space velocity (R16G16_FLOAT), consumed by the
 *                motion-blur resolve. Only bound when the MB tier is on
 *                (2-RT prepass pipeline); the 1-RT pipeline drops it.
 *
 * Velocity is (ndc_now − ndc_prev) mapped to UV space: the VS supplies
 * both clip positions, the per-pixel divide happens here.
 * Depth write is on (pipeline state); this pass establishes the early-Z.
 */

#include "scene_pbr_prepass_common.hlsli"
#include "scene_pbr_vsout.hlsli"

PbrPrepassFSOut main(PbrPrepassVSOut i)
{
    return pbr_build_prepass_output(i.position, i.world_pos, i.world_normal,
                                    i.clip_curr, i.clip_prev);
}
