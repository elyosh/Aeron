/*
 * Small shared helpers for the aeron_scene translation units.
 */

#include "internal.h"

#include "aeron/aeron.h"

AeronShader* AeronSceneInternal_CompileShader(const char* name, AeronShaderStage stage,
											  uint32_t samplers, uint32_t ubs, uint32_t sbs) {
	AeronShader* sh = Aeron_CreateShader(&(AeronShaderDesc){ .name                 = name,
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
	return Aeron_CreateRenderTarget(&(AeronRenderTargetDesc){ .width = w, .height = h,
														  .format = fmt, .debug_name = debug_name });
}

AeronBlendStateDesc AeronSceneInternal_BlendOpaque(void) {
	AeronBlendStateDesc b = { 0 };
	b.color_write_mask_enable = 1;
	b.color_write_mask        = 0xF;
	return b;
}

void AeronSceneInternal_QuatToMat3(const float q[4], float m[9]) {
	const float w = q[0], x = q[1], y = q[2], z = q[3];
	const float xx = x * x, yy = y * y, zz = z * z;
	const float xy = x * y, xz = x * z, yz = y * z;
	const float wx = w * x, wy = w * y, wz = w * z;
	m[0] = 1.0f - 2.0f * (yy + zz); m[1] = 2.0f * (xy - wz);        m[2] = 2.0f * (xz + wy);
	m[3] = 2.0f * (xy + wz);        m[4] = 1.0f - 2.0f * (xx + zz); m[5] = 2.0f * (yz - wx);
	m[6] = 2.0f * (xz - wy);        m[7] = 2.0f * (yz + wx);        m[8] = 1.0f - 2.0f * (xx + yy);
}

/* Process-shared 1x1 white texture (AO placeholder etc.). */
AeronTexture* AeronSceneInternal_WhiteTexture(void) {
	static AeronTexture* tex;
	if (tex) {
		return tex;
	}
	tex = Aeron_CreateTexture(&(AeronTextureDesc){
		.width  = 1,
		.height = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage  = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "scene.white_1x1",
	});
	if (!tex) {
		Aeron_RequestFatalRendererError("shared scene texture creation");
		return NULL;
	}
	static const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	if (!Aeron_UploadTextureData(&(AeronTextureUploadDesc){
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
	tex = Aeron_CreateTexture(&(AeronTextureDesc){
		.width  = 1,
		.height = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage  = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.cube = 1,
		.debug_name = "scene.white_cube_1x1",
	});
	if (!tex) {
		Aeron_RequestFatalRendererError("shared scene cube texture creation");
		return NULL;
	}
	static const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	for (int face = 0; face < 6; face++) {
		if (!Aeron_UploadTextureData(&(AeronTextureUploadDesc){
				.texture = tex, .width = 1, .height = 1, .raw_data = white,
				.raw_size = sizeof white, .layer = face })) {
			Aeron_DestroyTexture(tex);
			tex = NULL;
			Aeron_RequestFatalRendererError("shared scene cube texture upload");
			break;
		}
	}
	return tex;
}
