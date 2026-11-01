#ifndef AERON_SCENE_LENS_FLARE_HLSLI
#define AERON_SCENE_LENS_FLARE_HLSLI

/* The procedural lens flare's elements (AeronScene_SetLensFlare), shared by
 * the half-resolution pass of the halos and discs and the full-resolution
 * pass that adds the hexagons and streaks. The includer declares
 * g_elements, three float4 per element, sorted by shape (halos, discs,
 * hexagons, then streaks):
 *   centre x, centre y, stretch, size (discs: 1 / size^2);
 *   power (discs: power / 2), cos(rotation), sin(rotation), unused;
 *   colour, with the tint and the intensity, unused. */

cbuffer LensFlareFS : register(b0, space3)
{
    float4 frame;  /* xy = the viewport's centre, in this pass's pixels times `scale.x`;
                      z = flare units per output pixel; w = radiance of a display value of 1 */
    float4 scale;  /* x = output pixels per pixel of this pass; y = unused;
                      zw = the half-resolution image's uv per output pixel */
    float4 origin; /* xy = the output viewport's origin, output pixels; zw = the largest uv
                      that the half-resolution pass draws */
    uint4  ends;   /* where the halos, discs, hexagons and streaks end */
};

struct VSOut
{
    float4 position              : SV_Position;
    nointerpolation float visible : TEXCOORD0;
};

/* The pixel's place in flare units: 0 at the viewport's centre, 1 at half
 * its shorter side, y down. */
float2 flare_point(float2 position)
{
    return (position * scale.x - frame.xy) * frame.z;
}

/* The pixel's offset from the element's centre, with the element's stretch
 * toward the screen edges. u is p * |p|. */
float2 flare_offset(float2 p, float2 u, float4 shape)
{
    return p + shape.z * (u - p) - shape.xy;
}

/* The offset turned by -rotation. */
float2 flare_turn(float2 d, float4 place)
{
    return float2(dot(d, place.yz), dot(d, float2(-place.z, place.y)));
}

/* The halos and the discs: smooth shapes, drawn at half resolution. */
float3 flare_smooth(float2 p)
{
    const float2 u   = p * length(p);
    float3       sum = 0.0f;
    uint         e   = 0u;
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
    return sum;
}

/* The hexagons and the streaks: shapes with sharp edges or a thin core,
 * drawn at full resolution. */
float3 flare_sharp(float2 p)
{
    const float2 u   = p * length(p);
    float3       sum = 0.0f;
    uint         e   = ends.y;
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
    return sum;
}

#endif
