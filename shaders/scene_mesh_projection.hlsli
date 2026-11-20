/* What the vertex and fragment shaders of the projected overlays share
 * (mesh_overlay.h). */
#ifndef SCENE_MESH_PROJECTION_HLSLI
#define SCENE_MESH_PROJECTION_HLSLI

/* The most images in one pass (AERON_SCENE_MESH_PROJECTIONS_MAX). */
#define PROJECTION_MAX 4

struct Projection {
	/* The image's UV at a position of the mesh, before articulation:
	 * u = dot(u_row.xyz, position) + u_row.w, and v the same way. */
	float4 u_row;
	float4 v_row;
	float4 uv_rect; /* min u, min v, max u, max v */
	float4 color;
	/* The mesh-table rows that take the image: bit r % 32 of word r / 32,
	 * four words in each uint4. */
	uint4 rows[2];
};

/* A vertex whose row doesn't take an image gets this UV for it, which is
 * outside every image. */
#define PROJECTION_NO_UV -1.0e4f

#endif
