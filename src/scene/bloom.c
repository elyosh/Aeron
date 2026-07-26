/*
 * HD bloom post-process — see flight_bloom.h for the public design.
 *
 * Algorithm: dual-filter Kawase chain (½, ¼, ⅛, ...) of the flight RT.
 * Each downsample is a 4-tap bilinear box, each upsample a 4-tap
 * bilinear tent. Bandwidth-friendly for PS4 GCN at 4K base.
 *
 * Pass sequence per call (with N = BLOOM_LEVEL_COUNT):
 *   1.      bright-pass    flight_rt   → mip0   (clear, scissored to y < bar)
 *   2..N.   downsample     mip[i-1]    → mip[i] (clear)
 *   N+1..2N-1. upsample    mip[i]      → mip[i-1] (additive into prev)
 *
 * NO dedicated final-composite pass — mip0's accumulated bloom is
 * sampled directly by the final present shader (flight_tonemap.frag),
 * which folds the additive bloom into the HDR-scene tonemap before
 * PMA-emitting to the swapchain. That saves a full-resolution render
 * pass on the flight RT (~70 MB BW + ~150 µs GPU at 4K) at the cost
 * of one extra sampler binding and 4 extra texture taps in the
 * present quad.
 *
 * Bright-pass scissor: the masked rows arrive at mip0 as cleared
 * zeros, so bloom can't ORIGINATE from the message bar. The chain
 * spreads bloom slightly across the boundary so mip0 below the bar
 * is not exactly zero; the present shader suppresses the bloom
 * contribution below `bar_y_uv` to prevent any bleed onto the bar.
 */

#include <stdio.h>
#include "aeron/scene/bloom.h"

#include "internal.h"

#include "aeron/render.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ===== Tuned constants (v1: hardcoded) ============================== */

/* Bright-pass threshold. The flight RT is R11G11B10_UFLOAT and
 * emissive pixel shaders (laser bolts, explosion cores) write
 * values >1.0; everything else (lit ship hulls, cockpit chrome,
 * skybox tonemap-input, HUD widgets) stays ≤ 1.0 by construction.
 * A hard 1.0 threshold cleanly separates the two. */
#define BLOOM_THRESHOLD 1.0f

/* Soft-knee width above the threshold. The bright-pass smoothstep
 * ramps from 0 at br=thr to 1 at br=thr+knee. 0.15 gives a gentle
 * ramp into the emissive band so pixels skimming just above 1.0
 * (e.g. from bilinear interpolation between an emissive face and a
 * non-emissive neighbour) don't pop in/out frame-to-frame. */
#define BLOOM_KNEE      0.3f

/* Post-chain multiplier the swapchain composite applies to the
 * sampled bloom contribution. Emissive sources carry their own HDR
 * magnitude into the chain; this controls "how much halo" only.
 * The composite shader's ACES tonemap folds the (scene + bloom) HDR
 * result into the [0, 1] swapchain range. Lower than pre-tonemap
 * values: when the tonemap was just a soft-shoulder clamp, bloom
 * needed to punch through it; with a real tonemap doing the
 * compression, the bloom contribution composes additively in linear
 * HDR and the curve handles the rest. */
#define BLOOM_INTENSITY 0.5f

/* Chain depth. Smallest mip = rt_dim / (1 << BLOOM_LEVEL_COUNT).
 * Each added level halves the smallest mip's spatial extent and roughly
 * doubles the bloom radius. */
#define BLOOM_LEVEL_COUNT 4

/* Chain mip format. R11G11B10_UFLOAT — 4 B/pixel, float range so
 * the additive upsample can accumulate >1.0 per channel without
 * clipping. No alpha channel; chain shaders only read `.rgb` and
 * the upsample's `a=0` write is discarded. Supported on Metal,
 * Vulkan, D3D12. */
#define BLOOM_CHAIN_FORMAT AERON_TEXTURE_FORMAT_R11G11B10_UFLOAT

typedef struct AeronSceneBloomLevel {
    AeronRenderTarget *tex;
    int             w;
    int             h;
} AeronSceneBloomLevel;

struct AeronSceneBloom {
    int                      rt_w;
    int                      rt_h;

    /* Fullscreen-quad vertex shader — reused from the existing
     * two-RT compositor (same SV_VertexID 0..3 TRIANGLESTRIP convention,
     * same VSOut layout). No bloom-specific VS needed. */
    AeronShader           *vs;
    AeronShader           *brightpass_ps;
    AeronShader           *downsample_ps;
    AeronShader           *upsample_ps;

    AeronGraphicsPipeline *brightpass_pipeline;
    AeronGraphicsPipeline *downsample_pipeline;
    AeronGraphicsPipeline *upsample_pipeline;       /* additive blend */

    /* Linear+clamp sampler for every chain tap. */
    AeronSampler          *sampler;

    AeronSceneBloomLevel         levels[BLOOM_LEVEL_COUNT];

    bool                     ready;
};

/* ===== Pipeline creation ============================================ */

/* Two blend-state flavours: OPAQUE (bright-pass, downsample) and
 * ADDITIVE (upsample, final composite). All three target a single
 * BLOOM_CHAIN_FORMAT (R11G11B10_UFLOAT) mip — the flight RT format
 * is not used here. No depth attachment. */
static AeronGraphicsPipeline *create_pipeline(AeronShader *vs,
                                              AeronShader *ps,
                                              AeronTextureFormat fmt,
                                              bool additive)
{
    AeronBlendStateDesc bs;
    if (additive) {
        /* Additive (src + dst). Alpha additive too, but the upsample
         * shader emits a=0 so the destination alpha (used by the
         * swapchain PMA-over composite) is preserved untouched. */
        bs = (AeronBlendStateDesc){
            .enabled   = 1,
            .src_color = AERON_BLEND_ONE,
            .dst_color = AERON_BLEND_ONE,
            .color_op  = AERON_BLEND_OP_ADD,
            .src_alpha = AERON_BLEND_ONE,
            .dst_alpha = AERON_BLEND_ONE,
            .alpha_op  = AERON_BLEND_OP_ADD,
        };
    } else {
        bs = AeronSceneInternal_BlendOpaque();
    }
    return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc){
        .vertex_shader   = vs,
        .fragment_shader = ps,
        .primitive_type  = AERON_PRIMITIVE_TRIANGLE_STRIP,
        .cull_mode       = AERON_CULL_NONE,
        .color_format    = fmt,
        .blend           = bs,
    });
}

/* ===== Lifecycle =================================================== */

AeronSceneBloom *AeronSceneBloom_Create(int rt_w, int rt_h)
{
    if (rt_w <= 0 || rt_h <= 0) return NULL;
    AeronSceneBloom *b = (AeronSceneBloom *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->rt_w      = rt_w;
    b->rt_h      = rt_h;

    /* Shaders. Shared full-screen VS reused from composite_two_rt.vert.
     * Each FS binds (1 sampler + 1 uniform buffer). */
    b->vs = AeronSceneInternal_CompileShader("scene_fullscreen_quad.vert",
                                      AERON_SHADER_STAGE_VERTEX, 0, 0, 0);
    b->brightpass_ps = AeronSceneInternal_CompileShader("scene_bloom_brightpass.frag",
                                      AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
    b->downsample_ps = AeronSceneInternal_CompileShader("scene_bloom_downsample.frag",
                                      AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
    b->upsample_ps   = AeronSceneInternal_CompileShader("scene_bloom_upsample.frag",
                                      AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
    if (!b->vs || !b->brightpass_ps || !b->downsample_ps || !b->upsample_ps) {
        fprintf(stderr, "[flight_bloom] shader compile failed\n");
        AeronSceneBloom_Destroy(b);
        return NULL;
    }

    b->brightpass_pipeline = create_pipeline(b->vs, b->brightpass_ps,
                                             BLOOM_CHAIN_FORMAT, /*additive=*/false);
    b->downsample_pipeline = create_pipeline(b->vs, b->downsample_ps,
                                             BLOOM_CHAIN_FORMAT, /*additive=*/false);
    b->upsample_pipeline   = create_pipeline(b->vs, b->upsample_ps,
                                             BLOOM_CHAIN_FORMAT, /*additive=*/true);
    if (!b->brightpass_pipeline || !b->downsample_pipeline ||
        !b->upsample_pipeline) {
        fprintf(stderr, "[flight_bloom] pipeline create failed\n");
        AeronSceneBloom_Destroy(b);
        return NULL;
    }

    /* Linear+clamp sampler. Bilinear taps are central to the dual-filter
     * trick — switching to NEAREST would halve the effective kernel
     * footprint per mip. */
    b->sampler = Aeron_CreateSampler(&(AeronSamplerDesc){
        .min_filter = AERON_FILTER_LINEAR,
        .mag_filter = AERON_FILTER_LINEAR,
        .mip_filter = AERON_FILTER_NEAREST,
        .address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
        .address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
        .address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
        .min_lod    = 0.0f,
        .max_lod    = 0.0f,
    });
    if (!b->sampler) {
        fprintf(stderr, "[flight_bloom] sampler create failed\n");
        AeronSceneBloom_Destroy(b);
        return NULL;
    }

    /* Chain mips at 1/2, 1/4, 1/8 of the flight RT. Clamp to 1×1 so
     * very narrow RT shapes don't produce zero-sized textures. */
    for (int i = 0; i < BLOOM_LEVEL_COUNT; ++i) {
        int divisor = 1 << (i + 1);
        int lw = rt_w / divisor; if (lw < 1) lw = 1;
        int lh = rt_h / divisor; if (lh < 1) lh = 1;
        b->levels[i].w   = lw;
        b->levels[i].h   = lh;
        char name[32];
        snprintf(name, sizeof name, "bloom.level%d", i);
        b->levels[i].tex = AeronSceneInternal_CreateColorRt(BLOOM_CHAIN_FORMAT, lw, lh, name);
        if (!b->levels[i].tex) {
            fprintf(stderr, "[flight_bloom] mip %d (%dx%d) create failed\n",
                    i, lw, lh);
            AeronSceneBloom_Destroy(b);
            return NULL;
        }
    }

    b->ready = true;
    return b;
}

void AeronSceneBloom_Destroy(AeronSceneBloom *b)
{
    if (!b) return;
    for (int i = 0; i < BLOOM_LEVEL_COUNT; ++i) {
        if (b->levels[i].tex) Aeron_DestroyRenderTarget(b->levels[i].tex);
    }
    if (b->brightpass_pipeline)
        Aeron_DestroyGraphicsPipeline(b->brightpass_pipeline);
    if (b->downsample_pipeline)
        Aeron_DestroyGraphicsPipeline(b->downsample_pipeline);
    if (b->upsample_pipeline)
        Aeron_DestroyGraphicsPipeline(b->upsample_pipeline);
    if (b->vs)            Aeron_DestroyShader(b->vs);
    if (b->brightpass_ps) Aeron_DestroyShader(b->brightpass_ps);
    if (b->downsample_ps) Aeron_DestroyShader(b->downsample_ps);
    if (b->upsample_ps)   Aeron_DestroyShader(b->upsample_ps);
    if (b->sampler)       Aeron_DestroySampler(b->sampler);
    free(b);
}

/* ===== Apply ======================================================== */

/* Internal helper: open one pass on (tex, w, h), set viewport+scissor,
 * draw one fullscreen quad sampling `src` with the bound pipeline +
 * a single `uniforms` cbuffer at slot 0. */
static int bloom_pass(AeronCommandBuffer *cmd,
                       AeronRenderTarget *dst, int dst_w, int dst_h,
                       AeronGraphicsPipeline *pipe,
                       AeronTexture *src,
                       AeronSampler *sampler,
                       const void *uniforms, size_t uniform_size,
                       bool clear,
                       const AeronRectI *scissor /*NULL → full RT*/,
                       const char *label)
{
    AeronRenderPass *pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc){
        .color_target     = dst,
        .viewport         = { 0, 0, dst_w, dst_h },
        .scissor          = scissor ? *scissor : (AeronRectI){ 0, 0, 0, 0 },
        .clear_color      = clear ? 1 : 0,
        .clear_color_rgba = { 0.0f, 0.0f, 0.0f, 0.0f },
        .command_buffer   = cmd,
        .debug_label      = label,
    });
    if (!pass) return 0;

    Aeron_BindGraphicsPipeline(pass, pipe);
    Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0, src, sampler);
    Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0,
                          uniforms, (uint32_t)uniform_size);
    Aeron_Draw(pass, 4, 0);

    Aeron_EndRenderPass(pass);
    return 1;
}

int AeronSceneBloom_Apply(AeronSceneBloom *b,
                        AeronCommandBuffer *cmd,
                        AeronTexture *flight_color_rt,
                        int rt_w, int rt_h,
                        int scissor_max_y)
{
    if (!b || !b->ready || !cmd || !flight_color_rt) return 0;
    if (rt_w <= 0 || rt_h <= 0) return 0;

    Aeron_GpuDebugPush(cmd, "Bloom");

    /* Scissor-eligibility: a value of [0, rt_h) confines the bright
     * pass to the top portion of mip0 (message-bar exclusion at the
     * source). The swapchain composite handles the destination-side
     * exclusion via its own bar_y_uv gate. */
    bool       has_scissor = (scissor_max_y > 0 && scissor_max_y < rt_h);
    AeronRectI bright_scissor = { 0, 0, b->levels[0].w, b->levels[0].h };
    if (has_scissor) {
        /* Map the flight-RT pixel y onto the half-res mip0 coordinate. */
        int max_y_mip0 = (scissor_max_y * b->levels[0].h) / rt_h;
        if (max_y_mip0 < 1) max_y_mip0 = 1;
        if (max_y_mip0 > b->levels[0].h) max_y_mip0 = b->levels[0].h;
        bright_scissor.height = max_y_mip0;
    }

    /* --- 1. Bright pass: flight RT → mip0 ----------------------- */
    {
        struct {
            float params[4];   /* x=threshold, y=knee, z/w unused */
        } u = { { BLOOM_THRESHOLD, BLOOM_KNEE, 0.0f, 0.0f } };
        if (!bloom_pass(cmd,
                        b->levels[0].tex, b->levels[0].w, b->levels[0].h,
                        b->brightpass_pipeline,
                        flight_color_rt, b->sampler,
                        &u, sizeof u,
                        /*clear=*/true,
                        has_scissor ? &bright_scissor : NULL,
                        "Bloom brightpass")) {
            Aeron_GpuDebugPop(cmd);
            return 0;
        }
    }

    /* --- 2..3. Downsample chain --------------------------------- */
    for (int i = 1; i < BLOOM_LEVEL_COUNT; ++i) {
        const AeronSceneBloomLevel *src = &b->levels[i - 1];
        const AeronSceneBloomLevel *dst = &b->levels[i];
        struct {
            float src_texel[4];  /* xy = 1/src_size */
        } u = { { 1.0f / (float)src->w, 1.0f / (float)src->h, 0.0f, 0.0f } };
        char label[32];
        snprintf(label, sizeof label, "Bloom down %d->%d", i - 1, i);
        if (!bloom_pass(cmd,
                        dst->tex, dst->w, dst->h,
                        b->downsample_pipeline,
                        Aeron_RenderTargetGetTexture(src->tex), b->sampler,
                        &u, sizeof u,
                        /*clear=*/true,
                        NULL,
                        label)) {
            Aeron_GpuDebugPop(cmd);
            return 0;
        }
    }

    /* --- 4..5. Upsample chain (additive into next-larger mip) --- */
    for (int i = BLOOM_LEVEL_COUNT - 1; i > 0; --i) {
        const AeronSceneBloomLevel *src = &b->levels[i];
        const AeronSceneBloomLevel *dst = &b->levels[i - 1];
        struct {
            float dst_texel[4];  /* xy = 1/dst_size, z = intensity */
        } u = { { 1.0f / (float)dst->w, 1.0f / (float)dst->h,
                  1.0f /*chain-internal intensity*/, 0.0f } };
        char label[32];
        snprintf(label, sizeof label, "Bloom up %d->%d", i, i - 1);
        if (!bloom_pass(cmd,
                        dst->tex, dst->w, dst->h,
                        b->upsample_pipeline,
                        Aeron_RenderTargetGetTexture(src->tex), b->sampler,
                        &u, sizeof u,
                        /*clear=*/false,   /* additive onto existing downsample result */
                        NULL,
                        label)) {
            Aeron_GpuDebugPop(cmd);
            return 0;
        }
    }

    /* No final composite — mip0 is sampled directly by the swapchain
     * composite shader. `flight_color_rt` is only used as the bright-
     * pass sampling source above. */

    Aeron_GpuDebugPop(cmd);
    return 1;
}

/* ===== Accessors =================================================== */

AeronRenderTarget *AeronSceneBloom_ColorRt(const AeronSceneBloom *b)
{
    if (!b || !b->ready) return NULL;
    return b->levels[0].tex;
}

static float s_bloom_intensity = BLOOM_INTENSITY;

float AeronSceneBloom_Intensity(void)
{
    return s_bloom_intensity;
}

void AeronSceneBloom_SetIntensity(float v)
{
    if (v < 0.0f) v = 0.0f;
    s_bloom_intensity = v;
}
