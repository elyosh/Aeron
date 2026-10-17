/* The floor's reflection of what stands on it.
 *
 * A probe set's reflection cubes were captured without the ships standing
 * in its grid, and project what they hold onto the room's box, so a floor
 * cannot reflect the ships from them. When the first probe set has a
 * reflection plane (a hangar's floor, authored with its probes), the
 * instances that take that set's light, other than its owner, are drawn
 * mirrored across the plane at half resolution with the probe shaders,
 * whose lighting does not depend on the screen. A compute pass then keeps,
 * side by side in one texture, the colour premultiplied by coverage and the
 * reflected surface's height above the plane, and builds their mips. The
 * owner's surfaces on the plane read it at their own screen position, at a
 * mip widening with their roughness and that height, in place of the
 * probes' reflection where it covers them. */
#include "internal.h"

#include "aeron/log.h"
#include "aeron/scene/environment.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* Mips of the reflection; each half's width and the height are padded to a
 * multiple of 1 << PLANAR_MIPS so that no mip mixes the two halves. */
#define PLANAR_MIPS 4
#define PLANAR_ALIGN (1 << PLANAR_MIPS)
#define PLANAR_THREADS 8
/* Around what the reflection covers (half-resolution texels): the floor
 * reads within PLANAR_READ of it, its coarsest taps reach PLANAR_REACH. */
#define PLANAR_READ 16
#define PLANAR_REACH 32

/* Mirrors scene_planar_reflection_resolve.comp.hlsl. */
typedef struct ResolveUniform {
	float   z_row[4];    /* clip to the model z of the mirrored point, before the divide */
	float   w_row[4];    /* clip to its w */
	float   viewport[4]; /* the mirrored view's viewport, half-resolution texels */
	float   plane[4];    /* x: the plane's model z */
	int32_t dims[4];     /* each half's width and the height (padded), the drawn width and height */
	int32_t region[4];   /* the texels to write: x0, y0, x1, y1 (left half) */
} ResolveUniform;

static int align_up(int v, int a) { return (v + a - 1) / a * a; }

static void mat4_mul(float out[16], const float a[16], const float b[16]) {
	float t[16];
	for (int row = 0; row < 4; ++row)
		for (int col = 0; col < 4; ++col)
			t[row * 4 + col] = a[row * 4 + 0] * b[0 * 4 + col] + a[row * 4 + 1] * b[1 * 4 + col] +
							   a[row * 4 + 2] * b[2 * 4 + col] + a[row * 4 + 3] * b[3 * 4 + col];
	memcpy(out, t, sizeof t);
}

/* General 4 x 4 inverse; 0 when singular. */
static int mat4_invert(float out[16], const float m[16]) {
	float inv[16];
	inv[0]  = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
			  m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
			  m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8]  = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
			  m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
			  m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
			  m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5]  = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
			  m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9]  = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
			  m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
			  m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2]  = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
			  m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6]  = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
			  m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
			  m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
			  m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3]  = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
			  m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7]  = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
			  m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
			  m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
			  m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det == 0.0f || !isfinite(det)) {
		return 0;
	}
	for (int k = 0; k < 16; ++k) {
		out[k] = inv[k] / det;
	}
	return 1;
}

/* The first probe set's reflection plane; NULL when it has none. */
static const AeronFlightProbeLayout* plane_definition(const AeronScene3D* s) {
	if (s->probe_set_count == 0 || !s->probe_sets[0].probes) {
		return NULL;
	}
	const AeronFlightProbeLayout* d = &s->probe_sets[0].probes->layout;
	return d->has_reflection_plane ? d : NULL;
}

/* The plane n.p = h in scene units, from the model's z = `height`. */
static int world_plane(const AeronSceneProbeSet* set, float height, float n[3], float* h) {
	const float* row    = &set->world_to_model[8];
	const float  length = sqrtf(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
	if (!(length > 0.0f)) {
		return 0;
	}
	for (int k = 0; k < 3; ++k) {
		n[k] = row[k] / length;
	}
	*h = (height - row[3]) / length;
	return 1;
}

/* Model metres to scene units: the inverse of a set's world_to_model. */
static int model_to_world(const AeronSceneProbeSet* set, float out[16]) {
	float m[16] = { 0 };
	memcpy(m, set->world_to_model, 12 * sizeof(float));
	m[15] = 1.0f;
	return mat4_invert(out, m);
}

static void release_targets(AeronScenePlanarReflection* p) {
	Aeron_DestroyRenderTarget(p->color);
	Aeron_DestroyDepthTarget(p->depth);
	Aeron_DestroyTexture(p->image);
	p->color  = NULL;
	p->depth  = NULL;
	p->image  = NULL;
	p->width  = 0;
	p->height = 0;
}

void AeronScenePlanarReflection_Release(AeronScene3D* s) {
	if (!s) {
		return;
	}
	AeronScenePlanarReflection* p = &s->planar_reflection;
	release_targets(p);
	Aeron_DestroyComputePipeline(p->resolve);
	Aeron_DestroySampler(p->sampler);
	memset(p, 0, sizeof *p);
}

AeronSampler* AeronScenePlanarReflection_Sampler(AeronScene3D* s) {
	AeronScenePlanarReflection* p = &s->planar_reflection;
	if (!p->sampler) {
		p->sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
			.min_filter = AERON_FILTER_LINEAR,
			.mag_filter = AERON_FILTER_LINEAR,
			.mip_filter = AERON_FILTER_LINEAR,
			.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.max_lod    = (float)(PLANAR_MIPS - 1),
		});
	}
	return p->sampler;
}

AeronTexture* AeronScenePlanarReflection_Image(AeronScene3D* s) {
	const AeronScenePlanarReflection* p = &s->planar_reflection;
	/* Read only by the plane's receiver, which only an active reflection
	 * has: inactive, any 2D texture keeps the slot bound. */
	return p->active && p->image ? p->image : AeronSceneInternal_WhiteTexture();
}

void AeronScenePlanarReflection_LightingFields(const AeronScene3D* s, int set, int receiver, float rect[4],
											   float params[4], float eye[4], float screen[4]) {
	const AeronScenePlanarReflection* p = &s->planar_reflection;
	const AeronFlightProbeLayout* d = set == 0 ? plane_definition(s) : NULL;
	memset(rect, 0, 4 * sizeof(float));
	memset(params, 0, 4 * sizeof(float));
	memset(eye, 0, 4 * sizeof(float));
	memcpy(screen, p->screen, sizeof p->screen);
	if (d) {
		rect[0]   = d->reflection_plane_center[0];
		rect[1]   = d->reflection_plane_center[1];
		rect[2]   = 0.5f * d->reflection_plane_size[0];
		rect[3]   = 0.5f * d->reflection_plane_size[1];
		params[0] = d->reflection_plane_center[2];
		params[1] = d->reflection_plane_distance;
		params[2] = receiver && p->active ? 1.0f : 0.0f;
		params[3] = (float)PLANAR_MIPS;
	}
	if (p->drawing) {
		memcpy(eye, p->eye, sizeof p->eye);
		eye[3] = 1.0f;
	}
}

static int ensure_targets(AeronScene3D* s) {
	AeronScenePlanarReflection* p = &s->planar_reflection;
	const int                   w = (s->render_w + 1) / 2;
	const int                   h = (s->render_h + 1) / 2;
	if (p->color && p->width == w && p->height == h) {
		return 1;
	}
	release_targets(p);
	const int half   = align_up(w, PLANAR_ALIGN);
	const int height = align_up(h, PLANAR_ALIGN);
	p->color =
		AeronSceneInternal_CreateColorRt(AERON_TEXTURE_FORMAT_RGBA16_FLOAT, w, h, "scene.planar_color");
	p->depth   = Aeron_CreateDepthTarget(&(AeronDepthTargetDesc) {
		.width      = w,
		.height     = h,
		.format     = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.sampled    = 1,
		.debug_name = "scene.planar_depth",
	});
	p->image   = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width      = 2 * half,
		.height     = height,
		.mip_count  = PLANAR_MIPS,
		.format     = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
		.usage      = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE,
		.debug_name = "scene.planar_reflection",
	});
	if (!p->color || !p->depth || !p->image) {
		release_targets(p);
		return 0;
	}
	p->width  = w;
	p->height = h;
	return 1;
}

static int ensure_pipelines(AeronScenePlanarReflection* p) {
	if (!p->resolve) {
		p->resolve = Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
			.name                            = "scene_planar_reflection_resolve.comp",
			.sampler_count                   = 2,
			.readwrite_storage_texture_count = PLANAR_MIPS,
			.uniform_buffer_count            = 1,
			.thread_count_x                  = PLANAR_THREADS,
			.thread_count_y                  = PLANAR_THREADS,
			.thread_count_z                  = 1,
		});
	}
	return p->resolve != NULL;
}

static void disable(AeronScenePlanarReflection* p, const char* why) {
	Aeron_LogWarn("aeron.scene", "planar reflection off: %s", why);
	release_targets(p);
	p->failed = 1;
	p->active = 0;
}

/* Grows [lo, hi] by where `view_proj` puts world point `w` in `viewport`
 * (half-resolution texels); 0 when the point is behind the camera. */
static int grow_screen(const float view_proj[16], const float w[3], const AeronRectI* viewport, float lo[2],
					   float hi[2]) {
	float clip[4];
	for (int r = 0; r < 4; ++r) {
		const float* v = &view_proj[r * 4];
		clip[r]        = v[0] * w[0] + v[1] * w[1] + v[2] * w[2] + v[3];
	}
	if (clip[3] <= 1.0e-4f) {
		return 0;
	}
	const float x = (clip[0] / clip[3] * 0.5f + 0.5f) * (float)viewport->width + (float)viewport->x;
	const float y = (0.5f - clip[1] / clip[3] * 0.5f) * (float)viewport->height + (float)viewport->y;
	lo[0]         = fminf(lo[0], x);
	lo[1]         = fminf(lo[1], y);
	hi[0]         = fmaxf(hi[0], x);
	hi[1]         = fmaxf(hi[1], y);
	return 1;
}

void AeronScenePlanarReflection_Render(AeronScene3D* s, AeronCommandBuffer* cmd) {
	AeronScenePlanarReflection* p       = &s->planar_reflection;
	p->active                           = 0;
	p->drawing                          = 0;
	p->order_count                      = 0;
	const AeronFlightProbeLayout* d = s->planar_reflection_enabled ? plane_definition(s) : NULL;
	if (!d || p->failed) {
		return;
	}
	const AeronSceneProbeSet* set = &s->probe_sets[0];
	float                     n[3], h, m2w[16];
	if (!world_plane(set, d->reflection_plane_center[2], n, &h) || !model_to_world(set, m2w)) {
		return;
	}
	/* Nothing to see from below the floor. */
	const float eye_height = n[0] * s->camera.pos[0] + n[1] * s->camera.pos[1] + n[2] * s->camera.pos[2] - h;
	if (eye_height <= 0.0f) {
		return;
	}

	/* The mirror across the plane, and the view through it. */
	float mirror[16] = { 0 };
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 3; ++c) {
			mirror[r * 4 + c] = (r == c ? 1.0f : 0.0f) - 2.0f * n[r] * n[c];
		}
		mirror[r * 4 + 3] = 2.0f * h * n[r];
	}
	mirror[15] = 1.0f;
	mat4_mul(p->view_proj, s->jittered_view_proj, mirror);
	for (int k = 0; k < 3; ++k) {
		p->eye[k] = s->camera.pos[k] - 2.0f * eye_height * n[k];
	}

	/* The instances taking the set's light, other than its owner, above the
	 * plane and seen in the mirror. The mirrored view's planes cull them in
	 * place. */
	const AeronRectI viewport = { s->camera.viewport.x / 2, s->camera.viewport.y / 2,
								  (s->camera.viewport.width + 1) / 2, (s->camera.viewport.height + 1) / 2 };
	float            planes[5][4];
	AeronSceneInternal_FrustumPlanes(p->view_proj, viewport.width, viewport.height, planes);
	float              cover_lo[2] = { FLT_MAX, FLT_MAX }, cover_hi[2] = { -FLT_MAX, -FLT_MAX };
	int                in_view    = 1;
	static const float axes[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
	for (int i = 0; i < s->instance_count; ++i) {
		const AeronSceneMeshInstance* in = &s->instances[i];
		AeronSceneInstanceBox         box;
		if (!in->mesh || !in->mesh->vbo || in->probe_set_owner != 0 ||
			!AeronSceneInternal_InstanceBox(in, axes[0], axes[1], axes[2], &box) ||
			AeronSceneInternal_InstanceProbeSet(s, in) != 0) {
			continue;
		}
		const float reach = fabsf(n[0]) * box.half[0] + fabsf(n[1]) * box.half[1] + fabsf(n[2]) * box.half[2];
		if (n[0] * box.center[0] + n[1] * box.center[1] + n[2] * box.center[2] - h + reach <= 0.0f) {
			continue;
		}
		int seen = 1;
		for (int k = 0; k < 5 && seen; ++k) {
			const float distance = planes[k][0] * box.center[0] + planes[k][1] * box.center[1] +
								   planes[k][2] * box.center[2] + planes[k][3];
			seen = distance >= -(fabsf(planes[k][0]) * box.half[0] + fabsf(planes[k][1]) * box.half[1] +
								 fabsf(planes[k][2]) * box.half[2]);
		}
		if (!seen) {
			continue;
		}
		/* Its colour draws take the set's light even off the main view. */
		s->prepared_instances[i].probe_set = 0;
		p->order[p->order_count++]         = (uint16_t)i;
		/* Where its reflection can show. */
		for (int c = 0; c < 8 && in_view; ++c) {
			const float corner[3] = { box.center[0] + ((c & 1) ? box.half[0] : -box.half[0]),
									  box.center[1] + ((c & 2) ? box.half[1] : -box.half[1]),
									  box.center[2] + ((c & 4) ? box.half[2] : -box.half[2]) };
			in_view               = grow_screen(p->view_proj, corner, &viewport, cover_lo, cover_hi);
		}
	}
	if (p->order_count == 0) {
		return;
	}
	/* ... within the plane's area. A point behind the camera takes the
	 * whole view. */
	float floor_lo[2] = { FLT_MAX, FLT_MAX }, floor_hi[2] = { -FLT_MAX, -FLT_MAX };
	int   floor_in_view = 1;
	for (int c = 0; c < 4 && floor_in_view; ++c) {
		const float model[3] = {
			d->reflection_plane_center[0] + ((c & 1) ? 0.5f : -0.5f) * d->reflection_plane_size[0],
			d->reflection_plane_center[1] + ((c & 2) ? 0.5f : -0.5f) * d->reflection_plane_size[1],
			d->reflection_plane_center[2],
		};
		float world[3];
		for (int r = 0; r < 3; ++r) {
			world[r] = m2w[r * 4 + 0] * model[0] + m2w[r * 4 + 1] * model[1] + m2w[r * 4 + 2] * model[2] +
					   m2w[r * 4 + 3];
		}
		floor_in_view = grow_screen(s->unjittered_view_proj, world, &viewport, floor_lo, floor_hi);
	}
	float lo[2] = { (float)viewport.x, (float)viewport.y };
	float hi[2] = { (float)(viewport.x + viewport.width), (float)(viewport.y + viewport.height) };
	for (int k = 0; k < 2; ++k) {
		if (in_view) {
			lo[k] = fmaxf(lo[k], cover_lo[k]);
			hi[k] = fminf(hi[k], cover_hi[k]);
		}
		if (floor_in_view) {
			lo[k] = fmaxf(lo[k], floor_lo[k]);
			hi[k] = fminf(hi[k], floor_hi[k]);
		}
	}
	if (hi[0] <= lo[0] || hi[1] <= lo[1]) {
		return;
	}
	const AeronRectI scissor = { (int)floorf(lo[0]), (int)floorf(lo[1]),
								 (int)ceilf(hi[0]) - (int)floorf(lo[0]),
								 (int)ceilf(hi[1]) - (int)floorf(lo[1]) };
	if (!ensure_targets(s) || !ensure_pipelines(p)) {
		disable(p, "its resources could not be created");
		return;
	}

	/* The mirrored view. Its winding is reversed: the instances cull their
	 * other faces. */
	AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target      = p->color,
		.depth_target      = p->depth,
		.viewport          = viewport,
		.scissor           = scissor,
		.clear_color       = 1,
		.clear_color_rgba  = { 0.0f, 0.0f, 0.0f, 0.0f },
		.clear_depth       = 1,
		.clear_depth_value = 0.0f,
		.command_buffer    = cmd,
		.debug_label       = "Planar reflection",
	});
	if (!pass) {
		disable(p, "its render pass failed");
		return;
	}
	AeronSceneInternal_BindFrameUniforms(s, pass);
	p->drawing = 1;
	for (int k = 0; k < p->order_count; ++k) {
		AeronSceneMeshInstance* in = &s->instances[p->order[k]];
		in->cull_mode              = in->cull_mode == AERON_CULL_BACK    ? AERON_CULL_FRONT
									 : in->cull_mode == AERON_CULL_FRONT ? AERON_CULL_BACK
																		 : in->cull_mode;
	}
	const int drawn = AeronScenePbr_DrawInstances(s, cmd, pass, AERON_PBR_PIPE_PLANAR, 0, 0, NULL);
	for (int k = 0; k < p->order_count; ++k) {
		AeronSceneMeshInstance* in = &s->instances[p->order[k]];
		in->cull_mode              = in->cull_mode == AERON_CULL_BACK    ? AERON_CULL_FRONT
									 : in->cull_mode == AERON_CULL_FRONT ? AERON_CULL_BACK
																		 : in->cull_mode;
	}
	p->drawing = 0;
	Aeron_EndRenderPass(pass);
	if (!drawn) {
		disable(p, "its draws failed");
		return;
	}

	/* Colour x coverage and coverage, then the reflected point's height x
	 * coverage and coverage, side by side; then their mips. */
	const int      half   = align_up(p->width, PLANAR_ALIGN);
	const int      height = align_up(p->height, PLANAR_ALIGN);
	float          inverse[16];
	ResolveUniform u = { 0 };
	if (!mat4_invert(inverse, p->view_proj)) {
		return;
	}
	const float* z = &set->world_to_model[8];
	for (int c = 0; c < 4; ++c) {
		u.z_row[c] = z[0] * inverse[0 * 4 + c] + z[1] * inverse[1 * 4 + c] + z[2] * inverse[2 * 4 + c] +
					 z[3] * inverse[3 * 4 + c];
		u.w_row[c] = inverse[3 * 4 + c];
	}
	u.viewport[0] = (float)viewport.x;
	u.viewport[1] = (float)viewport.y;
	u.viewport[2] = (float)viewport.width;
	u.viewport[3] = (float)viewport.height;
	u.plane[0]    = d->reflection_plane_center[2];
	u.dims[0]     = half;
	u.dims[1]     = height;
	u.dims[2]     = p->width;
	u.dims[3]     = p->height;
	/* What the floor reads, on the coarsest level's lattice. */
	int region[4] = { scissor.x - PLANAR_REACH, scissor.y - PLANAR_REACH,
					  scissor.x + scissor.width + PLANAR_REACH, scissor.y + scissor.height + PLANAR_REACH };
	for (int k = 0; k < 4; ++k) {
		const int limit = k & 1 ? height : half;
		region[k]       = k < 2 ? region[k] / PLANAR_ALIGN * PLANAR_ALIGN : align_up(region[k], PLANAR_ALIGN);
		region[k]       = region[k] < 0 ? 0 : region[k] > limit ? limit : region[k];
	}
	memcpy(u.region, region, sizeof region);
	p->screen[0] = lo[0] - PLANAR_READ;
	p->screen[1] = lo[1] - PLANAR_READ;
	p->screen[2] = hi[0] + PLANAR_READ;
	p->screen[3] = hi[1] + PLANAR_READ;

	/* The image and its mips, each written by its own binding. A fresh
	 * image each frame: the last frame's floor, maybe still in flight, reads
	 * the last one. The floor reads only the region written. */
	AeronComputeTextureBinding levels[PLANAR_MIPS];
	for (int level = 0; level < PLANAR_MIPS; ++level) {
		levels[level] = (AeronComputeTextureBinding) { p->image, (uint32_t)level, 0, level == 0 };
	}
	AeronComputePass* resolve = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer      = cmd,
		.write_textures      = levels,
		.write_texture_count = PLANAR_MIPS,
		.debug_label         = "Planar reflection resolve",
	});
	if (!resolve) {
		disable(p, "its compute pass failed");
		return;
	}
	Aeron_BindComputePipeline(resolve, p->resolve);
	Aeron_BindComputeTextureSampler(resolve, 0, Aeron_RenderTargetGetTexture(p->color),
									AeronScenePlanarReflection_Sampler(s));
	Aeron_BindComputeTextureSampler(resolve, 1, Aeron_DepthTargetGetTexture(p->depth),
									AeronScenePlanarReflection_Sampler(s));
	Aeron_BindComputeUniformData(resolve, 0, &u, sizeof u);
	Aeron_DispatchCompute(resolve, (uint32_t)(region[2] - region[0] + PLANAR_THREADS - 1) / PLANAR_THREADS,
						  (uint32_t)(region[3] - region[1] + PLANAR_THREADS - 1) / PLANAR_THREADS, 1);
	Aeron_EndComputePass(resolve);

	p->active = 1;
}
