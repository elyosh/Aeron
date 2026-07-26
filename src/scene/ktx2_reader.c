/*
 * Minimal KTX2 reader — implementation.
 *
 * The whole file is read into one heap allocation; `data` pointers
 * for each mip level are computed offsets into that blob and stay
 * valid until ktx2_close. No dynamic allocations beyond the blob +
 * a small Ktx2 record + the per-level array.
 */

#include "aeron/scene/ktx2_reader.h"

#include <stdarg.h>
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

/* KTX2 file identifier — first 12 bytes. Verbatim from the spec:
 *   «KTX 20»\r\n\x1A\n  (with the « and » being the Latin-1
 *   left/right guillemet characters 0xAB / 0xBB). */
static const uint8_t kKtx2Magic[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
    0x0D, 0x0A, 0x1A, 0x0A
};

/* Vulkan VkFormat values for the formats we support. The KTX2 spec
 * mandates these are the canonical Vulkan numeric IDs. */
#define VK_FORMAT_R8G8B8A8_UNORM      37
#define VK_FORMAT_R8G8B8A8_SRGB       43
#define VK_FORMAT_BC1_RGBA_UNORM_BLK 133
#define VK_FORMAT_BC1_RGBA_SRGB_BLK  134
#define VK_FORMAT_BC3_UNORM_BLK      137
#define VK_FORMAT_BC3_SRGB_BLK       138
#define VK_FORMAT_BC4_UNORM_BLK      139
#define VK_FORMAT_BC5_UNORM_BLK      141
#define VK_FORMAT_BC6H_UFLOAT_BLK    143
#define VK_FORMAT_BC6H_SFLOAT_BLK    144
#define VK_FORMAT_BC7_UNORM_BLK      145
#define VK_FORMAT_BC7_SRGB_BLK       146

struct Ktx2 {
    uint8_t              *blob;          /* whole file, owned */
    size_t                blob_size;
    AeronTextureFormat  format;
    int                   width;
    int                   height;
    int                   level_count;
    int                   face_count;     /* 1 for 2D, 6 for cubemap */
    bool                  block_compressed;
    int                   bytes_per_block;
    /* Per-level views. For supercompressionScheme=0 these point into
     * `blob`. For scheme=2 (zstd) each level owns a freshly-allocated
     * decompressed buffer (`level_owned[i]`) and `levels[i].data`
     * points into that buffer. ktx2_close frees both.
     *
     * For cubemap levels, `levels[i].size` is the level total = 6 ×
     * face_size and `data` covers all 6 faces. face_size is the same
     * across faces (KTX2 mandates equal face dims). */
    Ktx2Level            *levels;
    uint8_t             **level_owned;
};

static void ktx2_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static uint32_t rd_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0])       | ((uint32_t)p[1] << 8)  |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64_le(const uint8_t *p)
{
    return ((uint64_t)rd_u32_le(p)) |
           ((uint64_t)rd_u32_le(p + 4) << 32);
}

/* Map vkFormat → AeronTextureFormat. Returns
 * AERON_TEXTURE_FORMAT_UNKNOWN on unsupported formats. The bool
 * outputs flag whether the format is block-compressed and what the
 * bytes-per-block size is (16 for BC1..3, 8 for BC4, etc.). */
static bool map_format(uint32_t vk, AeronTextureFormat *out_fmt,
                       bool *out_block, int *out_bpb)
{
    *out_block = true;     /* default: every BC family is block-compressed */
    switch (vk) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        *out_fmt = AERON_TEXTURE_FORMAT_RGBA8_UNORM;
        *out_block = false; *out_bpb = 0;
        return true;
    case VK_FORMAT_R8G8B8A8_SRGB:
        *out_fmt = AERON_TEXTURE_FORMAT_RGBA8_SRGB;
        *out_block = false; *out_bpb = 0;
        return true;
    case VK_FORMAT_BC1_RGBA_UNORM_BLK:
        *out_fmt = AERON_TEXTURE_FORMAT_BC1_RGBA_UNORM;
        *out_bpb = 8;
        return true;
    case VK_FORMAT_BC1_RGBA_SRGB_BLK:
        *out_fmt = AERON_TEXTURE_FORMAT_BC1_RGBA_SRGB;
        *out_bpb = 8;
        return true;
    case VK_FORMAT_BC3_UNORM_BLK:
        *out_fmt = AERON_TEXTURE_FORMAT_BC3_RGBA_UNORM;
        *out_bpb = 16;
        return true;
    case VK_FORMAT_BC3_SRGB_BLK:
        *out_fmt = AERON_TEXTURE_FORMAT_BC3_RGBA_SRGB;
        *out_bpb = 16;
        return true;
    case VK_FORMAT_BC4_UNORM_BLK:
        *out_fmt = AERON_TEXTURE_FORMAT_BC4_R_UNORM;
        *out_bpb = 8;
        return true;
    case VK_FORMAT_BC5_UNORM_BLK:
        *out_fmt = AERON_TEXTURE_FORMAT_BC5_RG_UNORM;
        *out_bpb = 16;
        return true;
    case VK_FORMAT_BC6H_UFLOAT_BLK:
        *out_fmt = AERON_TEXTURE_FORMAT_BC6H_RGB_UFLOAT;
        *out_bpb = 16;
        return true;
    case VK_FORMAT_BC6H_SFLOAT_BLK:
        *out_fmt = AERON_TEXTURE_FORMAT_BC6H_RGB_FLOAT;
        *out_bpb = 16;
        return true;
    case VK_FORMAT_BC7_UNORM_BLK:
        *out_fmt = AERON_TEXTURE_FORMAT_BC7_RGBA_UNORM;
        *out_bpb = 16;
        return true;
    case VK_FORMAT_BC7_SRGB_BLK:
        *out_fmt = AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB;
        *out_bpb = 16;
        return true;
    default:
        return false;
    }
}

/* Read the entire file into a freshly-allocated heap blob. Returns
 * NULL on I/O error. The caller frees with free(). */
static uint8_t *slurp(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    *out_size = (size_t)sz;
    return buf;
}

/* Shared parser used by both ktx2_open (path-based) and
 * ktx2_open_mem (buffer-based). When `own_bytes` is true, the
 * returned Ktx2 takes ownership of `bytes` and ktx2_close will free
 * it; when false, the caller must keep `bytes` alive for the
 * Ktx2's lifetime — raw (non-zstd) level data pointers index into
 * it. `src_label` is the diagnostic name used in diagnostic lines. */
static Ktx2 *ktx2_parse_bytes(const uint8_t *bytes, size_t blob_size,
                              const char *src_label, bool own_bytes,
                              Ktx2OpenStatus *status)
{
    const char *path = src_label ? src_label : "<mem>";
    if (status) *status = KTX2_OPEN_INVALID_DATA;
    if (!bytes) return NULL;
    /* Magic + minimum header (12 + 17×4 = 80 bytes). The level
     * index follows; for levelCount=1 we need an additional 24 bytes,
     * so the absolute minimum file size is 80 + 24 = 104. */
    if (blob_size < 80 || memcmp(bytes, kKtx2Magic, 12) != 0) {
        ktx2_log("[ktx2] %s: bad magic / too small", path);
        if (own_bytes) free((void *)bytes);
        return NULL;
    }
    /* The original implementation always owned a freshly-allocated
     * blob; preserving the variable name keeps the rest of the
     * function readable. For mem mode the cast strips const because
     * we promise not to write through it; the field below is the
     * only place a non-const pointer is needed and is only stored
     * (and freed) when own_bytes is true. */
    uint8_t *blob = (uint8_t *)bytes;
    const uint8_t *p = blob + 12;
    uint32_t vkFormat                = rd_u32_le(p +  0);
    /*uint32_t typeSize              = rd_u32_le(p +  4);*/
    uint32_t pixelWidth              = rd_u32_le(p +  8);
    uint32_t pixelHeight             = rd_u32_le(p + 12);
    uint32_t pixelDepth              = rd_u32_le(p + 16);
    uint32_t layerCount              = rd_u32_le(p + 20);
    uint32_t faceCount               = rd_u32_le(p + 24);
    uint32_t levelCount              = rd_u32_le(p + 28);
    uint32_t supercompressionScheme  = rd_u32_le(p + 32);
    /* dfd, kvd, sgd indices follow; ignored. */

    AeronTextureFormat fmt;
    bool block_compressed;
    int  bytes_per_block = 0;
    if (!map_format(vkFormat, &fmt, &block_compressed, &bytes_per_block)) {
        ktx2_log("[ktx2] %s: unsupported vkFormat=%u", path, vkFormat);
        if (own_bytes) free(blob);
        return NULL;
    }
    if ((faceCount != 1 && faceCount != 6) ||
        (layerCount > 1) || (pixelDepth > 1)) {
        ktx2_log("[ktx2] %s: only 2D or cubemap textures supported "
                "(faces=%u layers=%u depth=%u)",
                path, faceCount, layerCount, pixelDepth);
        if (own_bytes) free(blob);
        return NULL;
    }
    if (faceCount == 6 && pixelWidth != pixelHeight) {
        ktx2_log("[ktx2] %s: cubemap requires square faces (%u x %u)",
                path, pixelWidth, pixelHeight);
        if (own_bytes) free(blob);
        return NULL;
    }
    if (supercompressionScheme != 0 && supercompressionScheme != 2) {
        ktx2_log("[ktx2] %s: supercompression scheme %u not supported "
                "(only 0=raw and 2=zstd)",
                path, supercompressionScheme);
        if (own_bytes) free(blob);
        return NULL;
    }
    bool zstd_supercompressed = (supercompressionScheme == 2);
    if (levelCount == 0) {
        ktx2_log("[ktx2] %s: levelCount=0 (runtime mip generation) "
                "not supported", path);
        if (own_bytes) free(blob);
        return NULL;
    }

    /* Level index begins at offset 80 (12 magic + 68 header). Each
     * entry is 24 bytes (3× uint64_t). */
    size_t lvl_index_offset = 80;
    size_t lvl_index_size   = (size_t)levelCount * 24u;
    if (blob_size < lvl_index_offset + lvl_index_size) {
        ktx2_log("[ktx2] %s: file truncated before level index", path);
        if (own_bytes) free(blob);
        return NULL;
    }

    Ktx2Level *levels      = (Ktx2Level *)calloc(levelCount, sizeof *levels);
    uint8_t  **level_owned = (uint8_t  **)calloc(levelCount, sizeof *level_owned);
    if (!levels || !level_owned) {
        if (status) *status = KTX2_OPEN_RESOURCE_FAILURE;
        free(levels); free(level_owned); if (own_bytes) free(blob);
        return NULL;
    }
    for (uint32_t i = 0; i < levelCount; i++) {
        const uint8_t *q = blob + lvl_index_offset + i * 24u;
        uint64_t off = rd_u64_le(q + 0);
        uint64_t len = rd_u64_le(q + 8);
        uint64_t ulen= rd_u64_le(q + 16);
        if (off > blob_size || off + len > blob_size) {
            ktx2_log("[ktx2] %s: level %u index out of range "
                    "(off=%llu len=%llu file=%zu)",
                    path, i, (unsigned long long)off,
                    (unsigned long long)len, blob_size);
            for (uint32_t j = 0; j < i; j++) free(level_owned[j]);
            free(level_owned); free(levels); if (own_bytes) free(blob);
            return NULL;
        }
        int lw = (int)(pixelWidth  >> i); if (lw < 1) lw = 1;
        int lh = (int)(pixelHeight >> i); if (lh < 1) lh = 1;
        if (zstd_supercompressed) {
            /* Allocate a buffer sized to the recorded uncompressed
             * length and decompress into it. We trust ulen — the
             * writer set it from the source data, and zstd's frame
             * also encodes the original size which we double-check
             * via the return value. */
            uint8_t *out = (uint8_t *)malloc((size_t)ulen);
            if (!out) {
                if (status) *status = KTX2_OPEN_RESOURCE_FAILURE;
                ktx2_log("[ktx2] %s: level %u alloc %llu failed",
                        path, i, (unsigned long long)ulen);
                for (uint32_t j = 0; j < i; j++) free(level_owned[j]);
                free(level_owned); free(levels); if (own_bytes) free(blob);
                return NULL;
            }
            size_t got = ZSTD_decompress(out, (size_t)ulen,
                                         blob + off, (size_t)len);
            if (ZSTD_isError(got) || got != (size_t)ulen) {
                ktx2_log("[ktx2] %s: level %u zstd decompress failed (%s)",
                        path, i,
                        ZSTD_isError(got) ? ZSTD_getErrorName(got)
                                          : "size mismatch");
                free(out);
                for (uint32_t j = 0; j < i; j++) free(level_owned[j]);
                free(level_owned); free(levels); if (own_bytes) free(blob);
                return NULL;
            }
            level_owned[i]   = out;
            levels[i].data   = out;
            levels[i].size   = (size_t)ulen;
        } else {
            level_owned[i]   = NULL;
            levels[i].data   = blob + off;
            levels[i].size   = (size_t)len;
        }
        levels[i].width  = lw;
        levels[i].height = lh;
    }

    Ktx2 *k = (Ktx2 *)calloc(1, sizeof *k);
    if (!k) {
        if (status) *status = KTX2_OPEN_RESOURCE_FAILURE;
        for (uint32_t i = 0; i < levelCount; i++) free(level_owned[i]);
        free(level_owned); free(levels); if (own_bytes) free(blob);
        return NULL;
    }
    /* Only register `blob` for free() when this Ktx2 owns it. In
     * mem-mode the caller retains ownership of the source bytes and
     * raw (non-zstd) level data pointers index into them directly. */
    k->blob             = own_bytes ? blob : NULL;
    k->blob_size        = blob_size;
    k->format           = fmt;
    k->width            = (int)pixelWidth;
    k->height           = (int)pixelHeight;
    k->level_count      = (int)levelCount;
    k->face_count       = (int)faceCount;
    k->block_compressed = block_compressed;
    k->bytes_per_block  = bytes_per_block;
    k->levels           = levels;
    k->level_owned      = level_owned;
    if (status) *status = KTX2_OPEN_SUCCESS;
    return k;
}

Ktx2 *ktx2_open(const char *path)
{
    if (!path) return NULL;
    size_t blob_size = 0;
    uint8_t *blob = slurp(path, &blob_size);
    if (!blob) return NULL;
    /* ktx2_parse_bytes takes ownership on success; on failure it
     * free()s blob itself. Either way, the caller is off the hook. */
    return ktx2_parse_bytes(blob, blob_size, path, true, NULL);
}

Ktx2 *ktx2_open_mem(const uint8_t *bytes, size_t size, const char *src_label)
{
    return ktx2_parse_bytes(bytes, size, src_label, false, NULL);
}

Ktx2 *ktx2_open_mem_status(const uint8_t *bytes, size_t size, const char *src_label,
                           Ktx2OpenStatus *status)
{
    return ktx2_parse_bytes(bytes, size, src_label, false, status);
}

void ktx2_close(Ktx2 *k)
{
    if (!k) return;
    if (k->level_owned) {
        for (int i = 0; i < k->level_count; i++)
            free(k->level_owned[i]);
        free(k->level_owned);
    }
    free(k->levels);
    free(k->blob);
    free(k);
}

AeronTextureFormat ktx2_format     (const Ktx2 *k) { return k ? k->format       : AERON_TEXTURE_FORMAT_UNKNOWN; }
int                  ktx2_width      (const Ktx2 *k) { return k ? k->width        : 0; }
int                  ktx2_height     (const Ktx2 *k) { return k ? k->height       : 0; }
int                  ktx2_level_count(const Ktx2 *k) { return k ? k->level_count  : 0; }
int                  ktx2_face_count (const Ktx2 *k) { return k ? k->face_count   : 0; }
bool                 ktx2_is_block_compressed(const Ktx2 *k)
{
    return k && k->block_compressed;
}

Ktx2Level ktx2_level(const Ktx2 *k, int level)
{
    if (!k || level < 0 || level >= k->level_count)
        return (Ktx2Level){ .data = NULL, .size = 0, .width = 0, .height = 0 };
    return k->levels[level];
}

Ktx2Level ktx2_level_face(const Ktx2 *k, int level, int face)
{
    Ktx2Level lvl = ktx2_level(k, level);
    if (!lvl.data || k->face_count <= 0 ||
        face < 0 || face >= k->face_count)
        return (Ktx2Level){ .data = NULL, .size = 0, .width = 0, .height = 0 };
    /* KTX2 stores face data contiguously within a level; the spec
     * guarantees equal face_size across all faces of a given level,
     * so face_size = level.size / face_count. */
    size_t face_size = lvl.size / (size_t)k->face_count;
    return (Ktx2Level){
        .data   = lvl.data + (size_t)face * face_size,
        .size   = face_size,
        .width  = lvl.width,
        .height = lvl.height,
    };
}

void ktx2_block_info(const Ktx2 *k, int *block_w, int *block_h, int *bpb)
{
    int bw = 0, bh = 0, b = 0;
    if (k && k->block_compressed) {
        /* All BC formats use 4×4 blocks. */
        bw = 4; bh = 4; b = k->bytes_per_block;
    }
    if (block_w) *block_w = bw;
    if (block_h) *block_h = bh;
    if (bpb)     *bpb     = b;
}
