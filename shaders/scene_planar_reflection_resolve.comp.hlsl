/* The floor's reflection (planar_reflection.c) and its mips, in one pass:
 * in the left half, the mirrored view's colour premultiplied by its
 * coverage and the coverage; in the right half, the height of the
 * reflected point above the plane in metres times the coverage, and the
 * coverage, so that the mips keep the mean height of what they cover. A
 * group resolves 8 x 8 texels of each half, then reduces them to the
 * 4 x 4, 2 x 2 and 1 x 1 texels of the next three levels, each the mean of
 * the 2 x 2 below as stored (16-bit). Only the region the floor reads is
 * written; it is a multiple of 16 texels. */

cbuffer PlanarReflectionResolve : register(b0, space2)
{
    float4 z_row;    /* clip to the model z of the mirrored point, before the divide */
    float4 w_row;    /* clip to its w */
    float4 viewport; /* the mirrored view's viewport, texels */
    float4 plane;    /* x: the plane's model z */
    int4   dims;     /* each half's width and the height (padded), the drawn width and height */
    int4   region;   /* the texels to write: x0, y0, x1, y1 (left half) */
};

Texture2D<float4>   g_color         : register(t0, space0);
SamplerState        g_color_sampler : register(s0, space0);
Texture2D<float>    g_depth         : register(t1, space0);
SamplerState        g_depth_sampler : register(s1, space0);
RWTexture2D<float4> g_level0        : register(u0, space1);
RWTexture2D<float4> g_level1        : register(u1, space1);
RWTexture2D<float4> g_level2        : register(u2, space1);
RWTexture2D<float4> g_level3        : register(u3, space1);

/* Each half's texels of the level being reduced, as stored. */
groupshared float4 g_colors[64];
groupshared float4 g_heights[64];

float4 stored(float4 v)
{
    return f16tof32(f32tof16(v));
}

/* The texel at (x, y) of a `size` x `size` level held in the group: the
 * mean of the 2 x 2 below it in the finer one (2 * size across). */
float4 reduce(bool height, uint x, uint y, uint size)
{
    uint i = 2u * y * (2u * size) + 2u * x;
    if (height)
        return 0.25f * (g_heights[i] + g_heights[i + 1u] + g_heights[i + 2u * size] +
                        g_heights[i + 2u * size + 1u]);
    return 0.25f * (g_colors[i] + g_colors[i + 1u] + g_colors[i + 2u * size] + g_colors[i + 2u * size + 1u]);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    int2   texel  = region.xy + (int2)id.xy;
    float4 color  = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 height = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (all(texel < dims.zw)) {
        float depth = g_depth.Load(int3(texel, 0));
        if (depth > 0.0f) {
            float2 p    = ((float2)texel + 0.5f - viewport.xy) / viewport.zw;
            float4 clip = float4(p.x * 2.0f - 1.0f, 1.0f - p.y * 2.0f, depth, 1.0f);
            float  z    = dot(z_row, clip) / dot(w_row, clip);
            color       = float4(g_color.Load(int3(texel, 0)).rgb, 1.0f);
            height      = float4(abs(z - plane.x), 1.0f, 0.0f, 0.0f);
        }
    }
    g_level0[uint2(texel)]                   = color;
    g_level0[uint2(texel + int2(dims.x, 0))] = height;
    uint i       = local.y * 8u + local.x;
    g_colors[i]  = stored(color);
    g_heights[i] = stored(height);

    /* Levels 1 to 3: the group's texels start at its first texel's, halved
     * per level; the right half at each level's half width. */
    uint2 base = uint2(region.xy) / 2u + group.xy * 4u;
    uint  side = uint(dims.x) / 2u;
    [unroll] for (uint level = 1u; level <= 3u; ++level) {
        uint   size = 8u >> level;
        bool   owns = all(local.xy < size);
        float4 c    = float4(0.0f, 0.0f, 0.0f, 0.0f);
        float4 h    = float4(0.0f, 0.0f, 0.0f, 0.0f);
        GroupMemoryBarrierWithGroupSync();
        if (owns) {
            c = stored(reduce(false, local.x, local.y, size));
            h = stored(reduce(true, local.x, local.y, size));
        }
        GroupMemoryBarrierWithGroupSync();
        if (owns) {
            uint j       = local.y * size + local.x;
            g_colors[j]  = c;
            g_heights[j] = h;
            uint2 at     = base + local.xy;
            if (level == 1u) {
                g_level1[at]                    = c;
                g_level1[at + uint2(side, 0u)] = h;
            } else if (level == 2u) {
                g_level2[at]                    = c;
                g_level2[at + uint2(side, 0u)] = h;
            } else {
                g_level3[at]                    = c;
                g_level3[at + uint2(side, 0u)] = h;
            }
        }
        base /= 2u;
        side /= 2u;
    }
}
