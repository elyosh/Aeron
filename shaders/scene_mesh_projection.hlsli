/* Shared by the vertex and fragment shaders of the projected overlays (see
 * mesh_overlay.h). */
#ifndef SCENE_MESH_PROJECTION_HLSLI
#define SCENE_MESH_PROJECTION_HLSLI

/* The maximum number of images in one pass. It must match
 * AERON_SCENE_MESH_PROJECTIONS_MAX. */
#define PROJECTION_MAX 4

struct Projection {
	/* The image's UV is computed from the vertex position, before the mesh
	 * table moves the vertex:
	 *   u = dot(u_row.xyz, position) + u_row.w
	 *   v = dot(v_row.xyz, position) + v_row.w */
	float4 u_row;
	float4 v_row;
	float4 uv_rect; /* min u, min v, max u, max v */
	float4 color;
	/* Which mesh-table rows the image applies to. Row r uses bit r % 32 of
	 * word r / 32. Each uint4 holds four words. */
	uint4 rows[2];
};

/* The UV given to a vertex when an image doesn't apply to its row. It is
 * outside every image, so nothing is drawn. */
#define PROJECTION_NO_UV -1.0e4f

#endif
