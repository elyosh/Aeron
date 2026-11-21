/* The depth of an opaque range at the start of the MSAA colour pass
 * (AeronScene3DDesc.msaa_depth_prepass). The pipeline writes no colour, but
 * Metal needs an output with as many components as the colour target. */

#include "scene_pbr_vsout.hlsli"

float4 main(PbrMsaaDepthVSOut i) : SV_Target0
{
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
}
