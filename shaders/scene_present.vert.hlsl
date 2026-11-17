/*
 * Vertex shader of the present chain (src/scene/present.c).
 *
 * It is the shared full-screen vertex shader (scene_fullscreen_quad.vert)
 * with a source rectangle: the part of the scene texture that the draw
 * shows. A scene whose camera has a viewport smaller than the scene is
 * shown with that viewport as the source.
 */

cbuffer PresentVS : register(b0, space1)
{
    /* xy = the rectangle's upper left corner, zw = its size, in texture
     * coordinates. The whole texture is (0, 0, 1, 1). */
    float4 source;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOut main(float2 position : TEXCOORD0)
{
    VSOut o;
    o.position = float4(position, 0.0f, 1.0f);
    o.uv = source.xy + source.zw * float2(position.x * 0.5f + 0.5f, 0.5f - position.y * 0.5f);
    return o;
}
