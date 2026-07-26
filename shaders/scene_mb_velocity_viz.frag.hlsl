/*
 * Velocity-buffer debug visualisation. Fullscreen pass drawn into the
 * color pass when the MB inspector's "velocity viz" toggle is on:
 * false-colours velocity_rt over the scene so the generated motion
 * vectors (per-object + the camera-rotational sky fill) can be
 * inspected. The starfield should show a smooth rotational gradient as
 * the camera turns.
 *
 * Encoding: +X velocity → red, +Y velocity → green, centred at grey
 * (0.5). `gain` (cbuffer .x) scales the small UV-space velocities into a
 * visible range; magnitude is added to blue for a faint brightness cue.
 */

cbuffer MbVelocityVizUniforms : register(b0, space3)
{
    float gain;      /* viz amplification of the UV-space velocity */
    uint direct_fsr_motion;
    uint2 velocity_size;
};

Texture2D    g_velocity : register(t0, space2);
SamplerState g_sampler  : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(VSOut i) : SV_Target0
{
    uint2 pixel = min(uint2(saturate(i.uv) * float2(velocity_size)), velocity_size - 1u);
    float2 source = direct_fsr_motion != 0u
        ? -g_velocity.Load(int3(pixel, 0)).rg
        : g_velocity.Sample(g_sampler, i.uv).rg;
    float2 v = source * gain;
    float  m = saturate(length(v));
    return float4(saturate(0.5f + v.x), saturate(0.5f + v.y), m, 1.0f);
}
