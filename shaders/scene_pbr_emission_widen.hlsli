/*
 * The minimum width (AeronScene_SetEmissionMinWidth) and the fade
 * (AeronScene_SetEmissionFade) of emission_only instances, shared by their
 * colour and velocity-stamp vertex shaders. Include it after
 * scene_pbr_vertex_common.hlsli with AERON_PBR_INSTANCED set.
 */

#ifndef SCENE_PBR_EMISSION_WIDEN_HLSLI
#define SCENE_PBR_EMISSION_WIDEN_HLSLI

/* Mirrors AeronScenePbrEmissionVsBlock. The emissive scale is passed here
 * instead of in the fragment shader's cluster uniform. That uniform is at
 * slot 2, and on Metal a fragment shader that declares only that uniform
 * gets its storage buffers at the wrong indices. */
cbuffer PbrEmissionVSUniforms : register(b1, space1) {
	/* The mesh's bounds centre and half its width, in the mesh's frame. */
	float3 emission_center;
	float  emission_half_width;
	/* The unit axis of that width: the bounds' second-longest axis. */
	float3 emission_width_axis;
	/* In render pixels; 0 is off. */
	float  emission_min_width_px;
	float2 emission_render_size;
	/* The widest the strip gets, as a multiple of its real width. */
	float  emission_max_widen;
	/* AeronScene_EmissiveScale; the other shaders read it as fs_emission.x. */
	float  emissive_scale;
	/* In render pixels; 0 is off. */
	float  emission_fade_width_px;
	float  emission_fade_power;
	float2 _emission_pad;
};

/* Widens the instance to the minimum width when it's thinner on screen, and
 * returns the factor of its emission: its real width / drawn width, times
 * the fade. The width is measured at the bounds' centre, so every vertex of
 * an instance gets the same factor. It uses the camera without the FSR
 * jitter, so the jitter doesn't change it. Call it before
 * pbr_build_current_vertex, which then places the widened position, as
 * pbr_build_velocity does. */
float pbr_emission_widen(inout VSIn input)
{
	pbr_load_instance(input.instance_id);
	if (emission_min_width_px <= 0.0f && emission_fade_width_px <= 0.0f)
		return 1.0f;
	const float4 center = mul(craft_to_world, float4(emission_center, 1.0f));
	const float3 half_width =
		mul((float3x3)craft_to_world, emission_width_axis * emission_half_width);
	const float4 clip_center = mul(unjittered_view_proj, center);
	const float4 clip_edge   = mul(unjittered_view_proj, float4(center.xyz + half_width, 1.0f));
	if (clip_center.w <= 1e-4f || clip_edge.w <= 1e-4f)
		return 1.0f;
	const float2 half_px = (clip_edge.xy / clip_edge.w - clip_center.xy / clip_center.w) * 0.5f *
	                       emission_render_size;
	const float width_px = 2.0f * length(half_px);
	/* The fade dims the instance as it gets thinner; there is no sudden cut. */
	const float fade = emission_fade_width_px > 0.0f
	                       ? pow(saturate(width_px / emission_fade_width_px), emission_fade_power)
	                       : 1.0f;
	if (emission_min_width_px <= 0.0f)
		return fade;
	const float widen = clamp(width_px / emission_min_width_px, 1.0f / emission_max_widen, 1.0f);
	if (widen < 1.0f) {
		const float offset = dot(input.position - emission_center, emission_width_axis);
		input.position += emission_width_axis * (offset * (1.0f / widen - 1.0f));
	}
	return widen * fade;
}

#endif /* SCENE_PBR_EMISSION_WIDEN_HLSLI */
