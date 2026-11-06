/* The visible part of the lens flare's source (AeronScene_SetLensFlare): 64
 * depth samples on a spiral over the source's disc on screen, each 1 where
 * nothing is in front of the source. Their average goes to the visibility
 * buffer, which the flare's vertex shader reads. Samples outside the viewport
 * count as hidden. */

#define FLARE_SAMPLES 64u

Texture2D<float> g_depth   : register(t0, space0);
SamplerState     g_depth_s : register(s0, space0);

/* x = the visible part, from 0 to 1. */
RWStructuredBuffer<float4> g_visibility : register(u0, space1);

cbuffer LensFlareVisibility : register(b0, space2)
{
    float4 disc;     /* xy = centre, render pixels; z = radius, render pixels; w = source depth */
    float4 viewport; /* xy = origin, zw = size, render pixels */
};

groupshared float s_open[FLARE_SAMPLES];

/* 1 where nothing is in front of the source at this texel. */
float flare_open(int2 texel)
{
    if (any(texel < (int2)viewport.xy) || any(texel >= (int2)(viewport.xy + viewport.zw)))
        return 0.0f;
    /* Reversed-Z: geometry in front of the source has a greater depth. The
     * margin is 1% of the source's depth, so geometry at any distance
     * hides a far source. */
    return g_depth.Load(int3(texel, 0)) <= disc.w * 1.01f ? 1.0f : 0.0f;
}

[numthreads(FLARE_SAMPLES, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    /* Equal areas: the radius goes as the square root of the index, and the
     * angle turns by the golden angle. */
    const float  r     = sqrt(((float)index + 0.5f) / (float)FLARE_SAMPLES) * disc.z;
    const float  a     = (float)index * 2.39996323f;
    const float2 pixel = disc.xy + r * float2(cos(a), sin(a));

    /* The four texels around the sample, blended by its place between their
     * centres. The upscaler's jitter moves edges by less than a texel, which
     * then changes the result a little instead of turning whole samples on
     * or off. */
    const float2 t    = pixel - 0.5f;
    const int2   base = (int2)floor(t);
    const float2 f    = t - floor(t);
    const float  open = lerp(lerp(flare_open(base), flare_open(base + int2(1, 0)), f.x),
                             lerp(flare_open(base + int2(0, 1)), flare_open(base + int2(1, 1)), f.x), f.y);
    s_open[index] = open;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint step = FLARE_SAMPLES / 2u; step > 0u; step >>= 1u)
    {
        if (index < step)
            s_open[index] += s_open[index + step];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0u)
        g_visibility[0] = float4(s_open[0] / (float)FLARE_SAMPLES, 0.0f, 0.0f, 0.0f);
}
