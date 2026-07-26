/*
 * Scene sky-cube vertex shader (AeronScene_SetSkyCube).
 *
 * Draws a fullscreen triangle from SV_VertexID; the rasterizer
 * interpolates a cube-space ray direction the fragment shader uses to
 * sample the sky cube map.
 *
 * Unprojection identity — must mirror the scene's mesh projection
 * (scene_mat4_perspective_reverse_z_xy) so the sky's angular extent
 * and horizon line up with rendered meshes:
 *
 *   ndc.x =  eye.x / eye.z × (1/tan_h_half) + proj_x_offset
 *   ndc.y = -eye.y / eye.z × (1/tan_v_half) + proj_y_offset
 *
 * Solved at eye.z = 1 (sky sits at infinity, no z-divide):
 *
 *   eye.x =  (ndc.x - proj_x_offset) × tan_h_half
 *   eye.y = -(ndc.y - proj_y_offset) × tan_v_half
 *   eye.z =  1
 *
 * eye_to_cube is pre-multiplied host-side from the scene camera's
 * eye→world rotation and the game's world→cube basis, so cube maps can
 * be authored in the standard +X right / +Y up / -Z forward convention
 * while each game keeps its own world-axis convention.
 *
 * Depth: writes pos.z = 0 = reverse-Z far plane. Pipeline depth state
 * is GREATER_OR_EQUAL with depth-write OFF, so the sky sits at the
 * cleared 0.0 and every mesh fragment (ndc.z > 0) wins over it.
 */

cbuffer SkyCubeVS : register(b0, space1)
{
    row_major float3x3 eye_to_cube;
    /* x = tan_h_half, y = tan_v_half, z = proj_x_offset, w = proj_y_offset */
    float4             unproj;
};

struct VSOut
{
    float4 position  : SV_Position;
    float3 direction : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    /* Fullscreen triangle covering [-1, 1]² (one vertex off-screen). */
    float2 uv  = float2((vid << 1) & 2, vid & 2);
    float2 ndc = uv * 2.0f - 1.0f;

    float3 eye_ray;
    eye_ray.x =  (ndc.x - unproj.z) * unproj.x;
    eye_ray.y = -(ndc.y - unproj.w) * unproj.y;
    eye_ray.z =  1.0f;

    VSOut o;
    o.position  = float4(ndc, 0.0f, 1.0f);
    o.direction = mul(eye_to_cube, eye_ray);
    return o;
}
