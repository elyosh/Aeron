/* The visible part of the lens flare's source (AeronScene_SetLensFlare): 64
 * depth samples on a spiral over the source's disc on screen, each 1 where
 * nothing is in front of the source. Their average goes to the visibility
 * buffer, which the flare's shaders read. Samples outside the viewport count
 * as hidden.
 *
 * The pass also dims this frame's streaks by how visible the band of the
 * disc along each streak is. A streak comes from that band, so it goes out
 * when something hides the middle of the disc, even if its edges still
 * show. It doesn't move. The flare's shaders also scale the streaks by the
 * visible part, as they do the other shapes. */

#define FLARE_SAMPLES 64u
/* The band's falloff across the streak: 1 / (2 sigma^2), with sigma a third
 * of the disc's radius. */
#define FLARE_STREAK_BAND 4.5f

Texture2D<float> g_depth   : register(t0, space0);
SamplerState     g_depth_s : register(s0, space0);

/* x = the visible part, from 0 to 1. */
RWStructuredBuffer<float4> g_visibility : register(u0, space1);
/* Three float4 per element, as scene_lens_flare.hlsli describes them. The
 * colour is in the third. */
RWStructuredBuffer<float4> g_elements : register(u1, space1);

cbuffer LensFlareVisibility : register(b0, space2)
{
    float4 disc;     /* xy = centre, render pixels; z = radius, render pixels; w = source depth */
    float4 viewport; /* xy = origin, zw = size, render pixels */
    uint4  ends;     /* x = the number of elements; y = the first streak */
};

groupshared float s_open[FLARE_SAMPLES];
/* Each sample's open part, kept for the streaks while s_open is added up,
 * and its offset from the disc's centre, in radii. */
groupshared float  s_sample[FLARE_SAMPLES];
groupshared float2 s_offset[FLARE_SAMPLES];

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
    const float  r      = sqrt(((float)index + 0.5f) / (float)FLARE_SAMPLES);
    const float  a      = (float)index * 2.39996323f;
    const float2 offset = r * float2(cos(a), sin(a));
    const float2 pixel  = disc.xy + offset * disc.z;

    /* The four texels around the sample, blended by its place between their
     * centres. The upscaler's jitter moves edges by less than a texel, which
     * then changes the result a little instead of turning whole samples on
     * or off. */
    const float2 t    = pixel - 0.5f;
    const int2   base = (int2)floor(t);
    const float2 f    = t - floor(t);
    const float  open = lerp(lerp(flare_open(base), flare_open(base + int2(1, 0)), f.x),
                             lerp(flare_open(base + int2(0, 1)), flare_open(base + int2(1, 1)), f.x), f.y);
    s_open[index]   = open;
    s_sample[index] = open;
    s_offset[index] = offset;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint step = FLARE_SAMPLES / 2u; step > 0u; step >>= 1u)
    {
        if (index < step)
            s_open[index] += s_open[index + step];
        GroupMemoryBarrierWithGroupSync();
    }
    const float visible = s_open[0] / (float)FLARE_SAMPLES;
    if (index == 0u)
        g_visibility[0] = float4(visible, 0.0f, 0.0f, 0.0f);
    /* There are fewer elements than threads, so each thread changes at most
     * one. */
    if (index >= ends.y && index < ends.x)
    {
        /* Across the streak, from its rotation. */
        const float4 place  = g_elements[index * 3u + 1u];
        const float2 across = float2(-place.z, place.y);
        float        open_sum   = 0.0f;
        float        weight_sum = 0.0f;
        [loop] for (uint j = 0u; j < FLARE_SAMPLES; ++j)
        {
            const float v = dot(s_offset[j], across);
            const float w = exp(-FLARE_STREAK_BAND * v * v);
            open_sum += w * s_sample[j];
            weight_sum += w;
        }
        g_elements[index * 3u + 2u] *= open_sum / weight_sum;
    }
}
