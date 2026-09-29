/* Resolution-scaled dual-filter bloom. The four equally weighted bands at
 * 2160p define the reference look; other heights redistribute their weights
 * over the native half-resolution pyramid. Presentation composites mip0. */

#include "aeron/scene/bloom.h"
#include <stdio.h>

#include "internal.h"

#include "aeron/render.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLOOM_THRESHOLD 1.0f
#define BLOOM_KNEE 0.3f
#define BLOOM_INTENSITY 0.5f
#define BLOOM_REFERENCE_LEVEL_COUNT 4
#define BLOOM_MAX_LEVEL_COUNT 8
#define BLOOM_CHAIN_FORMAT AERON_TEXTURE_FORMAT_R11G11B10_UFLOAT

typedef struct AeronSceneBloomLevel {
	AeronRenderTarget* tex;
	int                w;
	int                h;
	float              weight;
} AeronSceneBloomLevel;

struct AeronSceneBloom {
	int rt_w;
	int rt_h;
	int level_count;

	/* Fullscreen-quad vertex shader — reused from the existing
	 * two-RT compositor (same SV_VertexID 0..3 TRIANGLESTRIP convention,
	 * same VSOut layout). No bloom-specific VS needed. */
	AeronShader* vs;
	AeronShader* brightpass_ps;
	AeronShader* downsample_ps;
	AeronShader* upsample_ps;

	AeronGraphicsPipeline* brightpass_pipeline;
	AeronGraphicsPipeline* downsample_pipeline;
	AeronGraphicsPipeline* upsample_pipeline; /* weighted additive blend */

	/* Linear+clamp sampler for every chain tap. */
	AeronSampler* sampler;

	AeronSceneBloomLevel levels[BLOOM_MAX_LEVEL_COUNT];

	bool ready;
};

/* ===== Pipeline creation ============================================ */

/* Upsampling adds the coarser contribution and weights the destination's
 * existing band using fragment alpha. The chain format stores RGB only. */
static AeronGraphicsPipeline* create_pipeline(AeronShader* vs, AeronShader* ps, AeronTextureFormat fmt,
											  bool additive) {
	AeronBlendStateDesc bs;
	if (additive) {
		bs = (AeronBlendStateDesc) {
			.enabled   = 1,
			.src_color = AERON_BLEND_ONE,
			.dst_color = AERON_BLEND_SRC_ALPHA,
			.color_op  = AERON_BLEND_OP_ADD,
			.src_alpha = AERON_BLEND_ONE,
			.dst_alpha = AERON_BLEND_ONE,
			.alpha_op  = AERON_BLEND_OP_ADD,
		};
	} else {
		bs = AeronSceneInternal_BlendOpaque();
	}
	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader   = vs,
		.fragment_shader = ps,
		.primitive_type  = AERON_PRIMITIVE_TRIANGLE_STRIP,
		.cull_mode       = AERON_CULL_NONE,
		.color_format    = fmt,
		.blend           = bs,
	});
}

/* Squared filter radius grows by four per pyramid level. Interpolate in
 * that space so fractional resolutions change continuously. Clamping bands
 * finer than mip0 preserves their energy at the available spatial detail. */
static void configure_levels(AeronSceneBloom* b) {
	float scale          = (float)b->rt_h / AERON_SCENE_BLOOM_REFERENCE_HEIGHT;
	float radius_squared = scale * scale;
	b->level_count       = 1;
	for (int band = 0; band < BLOOM_REFERENCE_LEVEL_COUNT; ++band, radius_squared *= 4.0f) {
		int   level = 0;
		float lower = 1.0f;
		while (level < BLOOM_MAX_LEVEL_COUNT - 1 && radius_squared >= lower * 4.0f) {
			++level;
			lower *= 4.0f;
		}
		float fraction = radius_squared > lower ? (radius_squared - lower) / (3.0f * lower) : 0.0f;
		if (level == BLOOM_MAX_LEVEL_COUNT - 1)
			fraction = 0.0f;
		b->levels[level].weight += 1.0f - fraction;
		if (fraction > 0.0f) {
			b->levels[++level].weight += fraction;
		}
		if (b->level_count < level + 1)
			b->level_count = level + 1;
	}
}

/* ===== Lifecycle =================================================== */

AeronSceneBloom* AeronSceneBloom_Create(int rt_w, int rt_h) {
	if (rt_w <= 0 || rt_h <= 0)
		return NULL;
	AeronSceneBloom* b = (AeronSceneBloom*)calloc(1, sizeof *b);
	if (!b)
		return NULL;
	b->rt_w = rt_w;
	b->rt_h = rt_h;
	configure_levels(b);

	/* Shaders. Shared full-screen VS reused from composite_two_rt.vert.
	 * Each FS binds (1 sampler + 1 uniform buffer). */
	b->vs =
		AeronSceneInternal_CompileShader("scene_fullscreen_quad.vert", AERON_SHADER_STAGE_VERTEX, 0, 0, 0);
	b->brightpass_ps =
		AeronSceneInternal_CompileShader("scene_bloom_brightpass.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
	b->downsample_ps =
		AeronSceneInternal_CompileShader("scene_bloom_downsample.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
	b->upsample_ps =
		AeronSceneInternal_CompileShader("scene_bloom_upsample.frag", AERON_SHADER_STAGE_FRAGMENT, 1, 1, 0);
	if (!b->vs || !b->brightpass_ps || !b->downsample_ps || !b->upsample_ps) {
		Aeron_LogError("aeron.scene", "bloom shader compilation failed");
		AeronSceneBloom_Destroy(b);
		return NULL;
	}

	b->brightpass_pipeline = create_pipeline(b->vs, b->brightpass_ps, BLOOM_CHAIN_FORMAT, /*additive=*/false);
	b->downsample_pipeline = create_pipeline(b->vs, b->downsample_ps, BLOOM_CHAIN_FORMAT, /*additive=*/false);
	b->upsample_pipeline   = create_pipeline(b->vs, b->upsample_ps, BLOOM_CHAIN_FORMAT, /*additive=*/true);
	if (!b->brightpass_pipeline || !b->downsample_pipeline || !b->upsample_pipeline) {
		Aeron_LogError("aeron.scene", "bloom pipeline creation failed");
		AeronSceneBloom_Destroy(b);
		return NULL;
	}

	/* Linear+clamp sampler. Bilinear taps are central to the dual-filter
	 * trick — switching to NEAREST would halve the effective kernel
	 * footprint per mip. */
	b->sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
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
		Aeron_LogError("aeron.scene", "bloom sampler creation failed");
		AeronSceneBloom_Destroy(b);
		return NULL;
	}

	/* Allocate through the last contributing level. Clamp narrow targets to 1x1. */
	for (int i = 0; i < b->level_count; ++i) {
		int divisor = 1 << (i + 1);
		int lw      = rt_w / divisor;
		if (lw < 1)
			lw = 1;
		int lh = rt_h / divisor;
		if (lh < 1)
			lh = 1;
		b->levels[i].w = lw;
		b->levels[i].h = lh;
		char name[32];
		snprintf(name, sizeof name, "bloom.level%d", i);
		b->levels[i].tex = AeronSceneInternal_CreateColorRt(BLOOM_CHAIN_FORMAT, lw, lh, name);
		if (!b->levels[i].tex) {
			Aeron_LogError("aeron.scene", "bloom mip %d (%dx%d) creation failed", i, lw, lh);
			AeronSceneBloom_Destroy(b);
			return NULL;
		}
	}

	b->ready = true;
	return b;
}

void AeronSceneBloom_Destroy(AeronSceneBloom* b) {
	if (!b)
		return;
	for (int i = 0; i < b->level_count; ++i) {
		if (b->levels[i].tex)
			Aeron_DestroyRenderTarget(b->levels[i].tex);
	}
	if (b->brightpass_pipeline)
		Aeron_DestroyGraphicsPipeline(b->brightpass_pipeline);
	if (b->downsample_pipeline)
		Aeron_DestroyGraphicsPipeline(b->downsample_pipeline);
	if (b->upsample_pipeline)
		Aeron_DestroyGraphicsPipeline(b->upsample_pipeline);
	if (b->vs)
		Aeron_DestroyShader(b->vs);
	if (b->brightpass_ps)
		Aeron_DestroyShader(b->brightpass_ps);
	if (b->downsample_ps)
		Aeron_DestroyShader(b->downsample_ps);
	if (b->upsample_ps)
		Aeron_DestroyShader(b->upsample_ps);
	if (b->sampler)
		Aeron_DestroySampler(b->sampler);
	free(b);
}

/* ===== Apply ======================================================== */

/* Internal helper: open one pass on (tex, w, h), set viewport+scissor,
 * draw one fullscreen quad sampling `src` with the bound pipeline +
 * a single `uniforms` cbuffer at slot 0. */
static int bloom_pass(AeronCommandBuffer* cmd, AeronRenderTarget* dst, int dst_w, int dst_h,
					  AeronGraphicsPipeline* pipe, AeronTexture* src, AeronSampler* sampler,
					  const void* uniforms, size_t uniform_size, bool clear,
					  const AeronRectI* scissor /*NULL → full RT*/, const char* label) {
	AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target     = dst,
		.viewport         = { 0, 0, dst_w, dst_h },
		.scissor          = scissor ? *scissor : (AeronRectI) { 0, 0, 0, 0 },
		.clear_color      = clear ? 1 : 0,
		.clear_color_rgba = { 0.0f, 0.0f, 0.0f, 0.0f },
		.command_buffer   = cmd,
		.debug_label      = label,
	});
	if (!pass)
		return 0;

	Aeron_BindGraphicsPipeline(pass, pipe);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0, src, sampler);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, uniforms, (uint32_t)uniform_size);
	Aeron_Draw(pass, 4, 0);

	Aeron_EndRenderPass(pass);
	return 1;
}

int AeronSceneBloom_Apply(AeronSceneBloom* b, AeronCommandBuffer* cmd, AeronTexture* flight_color_rt,
						  int rt_w, int rt_h, int scissor_max_y) {
	if (!b || !b->ready || !cmd || !flight_color_rt)
		return 0;
	if (rt_w != b->rt_w || rt_h != b->rt_h)
		return 0;

	Aeron_GpuDebugPush(cmd, "Bloom");

	/* Scissor-eligibility: a value of [0, rt_h) confines the bright
	 * pass to the top portion of mip0 (message-bar exclusion at the
	 * source). The swapchain composite handles the destination-side
	 * exclusion via its own bar_y_uv gate. */
	bool       has_scissor    = (scissor_max_y > 0 && scissor_max_y < rt_h);
	AeronRectI bright_scissor = { 0, 0, b->levels[0].w, b->levels[0].h };
	if (has_scissor) {
		/* Map the flight-RT pixel y onto the half-res mip0 coordinate. */
		int max_y_mip0 = (scissor_max_y * b->levels[0].h) / rt_h;
		if (max_y_mip0 < 1)
			max_y_mip0 = 1;
		if (max_y_mip0 > b->levels[0].h)
			max_y_mip0 = b->levels[0].h;
		bright_scissor.height = max_y_mip0;
	}

	/* Collapsed finer bands can give mip0 weight >1. Apply that
	 * weight after extraction, then undo it in the first downsample so the
	 * remaining bands stay independent. All blend factors stay in [0,1]. */
	float first_weight = b->levels[0].weight > 1.0f ? b->levels[0].weight : 1.0f;

	/* Bright pass: flight RT -> mip0. */
	{
		struct {
			float params[4]; /* xy=threshold/knee, zw=source texel size */
			float weight[4];
		} u = { { BLOOM_THRESHOLD, BLOOM_KNEE, 1.0f / (float)rt_w, 1.0f / (float)rt_h },
				{ first_weight, 0, 0, 0 } };

		if (!bloom_pass(cmd, b->levels[0].tex, b->levels[0].w, b->levels[0].h, b->brightpass_pipeline,
						flight_color_rt, b->sampler, &u, sizeof u,
						/*clear=*/true, has_scissor ? &bright_scissor : NULL, "Bloom brightpass")) {
			Aeron_GpuDebugPop(cmd);
			return 0;
		}
	}

	/* Downsample unweighted bands. */
	for (int i = 1; i < b->level_count; ++i) {
		const AeronSceneBloomLevel* src = &b->levels[i - 1];
		const AeronSceneBloomLevel* dst = &b->levels[i];

		struct {
			float src_texel[4]; /* xy = 1/src_size, z = input normalization */
		} u = { { 1.0f / (float)src->w, 1.0f / (float)src->h, i == 1 ? 1.0f / first_weight : 1.0f, 0.0f } };

		char label[32];
		snprintf(label, sizeof label, "Bloom down %d->%d", i - 1, i);
		if (!bloom_pass(cmd, dst->tex, dst->w, dst->h, b->downsample_pipeline,
						Aeron_RenderTargetGetTexture(src->tex), b->sampler, &u, sizeof u,
						/*clear=*/true, NULL, label)) {
			Aeron_GpuDebugPop(cmd);
			return 0;
		}
	}

	/* Accumulate each band with its own weight, preserving total gain 4. */
	for (int i = b->level_count - 1; i > 0; --i) {
		const AeronSceneBloomLevel* src = &b->levels[i];
		const AeronSceneBloomLevel* dst = &b->levels[i - 1];

		struct {
			float dst_texel[4]; /* xy = 1/dst_size, zw = source/destination weights */
		} u = { { 1.0f / (float)dst->w, 1.0f / (float)dst->h, i == b->level_count - 1 ? src->weight : 1.0f,
				  i == 1 ? dst->weight / first_weight : dst->weight } };

		char label[32];
		snprintf(label, sizeof label, "Bloom up %d->%d", i, i - 1);
		if (!bloom_pass(cmd, dst->tex, dst->w, dst->h, b->upsample_pipeline,
						Aeron_RenderTargetGetTexture(src->tex), b->sampler, &u, sizeof u,
						/*clear=*/false, /* additive onto existing downsample result */
						NULL, label)) {
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

AeronRenderTarget* AeronSceneBloom_ColorRt(const AeronSceneBloom* b) {
	if (!b || !b->ready)
		return NULL;
	return b->levels[0].tex;
}

static float s_bloom_intensity = BLOOM_INTENSITY;

float AeronSceneBloom_Intensity(void) { return s_bloom_intensity; }

void AeronSceneBloom_SetIntensity(float v) {
	if (v < 0.0f)
		v = 0.0f;
	s_bloom_intensity = v;
}
