/*
 * Shared HLSL helper for sampling a 2D-array atlas through a sub-rect LUT.
 *
 * Sub-rect convention (matches FlightAtlasSubRect's normalised fields):
 *   sub_rect.xy = origin in atlas-normalised UV
 *   sub_rect.zw = size   in atlas-normalised UV
 * `page` selects the array layer; `max_lod` is the deepest mip level the
 * sub-rect may sample (15 = unclamped).
 *
 * Behaviour:
 *   - `frac(uv)` reproduces REPEAT semantics for UVs outside [0, 1].
 *     OPT face groups commonly tile; glTF artist UVs may tile or not.
 *   - Mip / aniso selection is driven by `ddx/ddy` of the ORIGINAL `uv`
 *     scaled by sub-rect size — never by the wrapped UV. Computing
 *     derivatives on `frac(uv)` would produce infinities at integer
 *     crossings and pin the sampler to mip 0.
 *   - SampleGrad bypasses the implicit-LOD path so the wrap step
 *     above doesn't affect mip selection.
 *   - The LOD limit shrinks both gradients by the same factor, which
 *     clamps the selected level without changing the anisotropy ratio.
 *     (The SampleGrad overload taking a clamp does not translate through
 *     shadercross.) A sub-rect packed with neighbours must not sample
 *     levels whose texels mix footprints.
 *
 * Sub-rect with `zw == 0` (sentinel) means "channel absent for this
 * material / face group" — caller gates BEFORE invoking this helper,
 * since the fallback varies per path (OPT: untextured_color; glTF:
 * per-material factor). The helper itself does not handle sentinels.
 */

#ifndef FLIGHT_ATLAS_SAMPLE_HLSLI
#define FLIGHT_ATLAS_SAMPLE_HLSLI

float4 atlas_sample(Texture2DArray tex, SamplerState s,
                    float2 uv, float4 sub_rect, uint page, uint max_lod)
{
    float2 wrapped  = frac(uv);
    float2 atlas_uv = sub_rect.xy + wrapped * sub_rect.zw;
    float2 dx       = ddx(uv) * sub_rect.zw;
    float2 dy       = ddy(uv) * sub_rect.zw;
    uint width, height, layers, levels;
    tex.GetDimensions(0, width, height, layers, levels);
    float2 size   = float2(width, height);
    float2 tdx    = dx * size;
    float2 tdy    = dy * size;
    float  lod    = 0.5f * log2(max(max(dot(tdx, tdx), dot(tdy, tdy)), 1e-20f));
    float  shrink = exp2(min((float)max_lod - lod, 0.0f));
    return tex.SampleGrad(s, float3(atlas_uv, (float)page), dx * shrink, dy * shrink);
}

#endif
