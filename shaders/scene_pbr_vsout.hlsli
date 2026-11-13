/* Pass-specific vertex-to-fragment interfaces for the glTF mesh path. */

#ifndef SCENE_PBR_VSOUT_HLSLI
#define SCENE_PBR_VSOUT_HLSLI

/* AERON_PBR_ANIM 1: the material-animation variants, which also carry the
 * vertex's model-frame position and normal (before its articulation, so
 * effects stay on moving components). */
#ifndef AERON_PBR_ANIM
#define AERON_PBR_ANIM 0
#endif

/* The bits of PbrForwardVSOut.shadow_flags: the surface receives the key
 * light's shadow, and it reads that shadow from the screen-space pass. The
 * two share one value because the blended variants never read the second.
 * On its own, the compiler would drop it from their inputs, and Direct3D 12
 * rejects a pipeline whose fragment inputs sit in other registers than the
 * vertex outputs. */
#define PBR_SHADOW_RECEIVE 1u
#define PBR_SHADOW_SCREEN 2u

struct PbrForwardVSOut {
	float4 position : SV_Position;
	float3 world_pos : TEXCOORD1;
	float3 world_normal : NORMAL0;
	float3 world_tangent : TANGENT0;
	/* Bitangent sign — propagates the handedness encoded in
	 * input tangent.w. FS recovers bitangent = cross(N, T) * sign. */
	float  tangent_sign : TEXCOORD2;
	float2 uv : TEXCOORD0;
	float  emissive_mul : COLOR0;
	/* Flat-interpolated material identity for the FS atlas-sample
	 * resolution. */
	nointerpolation uint  prim_id : COLOR2;
	nointerpolation uint  shadow_flags : TEXCOORD5;
	nointerpolation uint  variant_row_base : TEXCOORD6;
	nointerpolation uint  variant_group_count : TEXCOORD7;
	nointerpolation uint  material_count : TEXCOORD8;
#if AERON_PBR_ANIM
	float3 model_pos : TEXCOORD9;
	float3 model_normal : TEXCOORD10;
#endif
};

struct PbrPrepassVSOut {
	float4 position : SV_Position;
	float3 world_pos : TEXCOORD0;
	float3 world_normal : TEXCOORD1;
	float4 clip_curr : TEXCOORD2;
	float4 clip_prev : TEXCOORD3;
	/* Flags the normal G-buffer for the screen shadow pass. */
	nointerpolation uint receiver_local_shadow : TEXCOORD4;
};

struct PbrMaskPrepassVSOut {
	float4 position : SV_Position;
	float3 world_pos : TEXCOORD0;
	float3 world_normal : TEXCOORD1;
	float4 clip_curr : TEXCOORD2;
	float4 clip_prev : TEXCOORD3;
	float2 uv : TEXCOORD4;
	/* x=prim_id, y=variant_row_base, z=variant_group_count,
	 * w=material_count. */
	nointerpolation uint4 material_lookup : TEXCOORD5;
	nointerpolation uint receiver_local_shadow : TEXCOORD6;
};

struct PbrStampVSOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	/* x=prim_id, y=variant_row_base, z=variant_group_count,
	 * w=material_count. */
	nointerpolation uint4 material_lookup : TEXCOORD1;
	float4 clip_curr : TEXCOORD2;
	float4 clip_prev : TEXCOORD3;
};

/* Vertex outputs for the emission_only batches
 * (scene_pbr_emission_instanced.vert and scene_pbr_emission.frag). They
 * hold only what the emission needs. */
struct PbrEmissionVSOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	/* The emissive lane times the scene's emissive scale. */
	float  emissive_mul : COLOR0;
	/* x=prim_id, y=variant_row_base, z=variant_group_count,
	 * w=material_count. */
	nointerpolation uint4 material_lookup : TEXCOORD1;
};

/* PbrEmissionVSOut, plus what the bloom draw needs for its depth test and
 * its light per pixel column (scene_pbr_emission_bloom.frag). */
struct PbrEmissionBloomVSOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float  emissive_mul : COLOR0;
	nointerpolation uint4 material_lookup : TEXCOORD1;
	nointerpolation float4 depth_map : TEXCOORD2;
	/* Where the pixel lies across the instance's width, from -1 to 1. */
	float across : TEXCOORD3;
	/* 1 when the instance is a flat strip, such as a laser bolt. */
	nointerpolation float strip : TEXCOORD4;
};

#endif /* SCENE_PBR_VSOUT_HLSLI */
