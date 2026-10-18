/* This shader makes levels 5 and up of the environment builder's source
 * atlas (environment_builder.c) from a copy of level 4, which holds the six
 * faces side by side. A texel of level k is the mean of the 2^(k-4) x 2^(k-4)
 * texels it covers in level 4. For blocks up to 8 x 8, a thread writes one
 * texel. For larger blocks, a group of 8 x 8 threads writes one texel: each
 * thread sums part of the block, and the group adds the 64 sums. The group's
 * z is the face. */

cbuffer EnvBuildDownsample : register(b0, space2)
{
    int4 params; /* level, level 4 face size */
};

Texture2D<float4>   g_level4         : register(t0, space0);
SamplerState        g_level4_sampler : register(s0, space0);
RWTexture2D<float4> g_level          : register(u0, space1);

groupshared float3 g_sums[64];

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    int  face      = (int)group.z;
    int  block     = (int)(1u << (uint)(params.x - 4));
    int  size      = params.y / block;
    bool per_group = block > 8;

    int2   texel = per_group ? (int2)group.xy : (int2)id.xy;
    int2   start = int2(face * params.y, 0) + texel * block;
    float3 sum   = float3(0.0f, 0.0f, 0.0f);
    if (per_group) {
        for (int y = (int)local.y; y < block; y += 8)
            for (int x = (int)local.x; x < block; x += 8)
                sum += g_level4.Load(int3(start + int2(x, y), 0)).rgb;
    } else if (all(texel < size)) {
        for (int y = 0; y < block; ++y)
            for (int x = 0; x < block; ++x)
                sum += g_level4.Load(int3(start + int2(x, y), 0)).rgb;
    }

    uint i    = local.y * 8u + local.x;
    g_sums[i] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint width = 32u; width > 0u; width >>= 1u) {
        if (i < width)
            g_sums[i] += g_sums[i + width];
        GroupMemoryBarrierWithGroupSync();
    }

    float scale = 1.0f / (float)(block * block);
    if (per_group) {
        if (i == 0u)
            g_level[uint2(face * size + texel.x, texel.y)] = float4(g_sums[0] * scale, 1.0f);
    } else if (all(texel < size)) {
        g_level[uint2(face * size + texel.x, texel.y)] = float4(sum * scale, 1.0f);
    }
}
