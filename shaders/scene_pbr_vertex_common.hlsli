/*
 * Shared mesh vertex input, uniforms, articulation, and projection helpers.
 * Pass-specific vertex shaders build their own narrow fragment interfaces on
 * top of these calculations.
 */

#ifndef SCENE_PBR_VERTEX_COMMON_HLSLI
#define SCENE_PBR_VERTEX_COMMON_HLSLI

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
	/* Current/previous articulation tables and the forward local-light slice. */
	uint current_table_index;
	uint previous_table_index;
	uint local_light_base;
	uint local_light_count;
	uint _pad_storage;
	/* Previous transforms used by the velocity-producing vertex variants. */
	row_major float4x4 prev_view_proj;
	row_major float4x4 prev_craft_to_world;
};

/* Flat stream of 160 float4 records per unique submitted mesh table:
 * 120 articulation rows, then 10 lanes each for visibility, highlight,
 * markings, and emissive. */
StructuredBuffer<float4> mesh_tables : register(t0, space0);
static const uint MESH_TABLE_STRIDE = 160u;
static const uint MESH_VISIBILITY_OFFSET = 120u;
static const uint MESH_EMISSIVE_OFFSET = 150u;

float4 mesh_table_load(uint table_index, uint offset)
{
	return mesh_tables[table_index * MESH_TABLE_STRIDE + offset];
}

struct VSIn {
	/* AeronGltfVertex: position/normal/tangent are mesh-local; mesh_index
	 * selects one of the instance's 40 articulated mesh slots. */
	float3 position : POSITION;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	float2 uv : TEXCOORD0;
	float  mesh_index : COLOR0;
	uint   prim_id : COLOR1;
};

struct PbrCurrentVertex {
	uint mesh_index;
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
 * Computes the current articulated world and raster positions. Forward and
 * prepass vertex shaders call this exact function; precise keeps the
 * depth-producing operation chain from being reassociated between variants.
 */
bool pbr_build_current_vertex(VSIn input, out PbrCurrentVertex current)
{
	uint mesh_index = (uint)clamp((int)round(input.mesh_index), 0, 39);
	float visibility =
		mesh_table_load(current_table_index,
						MESH_VISIBILITY_OFFSET + (mesh_index >> 2u))[mesh_index & 3u];

	current.mesh_index = mesh_index;
	current.row0 = float4(0.0f, 0.0f, 0.0f, 0.0f);
	current.row1 = float4(0.0f, 0.0f, 0.0f, 0.0f);
	current.row2 = float4(0.0f, 0.0f, 0.0f, 0.0f);
	current.rotated_local = float3(0.0f, 0.0f, 0.0f);
	current.world_position = float4(0.0f, 0.0f, 0.0f, 1.0f);
	current.raster_position = float4(0.0f, 0.0f, 0.0f, -1.0f);
	if (visibility < 0.5f) {
		return false;
	}

	current.row0 = mesh_table_load(current_table_index, mesh_index * 3u + 0u);
	current.row1 = mesh_table_load(current_table_index, mesh_index * 3u + 1u);
	current.row2 = mesh_table_load(current_table_index, mesh_index * 3u + 2u);

	current.rotated_local.x = dot(current.row0.xyz, input.position) + current.row0.w;
	current.rotated_local.y = dot(current.row1.xyz, input.position) + current.row1.w;
	current.rotated_local.z = dot(current.row2.xyz, input.position) + current.row2.w;

	precise float4 world_position =
		mul(craft_to_world, float4(current.rotated_local, 1.0f));
	precise float4 raster_position = mul(view_proj, world_position);
	current.world_position = world_position;
	current.raster_position = raster_position;
	return true;
}

/* Builds unjittered current/previous clip positions for motion velocity. */
void pbr_build_velocity(VSIn input, PbrCurrentVertex current,
						out float4 clip_curr, out float4 clip_prev)
{
	uint mesh_index = current.mesh_index;
	float4 row0 = mesh_table_load(previous_table_index, mesh_index * 3u + 0u);
	float4 row1 = mesh_table_load(previous_table_index, mesh_index * 3u + 1u);
	float4 row2 = mesh_table_load(previous_table_index, mesh_index * 3u + 2u);
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
