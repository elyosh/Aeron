/*
 * Scene batched-billboard VELOCITY fragment shader.
 *
 * Stamps the sprite's screen velocity into the motion-blur velocity
 * buffer (SV_Target1); SV_Target0 is the normal G-buffer, write-
 * masked by the pipeline. Alpha-TESTED at a firm 0.5 (vs the color
 * pass's 0.01): the velocity buffer is a single-opaque-layer signal,
 * so only substantially-covered pixels stamp — feathered edges keep
 * the underlying camera/geometry velocity, limiting background
 * smear-through.
 */

Texture2D<float4> g_atlas : register(t0, space2);
SamplerState      s_atlas : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float2 vel      : TEXCOORD1;
};

struct FSOut
{
    float2 normal   : SV_Target0;   /* masked off by the pipeline */
    float2 velocity : SV_Target1;
};

FSOut main(VSOut i)
{
    float a = g_atlas.Sample(s_atlas, i.uv).a;
    if (a < 0.5f)
        discard;
    FSOut o;
    o.normal   = float2(0.0f, 0.0f);
    o.velocity = i.vel;
    return o;
}
