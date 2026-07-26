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

#include "octahedral_normal.hlsli"
#include "scene_pbr_vsout.hlsli"


struct FSOut
{
    float2 normal   : SV_Target0;
    float2 velocity : SV_Target1;
    float  depth    : SV_Target2;
};

FSOut main(PbrPrepassVSOut i)
{
    FSOut o;

    /* Geometric face normal from world-position derivatives, oriented to
     * the same hemisphere as the interpolated normal. ddx/ddy are constant
     * per 2×2 quad, but at full res that's far finer than the half-res
     * reconstruction the SSAO pass used, and the bilateral blur cleans the
     * residue. */
    float3 ng = normalize(cross(ddx(i.world_pos), ddy(i.world_pos)));
    if (dot(ng, normalize(i.world_normal)) < 0.0f) ng = -ng;
    o.normal = oct_encode(ng);

    /* NDC → UV-space motion vector. The Y component is flipped because
     * UV grows downward while NDC Y grows upward. */
    float2 ndc_now  = i.clip_curr.xy / i.clip_curr.w;
    float2 ndc_prev = i.clip_prev.xy / i.clip_prev.w;
    o.velocity = (ndc_now - ndc_prev) * float2(0.5f, -0.5f);
    o.depth = i.position.z;
    return o;
}
