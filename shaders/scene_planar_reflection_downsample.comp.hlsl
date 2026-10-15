/* One texel of a mip of the floor's reflection (planar_reflection.c), in
 * each half: the mean of the 2 x 2 texels below it. Each half's width is a
 * multiple of the mips' span, so no texel mixes the two halves. The level
 * is written to a staging texture, then copied into the reflection, which a
 * compute pass may not read while writing it. */

cbuffer PlanarReflectionDownsample : register(b0, space2)
{
    int4 region; /* the level's texels to write: x0, y0, x1, y1 (left half) */
    int4 params; /* x: the level's half width; y: the level read, one finer */
};

Texture2D<float4>   g_source         : register(t0, space0);
SamplerState        g_source_sampler : register(s0, space0);
RWTexture2D<float4> g_level          : register(u0, space1);

float4 mean(int2 c)
{
    return 0.25f * (g_source.Load(int3(c, params.y)) + g_source.Load(int3(c + int2(1, 0), params.y)) +
                    g_source.Load(int3(c + int2(0, 1), params.y)) + g_source.Load(int3(c + int2(1, 1), params.y)));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int2 texel = region.xy + (int2)id.xy;
    if (any(texel >= region.zw)) return;
    g_level[uint2(texel)]                       = mean(texel * 2);
    g_level[uint2(texel + int2(params.x, 0))] = mean((texel + int2(params.x, 0)) * 2);
}
