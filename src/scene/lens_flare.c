/*
 * The procedural lens flare (AeronScene_SetLensFlare). Prepare places the
 * source on screen and uploads the elements before the frame's passes.
 * Visibility measures the visible part of the source's disc after the scene
 * passes, and dims the streaks by the visible part of the band of the disc
 * along them. Draw adds the flare in the LENS pass.
 */

#include "internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The visibility shader's sample count. Each of its threads changes at most
 * one element, so there can't be more elements than samples. */
#define FLARE_SAMPLES 64u
typedef char FlareElementsFitSamples[AERON_SCENE_LENS_FLARE_MAX_ELEMENTS <= FLARE_SAMPLES ? 1 : -1];
/* The smallest disc the samples cover, in render pixels, so a small source
 * doesn't flicker under the temporal upscaler's jitter. */
#define FLARE_MIN_RADIUS 2.0f
/* Three float4 per element, as the flare's fragment shader reads them. */
#define FLARE_ELEMENT_FLOATS 12u
#define FLARE_SHAPES 4

void AeronScene_SetLensFlareElements(AeronScene3D* s, const AeronSceneLensFlareElement* elements,
									 uint32_t count) {
	if (!s)
		return;
	if (!elements)
		count = 0;
	if (count > AERON_SCENE_LENS_FLARE_MAX_ELEMENTS)
		count = AERON_SCENE_LENS_FLARE_MAX_ELEMENTS;
	if (count)
		memcpy(s->flare_elements, elements, count * sizeof *elements);
	s->flare_element_count = count;
}

void AeronScene_SetLensFlare(AeronScene3D* s, const AeronSceneLensFlareDesc* desc) {
	if (!s)
		return;
	s->flare_set = desc != NULL;
	if (desc)
		s->flare = *desc;
}

static int flare_ensure(AeronScene3D* s) {
	if (s->flare_tried)
		return s->flare_pipe != NULL;
	s->flare_tried           = 1;
	s->flare_visibility_pipe = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
		.name                           = "scene_lens_flare_visibility.comp",
		.sampler_count                  = 1,
		.readwrite_storage_buffer_count = 2,
		.uniform_buffer_count           = 1,
		.thread_count_x                 = FLARE_SAMPLES,
		.thread_count_y                 = 1,
		.thread_count_z                 = 1,
	});
	s->flare_depth_sampler   = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_NEAREST,
		.mag_filter = AERON_FILTER_NEAREST,
		.mip_filter = AERON_FILTER_NEAREST,
		.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
	});
	/* The flare's depth test: 1 where nothing is in front of the source,
	 * blended between the four texels around the point. */
	s->flare_depth_compare_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter     = AERON_FILTER_LINEAR,
		.mag_filter     = AERON_FILTER_LINEAR,
		.mip_filter     = AERON_FILTER_NEAREST,
		.address_u      = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v      = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w      = AERON_ADDRESS_CLAMP_TO_EDGE,
		.enable_compare = 1,
		.compare        = AERON_COMPARE_GREATER_EQUAL,
	});
	s->flare_vs =
		AeronSceneInternal_CompileShader("scene_lens_flare.vert", AERON_SHADER_STAGE_VERTEX, 0, 0, 1);
	s->flare_fs =
		AeronSceneInternal_CompileShader("scene_lens_flare.frag", AERON_SHADER_STAGE_FRAGMENT, 2, 1, 2);
	s->flare_smooth_fs      = AeronSceneInternal_CompileShader("scene_lens_flare_smooth.frag",
															   AERON_SHADER_STAGE_FRAGMENT, 0, 1, 1);
	s->flare_smooth_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_LINEAR,
		.mag_filter = AERON_FILTER_LINEAR,
		.mip_filter = AERON_FILTER_NEAREST,
		.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
	});
	/* The halos and discs, at half the output's resolution. */
	s->flare_smooth_rt =
		AeronSceneInternal_CreateColorRt(AERON_TEXTURE_FORMAT_RGBA16_FLOAT, (s->output_w + 1) / 2,
										 (s->output_h + 1) / 2, "scene.lens_flare.smooth");
	s->flare_visibility = Aeron_CreateBuffer(
		&(AeronBufferDesc) { .size  = 4u * (uint32_t)sizeof(float),
							 .usage = AERON_BUFFER_USAGE_STORAGE | AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE,
							 .debug_name = "scene.lens_flare.visibility" });
	s->flare_element_buffer = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size         = AERON_SCENE_LENS_FLARE_MAX_ELEMENTS * FLARE_ELEMENT_FLOATS * (uint32_t)sizeof(float),
		.usage        = AERON_BUFFER_USAGE_STORAGE | AERON_BUFFER_USAGE_COMPUTE_STORAGE_WRITE,
		.memory_usage = AERON_MEMORY_USAGE_DYNAMIC,
		.debug_name   = "scene.lens_flare.elements" });
	if (s->flare_vs && s->flare_fs) {
		AeronColorTargetStateDesc cts[1] = { 0 };
		cts[0].format                    = s->color_format;
		cts[0].blend                     = (AeronBlendStateDesc) { .enabled   = 1,
																   .src_color = AERON_BLEND_ONE,
																   .dst_color = AERON_BLEND_ONE,
																   .color_op  = AERON_BLEND_OP_ADD,
																   .src_alpha = AERON_BLEND_ZERO,
																   .dst_alpha = AERON_BLEND_ONE,
																   .alpha_op  = AERON_BLEND_OP_ADD };
		s->flare_pipe                    = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
			.vertex_shader      = s->flare_vs,
			.fragment_shader    = s->flare_fs,
			.primitive_type     = AERON_PRIMITIVE_TRIANGLES,
			.cull_mode          = AERON_CULL_NONE,
			.depth_format       = AERON_TEXTURE_FORMAT_UNKNOWN,
			.color_target_count = 1,
			.color_targets      = cts,
		});
	}
	if (s->flare_vs && s->flare_smooth_fs) {
		AeronColorTargetStateDesc cts[1] = { 0 };
		cts[0].format                    = AERON_TEXTURE_FORMAT_RGBA16_FLOAT;
		cts[0].blend                     = AeronSceneInternal_BlendOpaque();
		s->flare_smooth_pipe             = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
			.vertex_shader      = s->flare_vs,
			.fragment_shader    = s->flare_smooth_fs,
			.primitive_type     = AERON_PRIMITIVE_TRIANGLES,
			.cull_mode          = AERON_CULL_NONE,
			.depth_format       = AERON_TEXTURE_FORMAT_UNKNOWN,
			.color_target_count = 1,
			.color_targets      = cts,
		});
	}
	if (!s->flare_visibility_pipe || !s->flare_depth_sampler || !s->flare_depth_compare_sampler ||
		!s->flare_visibility || !s->flare_element_buffer || !s->flare_pipe || !s->flare_smooth_pipe ||
		!s->flare_smooth_sampler || !s->flare_smooth_rt) {
		Aeron_LogError("aeron.scene",
					   "lens flare: could not build the pipelines or buffers; no flare is drawn");
		AeronSceneLensFlare_Release(s);
		s->flare_tried = 1;
		return 0;
	}
	return 1;
}

bool AeronScene_PrepareLensFlares(AeronScene3D* s) { return s && flare_ensure(s); }

static int flare_project(const float m[16], const float p[3], float clip[4]) {
	for (int i = 0; i < 4; ++i)
		clip[i] = m[i * 4 + 0] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2] + m[i * 4 + 3];
	return clip[3] > 1e-4f;
}

/* A clip position to pixels of `viewport`, y down. */
static void flare_to_pixels(const float clip[4], const AeronRectI* viewport, float out[2]) {
	out[0] = (float)viewport->x + (clip[0] / clip[3] * 0.5f + 0.5f) * (float)viewport->width;
	out[1] = (float)viewport->y + (0.5f - clip[1] / clip[3] * 0.5f) * (float)viewport->height;
}

int AeronSceneLensFlare_Prepare(AeronScene3D* s, AeronCommandBuffer* cmd) {
	s->flare_frame                   = 0;
	const AeronSceneLensFlareDesc* f = &s->flare;
	if (!s->flare_set || s->flare_element_count == 0 || !(f->intensity > 0.0f) || !flare_ensure(s))
		return 1;

	/* The disc, in render pixels, with the matrix the depth buffer is drawn
	 * with. Its edge is found from a point on a line across the view. */
	float clip[4];
	if (!flare_project(s->jittered_view_proj, f->anchor_world, clip))
		return 1;
	const float dir[3] = { f->anchor_world[0] - s->camera.pos[0], f->anchor_world[1] - s->camera.pos[1],
						   f->anchor_world[2] - s->camera.pos[2] };
	/* Across the view: dir x the world axis it is least aligned with. */
	const float ax = fabsf(dir[0]), ay = fabsf(dir[1]), az = fabsf(dir[2]);
	float       axis[3]                                 = { 0.0f, 0.0f, 0.0f };
	axis[ax <= ay && ax <= az ? 0 : (ay <= az ? 1 : 2)] = 1.0f;
	const float across[3]     = { dir[1] * axis[2] - dir[2] * axis[1], dir[2] * axis[0] - dir[0] * axis[2],
								  dir[0] * axis[1] - dir[1] * axis[0] };
	const float across_length = sqrtf(across[0] * across[0] + across[1] * across[1] + across[2] * across[2]);
	float       radius        = 0.0f;
	if (across_length > 0.0f && f->radius_world > 0.0f) {
		const float edge_world[3] = { f->anchor_world[0] + across[0] / across_length * f->radius_world,
									  f->anchor_world[1] + across[1] / across_length * f->radius_world,
									  f->anchor_world[2] + across[2] / across_length * f->radius_world };
		float       edge_clip[4], edge[2], centre[2];
		if (flare_project(s->jittered_view_proj, edge_world, edge_clip)) {
			flare_to_pixels(clip, &s->camera.viewport, centre);
			flare_to_pixels(edge_clip, &s->camera.viewport, edge);
			radius = hypotf(edge[0] - centre[0], edge[1] - centre[1]);
		}
	}
	flare_to_pixels(clip, &s->camera.viewport, s->flare_center);
	s->flare_radius = fmaxf(radius, FLARE_MIN_RADIUS);
	s->flare_depth  = clip[2] / clip[3];
	/* A disc wholly outside the viewport is hidden. */
	const AeronRectI* vp = &s->camera.viewport;
	if (s->flare_center[0] + s->flare_radius < (float)vp->x ||
		s->flare_center[0] - s->flare_radius >= (float)(vp->x + vp->width) ||
		s->flare_center[1] + s->flare_radius < (float)vp->y ||
		s->flare_center[1] - s->flare_radius >= (float)(vp->y + vp->height))
		return 1;

	/* S, in flare units of the output viewport, without the jitter. */
	if (!flare_project(s->unjittered_view_proj, f->anchor_world, clip))
		return 1;
	const AeronRectI* out    = &s->output_camera.viewport;
	const float       unit   = 0.5f * (float)(out->width < out->height ? out->width : out->height);
	const float       sun[2] = { clip[0] / clip[3] * 0.5f * (float)out->width / unit,
								 -clip[1] / clip[3] * 0.5f * (float)out->height / unit };
	/* The disc hides the flare. Its radius is in render pixels, so it is
	 * converted with the render viewport, which has the same shape as the
	 * output. */
	s->flare_disc[0] = sun[0];
	s->flare_disc[1] = sun[1];
	s->flare_disc[2] = radius / (0.5f * (float)(vp->width < vp->height ? vp->width : vp->height));

	/* The elements, sorted by shape, with what doesn't change across the
	 * screen worked out here: the centre, and the colour with the tint and
	 * the intensity. */
	float    packed[AERON_SCENE_LENS_FLARE_MAX_ELEMENTS * FLARE_ELEMENT_FLOATS];
	uint32_t n = 0;
	for (int shape = 0; shape < FLARE_SHAPES; ++shape) {
		for (uint32_t i = 0; i < s->flare_element_count; ++i) {
			const AeronSceneLensFlareElement* e = &s->flare_elements[i];
			if ((int)e->shape != shape)
				continue;
			float* o = &packed[n * FLARE_ELEMENT_FLOATS];
			o[0]     = e->position * sun[0] + e->offset[0];
			o[1]     = e->position * sun[1] + e->offset[1];
			o[2]     = e->stretch;
			o[3]     = e->size;
			o[4]     = e->power;
			o[5]     = cosf(e->rotation);
			o[6]     = sinf(e->rotation);
			o[7]     = 0.0f;
			if (shape == AERON_SCENE_LENS_FLARE_DISC) {
				/* The shader compares squared distances. */
				o[3] = 1.0f / (e->size * e->size);
				o[4] = 0.5f * e->power;
			}
			for (int c = 0; c < 3; ++c)
				o[8 + c] = e->color[c] * (e->tinted ? f->tint[c] : 1.0f) * f->intensity;
			o[11] = 0.0f;
			n++;
		}
		s->flare_shape_end[shape] = n;
	}
	if (!Aeron_UploadBufferDataCmd(cmd, s->flare_element_buffer, 0, packed,
								   n * FLARE_ELEMENT_FLOATS * (uint32_t)sizeof(float)))
		return 0;
	s->flare_frame = 1;
	return 1;
}

int AeronSceneLensFlare_Visibility(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s->flare_frame)
		return 1;
	AeronTexture* depth = Aeron_DepthTargetGetTexture(s->depth_rt);
	if (!depth) {
		s->flare_frame = 0;
		return 1;
	}
	/* The elements keep this frame's upload, so their buffer isn't cycled. */
	const AeronComputeBufferBinding buffers[2] = { { .buffer = s->flare_visibility, .cycle = 1 },
												   { .buffer = s->flare_element_buffer, .cycle = 0 } };
	AeronComputePass*               pass       = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer     = cmd,
		.write_buffers      = buffers,
		.write_buffer_count = 2,
		.debug_label        = "Lens flare visibility",
	});
	if (!pass)
		return 0;
	const AeronRectI* vp = &s->camera.viewport;
	struct {
		float    disc[4];
		float    viewport[4];
		uint32_t ends[4];
	} u = {
		.disc     = { s->flare_center[0], s->flare_center[1], s->flare_radius, s->flare_depth },
		.viewport = { (float)vp->x, (float)vp->y, (float)vp->width, (float)vp->height },
		.ends     = { s->flare_shape_end[AERON_SCENE_LENS_FLARE_STREAK],
					  s->flare_shape_end[AERON_SCENE_LENS_FLARE_HEXAGON], 0, 0 },
	};
	Aeron_BindComputePipeline(pass, s->flare_visibility_pipe);
	Aeron_BindComputeTextureSampler(pass, 0, depth, s->flare_depth_sampler);
	Aeron_BindComputeUniformData(pass, 0, &u, sizeof u);
	Aeron_DispatchCompute(pass, 1, 1, 1);
	Aeron_EndComputePass(pass);
	return 1;
}

/* The flare's uniforms. Pixels of the pass times `pixel_scale` are output
 * pixels from `origin`. */
typedef struct FlareUniforms {
	float    frame[4];
	float    scale[4];
	float    origin[4];
	uint32_t ends[4];
	float    disc[4];
	float    render[4];
} FlareUniforms;

static FlareUniforms flare_uniforms(const AeronScene3D* s, const float origin[2], float pixel_scale) {
	const AeronRectI* out = &s->output_camera.viewport;
	const AeronRectI* vp  = &s->camera.viewport;
	FlareUniforms     u;
	memset(&u, 0, sizeof u);
	u.frame[0]  = origin[0] + 0.5f * (float)out->width;
	u.frame[1]  = origin[1] + 0.5f * (float)out->height;
	u.frame[2]  = 2.0f / (float)(out->width < out->height ? out->width : out->height);
	u.frame[3]  = AeronScene_EmissiveScale() * s->image_radiance;
	u.scale[0]  = pixel_scale;
	u.scale[2]  = 0.5f / (float)((s->output_w + 1) / 2);
	u.scale[3]  = 0.5f / (float)((s->output_h + 1) / 2);
	u.origin[0] = (float)out->x;
	u.origin[1] = (float)out->y;
	/* The uv of the last texel centre that the half-resolution pass draws. */
	u.origin[2] = ((float)((out->width + 1) / 2) - 0.5f) / (float)((s->output_w + 1) / 2);
	u.origin[3] = ((float)((out->height + 1) / 2) - 0.5f) / (float)((s->output_h + 1) / 2);
	memcpy(u.ends, s->flare_shape_end, sizeof u.ends);
	u.disc[0]   = s->flare_disc[0];
	u.disc[1]   = s->flare_disc[1];
	u.disc[2]   = s->flare_disc[2] * s->flare_disc[2];
	u.disc[3]   = s->flare_depth;
	u.render[0] = (float)vp->width / (float)out->width / (float)s->render_w;
	u.render[1] = (float)vp->height / (float)out->height / (float)s->render_h;
	u.render[2] = (float)vp->x / (float)s->render_w;
	u.render[3] = (float)vp->y / (float)s->render_h;
	return u;
}

int AeronSceneLensFlare_DrawSmooth(AeronScene3D* s, AeronCommandBuffer* cmd) {
	if (!s->flare_frame)
		return 1;
	const AeronRectI* out  = &s->output_camera.viewport;
	AeronRenderPass*  pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target = s->flare_smooth_rt,
		.viewport     = { 0, 0, (out->width + 1) / 2, (out->height + 1) / 2 },
		/* The triangle covers the viewport, and nothing reads the image
		 * when it collapses. */
		.discard_color  = 1,
		.command_buffer = cmd,
		.debug_label    = "Lens flare halos and discs",
	});
	if (!pass)
		return 0;
	const float         origin[2] = { 0.0f, 0.0f };
	const FlareUniforms u         = flare_uniforms(s, origin, 2.0f);
	Aeron_BindGraphicsPipeline(pass, s->flare_smooth_pipe);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, s->flare_visibility);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 0, s->flare_element_buffer);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, &u, sizeof u);
	Aeron_Draw(pass, 3, 0);
	Aeron_EndRenderPass(pass);
	return 1;
}

void AeronSceneLensFlare_Draw(AeronScene3D* s, AeronRenderPass* pass) {
	if (!s->flare_frame)
		return;
	const AeronRectI*   out       = &s->output_camera.viewport;
	const float         origin[2] = { (float)out->x, (float)out->y };
	const FlareUniforms u         = flare_uniforms(s, origin, 1.0f);
	Aeron_BindGraphicsPipeline(pass, s->flare_pipe);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, s->flare_visibility);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0,
							 Aeron_RenderTargetGetTexture(s->flare_smooth_rt), s->flare_smooth_sampler);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 1, Aeron_DepthTargetGetTexture(s->depth_rt),
							 s->flare_depth_compare_sampler);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 0, s->flare_element_buffer);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_FRAGMENT, 1, s->flare_visibility);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_FRAGMENT, 0, &u, sizeof u);
	Aeron_Draw(pass, 3, 0);
}

void AeronSceneLensFlare_Release(AeronScene3D* s) {
	if (!s)
		return;
	Aeron_DestroyComputePipeline(s->flare_visibility_pipe);
	Aeron_DestroySampler(s->flare_depth_sampler);
	Aeron_DestroySampler(s->flare_depth_compare_sampler);
	if (s->flare_pipe)
		Aeron_DestroyGraphicsPipeline(s->flare_pipe);
	if (s->flare_vs)
		Aeron_DestroyShader(s->flare_vs);
	if (s->flare_fs)
		Aeron_DestroyShader(s->flare_fs);
	if (s->flare_smooth_pipe)
		Aeron_DestroyGraphicsPipeline(s->flare_smooth_pipe);
	if (s->flare_smooth_fs)
		Aeron_DestroyShader(s->flare_smooth_fs);
	if (s->flare_smooth_rt)
		Aeron_DestroyRenderTarget(s->flare_smooth_rt);
	Aeron_DestroySampler(s->flare_smooth_sampler);
	Aeron_DestroyBuffer(s->flare_visibility);
	Aeron_DestroyBuffer(s->flare_element_buffer);
	s->flare_visibility_pipe = NULL;
	s->flare_depth_sampler   = NULL;
	s->flare_depth_compare_sampler = NULL;
	s->flare_pipe            = NULL;
	s->flare_vs              = NULL;
	s->flare_fs              = NULL;
	s->flare_smooth_pipe     = NULL;
	s->flare_smooth_fs       = NULL;
	s->flare_smooth_rt       = NULL;
	s->flare_smooth_sampler  = NULL;
	s->flare_visibility      = NULL;
	s->flare_element_buffer  = NULL;
	s->flare_tried           = 0;
	s->flare_frame           = 0;
}

/* ---- settings ---- */

static bool flare_error(AeronConfigError* error, const AeronConfigNode* node, const char* format, ...) {
	if (error) {
		va_list args;
		memset(error, 0, sizeof *error);
		error->code = AERON_CONFIG_ERROR_INVALID_ARGUMENT;
		if (node) {
			const char* source_path = AeronConfigNode_SourcePath(node);
			error->root             = AeronConfigNode_SourceRoot(node);
			error->line             = AeronConfigNode_Line(node);
			error->column           = AeronConfigNode_Column(node);
			snprintf(error->path, sizeof error->path, "%s", source_path ? source_path : "<configuration>");
		}
		va_start(args, format);
		vsnprintf(error->message, sizeof error->message, format, args);
		va_end(args);
	}
	return false;
}

static bool flare_number(const AeronConfigNode* node, float* out) {
	const AeronConfigNodeType type = AeronConfigNode_Type(node);
	if (type != AERON_CONFIG_INT && type != AERON_CONFIG_FLOAT)
		return false;
	const double value = AeronConfigNode_Float(node, NAN);
	if (!isfinite(value))
		return false;
	*out = (float)value;
	return true;
}

static bool flare_numbers(const AeronConfigNode* node, float* out, size_t count) {
	if (AeronConfigNode_Type(node) != AERON_CONFIG_SEQUENCE || AeronConfigNode_SequenceCount(node) != count)
		return false;
	for (size_t i = 0; i < count; ++i)
		if (!flare_number(AeronConfigNode_SequenceGet(node, i), &out[i]))
			return false;
	return true;
}

bool AeronSceneLensFlare_ParseElements(const AeronConfigNode* list, AeronSceneLensFlareElement* out,
									   uint32_t capacity, uint32_t* count, AeronConfigError* error) {
	static const char* const shapes[] = { "halo", "disc", "hexagon", "streak" };
	static const char* const keys[]   = { "shape", "position", "offset", "stretch", "size",
										  "power", "rotation", "color",  "tinted" };
	if (count)
		*count = 0;
	if (!list || !out || !count)
		return flare_error(error, list, "flare elements: missing list");
	if (AeronConfigNode_Type(list) != AERON_CONFIG_SEQUENCE)
		return flare_error(error, list, "flare elements must be a list");
	const size_t n = AeronConfigNode_SequenceCount(list);
	if (n > capacity)
		return flare_error(error, list, "flare elements: at most %u entries", capacity);
	for (size_t i = 0; i < n; ++i) {
		const AeronConfigNode* item = AeronConfigNode_SequenceGet(list, i);
		if (AeronConfigNode_Type(item) != AERON_CONFIG_MAP)
			return flare_error(error, item, "flare element %zu must be a map", i);
		for (size_t k = 0; k < AeronConfigNode_MapCount(item); ++k) {
			const char* key   = AeronConfigNode_MapKeyAt(item, k);
			size_t      known = 0;
			while (known < sizeof keys / sizeof keys[0] && strcmp(key, keys[known]) != 0)
				known++;
			if (known == sizeof keys / sizeof keys[0])
				return flare_error(error, AeronConfigNode_MapValueAt(item, k),
								   "flare element %zu: unknown key '%s'", i, key);
		}
		AeronSceneLensFlareElement e = { .power = 1.0f };

		const char* shape = AeronConfigNode_String(AeronConfigNode_MapGet(item, "shape"), NULL);
		size_t      index = 0;
		while (shape && index < sizeof shapes / sizeof shapes[0] && strcmp(shape, shapes[index]) != 0)
			index++;
		if (!shape || index == sizeof shapes / sizeof shapes[0])
			return flare_error(error, item, "flare element %zu: shape must be halo, disc, hexagon or streak",
							   i);
		e.shape = (AeronSceneLensFlareShape)index;

		const AeronConfigNode* color = AeronConfigNode_MapGet(item, "color");
		if (!flare_numbers(color, e.color, 3) || e.color[0] < 0.0f || e.color[1] < 0.0f || e.color[2] < 0.0f)
			return flare_error(error, color ? color : item,
							   "flare element %zu: color must be [r, g, b], each 0 or more", i);

		const AeronConfigNode* size = AeronConfigNode_MapGet(item, "size");
		if (!size || !flare_number(size, &e.size) || e.size < 0.0f ||
			(e.size == 0.0f && e.shape != AERON_SCENE_LENS_FLARE_HALO))
			return flare_error(error, size ? size : item,
							   e.shape == AERON_SCENE_LENS_FLARE_HALO
								   ? "flare element %zu: size must be 0 or more"
								   : "flare element %zu: size must be above 0",
							   i);

		const AeronConfigNode* node = AeronConfigNode_MapGet(item, "position");
		if (node && !flare_number(node, &e.position))
			return flare_error(error, node, "flare element %zu: position must be a number", i);
		node = AeronConfigNode_MapGet(item, "offset");
		if (node && !flare_numbers(node, e.offset, 2))
			return flare_error(error, node, "flare element %zu: offset must be [x, y]", i);
		node = AeronConfigNode_MapGet(item, "stretch");
		if (node && !flare_number(node, &e.stretch))
			return flare_error(error, node, "flare element %zu: stretch must be a number", i);
		node = AeronConfigNode_MapGet(item, "rotation");
		if (node && !flare_number(node, &e.rotation))
			return flare_error(error, node, "flare element %zu: rotation must be a number", i);
		node = AeronConfigNode_MapGet(item, "power");
		if (node && (!flare_number(node, &e.power) || e.power < 0.0f ||
					 (e.power == 0.0f && e.shape == AERON_SCENE_LENS_FLARE_DISC)))
			return flare_error(error, node,
							   e.shape == AERON_SCENE_LENS_FLARE_DISC
								   ? "flare element %zu: power must be above 0"
								   : "flare element %zu: power must be 0 or more",
							   i);
		node = AeronConfigNode_MapGet(item, "tinted");
		if (node && AeronConfigNode_Type(node) != AERON_CONFIG_BOOL)
			return flare_error(error, node, "flare element %zu: tinted must be true or false", i);
		e.tinted = node && AeronConfigNode_Bool(node, 0);
		out[i]   = e;
	}
	*count = (uint32_t)n;
	return true;
}
