/* One texel of the floor's reflection (planar_reflection.c): in the left
 * half, the mirrored view's colour premultiplied by its coverage and the
 * coverage; in the right half, the height of the reflected point above the
 * plane in metres times the coverage, and the coverage, so that the mips
 * keep the mean height of what they cover. Only the region the floor reads
 * is written. */

cbuffer PlanarReflectionResolve : register(b0, space2)
{
    float4 z_row;    /* clip to the model z of the mirrored point, before the divide */
    float4 w_row;    /* clip to its w */
    float4 viewport; /* the mirrored view's viewport, texels */
    float4 plane;    /* x: the plane's model z */
    int4   dims;     /* each half's width and the height (padded), the drawn width and height */
    int4   region;   /* the texels to write: x0, y0, x1, y1 (left half) */
};

Texture2D<float4>   g_color         : register(t0, space0);
SamplerState        g_color_sampler : register(s0, space0);
Texture2D<float>    g_depth         : register(t1, space0);
SamplerState        g_depth_sampler : register(s1, space0);
RWTexture2D<float4> g_image         : register(u0, space1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int2 texel = region.xy + (int2)id.xy;
    if (any(texel >= region.zw)) return;
    float4 color  = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 height = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (all(texel < dims.zw)) {
        float depth = g_depth.Load(int3(texel, 0));
        if (depth > 0.0f) {
            float2 p    = ((float2)texel + 0.5f - viewport.xy) / viewport.zw;
            float4 clip = float4(p.x * 2.0f - 1.0f, 1.0f - p.y * 2.0f, depth, 1.0f);
            float  z    = dot(z_row, clip) / dot(w_row, clip);
            color       = float4(g_color.Load(int3(texel, 0)).rgb, 1.0f);
            height      = float4(abs(z - plane.x), 1.0f, 0.0f, 0.0f);
        }
    }
    g_image[uint2(texel)]                = color;
    g_image[uint2(texel + int2(dims.x, 0))] = height;
}
