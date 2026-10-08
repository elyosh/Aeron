/*
 * Octahedral encoding for unit 3-vectors → 2 components in [-1, 1].
 * Cigolle et al., JCGT 2014: matches an R16G16_SNORM normal G-buffer
 * exactly. Encode error vs raw float3 is well under 0.5° on a 16-bit
 * snorm, which is far below the visible threshold for SSAO.
 *
 * Used by:
 *   - Mesh FSes (classic-LUT, classic-PBR, OPT-PBR, glTF-PBR): write
 *     world-space normal to SV_Target1 via oct_encode.
 *   - SSAO FS: read the normal RT, oct_decode, then world→view rotate.
 */

#ifndef OCTAHEDRAL_NORMAL_HLSLI
#define OCTAHEDRAL_NORMAL_HLSLI

float2 oct_sign_not_zero(float2 v)
{
    return float2((v.x >= 0.0f) ? 1.0f : -1.0f,
                  (v.y >= 0.0f) ? 1.0f : -1.0f);
}

float2 oct_encode(float3 v)
{
    float l1 = abs(v.x) + abs(v.y) + abs(v.z);
    float2 p = v.xy / max(l1, 1e-8f);
    return (v.z <= 0.0f)
         ? (1.0f - abs(p.yx)) * oct_sign_not_zero(p)
         : p;
}

float3 oct_decode(float2 e)
{
    float3 v = float3(e.xy, 1.0f - abs(e.x) - abs(e.y));
    if (v.z < 0.0f) v.xy = (1.0f - abs(v.yx)) * oct_sign_not_zero(v.xy);
    return normalize(v);
}

/* A flag in the parity of the first R16G16_SNORM code: the encoded x is
 * quantized to that format's step (1/32767) and its last bit set, costing
 * one step of normal precision. The code stays within [-32766, 32767], so
 * the format's clamp of -1 cannot change the parity. */
float2 oct_encode_flagged(float3 v, bool flag)
{
    float2 e = oct_encode(v);
    int code = clamp((int)round(e.x * 32767.0f), -32766, 32766);
    code = (code & ~1) | (flag ? 1 : 0);
    return float2((float)code / 32767.0f, e.y);
}

bool oct_flag(float2 e)
{
    return ((int)round(e.x * 32767.0f) & 1) != 0;
}

#endif
