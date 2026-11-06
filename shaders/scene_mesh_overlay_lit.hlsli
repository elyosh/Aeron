/*
 * Lit mesh overlays (AeronSceneMeshOverlayDesc.lit): the overlay's image
 * times its colour is a base colour, shaded as the mesh shader shades an
 * opaque surface without probe sets: the key light with its shadow, the
 * other directional lights, the world ambient and the diffuse environment
 * with the ambient occlusion, and the point lights. The surface is a rough
 * dielectric. AERON_OVERLAY_ARRAY 1 reads the image from a 2D array
 * texture, as the unlit array overlay does.
 *
 * Fragment resource map:
 *   t0/s0 — the overlay's image
 *   t1/s1 — the visibility pass's output (R = SSAO, G = the key light's
 *           shadow), or a white texel
 *   t2/s2 — diffuse environment cube
 *   t3/s3 — comparison shadow depth
 *   t4/s4 — raw shadow depth
 *   t5    — point lights
 *   t6    — cluster depth bins and tile masks
 *   b0    — DirectionalShadowFS
 *   b1    — PbrLightFS
 *   b2    — scene lighting block
 *   b3    — OverlayLitFS
 */

#ifndef AERON_OVERLAY_ARRAY
#define AERON_OVERLAY_ARRAY 0
#endif

#define AERON_DIRECTIONAL_SHADOW_UNIFORM_REGISTER b0
#define AERON_PBR_LIGHT_UNIFORM_REGISTER b1
#define AERON_SHADOW_COMPARISON_TEXTURE_REGISTER t3
#define AERON_SHADOW_COMPARISON_SAMPLER_REGISTER s3
#define AERON_SHADOW_DEPTH_TEXTURE_REGISTER t4
#define AERON_SHADOW_DEPTH_SAMPLER_REGISTER s4
#define AERON_PBR_POINT_LIGHT_REGISTER t5
#define AERON_CLUSTER_DATA_REGISTER t6
#include "scene_pbr_lighting.hlsli"

#if AERON_OVERLAY_ARRAY
Texture2DArray<float4> g_texture : register(t0, space2);
#else
Texture2D<float4> g_texture : register(t0, space2);
#endif
SamplerState        s_texture     : register(s0, space2);
Texture2D<float4>   g_visibility  : register(t1, space2);
SamplerState        s_visibility  : register(s1, space2);
TextureCube<float4> g_environment : register(t2, space2);
SamplerState        s_environment : register(s2, space2);

cbuffer OverlayLitFS : register(b3, space3) {
	float4 uv_xform;
	float4 uv_rect;
	float4 color;
	float4 layer;  /* array textures: layer, max mip level, clamp outside uv_rect */
	float4 shading; /* x: 1 when the visibility pass holds the key light's shadow,
	                 * y: 1 when the image is premultiplied */
};

struct VSOut {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	float3 world_pos : TEXCOORD1;
	float3 world_normal : TEXCOORD2;
};

/* The perceptual roughness of the overlay's surface. */
#define AERON_OVERLAY_ROUGHNESS 0.6f

float3 overlay_environment(float3 world_direction)
{
	if (fs_environment_params.x <= 0.0f) return 0.0f;
	float3 local = float3(dot(world_direction, fs_environment_basis[0].xyz),
						  dot(world_direction, fs_environment_basis[1].xyz),
						  dot(world_direction, fs_environment_basis[2].xyz));
	return max(g_environment.SampleLevel(s_environment, normalize(local), 0.0f).rgb, 0.0f) *
		   fs_environment_params.x;
}

float4 main(VSOut input, bool is_front : SV_IsFrontFace) : SV_Target {
	/* Gradients before the discards, which make the rest non-uniform. */
	float3 world_pos_dx = ddx(input.world_pos);
	float3 world_pos_dy = ddy(input.world_pos);
	float2 atlas_uv     = input.uv * uv_xform.zw + uv_xform.xy;
#if AERON_OVERLAY_ARRAY
	float2 dx = ddx(atlas_uv);
	float2 dy = ddy(atlas_uv);
	if (layer.z == 0.0f && (any(atlas_uv < uv_rect.xy) || any(atlas_uv > uv_rect.zw))) {
		discard;
	}
	uint width, height, layers, levels;
	g_texture.GetDimensions(0, width, height, layers, levels);
	float2 size   = float2(width, height);
	float2 tdx    = dx * size;
	float2 tdy    = dy * size;
	float  lod    = 0.5f * log2(max(max(dot(tdx, tdx), dot(tdy, tdy)), 1e-20f));
	float  shrink = exp2(min(layer.y - lod, 0.0f));
	atlas_uv      = clamp(atlas_uv, uv_rect.xy, uv_rect.zw);
	float4 image  = g_texture.SampleGrad(s_texture, float3(atlas_uv, layer.x), dx * shrink, dy * shrink) * color;
#else
	float4 image = g_texture.Sample(s_texture, clamp(atlas_uv, uv_rect.xy, uv_rect.zw)) * color;
	if (any(atlas_uv < uv_rect.xy) || any(atlas_uv > uv_rect.zw)) {
		discard;
	}
#endif
	if (image.a < 0.01f) {
		discard;
	}
	bool   premultiplied = shading.y != 0.0f;
	float3 albedo        = premultiplied ? image.rgb / image.a : image.rgb;

	/* Normals as the mesh shader takes them. */
	float3 N          = normalize(input.world_normal) * (is_front ? 1.0f : -1.0f);
	float3 N_face     = N;
	float3 face_cross = cross(world_pos_dx, world_pos_dy);
	float  face_sq    = dot(face_cross, face_cross);
	if (face_sq > 1.0e-12f) {
		N_face = face_cross * rsqrt(face_sq);
		N_face = dot(N_face, N) < 0.0f ? -N_face : N_face;
	}
	float3 V      = normalize(fs_camera_pos_world - input.world_pos);
	float3 L      = fs_directional_dir;
	float3 N_spec = normalize(lerp(N_face, N, smoothstep(0.0f, 0.2f, dot(N, V))));

	/* The opaque surface under the overlay has the same pixel in the
	 * visibility pass. */
	bool   matched;
	float4 visibility = read_screen_visibility(g_visibility, input.pos.xy, input.pos.z, matched);
	float  ao         = ssao_intensity > 0.0f ? lerp(1.0f, pow(visibility.x, ssao_power), ssao_intensity) : 1.0f;
	float  shadow     = visibility.y;
	if (shading.x == 0.0f || !matched) {
		uint  cascade;
		float cascade_blend, coverage;
		shadow = directional_shadow_visibility(input.world_pos, N_face, max(dot(N, L), dot(N_face, L)),
											   world_pos_dx, world_pos_dy, input.pos.xy, true, cascade,
											   cascade_blend, coverage);
	}

	const float inv_pi    = 0.31830989f;
	float       ao_direct = lerp(1.0f, ao, ssao_direct);
	float3      extra_rgb = float3(0.0f, 0.0f, 0.0f);
	[unroll] for (int ed = 0; ed < 3; ed++) {
		extra_rgb += saturate(dot(N, fs_extra_dir[ed].xyz)) * inv_pi * fs_extra_col[ed].rgb;
	}
	float3 ao_a       = 2.0404f * albedo - 0.3324f;
	float3 ao_b       = -4.7951f * albedo + 0.6417f;
	float3 ao_c       = 2.7552f * albedo + 0.6903f;
	float3 ao_ambient = max(ao, ((ao * ao_a + ao_b) * ao + ao_c) * ao);
	float3 diffuse    = (sun_color * saturate(dot(N, L)) * inv_pi * shadow + extra_rgb) * ao_direct +
					 (world_ambient(N) + apply_ambient_minimum(overlay_environment(N))) * ao_ambient;

	PbrMaterialParams mp;
	mp.spec_intensity = 1.0f;
	mp.roughness      = AERON_OVERLAY_ROUGHNESS;
	mp.F0             = float3(0.04f, 0.04f, 0.04f);
	mp.F90            = 1.0f;
	mp.albedo         = albedo;
	float  G_term;
	float3 specular = cook_torrance_spec(N_spec, V, L, mp, G_term) * sun_color * shadow;
	if (fs_cluster_point_count > 0u) {
		diffuse += inv_pi * accumulate_point_lights(N, N_spec, V, input.world_pos, input.pos.xy, mp, specular);
	}
	float3 lit = albedo * diffuse + specular;
	return float4(premultiplied ? lit * image.a : lit, image.a);
}
