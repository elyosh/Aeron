#include "capture.h"

#include <string.h>

static AeronRenderTarget* color_target(int width, int height, const char* name) {
	return Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = width, .height = height, .format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT, .debug_name = name });
}

int CaptureBloom_Create(CaptureBloom* b, const CaptureConfig* c, int readback) {
	memset(b, 0, sizeof *b);
	/* Captures retain the raw chain even at zero intensity; benchmarks follow
	 * the game's zero-intensity fast path and always include presentation. */
	if (c->bloom_enabled && (readback || c->bloom_intensity > 0)) {
		b->bloom = AeronSceneBloom_Create(c->width, c->height);
		if (!b->bloom)
			return 0;
		AeronTexture* texture = Aeron_RenderTargetGetTexture(AeronSceneBloom_ColorRt(b->bloom));
		b->width              = Aeron_TextureGetWidth(texture);
		b->height             = Aeron_TextureGetHeight(texture);
		if (readback) {
			/* The capture runner allocates staging at scene resolution. */
			if (b->width <= 0 || b->height <= 0 || b->width > c->width || b->height > c->height)
				goto fail;
			b->readback_rt = color_target(b->width, b->height, "capture.bloom.readback");
			b->draws       = AeronDrawList_Create(1);
			if (!b->readback_rt || !b->draws)
				goto fail;
		}
	}
	b->present_rt = color_target(c->width, c->height, "capture.present");
	b->present    = AeronScenePresentChain_Create(AERON_TEXTURE_FORMAT_RGBA16_FLOAT);
	b->sampler    = Aeron_CreateSampler(&(AeronSamplerDesc) { .min_filter = AERON_FILTER_LINEAR,
															  .mag_filter = AERON_FILTER_LINEAR,
															  .address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
															  .address_v  = AERON_ADDRESS_CLAMP_TO_EDGE });
	if (!b->present_rt || !b->present || !b->sampler)
		goto fail;
	return 1;
fail:
	CaptureBloom_Destroy(b);
	return 0;
}

void CaptureBloom_Destroy(CaptureBloom* b) {
	AeronDrawList_Destroy(b->draws);
	Aeron_DestroySampler(b->sampler);
	AeronScenePresentChain_Destroy(b->present);
	Aeron_DestroyRenderTarget(b->present_rt);
	Aeron_DestroyRenderTarget(b->readback_rt);
	AeronSceneBloom_Destroy(b->bloom);
	memset(b, 0, sizeof *b);
}

int CaptureBloom_Record(CaptureBloom* b, const CaptureConfig* c, AeronRenderTarget* source,
						AeronCommandBuffer* cmd) {
	AeronTexture* scene = Aeron_RenderTargetGetTexture(source);
	AeronTexture* bloom = NULL;
	if (b->bloom) {
		if (!AeronSceneBloom_Apply(b->bloom, cmd, scene, c->width, c->height, c->height))
			return 0;
		bloom = Aeron_RenderTargetGetTexture(AeronSceneBloom_ColorRt(b->bloom));
	}
	if (b->readback_rt) {
		/* All finite R11G11B10 values fit in half floats. This conversion is
		 * diagnostic work and is never recorded by the benchmark. */
		AeronDrawList_Begin(b->draws, b->readback_rt, b->width, b->height, AERON_DRAWLIST2D_CLEAR, NULL);
		AeronDrawList_AddSprite(b->draws, &(AeronDrawList2DSprite) { .texture = bloom,
																	 .src_u1  = 1,
																	 .src_v1  = 1,
																	 .dst_w   = (float)b->width,
																	 .dst_h   = (float)b->height,
																	 .tint    = { 1, 1, 1, 1 },
																	 .blend   = AERON_BLIT2D_BLEND_NONE,
																	 .filter = AERON_BLIT2D_FILTER_NEAREST });
		if (!AeronDrawList_Prepare(b->draws, cmd))
			return 0;
		AeronDrawList_Render(b->draws, cmd);
	}
	AeronRenderPass* pass = Aeron_BeginRenderPass(
		&(AeronRenderPassDesc) { .color_target   = b->present_rt,
								 .clear_color    = 1,
								 .viewport       = { 0, 0, c->width, c->height },
								 .command_buffer = cmd,
								 .debug_label    = "Capture production bloom and tonemap" });
	if (!pass)
		return 0;
	AeronScenePresentChain_Draw(b->present, pass, scene, b->sampler, bloom, c->bloom_intensity, c->width,
								c->height, 1, NULL, 0);
	Aeron_EndRenderPass(pass);
	return 1;
}

int CaptureBloom_Render(CaptureBloom* b, const CaptureConfig* c, AeronRenderTarget* source) {
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd)
		return 0;
	if (!CaptureBloom_Record(b, c, source, cmd)) {
		Aeron_CancelCommandBuffer(cmd);
		return 0;
	}
	return Aeron_SubmitCommandBuffer(cmd);
}
