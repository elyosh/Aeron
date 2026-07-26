/*
 * Shared per-vertex local-light accumulator for the scene PBR vertex
 * shader. Active records are packed into one frame storage buffer.
 *
 * The accumulator runs in instance-local raw engine units so the
 * engine's `d > range << 7` (== `d > range * 128`) cutoff and the
 * `rg * gain / d2t` contribution land on the unit the engine
 * designed the formula for. Callers must provide:
 *   - rotated_local   : vertex position after per-mesh affine, in
 *                       instance-local raw engine units.
 *   - rotated_normal  : vertex outward normal in the same frame
 *                       (the same value the directional Lambert dot
 *                       consumes — flat path uses face normal, smooth
 *                       path uses the modified-or-engine vertex
 *                       normal per the toggle).
 *   - world_to_local  : 3×3 mapping (world_delta → instance-local raw),
 *                       pre-divided by the instance's total scale so
 *                       the result is already in raw engine units.
 *   - instance_world_pos : instance origin in snapshot world units —
 *                       subtracted from each light's world position
 *                       before applying world_to_local. The mesh VS derives
 *                       both from its model-to-world transform.
 */

#ifndef AERON_SCENE_PBR_LOCAL_LIGHTS_INCLUDED
#define AERON_SCENE_PBR_LOCAL_LIGHTS_INCLUDED

struct SceneLocalLight {
    float3 pos;        /* snapshot world units */
    float  range;      /* engine raw units (16..480 from
                        * tie_makelocallights' anim-frame table) */
    float3 color;      /* linear RGB, can exceed 1.0 for HDR */
    float  falloff_sq; /* engine raw units squared (host pre-squares
                        * the user-facing falloff_radius_engine) */
};

StructuredBuffer<SceneLocalLight> local_lights : register(t1, space0);

/* Per-vertex coloured local-light accumulator. Returns
 * sum_i { contrib_i × lights[i].color } where contrib_i is the
 * engine's saturate(range * gain / d2t) with gain =
 * dot(to-light, normal) + 1 and d2t = d²/falloff_sq + 1. Result is
 * additive on top of base diffuse; callers typically fold it as
 * `albedo * local_rgb`. */
float3 accumulate_local_lights(float3 rotated_local,
                               float3 rotated_normal,
                               float3x3 world_to_local,
                               float3 instance_world_pos)
{
    float3 local_rgb = float3(0.0f, 0.0f, 0.0f);
    for (uint li = 0; li < local_light_count; ++li) {
        SceneLocalLight light = local_lights[local_light_base + li];
        float3 lp_local = mul(world_to_local,
                              light.pos - instance_world_pos);
        float3 d3 = lp_local - rotated_local;
        float  d  = length(d3);
        float  rg = light.range;
        if (d > rg * 128.0f) continue;       /* engine `d > range << 7` */
        float contrib;
        if (d < 1e-3f) {
            contrib = 1.0f;                  /* engine `if (!d) saturate` */
        } else {
            float ndot = dot(d3 / d, rotated_normal);
            float gain = ndot + 1.0f;
            if (gain <= 0.0f) continue;
            float falloff_sq = max(light.falloff_sq, 1.0f);
            float d2t  = (d * d) / falloff_sq + 1.0f;
            contrib    = saturate(rg * gain / d2t);
        }
        local_rgb += contrib * light.color;
    }
    return local_rgb;
}

#endif
