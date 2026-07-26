/*
 * Shared fullscreen-quad vertex shader.
 *
 * Emits a TRIANGLESTRIP (4 vertices via SV_VertexID 0..3 → BL, BR,
 * TL, TR) covering NDC (-1, -1) → (+1, +1). No vertex buffer, no
 * cbuffer — the host issues `SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0)`
 * with no input bindings beyond the FS-side textures/sampler/cbuffer.
 *
 * UV is flipped on Y (uv.y = 1 - cy) so NDC bottom maps to texture
 * bottom — matches the convention used by the blit shader and by
 * every RT this VS feeds. Paired with:
 *   - flight_tonemap.frag (final present pass)
 *   - bloom_brightpass.frag / bloom_downsample.frag / bloom_upsample.frag
 */

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    float cx = (float)(vid & 1u);
    float cy = (float)((vid >> 1u) & 1u);

    VSOut o;
    o.position = float4(cx * 2.0f - 1.0f, cy * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv       = float2(cx, 1.0f - cy);
    return o;
}
