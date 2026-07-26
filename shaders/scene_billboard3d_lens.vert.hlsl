/*
 * LENS-stage billboard vertex shader: the color-pipeline VS plus the
 * flare SOURCE anchor projected to depth-buffer sampling coordinates
 * (uv + reference NDC depth), passed to the lens fragment shader
 * which performs the occlusion kernel (fragment-stage depth sampling
 * — the backend-proven path; SSAO uses the same one). An anchor
 * behind the camera or outside the frame is marked invalid with a
 * negative reference depth: the classic queue requires an on-screen
 * source, so the FS fades those to zero.
 */

cbuffer LensBillboardVS : register(b0, space1)
{
    row_major float4x4 view_proj;
    float4 anchor_world; /* xyz = flare source, world space */
};

struct VSIn
{
    float3 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float  bias  : TEXCOORD1;   /* unused: no depth attachment in the lens pass */
    float3 prev  : TEXCOORD2;   /* unused by the color pipeline */
};

struct VSOut
{
    float4 pos    : SV_Position;
    float2 uv     : TEXCOORD0;
    float4 color  : COLOR0;
    float3 anchor : TEXCOORD1;  /* xy = depth uv, z = ref depth (< 0: invalid) */
};

VSOut main(VSIn i)
{
    VSOut o;
    o.pos    = mul(view_proj, float4(i.pos, 1.0f));
    o.uv     = i.uv;
    o.color  = i.color;
    o.anchor = float3(0.0f, 0.0f, -1.0f);

    float4 ac = mul(view_proj, float4(anchor_world.xyz, 1.0f));
    if (ac.w > 0.0001f)
    {
        float2 ndc = ac.xy / ac.w;
        if (abs(ndc.x) <= 1.0f && abs(ndc.y) <= 1.0f)
        {
            o.anchor = float3(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f, ac.z / ac.w);
        }
    }
    return o;
}
