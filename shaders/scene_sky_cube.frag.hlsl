/*
 * Scene sky-cube fragment shader (AeronScene_SetSkyCube).
 *
 * Samples the sky cube map (LDR BC7 sRGB or HDR BC6H linear). Alpha =
 * 1; the sky writes opaque colour to the scene RT under the depth-far
 * z=0 reversed-Z plane.
 *
 * `params.x` is a per-sky exposure multiplier applied to the cube
 * sample. Games push 1.0 for LDR cubes (no-op) and a calibrated value
 * for HDR cubes whose source luminance lives well below the
 * displayable range (deep-space HDRIs are typically authored against
 * physical units and need scaling up to be visible).
 */

cbuffer SkyCubePS : register(b0, space3)
{
    /* x = exposure multiplier; y/z/w reserved. */
    float4 params;
};

TextureCube<float4> g_sky     : register(t0, space2);
SamplerState        g_sampler : register(s0, space2);

float4 main(float4 pos : SV_Position,
            float3 dir : TEXCOORD0) : SV_Target
{
    float3 d = normalize(dir);
    float4 c = g_sky.Sample(g_sampler, d);
    return float4(c.rgb * params.x, 1.0f);
}
