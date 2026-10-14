/*
 * Path-keyed resident image store over AeronTexture — see
 * aeron/scene/image_cache.h.
 *
 * Loads `.ktx2` files via the aeron_scene KTX2 reader,
 * uploading every mip level in one transfer buffer + one copy pass.
 * Block-compressed (BC1..BC7) and uncompressed RGBA8 KTX2 inputs are
 * both accepted — the format flows from the file's vkFormat through
 * to the AeronTexture's create desc. Optional zstd supercompression
 * (KTX2 supercompressionScheme=2) is decompressed inside the reader
 * before we see the level data; emitted by filmextract --bc7 by
 * default.
 *
 * Entries remain resident until their owner explicitly invalidates them.
 */

#include "aeron/scene/image_cache.h"

#include "aeron/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aeron/scene/ktx2_reader.h"
#include "rgba_upload.h"

/* ---------- resident store ---------- */

typedef struct CacheEntryGpu {
	char path[512];
	/* Embedded so the borrowed public pointer stays stable for the
	 * entry's lifetime. */
	AeronImageCacheEntry  pub;
	int                   owned;
	struct CacheEntryGpu* next;
} CacheEntryGpu;

struct AeronImageCache {
	CacheEntryGpu* entries;
};

AeronImageCache* Aeron_ImageCacheCreate(void) { return (AeronImageCache*)calloc(1, sizeof(AeronImageCache)); }

void Aeron_ImageCacheDestroy(AeronImageCache* a) {
	if (!a)
		return;
	CacheEntryGpu* e = a->entries;
	while (e) {
		CacheEntryGpu* next = e->next;
		if (e->owned && e->pub.tex)
			Aeron_DestroyTexture(e->pub.tex);
		free(e);
		e = next;
	}
	free(a);
}

void Aeron_ImageCacheInvalidate(AeronImageCache* a, const char* path) {
	if (!a || !path)
		return;
	CacheEntryGpu** link = &a->entries;
	while (*link) {
		CacheEntryGpu* cur = *link;
		if (strcmp(cur->path, path) == 0) {
			*link = cur->next;
			if (cur->owned && cur->pub.tex)
				Aeron_DestroyTexture(cur->pub.tex);
			free(cur);
			return;
		}
		link = &cur->next;
	}
}

static CacheEntryGpu* cache_find(const AeronImageCache* a, const char* path) {
	for (CacheEntryGpu* e = a->entries; e; e = e->next)
		if (strcmp(e->path, path) == 0)
			return e;
	return NULL;
}

/* Insert a freshly-uploaded texture, keyed by the caller-provided path. */
static const AeronImageCacheEntry* cache_insert(AeronImageCache* a, const char* path, AeronTexture* tex,
												int w, int h, int owned) {
	CacheEntryGpu* ne = (CacheEntryGpu*)calloc(1, sizeof *ne);
	if (!ne) {
		if (owned)
			Aeron_DestroyTexture(tex);
		return NULL;
	}
	size_t n = strlen(path);
	if (n + 1 > sizeof ne->path)
		n = sizeof ne->path - 1;
	memcpy(ne->path, path, n);
	ne->path[n] = '\0';
	ne->pub.tex = tex;
	ne->pub.w   = w;
	ne->pub.h   = h;
	ne->owned   = owned;
	ne->next    = a->entries;
	a->entries  = ne;
	return &ne->pub;
}

/* ---------- KTX2 → AeronTexture upload ----------
 *
 * The create-texture + per-mip upload lives in Aeron_ImageUploadKtx2();
 * this wrapper opens the file, delegates the upload, and records the
 * result in the resident store. */
static const AeronImageCacheEntry* load_via_ktx2(AeronImageCache* a, AeronCommandBuffer* cmd,
												 const char* path) {
	Ktx2* k = ktx2_open(path);
	if (!k)
		return NULL;

	int           w   = ktx2_width(k);
	int           h   = ktx2_height(k);
	AeronTexture* tex = Aeron_ImageUploadKtx2(cmd, k, path);
	if (!tex) {
		ktx2_close(k);
		return NULL;
	}

	ktx2_close(k);
	return cache_insert(a, path, tex, w, h, 1);
}

const AeronImageCacheEntry* Aeron_ImageCacheLoad(AeronImageCache* a, AeronCommandBuffer* cmd,
												 const char* path) {
	if (!a || !path || !path[0])
		return NULL;

	/* A resident hit doesn't need a command buffer. */
	CacheEntryGpu* hit = cache_find(a, path);
	if (hit)
		return &hit->pub;
	if (!cmd)
		return NULL;

	/* `path` already points at a `.ktx2` file (filled in by the
	 * manifest parser via resolve_asset). Load + upload; on failure
	 * the reader has logged and we skip the actor. */
	return load_via_ktx2(a, cmd, path);
}

const AeronImageCacheEntry* Aeron_ImageCacheLoadVfs(AeronImageCache* cache, AeronCommandBuffer* cmd,
													AeronVfs* vfs, AeronVfsRoot root, const char* path,
													size_t max_size) {
	if (!cache || !cmd || !vfs || !path || !path[0])
		return NULL;
	char key[576];
	int  length = snprintf(key, sizeof key, "%d:%s", (int)root, path);
	if (length < 0 || (size_t)length >= sizeof key)
		return NULL;
	CacheEntryGpu* hit = cache_find(cache, key);
	if (hit)
		return &hit->pub;
	uint8_t* bytes = NULL;
	size_t   size  = 0;
	if (!AeronVfs_ReadAll(vfs, root, path, max_size, &bytes, &size))
		return NULL;
	Ktx2* ktx = ktx2_open_mem(bytes, size, path);
	if (!ktx) {
		free(bytes);
		return NULL;
	}
	const int     width = ktx2_width(ktx), height = ktx2_height(ktx);
	AeronTexture* texture = Aeron_ImageUploadKtx2(cmd, ktx, path);
	ktx2_close(ktx);
	free(bytes);
	return texture ? cache_insert(cache, key, texture, width, height, 1) : NULL;
}

void Aeron_ImageCacheSetOverride(AeronImageCache* cache, const char* path, AeronTexture* texture, int width,
								 int height) {
	if (!cache || !path || !path[0] || !texture || width <= 0 || height <= 0)
		return;
	Aeron_ImageCacheInvalidate(cache, path);
	(void)cache_insert(cache, path, texture, width, height, 0);
}

void Aeron_ImageCacheClearOverride(AeronImageCache* cache, const char* path) {
	if (!cache || !path)
		return;
	CacheEntryGpu** link = &cache->entries;
	while (*link) {
		CacheEntryGpu* entry = *link;
		if (strcmp(entry->path, path) == 0 && !entry->owned) {
			*link = entry->next;
			free(entry);
			return;
		}
		link = &entry->next;
	}
}

void Aeron_ImageCacheClearOverrides(AeronImageCache* cache) {
	if (!cache)
		return;
	CacheEntryGpu** link = &cache->entries;
	while (*link) {
		CacheEntryGpu* entry = *link;
		if (!entry->owned) {
			*link = entry->next;
			free(entry);
			continue;
		}
		link = &entry->next;
	}
}

/* `as_array` creates a 2D array texture of the layers of every part, in
 * order (one layer for a part that is not a KTX2 array); the parts must
 * agree in size, format and level count. Otherwise the single part's
 * texture: 2D, cube or cube array; a KTX2 2D array is rejected. */
static AeronTexture* upload_ktx2(AeronCommandBuffer* cmd, const Ktx2* const* parts, uint32_t part_count,
								 bool as_array, const char* debug_name) {
	if (!cmd || !parts || !part_count || !parts[0] || (!as_array && part_count != 1))
		return NULL;
	const Ktx2* ktx    = parts[0];
	int         w      = ktx2_width(ktx);
	int         h      = ktx2_height(ktx);
	int         nl     = ktx2_level_count(ktx);
	int         faces  = ktx2_face_count(ktx);
	int         layers = 0;
	for (uint32_t p = 0; p < part_count; p++) {
		const Ktx2* part = parts[p];
		if (!part || ktx2_width(part) != w || ktx2_height(part) != h || ktx2_level_count(part) != nl ||
			ktx2_format(part) != ktx2_format(ktx) || ktx2_face_count(part) != faces) {
			Aeron_LogError("aeron.scene", "KTX2 part %u does not match the first (%s)", p,
						   debug_name ? debug_name : "<unnamed>");
			return NULL;
		}
		layers += ktx2_layer_count(part) > 0 ? ktx2_layer_count(part) : (as_array ? 1 : 0);
	}
	if (w <= 0 || h <= 0 || nl <= 0 || (layers > 0 && !as_array && faces != 6) || (as_array && faces != 1))
		return NULL;
	/* Slices per level: the array's layers, or each cube's faces. */
	const int slices = as_array ? layers : faces * (layers > 0 ? layers : 1);

	AeronTexture* tex = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width       = w,
		.height      = h,
		.mip_count   = nl,
		.format      = ktx2_format(ktx),
		.usage       = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.cube        = !as_array && faces == 6,
		.layer_count = layers,
		.debug_name  = debug_name,
	});
	if (!tex) {
		Aeron_LogError("aeron.scene", "KTX2 texture creation failed (%s)",
					   debug_name ? debug_name : "<unnamed>");
		return NULL;
	}

	/* Arrays walk each part's layers like faces. */
	const uint64_t upload_count64 = (uint64_t)nl * (uint64_t)slices;
	if (upload_count64 == 0 || upload_count64 > UINT32_MAX ||
		upload_count64 > SIZE_MAX / sizeof(AeronTextureUploadDesc)) {
		Aeron_LogError("aeron.scene", "invalid KTX2 mip/face count (%s)",
					   debug_name ? debug_name : "<unnamed>");
		Aeron_DestroyTexture(tex);
		return NULL;
	}
	const uint32_t          upload_count = (uint32_t)upload_count64;
	AeronTextureUploadDesc* uploads      = (AeronTextureUploadDesc*)calloc(upload_count, sizeof *uploads);
	if (!uploads) {
		Aeron_DestroyTexture(tex);
		return NULL;
	}

	uint32_t upload_index = 0;
	int      first_layer  = 0;
	for (uint32_t p = 0; p < part_count; p++) {
		const int part_faces =
			as_array ? (ktx2_layer_count(parts[p]) > 0 ? ktx2_layer_count(parts[p]) : 1) : slices;
		for (int i = 0; i < nl; i++) {
			for (int f = 0; f < part_faces; f++) {
				Ktx2Level lv = ktx2_level_face(parts[p], i, f);
				if (!lv.data || lv.size == 0 || lv.size > UINT32_MAX) {
					Aeron_LogError("aeron.scene", "invalid KTX2 level %d face %d (%s)", i, f,
								   debug_name ? debug_name : "<unnamed>");
					free(uploads);
					Aeron_DestroyTexture(tex);
					return NULL;
				}
				uploads[upload_index++] = (AeronTextureUploadDesc) {
					.texture   = tex,
					.mip_level = i,
					.layer     = first_layer + f,
					.width     = lv.width,
					.height    = lv.height,
					.raw_data  = lv.data,
					.raw_size  = (uint32_t)lv.size,
				};
			}
		}
		first_layer += part_faces;
	}
	if (!Aeron_UploadTextureBatchCmd(cmd, uploads, upload_count)) {
		Aeron_LogError("aeron.scene", "KTX2 mip/face batch upload failed (%s)",
					   debug_name ? debug_name : "<unnamed>");
		free(uploads);
		Aeron_DestroyTexture(tex);
		return NULL;
	}
	free(uploads);

	return tex;
}

AeronTexture* Aeron_ImageUploadKtx2(AeronCommandBuffer* cmd, const Ktx2* ktx, const char* debug_name) {
	return upload_ktx2(cmd, &ktx, 1, false, debug_name);
}

AeronTexture* Aeron_ImageUploadKtx2Array(AeronCommandBuffer* cmd, const Ktx2* ktx, const char* debug_name) {
	return upload_ktx2(cmd, &ktx, 1, true, debug_name);
}

AeronTexture* Aeron_ImageUploadKtx2Layers(AeronCommandBuffer* cmd, const Ktx2* const* parts,
										  uint32_t part_count, const char* debug_name) {
	return upload_ktx2(cmd, parts, part_count, true, debug_name);
}

int Aeron_ImageUploadKtx2IntoLayers(AeronCommandBuffer* cmd, const Ktx2* ktx, AeronTexture* texture,
									const uint32_t* layers) {
	if (!cmd || !ktx || !texture || !layers || ktx2_face_count(ktx) != 1)
		return 0;
	const int levels      = ktx2_level_count(ktx);
	const int layer_count = ktx2_layer_count(ktx) > 0 ? ktx2_layer_count(ktx) : 1;
	if (levels <= 0)
		return 0;
	const uint32_t          upload_count = (uint32_t)levels * (uint32_t)layer_count;
	AeronTextureUploadDesc* uploads      = (AeronTextureUploadDesc*)calloc(upload_count, sizeof *uploads);
	if (!uploads)
		return 0;
	uint32_t n = 0;
	for (int i = 0; i < levels; i++) {
		for (int l = 0; l < layer_count; l++) {
			Ktx2Level lv = ktx2_level_face(ktx, i, l);
			if (!lv.data || lv.size == 0 || lv.size > UINT32_MAX) {
				free(uploads);
				return 0;
			}
			uploads[n++] = (AeronTextureUploadDesc) {
				.texture   = texture,
				.mip_level = i,
				.layer     = (int)layers[l],
				.width     = lv.width,
				.height    = lv.height,
				.raw_data  = lv.data,
				.raw_size  = (uint32_t)lv.size,
			};
		}
	}
	const int ok = Aeron_UploadTextureBatchCmd(cmd, uploads, upload_count);
	free(uploads);
	return ok;
}

AeronTexture* Aeron_ImageUploadRgba8(AeronCommandBuffer* cmd, const uint8_t* rgba, int width, int height,
									 size_t pitch, AeronTextureFormat format, AeronColorSpace color_space,
									 AeronImageAlphaMode alpha_mode, bool generate_mips,
									 const char* debug_name) {
	return aeron_scene_upload_rgba8(cmd, rgba, width, height, pitch, format, color_space, alpha_mode,
									generate_mips, debug_name);
}

AeronTexture* Aeron_ImageLoadCubemapKtx2(AeronCommandBuffer* cmd, const char* path) {
	if (!cmd || !path || !path[0])
		return NULL;
	Ktx2* k = ktx2_open(path);
	if (!k)
		return NULL;
	AeronTexture* tex = NULL;
	if (ktx2_face_count(k) != 6) {
		Aeron_LogError("aeron.scene", "%s: expected KTX2 cube map (faces=%d)", path, ktx2_face_count(k));
	} else {
		tex = Aeron_ImageUploadKtx2(cmd, k, path);
	}
	ktx2_close(k);
	return tex;
}

AeronTexture* Aeron_ImageLoadCubemapKtx2Vfs(AeronCommandBuffer* cmd, AeronVfs* vfs, AeronVfsRoot root,
											const char* path, size_t max_size) {
	if (!cmd || !vfs || !path || !path[0])
		return NULL;
	uint8_t* bytes = NULL;
	size_t   size  = 0;
	if (!AeronVfs_ReadAll(vfs, root, path, max_size, &bytes, &size))
		return NULL;
	Ktx2*         ktx     = ktx2_open_mem(bytes, size, path);
	AeronTexture* texture = NULL;
	if (ktx && ktx2_face_count(ktx) == 6)
		texture = Aeron_ImageUploadKtx2(cmd, ktx, path);
	else
		Aeron_LogError("aeron.scene", "%s: expected a KTX2 cube map", path);
	if (ktx)
		ktx2_close(ktx);
	free(bytes);
	return texture;
}
