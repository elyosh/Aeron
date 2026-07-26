#ifndef AERON_SCENE_KTX2_READER_H
#define AERON_SCENE_KTX2_READER_H

/*
 * Minimal KTX2 reader.
 *
 * Spec: https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html
 *
 * Supported subset:
 *   - 2D textures (faceCount == 1, layerCount in {0,1}, pixelDepth in {0,1})
 *   - Cube maps (faceCount == 6, layerCount in {0,1}, pixelDepth in {0,1}).
 *     Cube faces in each KTX2 level are stored back-to-back in the
 *     standard order (+X, -X, +Y, -Y, +Z, -Z). Use ktx2_face_count() to
 *     branch + ktx2_level_face() to lift one face's bytes.
 *   - supercompressionScheme ∈ { 0 (raw), 2 (zstd) }
 *   - vkFormat ∈ { R8G8B8A8_UNORM, R8G8B8A8_SRGB,
 *                  BC1_RGBA_UNORM, BC1_RGBA_SRGB,
 *                  BC3_RGBA_UNORM, BC3_RGBA_SRGB,
 *                  BC4_R_UNORM,
 *                  BC5_RG_UNORM,
 *                  BC7_RGBA_UNORM, BC7_RGBA_SRGB }
 *   - levelCount >= 1 (zero-mip "generate at runtime" is rejected)
 *
 * Layout / DFD / KVD / SGD blocks are skipped — the reader only
 * walks the header + level index + raw mip data. Format mapping is the reader's
 * responsibility; consumers see only an AeronTextureFormat enum value.
 *
 * The reader copies the file into a heap blob at open time; mip
 * `data` pointers stay valid until ktx2_close. Errors return NULL +
 * a diagnostic line to stderr.
 */

#include <stddef.h>
#include <stdint.h>

#include <stdbool.h>

#include "aeron/render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ktx2 Ktx2;

typedef enum Ktx2OpenStatus {
	KTX2_OPEN_SUCCESS = 0,
	KTX2_OPEN_INVALID_DATA,
	KTX2_OPEN_RESOURCE_FAILURE,
} Ktx2OpenStatus;

typedef struct {
    const uint8_t *data;
    size_t         size;
    int            width;
    int            height;
} Ktx2Level;

/* Open a .ktx2 file and parse the header + level index. Returns
 * NULL if the file is missing, the magic doesn't match, the header
 * is malformed, the format isn't in the supported subset, or the
 * level index is inconsistent with the file size. The caller frees
 * via ktx2_close. */
Ktx2 *ktx2_open(const char *path);

/* Buffer-mode sibling: parse a KTX2 image already resident in memory
 * (e.g. a cgltf buffer_view into a GLB BIN chunk). The reader does
 * NOT take ownership of `bytes` — the caller must keep the buffer
 * alive for the lifetime of the returned Ktx2. `src_label` is used
 * only in diagnostic lines and may be NULL. */
Ktx2 *ktx2_open_mem(const uint8_t *bytes, size_t size, const char *src_label);
Ktx2 *ktx2_open_mem_status(const uint8_t *bytes, size_t size, const char *src_label,
						   Ktx2OpenStatus *status);

void  ktx2_close(Ktx2 *k);

/* Mapped Aeron texture format. Already the value the consumer
 * passes to Aeron_CreateTexture. */
AeronTextureFormat ktx2_format(const Ktx2 *k);

int    ktx2_width      (const Ktx2 *k);
int    ktx2_height     (const Ktx2 *k);
int    ktx2_level_count(const Ktx2 *k);

/* Cube faces per level. 1 for plain 2D textures; 6 for cube maps. */
int    ktx2_face_count (const Ktx2 *k);

/* Returns the level's data view. For cube maps, this is the full
 * level — all faces concatenated; use ktx2_level_face when you need
 * one face's bytes. Level 0 is the base (largest) level; subsequent
 * indices are progressively smaller mips. */
Ktx2Level ktx2_level(const Ktx2 *k, int level);

/* Returns one cube face of `level`. `face` ∈ [0, ktx2_face_count()).
 * The returned `data` points into the same blob ktx2_level returns;
 * face data is stored contiguously level-by-level, so face 0..5 of
 * level 0 land back-to-back, then face 0..5 of level 1, etc.
 * For non-cube textures (face_count == 1), face=0 is equivalent to
 * ktx2_level. */
Ktx2Level ktx2_level_face(const Ktx2 *k, int level, int face);

/* True when the format is a block-compressed BC family (BC1..7).
 * Used by callers to choose the upload path's row stride math
 * (block-aligned vs pixel-aligned). */
bool ktx2_is_block_compressed(const Ktx2 *k);

/* Block dim for block-compressed formats (always 4×4 for BC) and
 * bytes-per-block. Returns (0, 0) for uncompressed formats. */
void ktx2_block_info(const Ktx2 *k, int *block_w, int *block_h,
                     int *bytes_per_block);

#ifdef __cplusplus
}
#endif

#endif
