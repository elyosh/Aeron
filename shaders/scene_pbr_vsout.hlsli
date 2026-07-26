/* Pass-specific vertex-to-fragment interfaces for the glTF mesh path. */

#ifndef SCENE_PBR_VSOUT_HLSLI
#define SCENE_PBR_VSOUT_HLSLI

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
	/* Per-vertex coloured local-light accumulation from
	 * accumulate_local_lights(). FS folds `base_color × local_rgb`
	 * into the diffuse composition. */
	float3 local_rgb : COLOR1;
	/* Flat-interpolated material identity for the FS atlas-sample
	 * resolution. */
	nointerpolation uint  prim_id : COLOR2;
	nointerpolation float base_color_emissive_strength : COLOR3;
	nointerpolation uint  receive_shadow : TEXCOORD5;
	nointerpolation uint  screen_shadow : TEXCOORD6;
	nointerpolation uint  variant_row_base : TEXCOORD7;
	nointerpolation uint  variant_group_count : TEXCOORD8;
	nointerpolation uint  material_count : TEXCOORD9;
};

struct PbrPrepassVSOut {
	float4 position : SV_Position;
	float3 world_pos : TEXCOORD0;
	float3 world_normal : TEXCOORD1;
	float4 clip_curr : TEXCOORD2;
	float4 clip_prev : TEXCOORD3;
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

#endif /* SCENE_PBR_VSOUT_HLSLI */
