/*
 * Shared mesh vertex input, uniforms, articulation, and projection helpers.
 * Pass-specific vertex shaders build their own narrow fragment interfaces on
 * top of these calculations.
 */

#ifndef SCENE_PBR_VERTEX_COMMON_HLSLI
#define SCENE_PBR_VERTEX_COMMON_HLSLI

/* AERON_PBR_INSTANCED 1: the variants that draw a batch of instances of one
 * mesh in one draw. Each instance's values come from its record instead of
 * the uniforms. */
#ifndef AERON_PBR_INSTANCED
#define AERON_PBR_INSTANCED 0
#endif

#if AERON_PBR_INSTANCED
/* Mirrors AeronScenePbrBatchVsBlock. */
cbuffer PbrBatchVSUniforms : register(b0, space1) {
	row_major float4x4 view_proj;
	row_major float4x4 unjittered_view_proj;
	/* The previous camera, for the instances that move. */
	row_major float4x4 moving_prev_view_proj;
	uint variant_group_count;
	uint material_count;
	uint camera_mb;
	/* The batch's first record. SV_InstanceID starts at 0 on every backend. */
	uint instance_base;
};

/* Mirrors AeronScenePbrInstanceRecord. The previous transform and table are
 * the current ones for an instance without velocity. */
struct PbrInstanceRecord {
	float4 model_to_world[4];
	float4 prev_model_to_world[4];
	uint   variant_row_base;
	uint   current_table_base;
	uint   previous_table_base;
	uint   table_component_count;
	float  base_color_emissive_strength;
	uint   receive_shadow;
	uint   zero_velocity;
	uint   hidden_tag_groups;
};

StructuredBuffer<PbrInstanceRecord> instance_records : register(t1, space0);

/* The instance's values, which pbr_load_instance sets. */
static float4x4 craft_to_world;
static float4x4 prev_craft_to_world;
static float4x4 prev_view_proj;
static uint     variant_row_base;
static float    base_color_emissive_strength;
static uint     receive_shadow;
static uint     current_table_base;
static uint     previous_table_base;
static uint     table_component_count;
static uint     hidden_tag_groups;
/* Batches are blended, so they don't take the screen shadow pass or the
 * receiver-local map. */
static const uint screen_shadow         = 0u;
static const uint receiver_local_shadow = 0u;

/* The record is read field by field. Through shadercross, a copy of the whole
 * struct loads its arrays wrongly in DXIL: each matrix element comes from the
 * wrong row. */
void pbr_load_instance(uint instance_id)
{
	const uint index = instance_base + instance_id;
	craft_to_world = float4x4(instance_records[index].model_to_world[0],
							  instance_records[index].model_to_world[1],
							  instance_records[index].model_to_world[2],
							  instance_records[index].model_to_world[3]);
	prev_craft_to_world = float4x4(instance_records[index].prev_model_to_world[0],
								   instance_records[index].prev_model_to_world[1],
								   instance_records[index].prev_model_to_world[2],
								   instance_records[index].prev_model_to_world[3]);
	prev_view_proj = instance_records[index].zero_velocity != 0u ? unjittered_view_proj : moving_prev_view_proj;
	variant_row_base = instance_records[index].variant_row_base;
	base_color_emissive_strength = instance_records[index].base_color_emissive_strength;
	receive_shadow = instance_records[index].receive_shadow;
	current_table_base = instance_records[index].current_table_base;
	previous_table_base = instance_records[index].previous_table_base;
	table_component_count = instance_records[index].table_component_count;
	hidden_tag_groups = instance_records[index].hidden_tag_groups;
}
#else
cbuffer GltfMeshVSUniforms : register(b0, space1) {
	row_major float4x4 view_proj;
	/* Current camera without the FSR sub-pixel jitter. Rasterization uses
	 * view_proj; temporal velocity uses this matrix so motion vectors do not
	 * contain the jitter sequence. */
	row_major float4x4 unjittered_view_proj;
	row_major float4x4 craft_to_world;
	/* Material-variant row and bounds forwarded by passes that resolve an
	 * instance's primitive material. */
	uint variant_row_base;
	uint variant_group_count;
	uint material_count;
	/* Selects camera + object velocity or object-only velocity. */
	uint camera_mb;
	/* Forward-only material and shadow controls. */
	float base_color_emissive_strength;
	uint  receive_shadow;
	uint  screen_shadow;
	/* Current/previous articulation tables (float4 bases into mesh_tables,
	 * both holding at least table_component_count components). */
	uint current_table_base;
	uint previous_table_base;
	/* Receives shadows from the receiver-local map (the cockpit). */
	uint receiver_local_shadow;
	/* The mesh's tag groups this instance hides. */
	uint hidden_tag_groups;
	uint table_component_count;
	/* Previous transforms used by the velocity-producing vertex variants. */
	row_major float4x4 prev_view_proj;
	row_major float4x4 prev_craft_to_world;
};
#endif

/* One variable-length record per unique submitted mesh table, packed at
 * its component count; layout in mesh_table_layout.hlsli. */
#include "mesh_table_layout.hlsli"
StructuredBuffer<float4> mesh_tables : register(t0, space0);

/* The tag groups of each primitive's material, laid out as the fragment
 * shaders' g_prim_to_material: per variant row, one uint4 per four
 * primitives. */
#if AERON_PBR_INSTANCED
StructuredBuffer<uint4> prim_tag_groups : register(t2, space0);
#else
StructuredBuffer<uint4> prim_tag_groups : register(t1, space0);
#endif

/* Whether the instance hides the material of primitive `prim_id`. An
 * instance that hides nothing doesn't read the buffer. */
bool pbr_surface_hidden(uint prim_id)
{
	uint slot = prim_id >> 2u;
	if (hidden_tag_groups == 0u || slot >= variant_group_count) {
		return false;
	}
	uint4 packed = prim_tag_groups[variant_row_base + slot];
	uint comp = prim_id & 3u;
	uint groups = comp == 0u ? packed.x : comp == 1u ? packed.y : comp == 2u ? packed.z : packed.w;
	return (groups & hidden_tag_groups) != 0u;
}

/* Row r of a component's 3x4 affine. */
float4 mesh_table_row(uint table_base, uint component, uint r)
{
	return mesh_tables[table_base + component * 3u + r];
}

/* One component's value in a packed lane of the current table. */
float mesh_table_lane(uint lane, uint component)
{
	uint lane_vec4 = AERON_MESH_LANE_VEC4(table_component_count);
	return mesh_tables[current_table_base + 3u * table_component_count + lane * lane_vec4 +
					   (component >> 2u)][component & 3u];
}

struct VSIn {
	/* AeronGltfVertex: position/normal/tangent are mesh-local; component
	 * selects one of the instance's table_component_count articulated
	 * components. */
	float3 position : POSITION;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	float2 uv : TEXCOORD0;
	float  component : COLOR0;
	uint   prim_id : COLOR1;
#if AERON_PBR_INSTANCED
	uint instance_id : SV_InstanceID;
#endif
};

struct PbrCurrentVertex {
	uint component;
	float4 row0;
	float4 row1;
	float4 row2;
	float3 rotated_local;
	float4 world_position;
	float4 raster_position;
};

/* Length-guarded normalize for degenerate authored normals and tangents. */
float3 pbr_safe_normalize(float3 value, float3 fallback)
{
	float length_squared = dot(value, value);
	return length_squared > 1e-12f ? value * rsqrt(length_squared) : fallback;
}

/*
 * dot(a, b) as a fixed chain of fused multiply-adds. Every term is an
 * explicit mad, so no backend can contract or reorder it differently
 * between shaders. `precise` would also do that, but SPIRV-Cross emits it as
 * unoptimizable [[clang::optnone]] helpers that tripled the vertex cost on
 * Metal.
 */
float pbr_dot4_fma(float4 a, float4 b)
{
	return mad(a.w, b.w, mad(a.z, b.z, mad(a.y, b.y, mad(a.x, b.x, 0.0f))));
}

/* mul(m, v) for a row-major matrix, through pbr_dot4_fma. */
float4 pbr_mul_fma(float4x4 m, float4 v)
{
	return float4(pbr_dot4_fma(m[0], v), pbr_dot4_fma(m[1], v),
				  pbr_dot4_fma(m[2], v), pbr_dot4_fma(m[3], v));
}

/*
 * Computes the current articulated world and raster positions. Forward,
 * prepass and back-face vertex shaders call this exact function; its
 * explicit mad chains give them bit-identical depth.
 */
bool pbr_build_current_vertex(VSIn input, out PbrCurrentVertex current)
{
#if AERON_PBR_INSTANCED
	/* Every vertex shader calls this first. */
	pbr_load_instance(input.instance_id);
#endif
	uint component = (uint)clamp((int)round(input.component), 0,
								  (int)table_component_count - 1);
	float visibility = mesh_table_lane(AERON_MESH_LANE_VISIBILITY, component);

	current.component = component;
	current.row0 = float4(0.0f, 0.0f, 0.0f, 0.0f);
	current.row1 = float4(0.0f, 0.0f, 0.0f, 0.0f);
	current.row2 = float4(0.0f, 0.0f, 0.0f, 0.0f);
	current.rotated_local = float3(0.0f, 0.0f, 0.0f);
	current.world_position = float4(0.0f, 0.0f, 0.0f, 1.0f);
	current.raster_position = float4(0.0f, 0.0f, 0.0f, -1.0f);
	if (visibility < 0.5f || pbr_surface_hidden(input.prim_id)) {
		return false;
	}

	current.row0 = mesh_table_row(current_table_base, component, 0u);
	current.row1 = mesh_table_row(current_table_base, component, 1u);
	current.row2 = mesh_table_row(current_table_base, component, 2u);

	float4 local_position = float4(input.position, 1.0f);
	precise float3 rotated_local = float3(pbr_dot4_fma(current.row0, local_position),
										  pbr_dot4_fma(current.row1, local_position),
										  pbr_dot4_fma(current.row2, local_position));
	precise float4 world_position = pbr_mul_fma(craft_to_world, float4(rotated_local, 1.0f));
	precise float4 raster_position = pbr_mul_fma(view_proj, world_position);
	current.rotated_local = rotated_local;
	current.world_position = world_position;
	current.raster_position = raster_position;
	return true;
}

/* Builds unjittered current/previous clip positions for motion velocity. */
void pbr_build_velocity(VSIn input, PbrCurrentVertex current,
						out float4 clip_curr, out float4 clip_prev)
{
	uint component = current.component;
	float4 row0 = mesh_table_row(previous_table_base, component, 0u);
	float4 row1 = mesh_table_row(previous_table_base, component, 1u);
	float4 row2 = mesh_table_row(previous_table_base, component, 2u);
	float3 previous_local;
	previous_local.x = dot(row0.xyz, input.position) + row0.w;
	previous_local.y = dot(row1.xyz, input.position) + row1.w;
	previous_local.z = dot(row2.xyz, input.position) + row2.w;

	clip_curr = camera_mb != 0u
		? mul(unjittered_view_proj, current.world_position)
		: mul(prev_view_proj, current.world_position);
	clip_prev = mul(prev_view_proj,
					mul(prev_craft_to_world, float4(previous_local, 1.0f)));
}

#endif /* SCENE_PBR_VERTEX_COMMON_HLSLI */
