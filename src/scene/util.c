/*
 * Small shared helpers for the aeron_scene translation units.
 */

#include "internal.h"

#include "aeron/aeron.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

AeronShader* AeronSceneInternal_CompileShader(const char* name, AeronShaderStage stage, uint32_t samplers,
											  uint32_t ubs, uint32_t sbs) {
	AeronShader* sh = Aeron_CreateShader(&(AeronShaderDesc) { .name                 = name,
															  .stage                = stage,
															  .sampler_count        = samplers,
															  .uniform_buffer_count = ubs,
															  .storage_buffer_count = sbs });
	if (!sh) {
		Aeron_LogError("aeron.scene", "shader load failed: %s", name);
	}
	return sh;
}

AeronRenderTarget* AeronSceneInternal_CreateColorRt(AeronTextureFormat fmt, int w, int h,
													const char* debug_name) {
	return Aeron_CreateRenderTarget(
		&(AeronRenderTargetDesc) { .width = w, .height = h, .format = fmt, .debug_name = debug_name });
}

AeronBlendStateDesc AeronSceneInternal_BlendOpaque(void) {
	AeronBlendStateDesc b     = { 0 };
	b.color_write_mask_enable = 1;
	b.color_write_mask        = 0xF;
	return b;
}

void AeronSceneInternal_QuatToMat3(const float q[4], float m[9]) {
	const float w = q[0], x = q[1], y = q[2], z = q[3];
	const float xx = x * x, yy = y * y, zz = z * z;
	const float xy = x * y, xz = x * z, yz = y * z;
	const float wx = w * x, wy = w * y, wz = w * z;
	m[0] = 1.0f - 2.0f * (yy + zz);
	m[1] = 2.0f * (xy - wz);
	m[2] = 2.0f * (xz + wy);
	m[3] = 2.0f * (xy + wz);
	m[4] = 1.0f - 2.0f * (xx + zz);
	m[5] = 2.0f * (yz - wx);
	m[6] = 2.0f * (xz - wy);
	m[7] = 2.0f * (yz + wx);
	m[8] = 1.0f - 2.0f * (xx + yy);
}

/* Process-shared 1x1 white texture (AO placeholder etc.). */
AeronTexture* AeronSceneInternal_WhiteTexture(void) {
	static AeronTexture* tex;
	if (tex) {
		return tex;
	}
	tex = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width      = 1,
		.height     = 1,
		.format     = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage      = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "scene.white_1x1",
	});
	if (!tex) {
		Aeron_RequestFatalRendererError("shared scene texture creation");
		return NULL;
	}
	static const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	if (!Aeron_UploadTextureData(&(AeronTextureUploadDesc) {
			.texture = tex, .width = 1, .height = 1, .raw_data = white, .raw_size = sizeof white })) {
		Aeron_DestroyTexture(tex);
		tex = NULL;
		Aeron_RequestFatalRendererError("shared scene texture upload");
	}
	return tex;
}

/* Process-shared cube placeholder for optional PBR environment bindings. */
AeronTexture* AeronSceneInternal_WhiteCubeTexture(void) {
	static AeronTexture* tex;
	if (tex) {
		return tex;
	}
	tex = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width      = 1,
		.height     = 1,
		.format     = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage      = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.cube       = 1,
		.debug_name = "scene.white_cube_1x1",
	});
	if (!tex) {
		Aeron_RequestFatalRendererError("shared scene cube texture creation");
		return NULL;
	}
	static const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	for (int face = 0; face < 6; face++) {
		if (!Aeron_UploadTextureData(&(AeronTextureUploadDesc) { .texture  = tex,
																 .width    = 1,
																 .height   = 1,
																 .raw_data = white,
																 .raw_size = sizeof white,
																 .layer    = face })) {
			Aeron_DestroyTexture(tex);
			tex = NULL;
			Aeron_RequestFatalRendererError("shared scene cube texture upload");
			break;
		}
	}
	return tex;
}

/* ---- split-sum BRDF table ---------------------------------------------- */

#define BRDF_LUT_SIZE 64
#define BRDF_LUT_SAMPLES 512

static uint16_t brdf_float_to_half(float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	const uint32_t sign     = (bits >> 16) & 0x8000u;
	const int32_t  exponent = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
	uint32_t       mantissa = bits & 0x7FFFFFu;
	if (exponent <= 0) {
		if (exponent < -10)
			return (uint16_t)sign;
		mantissa |= 0x800000u;
		const uint32_t shift = (uint32_t)(14 - exponent);
		return (uint16_t)(sign | ((mantissa + (1u << (shift - 1u))) >> shift));
	}
	if (exponent >= 31)
		return (uint16_t)(sign | 0x7C00u);
	uint32_t half = sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
	if (mantissa & 0x1000u)
		++half; /* round half up; ample for a lookup table */
	return (uint16_t)half;
}

static float brdf_radical_inverse(uint32_t bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return (float)bits * 2.3283064365386963e-10f;
}

/* Split-sum integration (Karis 2013): GGX importance samples with N = +Z,
 * Smith-Schlick visibility with the image-based k = alpha / 2. */
static void brdf_integrate(float ndotv, float roughness, float* out_a, float* out_b) {
	const float alpha = roughness * roughness;
	const float k     = alpha * 0.5f;
	const float v[3]  = { sqrtf(1.0f - ndotv * ndotv), 0.0f, ndotv };
	double      a = 0.0, b = 0.0;
	for (uint32_t i = 0; i < BRDF_LUT_SAMPLES; ++i) {
		const float u1        = ((float)i + 0.5f) / (float)BRDF_LUT_SAMPLES;
		const float u2        = brdf_radical_inverse(i);
		const float phi       = 6.28318530718f * u2;
		const float cos_theta = sqrtf((1.0f - u1) / (1.0f + (alpha * alpha - 1.0f) * u1));
		const float sin_theta = sqrtf(1.0f - cos_theta * cos_theta);
		const float h[3]      = { sin_theta * cosf(phi), sin_theta * sinf(phi), cos_theta };
		const float vdoth     = v[0] * h[0] + v[1] * h[1] + v[2] * h[2];
		const float l_z       = 2.0f * vdoth * h[2] - v[2];
		if (l_z <= 0.0f || vdoth <= 0.0f)
			continue;
		const float g_v = ndotv / (ndotv * (1.0f - k) + k);
		const float g_l = l_z / (l_z * (1.0f - k) + k);
		const float vis = g_v * g_l * vdoth / (h[2] * ndotv);
		const float fc  = powf(1.0f - vdoth, 5.0f);
		a += (double)((1.0f - fc) * vis);
		b += (double)(fc * vis);
	}
	*out_a = (float)(a / BRDF_LUT_SAMPLES);
	*out_b = (float)(b / BRDF_LUT_SAMPLES);
}

AeronTexture* AeronSceneInternal_BrdfLutTexture(void) {
	static AeronTexture* tex;
	if (tex) {
		return tex;
	}
	static uint16_t texels[BRDF_LUT_SIZE * BRDF_LUT_SIZE * 2];
	for (int y = 0; y < BRDF_LUT_SIZE; ++y) {
		const float roughness = ((float)y + 0.5f) / (float)BRDF_LUT_SIZE;
		for (int x = 0; x < BRDF_LUT_SIZE; ++x) {
			const float ndotv = ((float)x + 0.5f) / (float)BRDF_LUT_SIZE;
			float       a, b;
			brdf_integrate(ndotv, roughness, &a, &b);
			texels[(y * BRDF_LUT_SIZE + x) * 2 + 0] = brdf_float_to_half(a);
			texels[(y * BRDF_LUT_SIZE + x) * 2 + 1] = brdf_float_to_half(b);
		}
	}
	tex = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width      = BRDF_LUT_SIZE,
		.height     = BRDF_LUT_SIZE,
		.format     = AERON_TEXTURE_FORMAT_R16G16_FLOAT,
		.usage      = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "scene.brdf_lut",
	});
	if (!tex) {
		Aeron_RequestFatalRendererError("BRDF table creation");
		return NULL;
	}
	if (!Aeron_UploadTextureData(&(AeronTextureUploadDesc) { .texture  = tex,
															 .width    = BRDF_LUT_SIZE,
															 .height   = BRDF_LUT_SIZE,
															 .raw_data = texels,
															 .raw_size = sizeof texels })) {
		Aeron_DestroyTexture(tex);
		tex = NULL;
		Aeron_RequestFatalRendererError("BRDF table upload");
	}
	return tex;
}
