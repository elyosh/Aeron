/* This shader computes the environment builder's diffuse cube
 * (environment_builder.c) as envmap_irradiance does. Each output texel is
 * the sum, over every texel of one source level, of its radiance times its
 * solid angle times the cosine to the texel's normal, divided by pi. The source level holds
 * the six faces side by side.
 *
 * A group has 8 x 16 output texels and 512 threads. Each thread has two
 * texels, 8 rows apart, and each texel has 8 threads. The group computes the
 * direction and the radiance times solid angle of 512 source texels at a
 * time and shares them. Each of a texel's 8 threads adds an eighth of them,
 * and the 8 sums are added at the end. The source texels behind a texel add
 * 0, through max(cos, 0), so that the loop has no branch. */

#include "env_build_common.hlsli"

cbuffer EnvBuildIrradiance : register(b0, space2)
{
    int4 params; /* source level, source size at that level, output size */
};

Texture2D<float4>   g_source         : register(t0, space0);
SamplerState        g_source_sampler : register(s0, space0);
RWTexture2D<float4> g_output         : register(u0, space1);

/* The number of threads per output texel. It must match BUILD_DIFFUSE_SPLIT
 * in environment_builder.c. */
#define SPLIT 8
#define CHUNK (64 * SPLIT)

groupshared float4 g_directions[CHUNK];
groupshared float4 g_radiance[CHUNK];
groupshared float3 g_sums[2 * CHUNK];

/* Returns one atan2 term of the exact texel solid angle
 * (envmap_texel_solid_angle). */
float corner_area(float x, float y)
{
    return atan2(x * y, sqrt(x * x + y * y + 1.0f));
}

[numthreads(8, 8, SPLIT)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    int    face    = (int)group.z;
    int    level   = params.x;
    int    n       = params.y;
    int    size    = params.z;
    int2   texel   = int2(group.x * 8u + local.x, group.y * 16u + local.y);
    float3 normal0 = env_texel_direction(face, texel.x, texel.y, size);
    float3 normal1 = env_texel_direction(face, texel.x, texel.y + 8, size);
    int    count   = 6 * n * n;
    float3 sum0    = float3(0.0f, 0.0f, 0.0f);
    float3 sum1    = float3(0.0f, 0.0f, 0.0f);
    for (int first = 0; first < count; first += CHUNK) {
        int source = first + (int)index;
        if (source < count) {
            int   f     = source / (n * n);
            int   x     = source % n;
            int   y     = (source / n) % n;
            float inv   = 1.0f / (float)n;
            float s0    = 2.0f * (float)x * inv - 1.0f;
            float t0    = 2.0f * (float)y * inv - 1.0f;
            float s1    = s0 + 2.0f * inv;
            float t1    = t0 + 2.0f * inv;
            float omega = corner_area(s0, t0) - corner_area(s0, t1) - corner_area(s1, t0) + corner_area(s1, t1);
            g_directions[index] = float4(env_texel_direction(f, x, y, n), 0.0f);
            g_radiance[index]   = float4(g_source.Load(int3(f * n + x, y, level)).rgb * omega, 0.0f);
        } else {
            g_directions[index] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            g_radiance[index]   = float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint j = 0u; j < CHUNK / SPLIT; ++j) {
            uint   k = j * SPLIT + local.z;
            float3 d = g_directions[k].xyz;
            float3 l = g_radiance[k].rgb;
            sum0 += l * max(dot(normal0, d), 0.0f);
            sum1 += l * max(dot(normal1, d), 0.0f);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    g_sums[index]         = sum0;
    g_sums[CHUNK + index] = sum1;
    GroupMemoryBarrierWithGroupSync();
    if (local.z == 0u) {
        float3 total0 = sum0;
        float3 total1 = sum1;
        for (uint z = 1u; z < SPLIT; ++z) {
            total0 += g_sums[index + 64u * z];
            total1 += g_sums[CHUNK + index + 64u * z];
        }
        g_output[uint2(face * size + texel.x, texel.y)]     = float4(total0 / ENV_BUILD_PI, 1.0f);
        g_output[uint2(face * size + texel.x, texel.y + 8)] = float4(total1 / ENV_BUILD_PI, 1.0f);
    }
}
