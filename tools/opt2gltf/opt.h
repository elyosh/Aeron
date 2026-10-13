/*
 * opt.h - Parser for the OPT mesh files of TIE Fighter 1998 and the XvT family.
 *
 * An OPT file is a tree of typed nodes linked by absolute "memory" pointers.
 * To get a file offset, subtract the globalBase value stored in the header
 * from the pointer. Jeremy Ansel documents the same structure for X-Wing
 * Alliance OPTs (https://github.com/JeremyAnsel/JeremyAnsel.Xwa.Opt). opt.c
 * notes where the 1998 family differs.
 *
 * The parser flattens the tree into a form that is easy to render:
 *   opt_file_t { meshes[], textures[] }
 *     opt_mesh_t  { vertices, normals, uvs, hardpoints, descriptor,
 *                   rotation_scale, lods[] }
 *     opt_lod_t   { distance_threshold, groups[] }
 *     opt_face_group_t { texture_index, faces[] }   one batch per texture
 *     opt_texture_t { palette[16*256*2], pixels }
 */

#ifndef OPT_H
#define OPT_H

#include <stddef.h>
#include <stdint.h>

#define OPT_Q15_UNIT 32767.0f

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Model-space axes, found by observation and consistent across the IVFILES set:
 *   +Z = up                 (top and bottom turrets have opposite Z signs)
 *   -Y = forward / nose     (bridge meshes are at -Y, engines at +Y)
 *    X = lateral            (the format does not fix the handedness)
 */

/* --- Node types (the int32 stored at offset 4 of every node) -------------- */
typedef enum {
	OPT_NODE_NULL                = -1,
	OPT_NODE_GROUP               = 0,
	OPT_NODE_FACE_DATA           = 1,
	OPT_NODE_MESH_VERTICES       = 3,
	OPT_NODE_NODE_REFERENCE      = 7,
	OPT_NODE_VERTEX_NORMALS      = 11,
	OPT_NODE_TEXTURE_COORDINATES = 13,
	OPT_NODE_TEXTURE             = 20,
	OPT_NODE_FACE_GROUPING       = 21,
	OPT_NODE_HARDPOINT           = 22,
	OPT_NODE_ROTATION_SCALE      = 23,
	OPT_NODE_NODE_SWITCH         = 24,
	OPT_NODE_MESH_DESCRIPTOR     = 25,
	/* XWA only (version 5). The 1998 family does not use them. */
	OPT_NODE_TEXTURE_ALPHA = 26,
	OPT_NODE_ENGINE_GLOW   = 28,
} opt_node_type_t;

/* MeshDescriptor.meshType. The engine animates the OPT_MT_ROTARY_* types. */
typedef enum {
	OPT_MT_DEFAULT             = 0,
	OPT_MT_MAIN_HULL           = 1,
	OPT_MT_WING                = 2,
	OPT_MT_FUSELAGE            = 3,
	OPT_MT_GUN_TURRET          = 4,
	OPT_MT_SMALL_GUN           = 5,
	OPT_MT_ENGINE              = 6,
	OPT_MT_BRIDGE              = 7,
	OPT_MT_SHIELD_GEN          = 8,
	OPT_MT_ENERGY_GEN          = 9,
	OPT_MT_LAUNCHER            = 10,
	OPT_MT_COMM_SYS            = 11,
	OPT_MT_BEAM_SYS            = 12,
	OPT_MT_COMMAND_BEAM        = 13,
	OPT_MT_DOCKING_PLAT        = 14,
	OPT_MT_LANDING_PLAT        = 15,
	OPT_MT_HANGAR              = 16,
	OPT_MT_CARGO_POD           = 17,
	OPT_MT_MISC_HULL           = 18,
	OPT_MT_ANTENNA             = 19,
	OPT_MT_ROTARY_WING         = 20,
	OPT_MT_ROTARY_GUN_TURRET   = 21,
	OPT_MT_ROTARY_LAUNCHER     = 22,
	OPT_MT_ROTARY_COMM_SYS     = 23,
	OPT_MT_ROTARY_BEAM_SYS     = 24,
	OPT_MT_ROTARY_COMMAND_BEAM = 25,
	OPT_MT_HATCH               = 26,
	OPT_MT_CUSTOM              = 27,
	OPT_MT_WEAPON_SYS_1        = 28,
	OPT_MT_WEAPON_SYS_2        = 29,
	OPT_MT_POWER_REGENERATOR   = 30,
	OPT_MT_REACTOR             = 31,
} opt_mesh_type_t;

typedef enum {
	OPT_HP_NONE                  = 0,
	OPT_HP_LASER_CANNON          = 1,
	OPT_HP_ION_CANNON            = 2,
	OPT_HP_TURBO_LASER           = 3,
	OPT_HP_ION_TURBO_LASER       = 4,
	OPT_HP_CLUSTER_MISSILE       = 5,
	OPT_HP_TORPEDO_MAG_PULSE     = 6,
	OPT_HP_CONCUSSION_MISSILE    = 7,
	OPT_HP_PROTON_TORPEDO        = 8,
	OPT_HP_ADV_CONCUSSION        = 9,
	OPT_HP_ADV_PROTON_TORPEDO    = 10,
	OPT_HP_ADV_TORPEDO_MAG_PULSE = 11,
	OPT_HP_BOMB                  = 12,
	OPT_HP_BEAM_WEAPON           = 13,
	OPT_HP_DUMB_BOMB             = 14,
	OPT_HP_FIRE_ROCKET           = 15,
} opt_hardpoint_type_t;

/* --- Vector types --------------------------------------------------------- */
typedef struct {
	float x, y, z;
} opt_vec3_t;

typedef struct {
	float u, v;
} opt_vec2_t;

/* --- Geometry ------------------------------------------------------------- */
/*
 * One face. A triangle sets verts[3] to -1, and a quad uses all four corners.
 * The index arrays point into the vertex, uv and normal tables of the parent
 * mesh. As in Wavefront OBJ, each corner takes its values from separate
 * tables, not from one packed vertex list. edges[] indexes the edge table of
 * the FaceData. Rendering does not need it, but it is kept.
 */
typedef struct {
	int32_t    verts[4];
	int32_t    edges[4];
	int32_t    uvs[4];
	int32_t    normals[4];
	opt_vec3_t face_normal;
	opt_vec3_t tex_direction;
	opt_vec3_t tex_magnitude;
} opt_face_t;

/*
 * All faces of one LOD that use the same texture, so one draw call.
 *
 * When the file uses a NodeSwitch to change the texture with the mesh state
 * (for example as damage grows), state_count > 1 and state_textures[s] is
 * the texture index for state s. texture_index is always equal to
 * state_textures[0]. Without state switching, state_count is 1 and
 * state_textures is NULL, so use texture_index.
 */
typedef struct {
	int32_t     texture_index;  /* state 0; -1 if untextured */
	int32_t     state_count;    /* number of NodeSwitch variants (1 = none) */
	int32_t*    state_textures; /* NULL if state_count == 1, otherwise state_count indices */
	int32_t     edges_count;
	int32_t     face_count;
	opt_face_t* faces;
} opt_face_group_t;

/* One LOD of a mesh. distance_threshold is compared with the camera distance. */
typedef struct {
	float             distance_threshold;
	int32_t           group_count;
	opt_face_group_t* groups;
} opt_lod_t;

typedef struct {
	opt_hardpoint_type_t type;
	opt_vec3_t           pos;
} opt_hardpoint_t;

/*
 * XWA EngineGlow (node type 28): a billboard glow sprite at an engine
 * exhaust. dimensions is the half-size of the sprite, and the three axes
 * form the local frame of the glow.
 */
typedef struct {
	int32_t    is_disabled;
	uint32_t   core_color;  /* 0xAARRGGBB */
	uint32_t   outer_color; /* 0xAARRGGBB */
	opt_vec3_t dimensions;
	opt_vec3_t position;
	opt_vec3_t look_axis;
	opt_vec3_t up_axis;
	opt_vec3_t right_axis;
} opt_engine_glow_t;

/* Local frame of a rotating component. Axes are unit vectors (Q15-normalised). */
typedef struct {
	opt_vec3_t pivot;
	opt_vec3_t rotation_axis;
	opt_vec3_t direction_axis;
	opt_vec3_t up_axis;
} opt_rotation_scale_t;

typedef struct {
	opt_mesh_type_t mesh_type;
	int32_t         explosion_type;
	opt_vec3_t      span;
	opt_vec3_t      center;
	opt_vec3_t      bbox_min;
	opt_vec3_t      bbox_max;
	/* Target point used by the AI and the T key to target components.
	 * target_id tells the points apart (for example Engine 1 and Engine 2).
	 * target is the model-space position that the AI and the lock-on aim
	 * at. About 42% of 1998-format meshes set non-zero values here. The
	 * layout is the same as the MeshDescriptor of the XWA reader (72 bytes
	 * in total). */
	int32_t    target_id;
	opt_vec3_t target;
} opt_mesh_descriptor_t;

/* --- Texture -------------------------------------------------------------- */
/*
 * 8-bit textures use a palette of 16 shade levels, each with 256 RGB565
 * entries. In 1998 files, shades 0..7 are usually uninitialised (0xCD
 * pattern), and shades 8..15 form a brightness ramp from dim to fully lit
 * (15 = primary).
 */
#define OPT_PALETTE_SHADES 16
#define OPT_PALETTE_COLORS 256
#define OPT_PALETTE_BPP 2 /* bytes per stored RGB565 entry */
#define OPT_PALETTE_BYTES (OPT_PALETTE_SHADES * OPT_PALETTE_COLORS * OPT_PALETTE_BPP)
#define OPT_NATIVE_SHADE_TABLE_BYTES (4096 + OPT_PALETTE_BYTES)
/*
 * The shade row that holds the unlit base color of a texture depends on the
 * game that wrote the file:
 *   TIE98/XvT/BoP (version 2): ramp in shades 8..15, primary at shade 15.
 *   XWA           (version 5): ramp centered on shade 8. Rows 1..7 darken
 *                              the base, rows 9..15 brighten it toward white,
 *                              and row 0 is cleared. XWA's own reader always
 *                              samples row 8.
 * Use opt_palette_base_shade() to pick the right row for a loaded file.
 * Shade 15 of an XWA texture is the near-white top of its ramp, which gives
 * the classic "wrong palette" look.
 */
#define OPT_PRIMARY_SHADE 15
#define OPT_XWA_BASE_SHADE 8

/*
 * Self-illumination is encoded in the shape of the ramp, not in a node. A
 * glowing texel has the same color in the dim shade as in the base shade,
 * so it never gets darker. The dim row used for this test depends on the
 * game (see opt_palette_index_emissive).
 */
#define OPT_TIE_DIM_SHADE 8 /* darkest of the 8..15 ramp */
#define OPT_XWA_DIM_SHADE 4 /* ~75% row of the ramp around 8 */

/* Emissive detection accepts a difference of 1 quantization step per 5-6-5
 * channel. Because of rounding, the almost flat ramp of a glow core can
 * differ from the base by 1 LSB. An exact match would reject it and leave a
 * black hole in the bright center. Texels that darken normally differ by 4
 * steps or more, so they are still rejected. */
#define OPT_EMISSIVE_SHADE_TOL 1

/* Glow row for the lightmap of the XWA hardware renderer. The engine
 * classifier (ModelTexture_FilterHardwarePalette @ 0x44A600) replaces the
 * row 0 color of a self-lit entry with its row 10 color (basePalette + 2560).
 * That color becomes the lightmap texel color. */
#define OPT_XWA_GLOW_SHADE 10

#define OPT_TEXTURE_NAME_MAX 64 /* real names have at most 8 characters */

/*
 * Texture pixels are stored as a mip chain in which each level halves the
 * size: level 0 has W*H texels, level 1 has (W/2)*(H/2) texels, and so on
 * down to 1x1. mip_count is 1 when the file stores no mips. Use
 * opt_texture_mip_offset() to find where a level starts.
 *
 * Texels are 1-byte palette indices. The exception is 32bpp textures, an
 * XWAU extension of version-5 OPTs (written by JeremyAnsel.Xwa.Opt), whose
 * texels are 4 bytes in B,G,R,A order. The file does not store the format.
 * It is deduced from the byte count of the pixel data, compared with W*H
 * (BitsPerPixel in JeremyAnsel.Xwa.Opt Texture.cs). A 32bpp texture has no
 * palette. The 8192-byte block after its pixels is all zero except two flag
 * bytes: [2] (has alpha) and [4] (has illumination).
 */
#define OPT_TEXTURE_32BPP_FLAG_ALPHA 2
#define OPT_TEXTURE_32BPP_FLAG_ILLUM 4

typedef struct {
	char     name[OPT_TEXTURE_NAME_MAX];
	int32_t  width;
	int32_t  height;
	int32_t  bytes_per_texel; /* 1 (palette index) or 4 (B,G,R,A) */
	int32_t  mip_count;
	int32_t  mip_chain_bytes; /* total bytes in pixels[] across all levels */
	uint8_t* pixels;          /* full mip chain, base level first */
	/*
	 * Alpha of each pixel (0 = transparent, 255 = opaque). NULL when the
	 * texture has no alpha, which makes it opaque. It has 1 byte per pixel
	 * and the same mip-chain layout as pixels[], so the base level is the
	 * first width*height bytes. It comes from:
	 *   - 8bpp: the XWA TextureAlpha child node. All 1998 textures and most
	 *     XWA ones have no such node.
	 *   - 32bpp: the A byte of each texel, only when the alpha flag of the
	 *     texture is set. Without the flag the texture is drawn opaque.
	 */
	uint8_t* alpha;
	/*
	 * 32bpp only: illumination of each pixel (0 = unlit .. 255). It is the
	 * first width*height bytes of the TextureAlpha child node, and is set
	 * only when the illumination flag is set. Only the base level is kept
	 * because the byte count of the child is not reliable: some files store
	 * 4 times the chain length, padded with zeros. NULL otherwise. For 8bpp
	 * textures, illumination is derived from the palette.
	 */
	uint8_t* illum;
	uint8_t  flag_alpha; /* 32bpp flag byte [2] != 0 */
	uint8_t  flag_illum; /* 32bpp flag byte [4] != 0 */
	uint8_t  palette[OPT_PALETTE_BYTES];
	/* Version 1 only: the original 4096-byte shade table followed by the
	 * RGB565 palette, kept for opt_upgrade_v1_memory. */
	uint8_t* native_shade_table;
} opt_texture_t;

/* --- Mesh ----------------------------------------------------------------- */
typedef struct {
	opt_mesh_descriptor_t descriptor;
	int                   has_descriptor;
	opt_rotation_scale_t  rotation_scale;
	int                   has_rotation_scale;

	int32_t     vertex_count;
	opt_vec3_t* vertices;

	int32_t     normal_count;
	opt_vec3_t* normals;

	int32_t     uv_count;
	opt_vec2_t* uvs;

	int32_t          hardpoint_count;
	opt_hardpoint_t* hardpoints;

	int32_t            engine_glow_count;
	opt_engine_glow_t* engine_glows;

	int32_t    lod_count;
	opt_lod_t* lods;
} opt_mesh_t;

typedef struct {
	int32_t version; /* 2 for TIE98/XvT/BoP, 5 for XWA */

	int32_t     mesh_count;
	opt_mesh_t* meshes;

	int32_t        texture_count;
	opt_texture_t* textures;
} opt_file_t;

/* --- API ------------------------------------------------------------------ */
typedef struct {
	char msg[256];
} opt_error_t;

opt_file_t* opt_load_file(const char* path, opt_error_t* err);
opt_file_t* opt_load_memory(const void* data, size_t size, opt_error_t* err);
void        opt_free(opt_file_t* opt);

/* Rebuild a version-1 TIE98 OPT as a self-contained version-2 image. */
int opt_upgrade_v1_memory(const void* source, size_t source_size, uint8_t** output, size_t* output_size,
						  opt_error_t* err);

/* These return a string literal, never NULL. An unknown value gives "?". */
const char* opt_node_type_name(opt_node_type_t t);
const char* opt_mesh_type_name(opt_mesh_type_t t);
const char* opt_hardpoint_type_name(opt_hardpoint_type_t t);

int opt_mesh_type_is_rotary(opt_mesh_type_t t);
int opt_rotation_scale_is_identity(const opt_rotation_scale_t* rs);

/* Decode one palette entry at the given shade level to 8-bit RGB. */
void opt_palette_rgb(const uint8_t palette[OPT_PALETTE_BYTES], int shade, int index, uint8_t* r, uint8_t* g,
					 uint8_t* b);

/* Returns the shade row that holds the unlit base color of the textures:
 * 8 for XWA (version 5), 15 otherwise (see OPT_PRIMARY_SHADE and
 * OPT_XWA_BASE_SHADE). */
int opt_palette_base_shade(const opt_file_t* opt);

/* Exact copy of the self-illumination classifier of the XWA hardware
 * renderer (ModelTexture_FilterHardwarePalette @ 0x44A600), for version-5
 * OPTs. Channels are read as 5-bit fields, and the lowest bit of green is
 * dropped (b = c & 0x1f, g = (c >> 6) & 0x1f, r = (c >> 11) & 0x1f). Entry
 * `index` is self-lit if and only if:
 *   - row 0 (the darkest) is bright: r^2 + g^2 + b^2 >= 32, and
 *   - rows 1..6 each stay within a squared distance of 16 from row 0, so
 *     the color does not respond to light.
 * The engine then takes the lightmap texel color from row 10
 * (OPT_XWA_GLOW_SHADE). The caller must also apply the per-texture checks
 * of CreateD3DfromTexture (@ 0x4CD...): the name must not start with '_',
 * and some but not all of the 256 entries must be classified as lit. */
int opt_palette_classic_lit(const uint8_t palette[OPT_PALETTE_BYTES], int index);

/* Heuristic for older files (before XWA, version < 5). Returns 1 if palette
 * color `index` is self-lit with the TIE98/XvT ramp convention: its dim
 * shade matches its base shade within `tol` 5-6-5 steps per channel, so it
 * never gets darker, and it is not near-black. With OPT_EMISSIVE_SHADE_TOL
 * as `tol`, ramp rounding is accepted. A larger value counts more colors as
 * glows. XWA files (version 5) use the exact opt_palette_classic_lit
 * classifier instead. */
int opt_palette_index_emissive(const opt_file_t* opt, const uint8_t palette[OPT_PALETTE_BYTES], int index,
							   int tol);

/*
 * Returns the byte offset of mip `level` in texture->pixels, and writes the
 * size of the level to *w and *h. The offset is the texel offset times
 * bytes_per_texel, so divide it by bytes_per_texel to index alpha[]. Level 0
 * is the base level. A level out of range returns 0 and the base size.
 */
size_t opt_texture_mip_offset(const opt_texture_t* t, int level, int32_t* w, int32_t* h);

/* Returns the texture index to bind for a mesh state. State 0 always gives
 * group->texture_index. Higher states use the NodeSwitch variant. */
int32_t opt_face_group_texture(const opt_face_group_t* g, int state);

#ifdef __cplusplus
}
#endif

#endif /* OPT_H */
