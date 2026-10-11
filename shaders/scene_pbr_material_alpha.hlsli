/* Shared PBR material layout, variant lookup, and base-alpha evaluation. */

#ifndef SCENE_PBR_MATERIAL_ALPHA_HLSLI
#define SCENE_PBR_MATERIAL_ALPHA_HLSLI

#ifndef AERON_PBR_MATERIAL_REGISTER
#define AERON_PBR_MATERIAL_REGISTER t7
#endif
#ifndef AERON_PBR_VARIANT_REGISTER
#define AERON_PBR_VARIANT_REGISTER t8
#endif
#ifndef AERON_PBR_BASE_TEXTURE_REGISTER
#define AERON_PBR_BASE_TEXTURE_REGISTER t0
#endif
#ifndef AERON_PBR_BASE_SAMPLER_REGISTER
#define AERON_PBR_BASE_SAMPLER_REGISTER s0
#endif

static const uint GLTF_NO_MATERIAL = 0xFFFFFFFFu;
static const uint GLTF_MATERIAL_HAS_NORMAL = 0x1u;
static const uint GLTF_MATERIAL_HAS_METALLIC_ROUGHNESS = 0x2u;
static const uint GLTF_MATERIAL_HAS_EMISSIVE = 0x4u;
static const uint GLTF_MATERIAL_ALPHA_BLEND = 0x8u;
static const uint GLTF_MATERIAL_LEGACY_EMISSIVE = 0x10u;
static const uint GLTF_MATERIAL_ALPHA_MASK = 0x20u;
static const uint GLTF_MATERIAL_TRANSMISSION = 0x40u;

static const uint GLTF_ATLAS_BASE = 0u;
static const uint GLTF_ATLAS_NORMAL = 1u;
static const uint GLTF_ATLAS_METALLIC_ROUGHNESS = 2u;
static const uint GLTF_ATLAS_EMISSIVE = 3u;

struct GltfMaterial
{
    float4 base_rect;
    float4 normal_rect;
    float4 mr_rect;
    float4 emissive_rect;
    float4 base_color_factor;
    float4 emissive_packed;
    float4 metal_rough; /* x=metallic, y=roughness, z=alpha cutoff, w=transmission */
    float4 specular;    /* KHR_materials_specular: rgb=color factor, a=factor */
    uint4  atlas;       /* per channel: bits 0-11 page, 12-15 max mip level */
    uint   flags;
    uint   screen_first; /* screen rectangles (scene_pbr_screen.hlsli) */
    uint   screen_count;
    uint   screen_flags;
    uint4  anim;         /* x: unused, y: first flipbook record | flipbook
                          * count << 16 (0: none) */
};

StructuredBuffer<GltfMaterial> g_materials
    : register(AERON_PBR_MATERIAL_REGISTER, space2);
StructuredBuffer<uint4> g_prim_to_material
    : register(AERON_PBR_VARIANT_REGISTER, space2);
Texture2DArray g_base_color
    : register(AERON_PBR_BASE_TEXTURE_REGISTER, space2);
SamplerState g_base_sampler
    : register(AERON_PBR_BASE_SAMPLER_REGISTER, space2);

uint pbr_resolve_material_index(uint prim_id, uint variant_row_base,
                                uint variant_group_count)
{
    uint slot = prim_id >> 2u;
    uint comp = prim_id & 3u;
    uint4 packed = slot < variant_group_count
        ? g_prim_to_material[variant_row_base + slot]
        : uint4(GLTF_NO_MATERIAL, GLTF_NO_MATERIAL,
                GLTF_NO_MATERIAL, GLTF_NO_MATERIAL);
    return comp == 0u ? packed.x
         : comp == 1u ? packed.y
         : comp == 2u ? packed.z
                      : packed.w;
}

/* Slot of a primitive's material in g_materials. Meshes store a default
 * material after their own (slot material_count) for primitives without
 * one, so the forward shader reads the fields where it uses them instead of
 * keeping the whole record in registers. */
uint pbr_material_slot(uint prim_id, uint variant_row_base,
                       uint variant_group_count, uint material_count)
{
    uint material_index = pbr_resolve_material_index(
        prim_id, variant_row_base, variant_group_count);
    return material_index < material_count ? material_index : material_count;
}

GltfMaterial pbr_resolve_material(uint prim_id, uint variant_row_base,
                                  uint variant_group_count,
                                  uint material_count)
{
    return g_materials[pbr_material_slot(prim_id, variant_row_base,
                                         variant_group_count, material_count)];
}

/* Samples one channel of the material through its atlas page and LOD
 * limit. */
float4 pbr_sample_channel(Texture2DArray tex, SamplerState s, GltfMaterial material,
                          uint channel, float4 rect, float2 uv)
{
    uint atlas = material.atlas[channel];
    return atlas_sample(tex, s, uv, rect, atlas & 0xFFFu, atlas >> 12);
}

float4 pbr_sample_base_color(GltfMaterial material, float2 uv)
{
    return material.base_rect.z > 0.0f && material.base_rect.w > 0.0f
        ? pbr_sample_channel(g_base_color, g_base_sampler, material,
                             GLTF_ATLAS_BASE, material.base_rect, uv)
        : float4(1, 1, 1, 1);
}

float pbr_base_alpha(GltfMaterial material, float2 uv)
{
    return pbr_sample_base_color(material, uv).a *
           material.base_color_factor.a;
}

void pbr_apply_alpha_mask(float alpha, float cutoff)
{
    clip(alpha - cutoff);
}

#endif /* SCENE_PBR_MATERIAL_ALPHA_HLSLI */
