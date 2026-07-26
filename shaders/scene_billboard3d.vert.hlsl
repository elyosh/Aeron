/*
 * Scene batched-billboard vertex shader (C5 unification of the TIE
 * flight_billboard / flight_backdrop paths and the XWA hook fans).
 *
 * Vertices are WORLD-space fan corners built game-side on the camera
 * right/up axes, so all corners of one billboard share the same
 * view-space depth and the quad projects as an exact screen-aligned
 * rectangle — the classic flat-sprite semantics both games need
 * (equivalent to the retired NDC-offset trick, see the TIE original).
 *
 * depth_bias (TEXCOORD1.x) pushes the TEST depth toward the camera in
 * view units without moving the sprite: with the project's reverse-Z
 * perspective (clip_z = near·k, clip_w = eye_z) the biased clip_z is
 * clip_z·clip_w / (clip_w − bias) so ndc_z = clip_z / (clip_w − bias).
 * All corners share pos-depth and bias, so the whole sprite gets one
 * flat depth — classic's flat-object rule (TIE xtrans2.c:488; XWA
 * bakes its explosion bias into the captured depth instead and
 * submits 0).
 */

cbuffer Billboard3DVS : register(b0, space1)
{
    row_major float4x4 view_proj;
};

struct VSIn
{
    float3 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float  bias  : TEXCOORD1;
    float3 prev  : TEXCOORD2;   /* unused by the color pipeline */
};

struct VSOut
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VSOut main(VSIn i)
{
    VSOut o;
    float4 cc = mul(view_proj, float4(i.pos, 1.0f));
    o.pos = cc;
    /* Apply the bias only when the vertex is safely in front of the
     * camera. Otherwise pass the TRUE clip coords through so the
     * hardware clipper cuts the triangle exactly (the classic
     * renderers 3D-clip such quads — XWA RenderQuad_DrawTextured3D).
     * Mangling z here (the old max() divisor clamp) corrupted the
     * clip interpolation for any vertex behind the camera plane, which
     * large backdrop strip segments legitimately have while still
     * partially on screen — corner pieces of close planets vanished.
     * Camera-facing sprites share one eye_z across their corners, so
     * the branch is uniform per sprite. */
    if (cc.w - i.bias > 0.001f)
        o.pos.z = cc.z * cc.w / (cc.w - i.bias);
    o.uv    = i.uv;
    o.color = i.color;
    return o;
}
