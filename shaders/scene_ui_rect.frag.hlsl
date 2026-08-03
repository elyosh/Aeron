/*
 * UI rounded-rect fragment shader — analytic SDF, no textures.
 *
 * One instance renders any of: flat fill, vertical gradient, border
 * ring, bevel highlight/shadow bands, focus outline (zero-alpha fill +
 * border), and soft drop shadows (large `soft`). All colors arrive
 * premultiplied; compositing below is PMA-over layering, and coverage
 * scales both rgb and a, so output feeds the standard PMA hardware
 * blend.
 *
 * params = (radius, border, bevel, soft), all in pixels:
 *   radius : corner radius (0 = square)
 *   border : ring width inward from the edge (0 = none)
 *   bevel  : band height inside the top/bottom inner edges (0 = flat)
 *   soft   : edge falloff width; 1 = crisp AA, large = shadow blur
 */

struct VSOut
{
    float4                 position    : SV_Position;
    nointerpolation float4 rect        : TEXCOORD0;
    nointerpolation float4 params      : TEXCOORD1;
    nointerpolation float4 fill_top    : TEXCOORD2;
    nointerpolation float4 fill_bottom : TEXCOORD3;
    nointerpolation float4 border_col  : TEXCOORD4;
    nointerpolation float4 bevel_hi    : TEXCOORD5;
    nointerpolation float4 bevel_lo    : TEXCOORD6;
};

float4 main(VSOut input) : SV_Target
{
    float2 half_size = input.rect.zw * 0.5f;
    float2 center    = input.rect.xy + half_size;
    float2 p         = input.position.xy - center;

    float radius = min(input.params.x, min(half_size.x, half_size.y));
    float border = input.params.y;
    float bevel  = input.params.z;
    float soft   = max(input.params.w, 1.0f);

    /* Signed distance to the rounded rect (negative inside). */
    float2 q = abs(p) - (half_size - radius);
    float  d = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - radius;

    float coverage = 1.0f - smoothstep(-0.5f * soft, 0.5f * soft, d);
    if (coverage <= 0.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    /* Vertical gradient across the rect height (equal endpoint colors
     * degenerate to a flat fill). */
    float  v = saturate((p.y + half_size.y) / max(input.rect.w, 1.0f));
    float4 c = lerp(input.fill_top, input.fill_bottom, v);

    /* Bevel bands hug the top/bottom inner edges (inside the border
     * ring). Weight fades linearly across the band height. */
    if (bevel > 0.0f)
    {
        float from_top    = (p.y + half_size.y) - border;
        float from_bottom = (half_size.y - p.y) - border;
        float w_hi = saturate(1.0f - from_top / bevel) * step(0.0f, from_top);
        float w_lo = saturate(1.0f - from_bottom / bevel) * step(0.0f, from_bottom);
        c = c * (1.0f - w_hi * input.bevel_hi.a) + input.bevel_hi * w_hi;
        c = c * (1.0f - w_lo * input.bevel_lo.a) + input.bevel_lo * w_lo;
    }

    /* Border ring: full border color past the inner edge at d = -border,
     * antialiased over 1 px. */
    if (border > 0.0f)
    {
        float w_b = smoothstep(-border - 0.5f, -border + 0.5f, d);
        c = c * (1.0f - w_b * input.border_col.a) + input.border_col * w_b;
    }

    return c * coverage;
}
