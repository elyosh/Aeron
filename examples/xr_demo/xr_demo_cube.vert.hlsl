/* xr_demo: one cube. The C side multiplies the view's matrix by the cube's
 * matrix and sends the rows. */

struct CubeUniforms
{
    float4 clip_rows[4]; /* object space to clip space */
    float4 rotation[3];  /* object space to rig space, for the normal */
    float4 color;
};

cbuffer Cube : register(b0, space1)
{
    CubeUniforms u;
};

struct VSIn
{
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
};

struct VSOut
{
    float4 position : SV_Position;
    float3 normal   : TEXCOORD0;
    float3 color    : TEXCOORD1;
};

VSOut main(VSIn input)
{
    float4 p = float4(input.position, 1.0);
    VSOut  o;
    o.position = float4(dot(u.clip_rows[0], p), dot(u.clip_rows[1], p),
                        dot(u.clip_rows[2], p), dot(u.clip_rows[3], p));
    o.normal = float3(dot(u.rotation[0].xyz, input.normal), dot(u.rotation[1].xyz, input.normal),
                      dot(u.rotation[2].xyz, input.normal));
    o.color = u.color.rgb;
    return o;
}
