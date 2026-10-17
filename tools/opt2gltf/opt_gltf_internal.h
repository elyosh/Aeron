#ifndef AERON_OPT2GLTF_OPT_GLTF_INTERNAL_H
#define AERON_OPT2GLTF_OPT_GLTF_INTERNAL_H

/*
 * Declarations shared by the conversion files (opt_gltf.c,
 * opt_gltf_document.c, opt_gltf_material.c, opt_gltf_mesh.c). They are not
 * part of the library interface (opt_gltf.h).
 *
 * OPT axes (+X right, -Y forward, +Z up) map to standard glTF axes in meters
 * as glTF = (-OPT.x, OPT.z, -OPT.y). This mapping is a reflection, so it
 * turns the clockwise face order of OPT into the counterclockwise order of
 * glTF without changing the indices.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "opt.h"
#include "opt_gltf.h"

static inline void swap_axis_v3(const opt_vec3_t* in, float out[3]) {
	out[0] = -in->x;
	out[1] = in->z;
	out[2] = -in->y;
}

#define OPT_METERS_PER_UNIT (1600.0f / 65536.0f)
#define OPT_Q15_TO_UNIT 32768.0f

static inline char* xstrdup(const char* s) {
	size_t n = strlen(s);
	char*  r = (char*)malloc(n + 1);
	if (r)
		memcpy(r, s, n + 1);
	return r;
}

static inline char* xprintf_dup(const char* fmt, ...) {
	char    buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return xstrdup(buf);
}

static inline void set_color(float out[], float r, float g, float b) {
	out[0] = r;
	out[1] = g;
	out[2] = b;
}

/* Arrays of cgltf structs. A counting pass finds all the sizes first, so
 * each array takes a single malloc. */
typedef struct {
	/* The .bin buffer holds the vertices and indices of each mesh in turn.
	 * Buffer views and accessors point into it. */
	uint8_t* bin;
	size_t   bin_size;
	size_t   bin_cap;

	cgltf_buffer       buffer;
	cgltf_buffer_view* views;
	size_t             views_count, views_cap;
	cgltf_accessor*    accessors;
	size_t             accessors_count, accessors_cap;
	cgltf_image*       images;
	size_t             images_count;
	cgltf_texture*     textures;
	size_t             textures_count;
	cgltf_sampler      sampler; /* single repeat/linear sampler shared by all */
	cgltf_material*    materials;
	size_t             materials_count;
	/* For each material, its animation (its AERON_flight_model object) and
	 * its screen (a JSON object), or NULL. Copies of a material share them. */
	const char**            material_anim;
	const char**            material_screen;
	/* For each material, the given material whose skin layers it writes
	 * (OptGltfMaterial.skins), or NULL. Copies of a material share it. */
	const OptGltfMaterial** material_given;
	cgltf_mesh*             meshes;
	size_t                  meshes_count;
	cgltf_primitive*        primitives;
	size_t                  primitives_count, primitives_cap;
	cgltf_attribute*        attributes;
	size_t                  attributes_count, attributes_cap;
	cgltf_material_mapping* mappings;
	size_t                  mappings_count, mappings_cap;
	cgltf_material_variant* variants;
	size_t                  variants_count;
	cgltf_node*             nodes;
	size_t                  nodes_count, nodes_cap;
	cgltf_node**            root_children;
	size_t                  root_children_count;

	/* Strings used in the JSON. cgltf does not copy them when it writes, so
	 * the build owns them. */
	char** owned_strings;
	size_t owned_strings_count, owned_strings_cap;

	/* Set when keeping a string or growing the .bin runs out of memory. The
	 * build then fails. */
	bool out_of_memory;
} GltfBuild;

typedef struct OptGltfOwnedImage {
	uint8_t* rgba;
	uint32_t width;
	uint32_t height;
	bool     borrowed; /* pixels of a given material, owned by the caller */
} OptGltfOwnedImage;

struct OptGltfDocument {
	GltfBuild          gb;
	cgltf_data         data;
	cgltf_scene        scene;
	cgltf_node**       scene_roots;
	OptGltfOwnedImage* image_pixels;
	size_t             image_pixels_count;
};

/* OPT faces index each corner separately, with separate vertex, UV and
 * normal arrays. glTF needs one (position, normal, uv) tuple per vertex.
 * With the classic normals (OPT_GLTF_NORMALS_CLASSIC), the position-index
 * remap of the classic renderer is reproduced first (see
 * opt_gltf_built_mesh_from_opt). Then equal tuples are merged within a mesh,
 * so UV seams still split vertices that have the same lighting normal.
 *
 * The merge uses an unsorted linear scan. This is fast enough because the
 * meshes have at most a couple thousand vertices.
 */

typedef struct {
	float pos[3];
	float nrm[3];
	float uv[2];
	float tan[4]; /* glTF TANGENT (xyz, w); zero until tangents are generated */
} GltfVertex;

typedef struct {
	GltfVertex* vertices;
	uint32_t*   indices; /* per face group, contiguous */
	uint32_t    vertex_count;
	uint32_t    vertex_cap;
	uint32_t    index_count;
	uint32_t    index_cap;
	uint32_t*   fg_index_start;
	uint32_t*   fg_index_count_arr;
	uint32_t    fg_count; /* face groups with non-empty indices */
	/* Defective canonical normals repaired while emitting vertices (see the
	 * classic remap in opt_gltf_built_mesh_from_opt). */
	uint32_t fixed_zero_normals;
	uint32_t fixed_flipped_normals;
	/* Triangles that the OPT lists twice (see built_mesh_emit_triangle).
	 * They are left out, or kept when the repeat has another texture. */
	uint32_t skipped_repeats;
	uint32_t kept_repeats;
	bool     has_tangents;  /* vertices carry TANGENT (opt_gltf_generate_tangents) */
	bool     out_of_memory; /* growing the vertices or indices failed */
} BuiltMesh;

/* Some shipped OPTs have meshes with vertices but no LOD 0 faces. For
 * example, meshes 2 and 3 of Interdictor2Hangar have two vertices and no
 * LOD. The game loads them as components without geometry. Each GLB
 * component needs a mesh. The flight-model topology keeps the positions of
 * a component whose triangles are all degenerate, and drops the triangles.
 * So such a mesh is converted to one untextured degenerate triangle over
 * its own vertices. The result has the same positions, no faces, and draws
 * nothing. */
typedef struct FacelessPatch {
	opt_file_t        file;
	opt_mesh_t*       meshes;
	opt_lod_t*        lods;
	opt_face_group_t* groups;
	opt_face_t*       faces;
} FacelessPatch;

/* State shared by the phases of the build. */
typedef struct Build {
	const opt_file_t*          opt;
	const char*                basename;
	const OptGltfBuildOptions* options;
	opt_error_t*               error;
	const OptGltfMaterial*     materials; /* the given materials, or NULL */
	const OptGltfVariants*     variants;
	const OptGltfScreen*       screens;
	float                      smooth_angle; /* negative to keep the OPT's own normals */
	bool                       repair_normals;
	bool                       emissive;
	bool                       fullbright;
	float                      fullbright_strength;
	/* The palette row that holds the base colours. It depends on the game
	 * that made the file (XWA = 8, TIE98/XvT = 15). Sampling the wrong row
	 * washes XWA textures out to a faintly magenta white. */
	int base_shade;

	OptGltfDocument* document;
	GltfBuild*       gb;
	size_t           image_slot; /* the next image and texture slot */
	size_t           image_count;

	/* For each texture, whether it has self-lit texels (stock emissive). */
	int* tex_emissive;
	/* Screen surfaces. For each texture, the image slot of the base image of
	 * its cover, or -1. The emissive image is in the next slot. Textures with
	 * the same cover share its images. */
	int32_t* screen_cover_image;
	/* Index of the untextured material. The texture materials come before
	 * it, and the variant materials come after it. */
	size_t untextured;

	BuiltMesh* built;
	int        max_state_count; /* drives KHR_materials_variants */
	size_t     state_variant_count;
	size_t     given_variant_count;
	/* Copies made by opt_gltf_promoted_material, indexed by
	 * [base material * 2 + rank - 1]. */
	cgltf_material** promoted;
	size_t           base_material_count;
} Build;

/* ---- opt_gltf.c --------------------------------------------------------- */

/* Sets the error message of the build and returns false. */
bool opt_gltf_build_fail(Build* b, const char* format, ...);

/* Sends a note or a warning to `options->log`. */
void opt_gltf_log(const OptGltfBuildOptions* options, bool warning, const char* format, ...);

/* ---- opt_gltf_document.c ------------------------------------------------ */

void*              opt_gltf_push_array(void** arr, size_t* count, size_t* cap, size_t elem_size, size_t want);
char*              opt_gltf_keep_string(GltfBuild* gb, char* s);
/* The texture whose image has the pixels `rgba`, or -1. */
int opt_gltf_texture_of_pixels(const OptGltfDocument* document, const uint8_t* rgba);
bool               opt_gltf_set_extension(GltfBuild* build, cgltf_node* node, cJSON* object);
size_t             opt_gltf_bin_append(GltfBuild* gb, const void* data, size_t bytes);
cgltf_buffer_view* opt_gltf_new_view(GltfBuild* gb, size_t offset, size_t bytes, cgltf_buffer_view_type type);
cgltf_accessor*    opt_gltf_new_accessor(GltfBuild* gb, cgltf_buffer_view* view, cgltf_size count,
										 cgltf_type type, cgltf_component_type comp);
cgltf_texture*     opt_gltf_set_image(OptGltfDocument* document, size_t slot, uint8_t* rgba, uint32_t width,
									  uint32_t height, char* name, char* file);
bool               opt_gltf_prune_unused(OptGltfDocument* document);

/* ---- opt_gltf_material.c ------------------------------------------------ */

bool            opt_gltf_validate_alpha_overrides(const OptGltfBuildOptions* options, opt_error_t* error);
bool            opt_gltf_prepare_textures(Build* b);
bool            opt_gltf_build_materials(Build* b);
int             opt_gltf_alpha_class_rank(const cgltf_material* m);
cgltf_material* opt_gltf_promoted_material(GltfBuild* build, cgltf_material* base, int rank,
										   size_t base_count, cgltf_material** cache);
bool            opt_gltf_add_energy(cJSON* object, unsigned bits);
bool            opt_gltf_material_extensions(OptGltfDocument* document, unsigned* common_energy);

/* ---- opt_gltf_mesh.c ---------------------------------------------------- */

void opt_gltf_built_mesh_free(BuiltMesh* bm);
bool opt_gltf_built_mesh_from_opt(BuiltMesh* bm, const opt_mesh_t* m, float meters_per_opt_unit, bool regen,
								  float smooth_angle_degrees, bool repair_normals, opt_error_t* error);
bool opt_gltf_generate_tangents(BuiltMesh* bm);
const opt_file_t* opt_gltf_faceless_patch_apply(const opt_file_t* opt, const OptGltfBuildOptions* options,
												FacelessPatch* patch);
void              opt_gltf_faceless_patch_free(FacelessPatch* patch);

#endif /* AERON_OPT2GLTF_OPT_GLTF_INTERNAL_H */
