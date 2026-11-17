/* The mesh that a full-screen pass draws (aeron/scene/fullscreen_mesh.h),
 * and the pipelines that draw it. */

#include "internal.h"

static AeronBuffer*             s_whole_buffer;
static AeronSceneFullscreenMesh s_whole;

static void fullscreen_release_whole(void) {
	if (s_whole_buffer) {
		Aeron_DestroyBuffer(s_whole_buffer);
	}
	s_whole_buffer = NULL;
	s_whole        = (AeronSceneFullscreenMesh) { 0 };
}

const AeronSceneFullscreenMesh* AeronSceneFullscreen_Whole(void) {
	if (s_whole_buffer) {
		return &s_whole;
	}
	/* Two triangles, with the same corners as a 4-vertex strip. One large
	 * triangle would cover the same pixels, but its texture coordinates
	 * round differently, and passes that sample on texel edges (the motion
	 * blur) then give slightly different pixels. */
	static const float positions[12] = { -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,
										 1.0f,  -1.0f, -1.0f, 1.0f,  1.0f,  1.0f };
	AeronBuffer*       buffer        = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size       = sizeof positions,
		.usage      = AERON_BUFFER_USAGE_VERTEX,
		.debug_name = "scene.fullscreen_whole",
	});
	if (!buffer) {
		return NULL;
	}
	if (!Aeron_UploadBufferData(buffer, 0, positions, sizeof positions)) {
		Aeron_DestroyBuffer(buffer);
		return NULL;
	}
	s_whole_buffer = buffer;
	s_whole        = (AeronSceneFullscreenMesh) { .vertices = buffer, .first = 0, .count = 6 };
	Aeron_AddRenderShutdownHook(fullscreen_release_whole);
	return &s_whole;
}

int AeronSceneFullscreen_IsWhole(const AeronSceneFullscreenMesh* mesh) {
	return !mesh || mesh->vertices == s_whole_buffer;
}

void AeronSceneFullscreen_Draw(AeronRenderPass* pass, const AeronSceneFullscreenMesh* mesh) {
	if (!mesh || !mesh->vertices || mesh->count < 3) {
		mesh = AeronSceneFullscreen_Whole();
	}
	if (!pass || !mesh) {
		return;
	}
	Aeron_BindVertexBuffer(pass, 0, mesh->vertices, 0);
	Aeron_Draw(pass, mesh->count, mesh->first);
}

AeronGraphicsPipeline* AeronSceneInternal_CreateFullscreenPipeline(const AeronGraphicsPipelineDesc* desc) {
	static const AeronVertexBufferLayoutDesc layout    = { .slot = 0, .stride = 2 * sizeof(float) };
	static const AeronVertexAttributeDesc    attribute = { 0, 0, AERON_VERTEX_FORMAT_FLOAT2, 0 };
	AeronGraphicsPipelineDesc                full      = *desc;
	full.primitive_type                                = AERON_PRIMITIVE_TRIANGLES;
	full.cull_mode                                     = AERON_CULL_NONE;
	full.vertex_buffers                                = &layout;
	full.vertex_buffer_count                           = 1;
	full.attributes                                    = &attribute;
	full.attribute_count                               = 1;
	/* The whole-target mesh is created here, outside of any render pass, so
	 * that a draw never has to upload it. */
	(void)AeronSceneFullscreen_Whole();
	return Aeron_CreateGraphicsPipeline(&full);
}
