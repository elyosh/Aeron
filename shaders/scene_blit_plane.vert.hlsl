/*
 * Rect-blit vertex shader for a 2D draw list drawn on planes in 3D
 * (AeronDrawList_RenderPlanes).
 *
 * It is scene_blit.vert with two changes:
 *   - a quad's pixels go through its plane and through a view matrix, in
 *     place of the pixel to NDC conversion:
 *         v = P * (x, y, 1),  clip = V * (v, w);
 *   - a solid fill grows by one pixel of the drawing on each side, so that
 *     scene_blit_plane.frag can work out its edge coverage.
 *
 * Storage layouts MUST match Blit2DInstance and Blit2DPlane in
 * draw_list2d.c. The instance's `mode` holds (source, mip_bias, plane index,
 * solid fill).
 */

struct BlitUniforms
{
    float4 dst_rect;
    float4 src_rect;
    float4 tint;
    float4 bias;
    float4 trap;
    float4 mode;
};

/* The rows of P, and w in the last value's x. */
struct BlitPlane
{
    float4 row_x;
    float4 row_y;
    float4 row_z;
    float4 w;
};

StructuredBuffer<BlitUniforms> quads  : register(t0, space0);
StructuredBuffer<BlitPlane>    planes : register(t1, space0);

cbuffer BlitPlaneRun : register(b0, space1)
{
    /* The rows x, y and w of a view-projection matrix. */
    float4 view_x;
    float4 view_y;
    float4 view_w;
    float  output_rgb_scale;
    float  opacity;
    uint   base_instance;
    float  unused;
};

struct VSOut
{
    float4 position   : SV_Position;
    float2 uv         : TEXCOORD0;
    float4 tint       : TEXCOORD1;
    float4 bias       : TEXCOORD2;
    float  monochrome : TEXCOORD3;
    float  source     : TEXCOORD4;
    float  mip_bias   : TEXCOORD5;
    float  solid      : TEXCOORD6;
};

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    BlitUniforms u = quads[base_instance + iid];
    BlitPlane    plane = planes[(uint)u.mode.z];

    float cx = (float)(vid & 1u);
    float cy = (float)((vid >> 1u) & 1u);
    /* Where the corner is across the quad: 0 to 1 on both axes, with y up
     * as in scene_blit.vert. A solid fill reaches one pixel further. */
    float2 t = float2(cx, cy);
    if (u.mode.w > 0.5f)
        t += (t * 2.0f - 1.0f) / max(u.dst_rect.zw, float2(1e-6f, 1e-6f));

    float2 pos_px = u.dst_rect.xy + float2(t.x, 1.0f - t.y) * u.dst_rect.zw;
    pos_px.x += t.y * lerp(u.trap.x, u.trap.y, t.x);

    float2 uv = float2(
        u.src_rect.x + t.x * (u.src_rect.z - u.src_rect.x),
        u.src_rect.w + t.y * (u.src_rect.y - u.src_rect.w));

    float3 p = float3(pos_px, 1.0f);
    float4 v = float4(dot(plane.row_x.xyz, p), dot(plane.row_y.xyz, p), dot(plane.row_z.xyz, p), plane.w.x);
    float3 clip = float3(dot(view_x, v), dot(view_y, v), dot(view_w, v));

    float w_top    = (u.trap.z > 0.0f) ? u.trap.z : 1.0f;
    float w_corner = lerp(1.0f, w_top, cy);

    VSOut o;
    o.position = float4(clip.x * w_corner, clip.y * w_corner, 0.0f, clip.z * w_corner);
    o.uv       = uv;
    o.tint     = float4(u.tint.rgb * output_rgb_scale, u.tint.a) * opacity;
    o.bias     = float4(u.bias.rgb * output_rgb_scale, u.bias.a) * opacity;
    o.monochrome = u.trap.w;
    o.source     = u.mode.x;
    o.mip_bias   = u.mode.y;
    o.solid      = u.mode.w;
    return o;
}
