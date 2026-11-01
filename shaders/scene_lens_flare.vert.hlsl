/* The lens flare's quad: one triangle over the whole viewport, from
 * SV_VertexID 0 to 2, with no vertex buffer. When the source is hidden, the
 * three corners sit on one point, so the flare draws no pixels. */

/* x = the visible part, written by the visibility pass. */
StructuredBuffer<float4> g_visibility : register(t0, space0);

struct VSOut
{
    float4 position              : SV_Position;
    nointerpolation float visible : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    const float visible = g_visibility[0].x;
    const float2 corner = float2((float)((vid << 1u) & 2u), (float)(vid & 2u));

    VSOut o;
    o.position = visible > 0.0f ? float4(corner * 2.0f - 1.0f, 0.0f, 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
    o.visible  = visible;
    return o;
}
