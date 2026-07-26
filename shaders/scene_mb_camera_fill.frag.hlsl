/*
 * Camera-velocity sky fill. Fullscreen pass run FIRST in the
 * motion-blur prepass, before geometry. It seeds velocity_rt with the
 * far-plane (infinitely distant) rotational motion vector so the
 * starfield / skybox streaks as the camera turns — sky pixels carry no
 * geometry and would otherwise stay sharp. Geometry then overwrites the
 * pixels it covers with their own (richer) velocity.
 *
 * Per pixel: reconstruct the eye-space view ray from the projection
 * scalars (matching the SSAO / skybox unprojection), rotate it to world
 * via the current inverse view rotation, reproject that world DIRECTION
 * (w = 0, so camera translation is ignored — only rotation matters)
 * through the previous frame's view_proj, and emit (ndc_now − ndc_prev)
 * in UV space.
 *
 * Writes velocity (SV_Target1) only; the normal target (SV_Target0) is
 * masked off by the pipeline and depth is neither tested nor written.
 */

cbuffer MbCameraFillUniforms : register(b0, space3)
{
    float tan_h_half;
    float tan_v_half;
    float proj_y_offset;
    float proj_x_offset;    /* m[2] — off-center NDC X bias */
    /* Current eye→world rotation, rows padded to vec4. */
    float4 inv_view_rot[3];
    row_major float4x4 prev_view_proj;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

struct FSOut
{
    float2 normal   : SV_Target0;   /* masked off by the pipeline */
    float2 velocity : SV_Target1;
};

FSOut main(VSOut i)
{
    FSOut o;
    o.normal = float2(0.0f, 0.0f);

    /* This pixel's current NDC. */
    float ndc_x = i.uv.x * 2.0f - 1.0f;
    float ndc_y = 1.0f - i.uv.y * 2.0f;

    /* Eye-space ray for this pixel (same unprojection as the SSAO /
     * skybox shaders), then rotate to world. */
    float3 view_dir = float3((ndc_x - proj_x_offset) * tan_h_half,
                             -(ndc_y - proj_y_offset) * tan_v_half,
                             1.0f);
    float3 world_dir;
    world_dir.x = dot(inv_view_rot[0].xyz, view_dir);
    world_dir.y = dot(inv_view_rot[1].xyz, view_dir);
    world_dir.z = dot(inv_view_rot[2].xyz, view_dir);

    /* Reproject the infinitely distant point (direction, w = 0) through
     * the previous frame's view_proj. */
    float4 clip_prev = mul(prev_view_proj, float4(world_dir, 0.0f));
    float2 ndc_prev  = (abs(clip_prev.w) > 1e-6f)
                         ? clip_prev.xy / clip_prev.w
                         : float2(ndc_x, ndc_y);   /* behind cam → no motion */

    float2 ndc_now = float2(ndc_x, ndc_y);
    o.velocity = (ndc_now - ndc_prev) * float2(0.5f, -0.5f);
    return o;
}
