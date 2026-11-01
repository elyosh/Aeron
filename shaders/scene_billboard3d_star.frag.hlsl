/*
 * Procedural star for SKY-stage billboards (AeronSceneBillboardStar): a
 * white disc, a corona of moving rays in the tint, and a fade near the
 * edges, drawn from the uv alone.
 *
 * The disc and the corona are worked out as values for a display image,
 * as the star shows over a black sky, then turned into linear values and
 * premultiplied by the alpha. The vertex color scales them. The core
 * radiance adds HDR light in the disc's core, which only shows through the
 * bloom.
 */

#include "srgb.hlsli"

cbuffer StarFS : register(b0, space3)
{
    float4 tint_time; /* xyz = tint (sRGB), w = time */
    float4 params;    /* x = core radiance */
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

/* A value from 0 to 1 for each lattice point, from an integer hash. */
float star_lattice(int3 p)
{
    uint h = (uint)p.x * 0x8da6b343u ^ (uint)p.y * 0xd8163841u ^ (uint)p.z * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (float)h * (1.0f / 4294967295.0f);
}

/* Value noise from 0 to 1: the values of the cell's eight lattice points,
 * blended with a smooth step. */
float star_noise(float3 x)
{
    float3 cell = floor(x);
    float3 f    = x - cell;
    f           = f * f * (3.0f - 2.0f * f);
    int3 c      = (int3)cell;
    float a = lerp(star_lattice(c + int3(0, 0, 0)), star_lattice(c + int3(1, 0, 0)), f.x);
    float b = lerp(star_lattice(c + int3(0, 1, 0)), star_lattice(c + int3(1, 1, 0)), f.x);
    float d = lerp(star_lattice(c + int3(0, 0, 1)), star_lattice(c + int3(1, 0, 1)), f.x);
    float e = lerp(star_lattice(c + int3(0, 1, 1)), star_lattice(c + int3(1, 1, 1)), f.x);
    return lerp(lerp(a, b, f.y), lerp(d, e, f.y), f.z);
}

/* A turn of 1 radian around (1, 2, 2), so the octaves' lattices don't line
 * up. */
static const float3x3 STAR_TURN = float3x3(0.5914f, -0.4588f, 0.6632f,
                                           0.6632f, 0.7446f, -0.0762f,
                                           -0.4588f, 0.4848f, 0.7446f);

/* Four octaves of noise, each at about twice the frequency and half the
 * strength of the one before: 0 to 0.94, 0.47 on average. */
float star_fbm(float3 q)
{
    float sum    = 0.0f;
    float weight = 0.5f;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        sum += weight * star_noise(q);
        q = mul(STAR_TURN, q) * 2.03f;
        weight *= 0.5f;
    }
    return sum;
}

float4 main(VSOut i) : SV_Target
{
    float2 v  = i.uv - 0.5f;
    float  r2 = dot(v, v);
    float  r  = sqrt(r2);

    /* The disc: 1 up to r = 0.1, then a quick fall. */
    float disc = saturate(pow(0.01f / max(r2, 1.0e-6f), 1.8f));

    /* The corona. The noise is read along the direction from the centre,
     * which makes rays, and moves outward with the time. rho is 0 at
     * r = 0.125. */
    float  rho    = 8.0f * r - 1.0f;
    float2 dir    = v / max(r, 1.0e-5f);
    float  n      = star_fbm(float3(4.0f * dir, 0.25f * (tint_time.w - rho)));
    float  f      = saturate(n + 1.5f * exp(-0.5f * rho) - 0.8f);
    float  white  = 0.75f * f;
    float3 corona = 0.9f * f * lerp(tint_time.rgb, 1.0f, white * white * white);

    float3 color = saturate(disc + corona);
    /* Fade out near the edges, on the largest of |v.x| and |v.y|. */
    float  fade  = saturate((0.5f - max(abs(v.x), abs(v.y))) / 0.12f);
    float  alpha = saturate(disc + 0.9f * f) * fade;

    /* Over a black sky, the display shows color * alpha. */
    float3 rgb = AeronSrgbToLinear(color * alpha) * i.color.rgb;
    /* The core follows the disc's falloff, and ends smoothly by r = 0.25. */
    float core = disc * smoothstep(0.25f, 0.1f, r);
    rgb += params.x * core * AeronSrgbToLinear(tint_time.rgb);
    return float4(rgb, alpha * i.color.a);
}
