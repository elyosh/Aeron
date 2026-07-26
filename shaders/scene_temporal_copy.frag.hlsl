Texture2D<float4> g_source : register(t0, space2);
SamplerState      g_sampler : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(VSOut input) : SV_Target
{
    return float4(g_source.SampleLevel(g_sampler, input.uv, 0.0f).rgb, 1.0f);
}
