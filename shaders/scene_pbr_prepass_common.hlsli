/* Shared depth/normal/velocity prepass fragment calculations. */

#ifndef SCENE_PBR_PREPASS_COMMON_HLSLI
#define SCENE_PBR_PREPASS_COMMON_HLSLI

#include "octahedral_normal.hlsli"

struct PbrPrepassFSOut
{
    float2 normal   : SV_Target0;
    float2 velocity : SV_Target1;
    float  depth    : SV_Target2;
};

float2 pbr_prepass_velocity(float4 clip_curr, float4 clip_prev)
{
    float2 ndc_now = clip_curr.xy / clip_curr.w;
    float2 ndc_prev = clip_prev.xy / clip_prev.w;
    return (ndc_now - ndc_prev) * float2(0.5f, -0.5f);
}

PbrPrepassFSOut pbr_build_prepass_output(float4 position, float3 world_pos,
                                         float3 world_normal,
                                         float4 clip_curr, float4 clip_prev)
{
    PbrPrepassFSOut output;

    /* Full-resolution derivatives recover the geometric face normal used by
     * SSAO, oriented to the interpolated normal's hemisphere. */
    float3 geometric_normal = normalize(cross(ddx(world_pos), ddy(world_pos)));
    if (dot(geometric_normal, normalize(world_normal)) < 0.0f)
        geometric_normal = -geometric_normal;
    output.normal = oct_encode(geometric_normal);

    output.velocity = pbr_prepass_velocity(clip_curr, clip_prev);
    output.depth = position.z;
    return output;
}

#endif /* SCENE_PBR_PREPASS_COMMON_HLSLI */
