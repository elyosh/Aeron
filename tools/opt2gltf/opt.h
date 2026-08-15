/*
 * opt.h - Parser for TIE Fighter 1998 / XvT-family OPT mesh files.
 *
 * The file is a tree of typed Nodes connected by absolute "memory" pointers.
 * A globalBase value stored in the header is subtracted from every pointer to
 * recover the file offset. The same skeleton is documented by Jeremy Ansel for
 * X-Wing Alliance OPTs (https://github.com/JeremyAnsel/JeremyAnsel.Xwa.Opt);
 * differences specific to the 1998 family are noted in opt.c.
 *
 * The parser flattens the tree into a renderer-friendly form:
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

/* World units to meters. 1 unit = 1600 / 65536 m, about 2.44 cm. */
#define OPT_UNIT_TO_METERS  (1600.0f / 65536.0f)

/*
 * Model-space axes (empirical, consistent across the IVFILES set):
 *   +Z = up                 (top/bottom turrets sit at opposite Z signs)
 *   -Y = forward / nose     (bridge meshes lie at -Y, engines at +Y)
 *    X = lateral            (handedness not fixed by the format)
 */

/* --- Node types (the int32 stored at offset 4 of every node) -------------- */
typedef enum {
    OPT_NODE_NULL                = -1,
    OPT_NODE_GROUP               =  0,
    OPT_NODE_FACE_DATA           =  1,
    OPT_NODE_MESH_VERTICES       =  3,
    OPT_NODE_NODE_REFERENCE      =  7,
    OPT_NODE_VERTEX_NORMALS      = 11,
    OPT_NODE_TEXTURE_COORDINATES = 13,
    OPT_NODE_TEXTURE             = 20,
    OPT_NODE_FACE_GROUPING       = 21,
    OPT_NODE_HARDPOINT           = 22,
    OPT_NODE_ROTATION_SCALE      = 23,
    OPT_NODE_NODE_SWITCH         = 24,
    OPT_NODE_MESH_DESCRIPTOR     = 25,
    /* XWA-only (version 5); absent from the 1998 family. */
    OPT_NODE_TEXTURE_ALPHA       = 26,
    OPT_NODE_ENGINE_GLOW         = 28,
} opt_node_type_t;

/* MeshDescriptor.meshType. Values OPT_MT_ROTARY_* are animated by the engine. */
typedef enum {
    OPT_MT_DEFAULT             =  0,
    OPT_MT_MAIN_HULL           =  1,
    OPT_MT_WING                =  2,
    OPT_MT_FUSELAGE            =  3,
    OPT_MT_GUN_TURRET          =  4,
    OPT_MT_SMALL_GUN           =  5,
    OPT_MT_ENGINE              =  6,
    OPT_MT_BRIDGE              =  7,
    OPT_MT_SHIELD_GEN          =  8,
    OPT_MT_ENERGY_GEN          =  9,
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
    OPT_HP_NONE                  =  0,
    OPT_HP_LASER_CANNON          =  1,
    OPT_HP_ION_CANNON            =  2,
    OPT_HP_TURBO_LASER           =  3,
    OPT_HP_ION_TURBO_LASER       =  4,
    OPT_HP_CLUSTER_MISSILE       =  5,
    OPT_HP_TORPEDO_MAG_PULSE     =  6,
    OPT_HP_CONCUSSION_MISSILE    =  7,
    OPT_HP_PROTON_TORPEDO        =  8,
    OPT_HP_ADV_CONCUSSION        =  9,
    OPT_HP_ADV_PROTON_TORPEDO    = 10,
    OPT_HP_ADV_TORPEDO_MAG_PULSE = 11,
    OPT_HP_BOMB                  = 12,
    OPT_HP_BEAM_WEAPON           = 13,
    OPT_HP_DUMB_BOMB             = 14,
    OPT_HP_FIRE_ROCKET           = 15,
} opt_hardpoint_type_t;

/* --- Vector types --------------------------------------------------------- */
typedef struct { float x, y, z; } opt_vec3_t;
typedef struct { float u, v;    } opt_vec2_t;

/* --- Geometry ------------------------------------------------------------- */
/*
 * One face. Triangles set verts[3] = -1; quads use all four corners.
 * The four index arrays reference the parent mesh's vertex/uv/normal tables
 * (not a packed vertex list - corners pull from independent tables, like
 * Wavefront OBJ). edges[] indexes a per-FaceData edge table; not needed for
 * rendering but kept for completeness.
 */
typedef struct {
    int32_t   verts[4];
    int32_t   edges[4];
    int32_t   uvs[4];
    int32_t   normals[4];
    opt_vec3_t face_normal;
    opt_vec3_t tex_direction;
    opt_vec3_t tex_magnitude;
} opt_face_t;

/*
 * All faces of one LOD that share a single texture (one draw call worth).
 *
 * If the source file used a NodeSwitch to swap textures by mesh state (e.g.
 * damage progression), state_count > 1 and state_textures[s] gives the
 * texture index for state s. texture_index always equals state_textures[0].
 * For groups without state switching, state_count == 1 and state_textures
 * is NULL (use texture_index directly).
 */
typedef struct {
    int32_t     texture_index;     /* state 0; -1 if untextured */
    int32_t     state_count;       /* number of NodeSwitch variants (1 = none) */
    int32_t    *state_textures;    /* NULL if state_count==1, else [state_count] indices */
    int32_t     edges_count;
    int32_t     face_count;
    opt_face_t *faces;
} opt_face_group_t;

/* One LOD slice of a mesh. distance_threshold compares against camera range. */
typedef struct {
    float             distance_threshold;
    int32_t           group_count;
    opt_face_group_t *groups;
} opt_lod_t;

typedef struct {
    opt_hardpoint_type_t type;
    opt_vec3_t           pos;
} opt_hardpoint_t;

/*
 * XWA EngineGlow (node type 28): a billboard glow sprite at an engine
 * exhaust. core_color / outer_color are 0xAARRGGBB; dimensions is the
 * sprite half-extent; the three axes form the glow's local frame.
 */
typedef struct {
    int32_t    is_disabled;
    uint32_t   core_color;       /* 0xAARRGGBB */
    uint32_t   outer_color;      /* 0xAARRGGBB */
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
    opt_mesh_type_t  mesh_type;
    int32_t           explosion_type;
    opt_vec3_t        span;
    opt_vec3_t        center;
    opt_vec3_t        bbox_min;
    opt_vec3_t        bbox_max;
    /* AI / T-key component-targeting reference. target_id discriminates
     * the addressable point (e.g. Engine 1 vs Engine 2); target is the
     * world-local position the AI / lock-on logic aims at. ~42% of
     * 1998-format meshes author non-zero values here. Same layout as
     * the XWA reader's MeshDescriptor (72 bytes total). */
    int32_t           target_id;
    opt_vec3_t        target;
} opt_mesh_descriptor_t;

/* --- Texture -------------------------------------------------------------- */
/*
 * 8-bit palettised. The palette is 16 shading levels of 256 RGB565 entries:
 * shades 0..7 are typically uninitialised (0xCD pattern) in 1998 files;
 * shades 8..15 form a brightness ramp from dim to fully lit (15 = primary).
 */
#define OPT_PALETTE_SHADES        16
#define OPT_PALETTE_COLORS       256
#define OPT_PALETTE_BPP            2   /* bytes per stored RGB565 entry */
#define OPT_PALETTE_BYTES        (OPT_PALETTE_SHADES * OPT_PALETTE_COLORS * OPT_PALETTE_BPP)
#define OPT_NATIVE_SHADE_TABLE_BYTES (4096 + OPT_PALETTE_BYTES)
/*
 * Which of the 16 shade rows holds the texture's unlit base color depends
 * on the game that authored the file:
 *   TIE98/XvT/BoP (version 2): ramp in shades 8..15, primary at shade 15.
 *   XWA           (version 5): ramp centered on shade 8 (rows 1..7 darken
 *                              the base, 9..15 brighten toward white, row 0
 *                              cleared). XWA's own reader always samples row 8.
 * Use opt_palette_base_shade() to pick the right row for a loaded file;
 * sampling shade 15 of an XWA texture lands in the near-white top of its
 * ramp, the classic "wrong palette" symptom.
 */
#define OPT_PRIMARY_SHADE         15
#define OPT_XWA_BASE_SHADE         8

/*
 * Self-illumination is encoded in the ramp shape, not a node: a glowing
 * texel's dim-light shade equals its base shade, so it never darkens. The
 * dim row sampled for that test is per-game (see opt_palette_index_emissive).
 */
#define OPT_TIE_DIM_SHADE          8   /* darkest of the 8..15 ramp */
#define OPT_XWA_DIM_SHADE          4   /* ~75% row of the ramp around 8 */

/* Emissive detection tolerates 1 quantization step per 5-6-5 channel: a
 * glow core's near-flat ramp can differ from the base by 1 LSB through
 * rounding, which an exact match wrongly rejects (leaving a black hole at
 * the bright center). Normal darkening texels differ by >=4 steps, so this
 * stays well clear of them. */
#define OPT_EMISSIVE_SHADE_TOL     1

/* XWA hardware-renderer lightmap glow row: the engine's classifier
 * (ModelTexture_FilterHardwarePalette @ 0x44A600) replaces a self-lit
 * entry's row-0 slot with the ROW 10 color (basePalette + 2560), which
 * becomes the lightmap texel color. */
#define OPT_XWA_GLOW_SHADE        10

#define OPT_TEXTURE_NAME_MAX      64   /* names in the wild are <= 8, plenty */

/*
 * Texture pixels are stored as a halving mip chain: level 0 is W*H bytes,
 * level 1 is (W/2)*(H/2) bytes, etc., until 1x1. mip_count is 1 if the file
 * stored no mips. Use opt_texture_mip_offset() to find a level's sub-buffer.
 */
typedef struct {
    char     name[OPT_TEXTURE_NAME_MAX];
    int32_t  width;
    int32_t  height;
    int32_t  mip_count;
    int32_t  mip_chain_bytes;            /* total bytes in pixels[] across all levels */
    uint8_t *pixels;                     /* full mip chain, base level first */
    /*
     * Per-pixel alpha (0 = transparent, 255 = opaque), from an XWA
     * TextureAlpha child node. NULL when the file carries none (all 1998
     * textures, most XWA ones). Same mip-chain layout/length as pixels[]
     * (1 byte/pixel), so the base level is the first width*height bytes.
     */
    uint8_t *alpha;
    uint8_t  palette[OPT_PALETTE_BYTES];
    /* Original indexed + RGB565 data retained for v1 runtime conversion. */
    uint8_t *native_shade_table;
} opt_texture_t;

/* --- Mesh ----------------------------------------------------------------- */
typedef struct {
    opt_mesh_descriptor_t descriptor;
    int                    has_descriptor;
    opt_rotation_scale_t   rotation_scale;
    int                    has_rotation_scale;

    int32_t       vertex_count;
    opt_vec3_t   *vertices;

    int32_t       normal_count;
    opt_vec3_t   *normals;

    int32_t       uv_count;
    opt_vec2_t   *uvs;

    int32_t          hardpoint_count;
    opt_hardpoint_t *hardpoints;

    int32_t            engine_glow_count;
    opt_engine_glow_t *engine_glows;

    int32_t    lod_count;
    opt_lod_t *lods;
} opt_mesh_t;

typedef struct {
    int32_t        version;          /* 2 for TIE98/XvT/BoP, 5 for XWA */

    int32_t        mesh_count;
    opt_mesh_t    *meshes;

    int32_t        texture_count;
    opt_texture_t *textures;
} opt_file_t;

/* --- API ------------------------------------------------------------------ */
typedef struct {
    char msg[256];
} opt_error_t;

opt_file_t *opt_load_file  (const char *path, opt_error_t *err);
opt_file_t *opt_load_memory(const void *data, size_t size, opt_error_t *err);
void        opt_free       (opt_file_t *opt);

/* Rebuild a version-1 TIE98 OPT as a self-contained version-2 image. */
int opt_upgrade_v1_memory(const void *source, size_t source_size,
                          uint8_t **output, size_t *output_size,
                          opt_error_t *err);

/* Lookups returning literal strings; never NULL, "?" on unknown value. */
const char *opt_node_type_name      (opt_node_type_t      t);
const char *opt_mesh_type_name      (opt_mesh_type_t      t);
const char *opt_hardpoint_type_name (opt_hardpoint_type_t t);

int  opt_mesh_type_is_rotary       (opt_mesh_type_t t);
int  opt_rotation_scale_is_identity(const opt_rotation_scale_t *rs);

/* Decode one palette entry at the given shade level to 8-bit RGB. */
void opt_palette_rgb(const uint8_t palette[OPT_PALETTE_BYTES],
                     int shade, int index,
                     uint8_t *r, uint8_t *g, uint8_t *b);

/* Palette shade row holding a texture's unlit base color, chosen by the
 * authoring game (see OPT_PRIMARY_SHADE / OPT_XWA_BASE_SHADE): shade 8 for
 * XWA (version 5), shade 15 otherwise. */
int opt_palette_base_shade(const opt_file_t *opt);

/* EXACT XWA hardware-renderer self-illumination classifier
 * (ModelTexture_FilterHardwarePalette @ 0x44A600), for version-5 OPTs.
 * Channels are read as 5-bit fields with green's LSB DROPPED
 * (b = c & 0x1f, g = (c >> 6) & 0x1f, r = (c >> 11) & 0x1f). Entry
 * `index` is self-lit iff:
 *   - row 0 (darkest) is bright: r^2 + g^2 + b^2 >= 32, AND
 *   - rows 1..6 each stay within squared distance 16 of row 0
 *     (the color does not respond to illumination).
 * The engine then colors the lightmap texel from row 10
 * (OPT_XWA_GLOW_SHADE). Per-texture gates applied by the caller
 * (CreateD3DfromTexture @ 0x4CD...): name not starting with '_', and
 * SOME but not ALL 256 entries classified lit. */
int opt_palette_classic_lit(const uint8_t palette[OPT_PALETTE_BYTES], int index);

/* Legacy (pre-XWA, version < 5) heuristic: true (1) if palette color
 * `index` is self-illuminated under the TIE98/XvT ramp convention: its
 * dim-light shade matches its base shade within `tol` 5-6-5 steps per
 * channel (so it never darkens) and it is not near-black. `tol` of
 * OPT_EMISSIVE_SHADE_TOL absorbs ramp rounding; raising it widens what
 * counts as a glow. XWA (version 5) files use the exact
 * opt_palette_classic_lit classifier instead. */
int opt_palette_index_emissive(const opt_file_t *opt,
                               const uint8_t palette[OPT_PALETTE_BYTES],
                               int index, int tol);

/*
 * Return the byte offset of mip `level` inside texture->pixels, and write
 * the level's dimensions into *w / *h. level 0 is the base mip. Returns 0
 * (and W/H = base) for any out-of-range level.
 */
size_t opt_texture_mip_offset(const opt_texture_t *t, int level,
                              int32_t *w, int32_t *h);

/* Texture index to bind for a given mesh state. state==0 always returns
 * group->texture_index; for higher states the NodeSwitch variant is used. */
int32_t opt_face_group_texture(const opt_face_group_t *g, int state);

#ifdef __cplusplus
}
#endif

#endif /* OPT_H */
