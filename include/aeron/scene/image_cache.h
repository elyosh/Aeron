#ifndef AERON_SCENE_IMAGE_CACHE_H
#define AERON_SCENE_IMAGE_CACHE_H

/*
 * Path-keyed resident image store over AeronTexture.
 *
 * Loads `.ktx2` files via the aeron_scene KTX2 reader, uploading every
 * mip level through the caller's command buffer. Block-compressed
 * (BC1..BC7) and uncompressed RGBA8 inputs are transparent to
 * consumers — by the time the cache hands back an entry the data is on
 * the GPU as a mip-chain AeronTexture. Optional zstd supercompression
 * is decompressed inside the reader.
 *
 * Entries remain resident until explicitly invalidated or until the store
 * is destroyed. Each consumer owns an instance and keys it on its own
 * asset paths.
 */

#include "aeron/render.h"
#include "aeron/image.h"
#include <stdbool.h>
#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronImageCache AeronImageCache;

typedef struct AeronImageCacheEntry {
	AeronTexture* tex;
	int           w, h;
} AeronImageCacheEntry;

AeronImageCache* Aeron_ImageCacheCreate(void);
void             Aeron_ImageCacheDestroy(AeronImageCache* cache);

/* First call reads the KTX2 and uploads a mip-chain texture through `cmd`
 * (which must have no open pass). Subsequent calls return the resident
 * entry. Passing NULL `cmd` performs lookup only. The returned pointer is
 * borrowed and remains valid until the path is invalidated or the store is
 * destroyed. */
const AeronImageCacheEntry* Aeron_ImageCacheLoad(AeronImageCache* cache, AeronCommandBuffer* cmd,
												 const char* path);
const AeronImageCacheEntry* Aeron_ImageCacheLoadVfs(
		AeronImageCache* cache, AeronCommandBuffer* cmd, AeronVfs* vfs,
		AeronVfsRoot root, const char* path, size_t max_size);

/* Destroy the resident entry for `path`, forcing the next load to read and
 * upload it again. */
void Aeron_ImageCacheInvalidate(AeronImageCache* cache, const char* path);

/* One-shot KTX2 → mip-chain AeronTexture upload through `cmd` (no
 * caching; the caller owns the returned texture). `debug_name` labels
 * failure logs. Shared by the cache's load path and by consumers with
 * their own keying (per-species atlas slots, cockpit assets). */
struct Ktx2;
AeronTexture* Aeron_ImageUploadKtx2(AeronCommandBuffer* cmd, const struct Ktx2* ktx,
									const char* debug_name);

/* Upload an RGBA8 image from memory with the same alpha, color-space and mip
 * handling used by runtime atlases and fonts. The caller owns the texture. */
AeronTexture* Aeron_ImageUploadRgba8(
		AeronCommandBuffer* cmd, const uint8_t* rgba, int width, int height,
		size_t pitch, AeronTextureFormat format, AeronColorSpace color_space,
		AeronImageAlphaMode alpha_mode, bool generate_mips,
		const char* debug_name);

/* One-shot cube-map load: open `path`, require faceCount == 6, upload
 * every mip x face through `cmd` (which must have no open pass), close.
 * The caller owns the returned texture; NULL on any error (missing
 * file, not a cube, upload failure) with a diagnostic on stderr.
 * Feeds AeronScene_SetSkyCube. */
AeronTexture* Aeron_ImageLoadCubemapKtx2(AeronCommandBuffer* cmd, const char* path);
AeronTexture* Aeron_ImageLoadCubemapKtx2Vfs(
		AeronCommandBuffer* cmd, AeronVfs* vfs, AeronVfsRoot root,
		const char* path, size_t max_size);

#ifdef __cplusplus
}
#endif

#endif /* AERON_SCENE_IMAGE_CACHE_H */
