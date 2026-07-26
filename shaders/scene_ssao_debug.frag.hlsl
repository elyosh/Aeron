cbuffer SsaoDebugUniforms : register(b0, space3)
{
    float intensity;
    float power;
    float2 _pad;
};

Texture2D<float2> g_visibility : register(t0, space2);
SamplerState g_sampler         : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(VSOut i) : SV_Target0
{
    float ao = g_visibility.Sample(g_sampler, i.uv).r;
    ao = lerp(1.0f, pow(ao, power), intensity);
    return float4(ao, ao, ao, 1.0f);
}
