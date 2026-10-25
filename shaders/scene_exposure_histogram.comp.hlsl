/* The image meter's histogram (AeronScene_SetAutoExposure). The centre
 * pixel of each 4x4 block of the scene's colour, before the exposure, adds
 * its weight to the bin of its EV100: a sixteenth of the pixels gives the
 * same average, at a sixteenth of the cost. Each group sums its pixels in
 * shared memory, then adds its sums to the frame's bins.
 * scene_exposure_adapt.comp reads and clears them. */

#define EXPOSURE_BINS 64u
#define EXPOSURE_GROUP_SIZE 16u
#define EXPOSURE_GROUP_THREADS (EXPOSURE_GROUP_SIZE * EXPOSURE_GROUP_SIZE)
/* Matches EXPOSURE_PIXEL_STEP in exposure.c. */
#define EXPOSURE_PIXEL_STEP 4u
/* Weights are added as fixed point: 8K at full weight stays below 2^32. */
#define EXPOSURE_WEIGHT_SCALE 256.0f

Texture2D<float4> g_color   : register(t0, space0);
Texture2D<float>  g_depth   : register(t1, space0);
SamplerState      g_color_s : register(s0, space0);
SamplerState      g_depth_s : register(s1, space0);

/* The bins, then the weight of the counted geometry pixels and of the
 * counted sky pixels. */
RWStructuredBuffer<uint> g_bins : register(u0, space1);

cbuffer ExposureHistogramUniforms : register(b0, space2)
{
    uint2 size;
    float edge_weight;
    float sky_weight;
    uint  channel;     /* 0 = luminance, 1 = the largest of r, g and b */
    float near_depth;  /* geometry with a greater depth is near */
    float near_weight;
    uint  _pad;
};

groupshared uint s_bins[EXPOSURE_BINS];
/* Each thread's weight as geometry and as sky, summed by a reduction. */
groupshared uint s_geometry[EXPOSURE_GROUP_THREADS];
groupshared uint s_sky[EXPOSURE_GROUP_THREADS];

[numthreads(EXPOSURE_GROUP_SIZE, EXPOSURE_GROUP_SIZE, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index < EXPOSURE_BINS)
        s_bins[index] = 0u;
    s_geometry[index] = 0u;
    s_sky[index]      = 0u;
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = min(id.xy * EXPOSURE_PIXEL_STEP + EXPOSURE_PIXEL_STEP / 2u, size - 1u);
    if (all(id.xy * EXPOSURE_PIXEL_STEP < size)) {
        float3 rgb = g_color.Load(int3(pixel, 0)).rgb;
        float  v   = channel != 0u ? max(rgb.r, max(rgb.g, rgb.b)) : dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
        /* EV100 of a reflected-light meter, K = 12.5, from scene units
         * (683 cd/m^2 each): log2(683 v * 100 / 12.5). Darker pixels, such
         * as black space, are not counted. */
        float ev = log2(max(v, 1e-30f) * 5464.0f);
        if (v > 0.0f && isfinite(v) && ev >= -8.0f) {
            uint bin = min(uint((ev + 8.0f) * 2.0f), EXPOSURE_BINS - 1u);
            /* 0 at the centre, 1 in the corners. */
            float2 offset = (float2(pixel) + 0.5f) / float2(size) * 2.0f - 1.0f;
            float  edge   = saturate(length(offset) * 0.70710678f);
            /* The cleared reversed-Z depth: no geometry, only the sky. */
            float depth  = g_depth.Load(int3(pixel, 0));
            bool  sky    = depth <= 0.0f;
            float kind   = sky ? sky_weight : (depth > near_depth ? near_weight : 1.0f);
            float weight = lerp(1.0f, edge_weight, edge) * kind;
            uint  fixed  = uint(weight * EXPOSURE_WEIGHT_SCALE + 0.5f);
            InterlockedAdd(s_bins[bin], fixed);
            if (sky)
                s_sky[index] = fixed;
            else
                s_geometry[index] = fixed;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint half_count = EXPOSURE_GROUP_THREADS / 2u; half_count > 0u; half_count >>= 1u) {
        if (index < half_count) {
            s_geometry[index] += s_geometry[index + half_count];
            s_sky[index] += s_sky[index + half_count];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (index < EXPOSURE_BINS && s_bins[index] != 0u)
        InterlockedAdd(g_bins[index], s_bins[index]);
    if (index == 0u) {
        if (s_geometry[0] != 0u)
            InterlockedAdd(g_bins[EXPOSURE_BINS], s_geometry[0]);
        if (s_sky[0] != 0u)
            InterlockedAdd(g_bins[EXPOSURE_BINS + 1u], s_sky[0]);
    }
}
