/*
 * Fragment shader for the bloom draw of emission_only instances
 * (AeronScene_DrawEmissionBloom). It outputs their emission and alpha as
 * scene_pbr_emission.frag does, for an additive blend into the bloom.
 *
 * The bloom target has no depth buffer, so the shader compares its depth
 * with the scene's depth: a pixel behind the scene (a bolt behind a ship)
 * adds nothing. The scene's depth is reversed: nearer is larger.
 *
 * The target has one sample per pixel, so a strip, such as a laser bolt,
 * lights only the pixels whose centres it covers. A strip that crosses a
 * column of pixels over a height of h pixels covers floor(h) or ceil(h)
 * pixel centres in that column, depending on its position against the
 * pixel grid. As the strip moves by a fraction of a pixel, that count
 * changes and its light jumps, which shows as flicker. Each pixel works
 * out the count from its position across the strip and scales its light
 * by h / count, so the column always gets the light of h pixels. For a
 * strip steeper than 45 degrees, rows are used instead of columns. Other
 * instances, such as warheads, keep an even weight.
 *
 * Bindings: the base colour atlas at sampler slot 0, the emissive atlas at
 * slot 1, the scene's depth at slot 2, and the material and variant
 * storage at slots 0 and 1.
 */

#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_vsout.hlsli"

#define AERON_PBR_MATERIAL_REGISTER t3
#define AERON_PBR_VARIANT_REGISTER t4
#include "scene_pbr_material_alpha.hlsli"

Texture2DArray g_emissive : register(t1, space2);
SamplerState g_em_sampler : register(s1, space2);
Texture2D g_depth : register(t2, space2);
SamplerState g_depth_sampler : register(s2, space2);

/* A bloom pixel may be up to this fraction farther than the scene's depth
 * and still count as visible: an opaque emitter, such as a warhead, is
 * itself in the scene's depth. */
#define EMISSION_BLOOM_DEPTH_TOLERANCE 0.001f

struct FSOut
{
    float4 color : SV_Target0;
};

FSOut main(PbrEmissionBloomVSOut i)
{
    FSOut o;
    const uint mat = pbr_material_slot(i.material_lookup.x, i.material_lookup.y,
                                       i.material_lookup.z, i.material_lookup.w);
    /* Sample and take derivatives before any branch: they are not
     * reliable inside a branch. */
    const float base_alpha = pbr_sample_base_color(g_materials[mat], i.uv).a;
    const float across_dx  = ddx(i.across);
    const float across_dy  = ddy(i.across);
    float3 emissive = g_materials[mat].emissive_packed.rgb;
    if ((g_materials[mat].flags & GLTF_MATERIAL_HAS_EMISSIVE) != 0u) {
        emissive *= pbr_sample_channel(g_emissive, g_em_sampler, g_materials[mat], GLTF_ATLAS_EMISSIVE,
                                       g_materials[mat].emissive_rect, i.uv).rgb;
    }
    const float scene_depth =
        g_depth.SampleLevel(g_depth_sampler, i.position.xy * i.depth_map.xy + i.depth_map.zw, 0.0f).r;
    float alpha = 1.0f;
    if ((g_materials[mat].flags & GLTF_MATERIAL_ALPHA_BLEND) != 0u) {
        alpha = saturate(base_alpha * g_materials[mat].base_color_factor.a);
    }
    if (i.position.z < scene_depth * (1.0f - EMISSION_BLOOM_DEPTH_TOLERANCE)) {
        alpha = 0.0f;
    }
    /* The light of a strip per pixel column (see above). `across` changes
     * by `rate` per pixel along the column, so the strip covers 2 / |rate|
     * pixels of it, centred where `across` is 0. */
    float column = 1.0f;
    const bool  along_column = abs(across_dy) >= abs(across_dx);
    const float rate         = along_column ? across_dy : across_dx;
    if (i.strip != 0.0f && abs(rate) > 1e-6f) {
        const float pixel  = along_column ? i.position.y : i.position.x;
        const float height = 2.0f / abs(rate);
        const float centre = pixel - i.across / rate;
        /* Pixel centres lie at whole numbers + 0.5. */
        const float count  = ceil(centre + 0.5f * height - 0.5f) - floor(centre - 0.5f * height - 0.5f) - 1.0f;
        column             = height / max(count, 1.0f);
    }
    o.color = float4(emissive * (g_materials[mat].emissive_packed.a * i.emissive_mul * column), alpha);
    return o;
}
