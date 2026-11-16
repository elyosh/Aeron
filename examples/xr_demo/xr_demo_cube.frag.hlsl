/* xr_demo: a cube face lit by one fixed light. The output is linear; the
 * target's sRGB format encodes it. */

struct VSOut
{
    float4 position : SV_Position;
    float3 normal   : TEXCOORD0;
    float3 color    : TEXCOORD1;
};

float4 main(VSOut input) : SV_Target
{
    const float3 to_light = normalize(float3(0.4, 0.8, 0.45));
    float        diffuse  = saturate(dot(normalize(input.normal), to_light));
    return float4(input.color * (0.15 + 0.85 * diffuse), 1.0);
}
