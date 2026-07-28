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

#include <stdlib.h>
#include <string.h>

#include "aeron/scene/ktx2_reader.h"

/* ---------- resident store ---------- */

typedef struct CacheEntryGpu {
    char            path[512];
    /* Embedded so the borrowed public pointer stays stable for the
     * entry's lifetime. */
    AeronImageCacheEntry pub;
    struct CacheEntryGpu *next;
} CacheEntryGpu;

struct AeronImageCache {
    CacheEntryGpu *entries;
};

AeronImageCache *Aeron_ImageCacheCreate(void) {
    return (AeronImageCache *)calloc(1, sizeof(AeronImageCache));
}

void Aeron_ImageCacheDestroy(AeronImageCache *a) {
    if (!a) return;
    CacheEntryGpu *e = a->entries;
    while (e) {
        CacheEntryGpu *next = e->next;
        if (e->pub.tex)
            Aeron_DestroyTexture(e->pub.tex);
        free(e);
        e = next;
    }
    free(a);
}

void Aeron_ImageCacheInvalidate(AeronImageCache *a, const char *path) {
    if (!a || !path) return;
    CacheEntryGpu **link = &a->entries;
    while (*link) {
        CacheEntryGpu *cur = *link;
        if (strcmp(cur->path, path) == 0) {
            *link = cur->next;
            if (cur->pub.tex)
                Aeron_DestroyTexture(cur->pub.tex);
            free(cur);
            return;
        }
        link = &cur->next;
    }
}

static CacheEntryGpu *cache_find(const AeronImageCache *a, const char *path) {
    for (CacheEntryGpu *e = a->entries; e; e = e->next)
        if (strcmp(e->path, path) == 0)
            return e;
    return NULL;
}

/* Insert a freshly-uploaded texture, keyed by the caller-provided path. */
static const AeronImageCacheEntry *cache_insert(AeronImageCache *a,
                                              const char *path,
                                              AeronTexture *tex,
                                              int w, int h)
{
    CacheEntryGpu *ne = (CacheEntryGpu *)calloc(1, sizeof *ne);
    if (!ne) {
        Aeron_DestroyTexture(tex);
        return NULL;
    }
    size_t n = strlen(path);
    if (n + 1 > sizeof ne->path) n = sizeof ne->path - 1;
    memcpy(ne->path, path, n); ne->path[n] = '\0';
    ne->pub.tex = tex;
    ne->pub.w   = w;
    ne->pub.h   = h;
    ne->next = a->entries;
    a->entries = ne;
    return &ne->pub;
}

/* ---------- KTX2 → AeronTexture upload ----------
 *
 * The create-texture + per-mip upload lives in Aeron_ImageUploadKtx2();
 * this wrapper opens the file, delegates the upload, and records the
 * result in the resident store. */
static const AeronImageCacheEntry *load_via_ktx2(AeronImageCache *a,
                                               AeronCommandBuffer *cmd,
                                               const char *path)
{
    Ktx2 *k = ktx2_open(path);
    if (!k) return NULL;

    int w  = ktx2_width(k);
    int h  = ktx2_height(k);
    AeronTexture *tex = Aeron_ImageUploadKtx2(cmd, k, path);
    if (!tex) {
        ktx2_close(k);
        return NULL;
    }

    ktx2_close(k);
    return cache_insert(a, path, tex, w, h);
}

const AeronImageCacheEntry *Aeron_ImageCacheLoad(AeronImageCache *a,
                                                   AeronCommandBuffer *cmd,
                                                   const char *path) {
    if (!a || !path || !path[0])
        return NULL;

    /* A resident hit doesn't need a command buffer. */
    CacheEntryGpu *hit = cache_find(a, path);
    if (hit)
        return &hit->pub;
    if (!cmd)
        return NULL;

    /* `path` already points at a `.ktx2` file (filled in by the
     * manifest parser via resolve_asset). Load + upload; on failure
     * the reader has logged and we skip the actor. */
    return load_via_ktx2(a, cmd, path);
}


AeronTexture *Aeron_ImageUploadKtx2(AeronCommandBuffer *cmd, const Ktx2 *ktx, const char *debug_name)
{
    if (!cmd || !ktx) return NULL;
    int w     = ktx2_width      (ktx);
    int h     = ktx2_height     (ktx);
    int nl    = ktx2_level_count(ktx);
    int faces = ktx2_face_count (ktx);
    if (w <= 0 || h <= 0 || nl <= 0) return NULL;

    AeronTexture *tex = Aeron_CreateTexture(&(AeronTextureDesc) {
        .width     = w,
        .height    = h,
        .mip_count = nl,
        .format    = ktx2_format(ktx),
        .usage     = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
        .cube      = faces == 6,
		.debug_name = debug_name,
    });
    if (!tex) {
        Aeron_LogError("aeron.scene", "KTX2 texture creation failed (%s)",
                       debug_name ? debug_name : "<unnamed>");
        return NULL;
    }

    const uint64_t upload_count64 = (uint64_t)nl * (uint64_t)faces;
    if (upload_count64 == 0 || upload_count64 > UINT32_MAX ||
        upload_count64 > SIZE_MAX / sizeof(AeronTextureUploadDesc)) {
        Aeron_LogError("aeron.scene", "invalid KTX2 mip/face count (%s)",
                       debug_name ? debug_name : "<unnamed>");
        Aeron_DestroyTexture(tex);
        return NULL;
    }
    const uint32_t upload_count = (uint32_t)upload_count64;
    AeronTextureUploadDesc *uploads =
        (AeronTextureUploadDesc *)calloc(upload_count, sizeof *uploads);
    if (!uploads) {
        Aeron_DestroyTexture(tex);
        return NULL;
    }

    uint32_t upload_index = 0;
    for (int i = 0; i < nl; i++) {
        for (int f = 0; f < faces; f++) {
            Ktx2Level lv = ktx2_level_face(ktx, i, f);
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
                .layer     = f,
                .width     = lv.width,
                .height    = lv.height,
                .raw_data  = lv.data,
                .raw_size  = (uint32_t)lv.size,
            };
        }
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

AeronTexture *Aeron_ImageLoadCubemapKtx2(AeronCommandBuffer *cmd, const char *path)
{
    if (!cmd || !path || !path[0]) return NULL;
    Ktx2 *k = ktx2_open(path);
    if (!k) return NULL;
    AeronTexture *tex = NULL;
    if (ktx2_face_count(k) != 6) {
        Aeron_LogError("aeron.scene", "%s: expected KTX2 cube map (faces=%d)", path, ktx2_face_count(k));
    } else {
        tex = Aeron_ImageUploadKtx2(cmd, k, path);
    }
    ktx2_close(k);
    return tex;
}
