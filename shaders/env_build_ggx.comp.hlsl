/* This shader computes one level of the environment builder's specular cube
 * (environment_builder.c), for levels 1 and up, as envmap_ggx_prefilter
 * does. It is a GGX prefilter of the source cube with N = V. The samples follow a
 * Hammersley sequence with a half offset. Each sample reads the source at
 * the lod that matches its solid angle, plus 1. The result is the mean of
 * the samples weighted by N.L, and samples with N.L <= 0 are skipped. The
 * level is written into an atlas that holds the six faces side by side. */

#include "env_build_common.hlsli"

cbuffer EnvBuildGgx : register(b0, space2)
{
    float4 params; /* roughness, source texel solid angle, highest source lod */
    int4   counts; /* output size, sample count */
};

TextureCube<float4> g_source         : register(t0, space0);
SamplerState        g_source_sampler : register(s0, space0);
RWTexture2D<float4> g_output         : register(u0, space1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int size = counts.x;
    if (any((int2)id.xy >= size))
        return;
    int    face   = (int)id.z;
    float3 normal = env_texel_direction(face, (int)id.x, (int)id.y, size);
    float3 ref    = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent   = normalize(cross(ref, normal));
    float3 bitangent = cross(normal, tangent);

    float alpha   = params.x * params.x;
    float a2      = alpha * alpha;
    int   samples = counts.y;
    float3 sum    = float3(0.0f, 0.0f, 0.0f);
    float  total  = 0.0f;
    for (int i = 0; i < samples; ++i) {
        float u1        = ((float)i + 0.5f) / (float)samples;
        float u2        = (float)reversebits((uint)i) * 2.3283064365386963e-10f;
        float phi       = 2.0f * ENV_BUILD_PI * u2;
        float cos_theta = sqrt((1.0f - u1) / (1.0f + (a2 - 1.0f) * u1));
        float sin_theta = sqrt(1.0f - cos_theta * cos_theta);
        float3 h = float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
        /* L = 2 (V.H) H - V with V = N = +Z. */
        float3 l = float3(2.0f * h.z * h.x, 2.0f * h.z * h.y, 2.0f * h.z * h.z - 1.0f);
        if (l.z <= 0.0f)
            continue;
        /* pdf(L) = D(H) (N.H) / (4 V.H) = D / 4 when N = V. */
        float denom = cos_theta * cos_theta * (a2 - 1.0f) + 1.0f;
        float pdf   = a2 / (ENV_BUILD_PI * denom * denom) / 4.0f;
        float omega = 1.0f / ((float)samples * pdf);
        float lod   = clamp(0.5f * log2(omega / params.y) + 1.0f, 0.0f, params.z);
        float3 dir  = tangent * l.x + bitangent * l.y + normal * l.z;
        sum += g_source.SampleLevel(g_source_sampler, dir, lod).rgb * l.z;
        total += l.z;
    }
    float3 value = total > 0.0f ? sum / total : float3(0.0f, 0.0f, 0.0f);
    g_output[uint2(face * size + (int)id.x, id.y)] = float4(value, 1.0f);
}
