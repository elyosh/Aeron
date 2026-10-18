/* This shader writes levels 0 to 4 of a rendered face into the environment
 * builder's source atlas (environment_builder.c). With mirror, the face's columns run from
 * right to left in the rendered texture. A group of 16 x 16 threads reads
 * 16 x 16 texels and writes them to level 0. It then averages them 2 x 2
 * into its 8 x 8, 4 x 4, 2 x 2 and 1 x 1 texels of levels 1 to 4, which
 * gives the box mip chain of envmap_build_box_mips. Level 4 is also written
 * to a separate texture, from which env_build_downsample makes the other
 * levels. */

cbuffer EnvBuildStoreFace : register(b0, space2)
{
    int4 params; /* face, unused, face size at level 0, mirror */
};

Texture2D<float4>   g_rendered         : register(t0, space0);
SamplerState        g_rendered_sampler : register(s0, space0);
RWTexture2D<float4> g_level0           : register(u0, space1);
RWTexture2D<float4> g_level1           : register(u1, space1);
RWTexture2D<float4> g_level2           : register(u2, space1);
RWTexture2D<float4> g_level3           : register(u3, space1);
RWTexture2D<float4> g_level4           : register(u4, space1);
RWTexture2D<float4> g_level4_copy      : register(u5, space1);

groupshared float3 g_values[256];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    uint face   = (uint)params.x;
    uint size   = (uint)params.z;
    uint source = params.w != 0 ? size - 1u - id.x : id.x;
    float3 c    = g_rendered.Load(int3(source, id.y, 0)).rgb;
    g_level0[uint2(face * size + id.x, id.y)] = float4(c, 1.0f);
    g_values[local.y * 16u + local.x] = c;

    /* Level k keeps its n x n texels at the start of g_values, row by row. */
    [unroll] for (uint level = 1u; level <= 4u; ++level) {
        uint   n    = 16u >> level;
        bool   owns = all(local.xy < n);
        float3 v    = float3(0.0f, 0.0f, 0.0f);
        GroupMemoryBarrierWithGroupSync();
        if (owns) {
            uint i = 2u * local.y * (2u * n) + 2u * local.x;
            v = 0.25f * (g_values[i] + g_values[i + 1u] + g_values[i + 2u * n] + g_values[i + 2u * n + 1u]);
        }
        GroupMemoryBarrierWithGroupSync();
        if (owns) {
            g_values[local.y * n + local.x] = v;
            uint  level_size = size >> level;
            uint2 at         = uint2(face * level_size, 0u) + group.xy * n + local.xy;
            if (level == 1u)
                g_level1[at] = float4(v, 1.0f);
            else if (level == 2u)
                g_level2[at] = float4(v, 1.0f);
            else if (level == 3u)
                g_level3[at] = float4(v, 1.0f);
            else {
                g_level4[at]      = float4(v, 1.0f);
                g_level4_copy[at] = float4(v, 1.0f);
            }
        }
    }
}
