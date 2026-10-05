/*
 * Scene sky-sun vertex shader (AeronScene_AddSkySun, disc visual).
 *
 * One quad per sun from SV_VertexID, in the plane perpendicular to the
 * sun's direction (gnomonic: an offset of tan(angle) along a tangent axis
 * is that angle on the sky). `tangent_coords` reaches the fragment shader
 * in the same units, so the disc stays circular in angle.
 *
 * Depth: writes pos.z = 0, the reversed-Z far plane, like the sky cube and
 * the SKY billboards; the GREATER_OR_EQUAL test without writes keeps every
 * mesh in front of the sun.
 */

cbuffer SkySunVS : register(b0, space1)
{
    row_major float4x4 view_proj;
    float4 center;   /* xyz = camera position + direction */
    float4 tangent0; /* xyz = first tangent axis, w = half extent (tan) */
    float4 tangent1; /* xyz = second tangent axis */
};

struct VSOut
{
    float4 position       : SV_Position;
    float2 tangent_coords : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    /* Two triangles: (-1,-1) (1,-1) (1,1) / (-1,-1) (1,1) (-1,1). */
    static const float2 corners[6] = {
        float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f),
        float2(-1.0f, -1.0f), float2(1.0f, 1.0f), float2(-1.0f, 1.0f)
    };
    float2 corner = corners[vid] * tangent0.w;
    float3 world  = center.xyz + tangent0.xyz * corner.x + tangent1.xyz * corner.y;

    VSOut o;
    o.position       = mul(view_proj, float4(world, 1.0f));
    o.position.z     = 0.0f;
    o.tangent_coords = corner;
    return o;
}
