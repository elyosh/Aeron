/*
 * Shared vertex shader of the scene's full-screen passes.
 *
 * The positions come from a vertex buffer, already in clip space
 * (src/scene/fullscreen_mesh.c). Normally the mesh is a quad that covers
 * the whole target. In a headset it is the eye's visible-area mesh.
 *
 * The texture coordinate is flipped on Y, so the bottom of the screen is
 * the bottom of the texture. Every target that this shader feeds uses that
 * convention.
 */

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOut main(float2 position : TEXCOORD0)
{
    VSOut o;
    o.position = float4(position, 0.0f, 1.0f);
    o.uv       = float2(position.x * 0.5f + 0.5f, 0.5f - position.y * 0.5f);
    return o;
}
