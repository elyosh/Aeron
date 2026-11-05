/* The procedural lens flare (AeronScene_SetLensFlare): the halos and discs
 * of the half-resolution pass plus the hexagons and streaks, as a display
 * value, turned into linear light and scaled by the visible part of the
 * source. */

#include "srgb.hlsli"

Texture2D<float4>        g_smooth   : register(t0, space2);
SamplerState             g_smooth_s : register(s0, space2);
Texture2D<float>         g_depth    : register(t1, space2);
SamplerComparisonState   g_depth_s  : register(s1, space2);
StructuredBuffer<float4> g_elements : register(t2, space2);
/* x = the visible part, written by the visibility pass. */
StructuredBuffer<float4> g_visibility : register(t3, space2);

#include "scene_lens_flare.hlsli"

/* The least the disc hides of the flare. Farther out, it hides nothing and
 * the depth isn't read, which keeps the depth reads near the disc. */
static const float FLARE_MIN_COVER = 0.05f;
/* Where the disc's falloff reaches FLARE_MIN_COVER, in squared radii. */
static const float FLARE_COVER_REACH = pow(1.0f / FLARE_MIN_COVER, 1.0f / 1.8f);

float4 main(VSOut i) : SV_Target
{
    /* The half-resolution pass leaves the rest of its image undefined. */
    const float2 uv      = min((i.position.xy - origin.xy) * scale.zw, origin.zw);
    const float3 sum     = g_smooth.SampleLevel(g_smooth_s, uv, 0.0f).rgb + flare_sharp(flare_point(i.position.xy));
    const float3 display = min(sum, 1.0f);
    /* The source's disc is in front of the flare, so the flare never makes
     * the disc brighter. The disc hides the flare the same way the
     * procedural star's disc (AeronSceneBillboardStar) hides the sky: fully
     * inside the radius, then less and less outside it, down to
     * FLARE_MIN_COVER. */
    const float2 d     = flare_point(i.position.xy) - disc.xy;
    const float  r2    = dot(d, d);
    float        cover = 0.0f;
    if (r2 < disc.z * FLARE_COVER_REACH)
    {
        const float falloff = pow(disc.z / max(r2, 1.0e-12f), 1.8f);
        /* Only the visible part of the disc hides the flare. Where something
         * is in front of the source, the flare shows over it. The sampler
         * tests the four depth texels around the pixel and blends the
         * results, so the edge of what is in front stays smooth. With
         * reversed Z, geometry in front of the source has a greater depth. */
        const float2 depth_uv = render.zw + (i.position.xy - origin.xy) * render.xy;
        const float  open     = g_depth.SampleCmpLevelZero(g_depth_s, depth_uv, disc.w + 1e-5f);
        cover = saturate((falloff - FLARE_MIN_COVER) / (1.0f - FLARE_MIN_COVER)) * open;
    }
    return float4(AeronSrgbToLinear(display) * (frame.w * g_visibility[0].x * (1.0f - cover)), 0.0f);
}
