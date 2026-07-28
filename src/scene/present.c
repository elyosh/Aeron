/*
 * aeron_scene tonemap/present runtime knobs — see
 * aeron/scene/present.h. One process-wide state block
 * serves the present chain.
 */

#include "aeron/scene/present.h"

#include "aeron/log.h"
#include "internal.h"

#include <stdlib.h>

static const char* tonemap_op_name(int op) {
	switch (op) {
		case AERON_SCENE_TONEMAP_ACES:
			return "ACES Fitted";
		case AERON_SCENE_TONEMAP_AGX_PARAMETRIC:
			return "AGX (parametric)";
		default:
			return "(invalid)";
	}
}

static int s_tonemap_op = AERON_SCENE_TONEMAP_AGX_PARAMETRIC;

int AeronScenePresent_TonemapOp(void) { return s_tonemap_op; }

void AeronScenePresent_SetTonemapOp(int op) {
	if (op < 0 || op >= AERON_SCENE_TONEMAP_COUNT || op == s_tonemap_op) {
		return;
	}
	s_tonemap_op = op;
	Aeron_LogInfo("aeron.scene", "tonemap operator: %s", tonemap_op_name(s_tonemap_op));
}

void AeronScenePresent_ToggleTonemapOp(void) {
	s_tonemap_op = (s_tonemap_op + 1) % AERON_SCENE_TONEMAP_COUNT;
	Aeron_LogInfo("aeron.scene", "tonemap operator: %s", tonemap_op_name(s_tonemap_op));
}

static int s_bloom_kernel = AERON_SCENE_BLOOM_KERNEL_4_TAP;

int AeronScenePresent_BloomKernel(void) { return s_bloom_kernel; }

void AeronScenePresent_SetBloomKernel(int mode) {
	if (mode < 0 || mode >= AERON_SCENE_BLOOM_KERNEL_COUNT || mode == s_bloom_kernel) {
		return;
	}
	s_bloom_kernel = mode;
	Aeron_LogInfo("aeron.scene", "bloom kernel: %s",
			  mode == AERON_SCENE_BLOOM_KERNEL_1_TAP ? "1 tap" : "4 tap");
}

static float s_eotf_exponent = 2.2f;

float AeronScenePresent_EotfExponent(void) { return s_eotf_exponent; }

void AeronScenePresent_SetEotfExponent(float v) {
	if (v < 1.8f) {
		v = 1.8f;
	}
	if (v > 2.6f) {
		v = 2.6f;
	}
	s_eotf_exponent = v;
}

static float s_aces_exposure = 1.6f;

float AeronScenePresent_AcesExposure(void) { return s_aces_exposure; }

void AeronScenePresent_SetAcesExposure(float v) {
	if (v < 1.0f) {
		v = 1.0f;
	}
	if (v > 3.0f) {
		v = 3.0f;
	}
	s_aces_exposure = v;
}


/* ===== Present chain ================================================ */

struct AeronScenePresentChain {
	AeronTextureFormat     target_format;
	AeronShader*           vs;
	AeronShader*           ps_sdr;
	AeronShader*           ps_hdr;
	AeronGraphicsPipeline* pipeline_sdr;
	AeronGraphicsPipeline* pipeline_hdr;
};

static AeronGraphicsPipeline* present_pipeline(AeronShader* vs, AeronShader* ps,
											   AeronTextureFormat fmt) {
	AeronBlendStateDesc pma = { 0 };
	pma.enabled             = 1;
	pma.src_color           = AERON_BLEND_ONE;
	pma.dst_color           = AERON_BLEND_ONE_MINUS_SRC_ALPHA;
	pma.color_op            = AERON_BLEND_OP_ADD;
	pma.src_alpha           = AERON_BLEND_ONE;
	pma.dst_alpha           = AERON_BLEND_ONE_MINUS_SRC_ALPHA;
	pma.alpha_op            = AERON_BLEND_OP_ADD;
	return Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc){
		.vertex_shader   = vs,
		.fragment_shader = ps,
		.primitive_type  = AERON_PRIMITIVE_TRIANGLE_STRIP,
		.cull_mode       = AERON_CULL_NONE,
		.color_format    = fmt,
		.blend           = pma,
	});
}

AeronScenePresentChain* AeronScenePresentChain_Create(AeronTextureFormat target_format) {
	AeronScenePresentChain* c = (AeronScenePresentChain*)calloc(1, sizeof *c);
	if (!c) {
		return NULL;
	}
	c->target_format = target_format;
	c->vs = AeronSceneInternal_CompileShader("scene_fullscreen_quad.vert",
											 AERON_SHADER_STAGE_VERTEX, 0, 0, 0);
	c->ps_sdr = AeronSceneInternal_CompileShader("scene_tonemap.frag",
												 AERON_SHADER_STAGE_FRAGMENT, 2, 1, 0);
	c->ps_hdr = AeronSceneInternal_CompileShader("scene_tonemap_hdr.frag",
												 AERON_SHADER_STAGE_FRAGMENT, 2, 1, 0);
	if (c->vs && c->ps_sdr) {
		c->pipeline_sdr = present_pipeline(c->vs, c->ps_sdr, target_format);
	}
	if (c->vs && c->ps_hdr) {
		c->pipeline_hdr = present_pipeline(c->vs, c->ps_hdr, target_format);
	}
	if (!c->pipeline_sdr && !c->pipeline_hdr) {
		AeronScenePresentChain_Destroy(c);
		return NULL;
	}
	return c;
}

void AeronScenePresentChain_Destroy(AeronScenePresentChain* c) {
	if (!c) {
		return;
	}
	if (c->pipeline_sdr) Aeron_DestroyGraphicsPipeline(c->pipeline_sdr);
	if (c->pipeline_hdr) Aeron_DestroyGraphicsPipeline(c->pipeline_hdr);
	if (c->vs) Aeron_DestroyShader(c->vs);
	if (c->ps_sdr) Aeron_DestroyShader(c->ps_sdr);
	if (c->ps_hdr) Aeron_DestroyShader(c->ps_hdr);
	free(c);
}

void AeronScenePresentChain_Draw(AeronScenePresentChain* c, AeronRenderPass* pass,
								 AeronTexture* scene_tex, AeronSampler* sampler,
								 AeronTexture* bloom_tex, float bloom_intensity, int rt_w,
								 int rt_h, float bar_y_uv, const float tint[4],
								 int src_coverage) {
	if (!c || !pass || !scene_tex || !sampler) {
		return;
	}
	AeronGraphicsPipeline* pp = NULL;
	if (Aeron_OutputHdrEnabled() && c->pipeline_hdr) {
		pp = c->pipeline_hdr;
	} else if (c->pipeline_sdr) {
		pp = c->pipeline_sdr;
	} else {
		pp = c->pipeline_hdr;
	}
	if (!pp) {
		return;
	}
	Aeron_BindGraphicsPipeline(pass, pp);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0, scene_tex, sampler);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 1,
							 bloom_tex ? bloom_tex : scene_tex, sampler);

	/* HDR headroom is relative to SDR white. The render pass supplies the
	 * platform encoding of that white when drawing directly to the swapchain. */
	float hdr_peak_scale = 1.0f;
	if (Aeron_OutputHdrEnabled()) {
		hdr_peak_scale = Aeron_OutputHdrHeadroom();
		if (hdr_peak_scale < 1.0f) {
			hdr_peak_scale = 1.0f;
		}
	}
	float u[16] = {
		bloom_tex ? bloom_intensity : 0.0f,
		rt_w > 0 ? 2.0f / (float)rt_w : 0.0f,
		rt_h > 0 ? 2.0f / (float)rt_h : 0.0f,
		bar_y_uv,
		1.0f, /* scene exposure */
		(float)AeronScenePresent_TonemapOp(),
		hdr_peak_scale,
		Aeron_RenderPassOutputRgbScale(pass),
		tint ? tint[0] : 1.0f,
		tint ? tint[1] : 1.0f,
		tint ? tint[2] : 1.0f,
		tint ? tint[3] : 1.0f,
		(float)AeronScenePresent_BloomKernel(),
		AeronScenePresent_EotfExponent(),
		AeronScenePresent_AcesExposure(),
		src_coverage ? 1.0f : 0.0f,
	};
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, u, sizeof u);
	Aeron_Draw(pass, 4, 0);
}
