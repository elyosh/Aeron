/*
 * Shared HLSL helper for sampling a 2D atlas through a sub-rect LUT.
 *
 * Sub-rect convention (matches FlightAtlasSubRect's normalised fields):
 *   sub_rect.xy = origin in atlas-normalised UV
 *   sub_rect.zw = size   in atlas-normalised UV
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
 *
 * Sub-rect with `zw == 0` (sentinel) means "channel absent for this
 * material / face group" — caller gates BEFORE invoking this helper,
 * since the fallback varies per path (OPT: untextured_color; glTF:
 * per-material factor). The helper itself does not handle sentinels.
 */

#ifndef FLIGHT_ATLAS_SAMPLE_HLSLI
#define FLIGHT_ATLAS_SAMPLE_HLSLI

float4 atlas_sample(Texture2D tex, SamplerState s,
                    float2 uv, float4 sub_rect)
{
    float2 wrapped  = frac(uv);
    float2 atlas_uv = sub_rect.xy + wrapped * sub_rect.zw;
    float2 dx       = ddx(uv) * sub_rect.zw;
    float2 dy       = ddy(uv) * sub_rect.zw;
    return tex.SampleGrad(s, atlas_uv, dx, dy);
}

#endif
