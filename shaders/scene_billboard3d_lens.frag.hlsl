/*
 * LENS-stage billboard fragment shader: atlas sample x vertex color,
 * scaled by the flare anchor's depth-buffer visibility.
 *
 * Reversed-Z: larger stored depth = closer; the anchor is occluded
 * where the scene holds a value greater than the anchor's reference
 * depth. The 5x5 kernel (radius params.x texels) yields a soft 0..1
 * fraction; it samples the SAME few texels for every pixel of a quad
 * (the anchor uv is a flat varying), so the taps stay cache-resident.
 */

Texture2D<float4> g_atlas : register(t0, space2);
SamplerState      s_atlas : register(s0, space2);
Texture2D<float>  g_depth : register(t1, space2);
SamplerState      s_depth : register(s1, space2);

cbuffer LensBillboardFS : register(b0, space3)
{
    float4 params; /* x = kernel radius (texels), y = rt_w, z = rt_h */
};

struct VSOut
{
    float4 pos    : SV_Position;
    float2 uv     : TEXCOORD0;
    float4 color  : COLOR0;
    float3 anchor : TEXCOORD1; /* xy = depth uv, z = ref depth (< 0: invalid) */
};

float4 main(VSOut i) : SV_Target
{
    float vis = 0.0f;
    if (i.anchor.z >= 0.0f)
    {
        float2 texel = params.x / float2(params.y, params.z);
        float  ref   = i.anchor.z + 1e-5f;
        float  open  = 0.0f;
        [unroll] for (int y = -2; y <= 2; ++y)
        {
            [unroll] for (int x = -2; x <= 2; ++x)
            {
                float d = g_depth.SampleLevel(s_depth, i.anchor.xy + float2(x, y) * texel * 0.5f, 0.0f);
                open += d <= ref ? 1.0f : 0.0f;
            }
        }
        vis = open * (1.0f / 25.0f);
    }
    float4 c = g_atlas.Sample(s_atlas, i.uv) * i.color * vis;
    if (c.a < 0.01f)
        discard;
    return c;
}
