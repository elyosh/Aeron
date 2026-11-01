/* The procedural lens flare (AeronScene_SetLensFlare): the sum of the
 * elements' shapes in flare units, as a display value, turned into linear
 * light and scaled by the visible part of the source. */

#include "srgb.hlsli"

/* Three float4 per element, sorted by shape (halos, discs, hexagons, then
 * streaks):
 *   centre x, centre y, stretch, size (discs: 1 / size^2);
 *   power (discs: power / 2), cos(rotation), sin(rotation), unused;
 *   colour, with the tint and the intensity, unused. */
StructuredBuffer<float4> g_elements : register(t0, space2);

cbuffer LensFlareFS : register(b0, space3)
{
    float4 frame; /* xy = the viewport's centre, output pixels; z = flare units per pixel;
                     w = radiance of a display value of 1 */
    uint4  ends;  /* where the halos, discs, hexagons and streaks end */
};

struct VSOut
{
    float4 position              : SV_Position;
    nointerpolation float visible : TEXCOORD0;
};

/* The pixel's offset from the element's centre, with the element's stretch
 * toward the screen edges. */
float2 flare_offset(float2 p, float2 u, float4 shape)
{
    return p + shape.z * (u - p) - shape.xy;
}

/* The offset turned by -rotation. */
float2 flare_turn(float2 d, float4 place)
{
    return float2(dot(d, place.yz), dot(d, float2(-place.z, place.y)));
}

float4 main(VSOut i) : SV_Target
{
    const float2 p = (i.position.xy - frame.xy) * frame.z;
    const float2 u = p * length(p);

    float3 sum = 0.0f;
    uint   e   = 0u;
    [loop] for (; e < ends.x; ++e)
    {
        const float4 shape = g_elements[e * 3u];
        const float2 d     = flare_offset(p, u, shape);
        sum += g_elements[e * 3u + 2u].rgb / (1.0f + shape.w * dot(d, d));
    }
    [loop] for (; e < ends.y; ++e)
    {
        const float4 shape = g_elements[e * 3u];
        const float2 d     = flare_offset(p, u, shape);
        /* The squared distance over the squared radius: 1 at the edge. */
        const float r2 = dot(d, d) * shape.w;
        if (r2 < 1.0f)
            sum += g_elements[e * 3u + 2u].rgb * (1.0f - pow(r2, g_elements[e * 3u + 1u].x));
    }
    [loop] for (; e < ends.z; ++e)
    {
        const float4 shape = g_elements[e * 3u];
        const float2 d     = flare_offset(p, u, shape);
        /* Past the corners (1.155 times the sides' distance) and the edge,
         * nothing is added. */
        const float reach = 1.155f * shape.w + frame.z;
        if (dot(d, d) < reach * reach)
        {
            const float2 a = abs(flare_turn(d, g_elements[e * 3u + 1u]));
            /* The distance to the nearest side, from the sides' normals at 0,
             * 60 and 120 degrees; the edge is one pixel wide. */
            const float h = max(a.x, 0.5f * a.x + 0.8660254f * a.y);
            sum += g_elements[e * 3u + 2u].rgb * saturate((shape.w - h) / frame.z + 0.5f);
        }
    }
    [loop] for (; e < ends.w; ++e)
    {
        const float4 shape = g_elements[e * 3u];
        const float4 place = g_elements[e * 3u + 1u];
        const float2 a     = abs(flare_turn(flare_offset(p, u, shape), place));
        sum += g_elements[e * 3u + 2u].rgb / max(a.x + place.x * a.y, shape.w);
    }

    const float3 display = min(sum, 1.0f);
    return float4(AeronSrgbToLinear(display) * (frame.w * i.visible), 0.0f);
}
