/*
 * Scene sky overlay fragment shader (AeronPreparedEnvironment.overlay).
 *
 * Samples the overlay cube (BC7 sRGB, colour premultiplied by alpha) with
 * the ray of scene_sky_cube.vert. The pipeline blends the result as
 * colour + target * (1 - alpha).
 *
 * `params.x` is the environment's intensity, which scales the colour as it
 * scales the sky.
 */

cbuffer SkyOverlayPS : register(b0, space3)
{
    /* x = exposure multiplier; y/z/w reserved. */
    float4 params;
};

TextureCube<float4> g_overlay : register(t0, space2);
SamplerState        g_sampler : register(s0, space2);

float4 main(float4 pos : SV_Position,
            float3 dir : TEXCOORD0) : SV_Target
{
    float4 c = g_overlay.Sample(g_sampler, normalize(dir));
    return float4(c.rgb * params.x, c.a);
}
