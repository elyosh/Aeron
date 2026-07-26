/*
 * Scene batched-billboard VELOCITY vertex shader.
 *
 * Rasterises the same fan as scene_billboard3d.vert (identical
 * projection and depth bias) so velocity coverage matches the color
 * draw's depth test, and computes the per-vertex screen velocity from
 * the previous-frame corner (TEXCOORD2) on the GPU:
 *   vel = (ndc_now − ndc_prev) × (0.5, −0.5)
 * — the same UV-space convention as the mesh prepass.
 *
 * params.x (camera_mb) mirrors the mesh prepass's camera_mb flag:
 * when 0, the "now" endpoint projects through the PREVIOUS camera so
 * only object motion survives (camera blur handled elsewhere / off);
 * when 1, through the current camera (full camera + object motion).
 * The raster position always uses the current camera.
 */

cbuffer Billboard3DVelVS : register(b0, space1)
{
    row_major float4x4 view_proj;
    row_major float4x4 unjittered_view_proj;
    row_major float4x4 prev_view_proj;
    float4             params;   /* x = camera_mb */
};

struct VSIn
{
    float3 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;      /* unused */
    float  bias  : TEXCOORD1;
    float3 prev  : TEXCOORD2;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float2 vel : TEXCOORD1;
};

VSOut main(VSIn i)
{
    VSOut o;
    float4 cc = mul(view_proj, float4(i.pos, 1.0f));
    o.pos = cc;
    /* Bias only when safely in front — behind-camera vertices keep
     * their TRUE clip coords for exact hardware clipping (same rule
     * as scene_billboard3d.vert; see the rationale there). */
    if (cc.w - i.bias > 0.001f)
        o.pos.z = cc.z * cc.w / (cc.w - i.bias);
    o.uv = i.uv;

    float4 cn = (params.x != 0.0f)
                                   ? mul(unjittered_view_proj, float4(i.pos, 1.0f))
                                   : mul(prev_view_proj, float4(i.pos, 1.0f));
    float4 pn = mul(prev_view_proj, float4(i.prev, 1.0f));
    o.vel = float2(0.0f, 0.0f);
    if (cn.w > 1e-6f && pn.w > 1e-6f) {
        float2 d = cn.xy / cn.w - pn.xy / pn.w;
        o.vel = d * float2(0.5f, -0.5f);
    }
    return o;
}
