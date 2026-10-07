/*
 * Velocity STAMP fragment shader for the screen primitives of an
 * alpha-BLEND range (holograms, transparent screens; instance flag
 * velocity_stamp_screens).
 *
 * Like scene_pbr_prepass_stamp.frag, it writes the instance's motion
 * vectors where the surface is visible, but visibility is the screen's
 * own alpha, as the forward shader computes it: the hologram panel and its
 * content, or a transparent screen's alpha from its luminance. Hidden
 * screens stamp nothing.
 *
 * t0/s0 base-color atlas, t1/s1 and t2/s2 the screen graphics and text;
 * storage t3 materials, t4 variant map, t5 screen rectangles, t6 screen
 * frame.
 */

#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_prepass_common.hlsli"
#include "scene_pbr_vsout.hlsli"
#include "srgb.hlsli"

#define AERON_PBR_MATERIAL_REGISTER t3
#define AERON_PBR_VARIANT_REGISTER t4
#include "scene_pbr_material_alpha.hlsli"

#define AERON_PBR_SCREEN_GRAPHICS_TEXTURE_REGISTER t1
#define AERON_PBR_SCREEN_GRAPHICS_SAMPLER_REGISTER s1
#define AERON_PBR_SCREEN_TEXT_TEXTURE_REGISTER t2
#define AERON_PBR_SCREEN_TEXT_SAMPLER_REGISTER s2
#define AERON_PBR_SCREEN_RECT_REGISTER t5
#define AERON_PBR_SCREEN_FRAME_REGISTER t6
#include "scene_pbr_screen.hlsli"

/* As scene_pbr_prepass_stamp.frag: texels below this alpha leave the
 * velocity of what lies behind. */
static const float STAMP_ALPHA_CUTOFF = 0.5f;

struct FSOut
{
    float2 normal   : SV_Target0; /* write-masked by the pipeline */
    float2 velocity : SV_Target1;
};

/* The straight alpha the forward shader outputs for this screen
 * (scene_pbr_mesh_impl.hlsli). */
float screen_alpha(GltfMaterial m, float2 uv)
{
    ScreenFrame frame = screen_load_frame();
    if ((m.screen_flags & SCREEN_HOLOGRAM) != 0u) {
        return frame.holo_fade <= 0.01f ? 0.0f : screen_hologram(m, uv, frame).a;
    }
    if ((m.screen_flags & SCREEN_TRANSPARENT) != 0u) {
        if ((frame.flags & SCREEN_FRAME_HIDE_TRANSPARENT) != 0u &&
            (m.screen_flags & SCREEN_ALWAYS_VISIBLE) == 0u) {
            return 0.0f;
        }
        float4 albedo_tex = pbr_sample_base_color(m, uv);
        float3 color = screen_evaluate(m, uv, frame);
        if ((m.screen_flags & SCREEN_COVER) != 0u) {
            color = albedo_tex.rgb * m.base_color_factor.rgb + color * (1.0f - albedo_tex.a);
        }
        float luma = dot(AeronLinearToSrgb(saturate(color)), float3(0.333f, 0.333f, 0.333f));
        return saturate(4.0f * luma);
    }
    /* A lit blended screen: its base alpha, as for other blended texels. */
    return pbr_base_alpha(m, uv);
}

FSOut main(PbrStampVSOut i)
{
    GltfMaterial material = pbr_resolve_material(
        i.material_lookup.x, i.material_lookup.y, i.material_lookup.z, i.material_lookup.w);
    pbr_apply_alpha_mask(screen_alpha(material, i.uv), STAMP_ALPHA_CUTOFF);

    FSOut o;
    o.normal = float2(0.0f, 0.0f);
    o.velocity = pbr_prepass_velocity(i.clip_curr, i.clip_prev);
    return o;
}
