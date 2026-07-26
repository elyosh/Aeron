/*
 * Motion-blur NeighborMax (High tier). Takes the 5×5 maximum-magnitude
 * velocity over the TileMax buffer, so each tile inherits the dominant
 * motion of its neighbours — this is what lets a fast object's blur
 * spill into the surrounding (possibly static) tiles. The reconstruct
 * gathers along this velocity for the High tier.
 *
 * The kernel radius sets how far the velocity dilates onto the
 * background, which must reach at least as far as the gather (≈ half the
 * blur-length clamp) or the object's silhouette softens over a shorter
 * band than its interior smears — reading as a sharp edge. ±2 tiles
 * covers the ~1.5-tile gather reach used by the resolve.
 */
#define MB_NEIGHBOR_RADIUS 2

cbuffer MbTileUniforms : register(b0, space3)
{
    float2 tile_texel;   /* 1/tile_w, 1/tile_h (= src_texel) */
    float2 _base_scale;  /* unused by NeighborMax */
    float2 _step_dir;    /* unused by NeighborMax */
    float2 _pad;
};

Texture2D    g_tile    : register(t0, space2);
SamplerState g_sampler : register(s0, space2);   /* CLAMP_TO_EDGE */

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float2 main(VSOut i) : SV_Target0
{
    float2 best     = float2(0.0f, 0.0f);
    float  best_len = -1.0f;
    [unroll] for (int dy = -MB_NEIGHBOR_RADIUS; dy <= MB_NEIGHBOR_RADIUS; dy++) {
        [unroll] for (int dx = -MB_NEIGHBOR_RADIUS; dx <= MB_NEIGHBOR_RADIUS; dx++) {
            float2 uv = i.uv + float2(dx, dy) * tile_texel;
            float2 v  = g_tile.SampleLevel(g_sampler, uv, 0.0f).rg;
            float  l  = dot(v, v);
            if (l > best_len) { best_len = l; best = v; }
        }
    }
    return best;
}
