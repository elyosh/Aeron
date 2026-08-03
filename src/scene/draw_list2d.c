/*
 * aeron_scene 2D blit layer — see aeron/scene/draw_list2d.h.
 * Process-wide singleton: Aeron owns the device, so one shader pair +
 * cache set serves every caller.
 */

#include "aeron/scene/draw_list2d.h"

#include "aeron/log.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <string.h>

/* Keyed by (color_format, blend, depth_format, sample_count, depth_test). A handful of combos
 * exist in practice (HDR scene formats, swapchain formats, cockpit
 * chrome); 24 leaves comfortable headroom. */
#define AERON_BLIT2D_PIPELINE_CACHE_CAP 24

typedef struct Blit2DPipelineEntry {
	AeronTextureFormat     format;
	AeronBlit2DBlend       blend;
	AeronTextureFormat     depth_format; /* UNKNOWN = no depth */
	AeronSampleCount       sample_count;
	int                    depth_test;
	AeronGraphicsPipeline* pipe;
} Blit2DPipelineEntry;

static struct {
	int                 initialized;
	int                 failed;
	AeronShader*        vs;
	AeronShader*        fs;
	AeronShader*        vs4; /* 4-corner free-quad vertex shader */
	Blit2DPipelineEntry pipelines[AERON_BLIT2D_PIPELINE_CACHE_CAP];
	int                 pipeline_count;
	Blit2DPipelineEntry pipelines4[AERON_BLIT2D_PIPELINE_CACHE_CAP];
	int                 pipeline4_count;
	/* Rounded-rect (UI) shader pair — loaded lazily on first use so
	 * consumers that never draw rrects pay nothing. */
	int                 rr_initialized;
	int                 rr_failed;
	AeronShader*        vs_rr;
	AeronShader*        fs_rr;
	Blit2DPipelineEntry pipelines_rr[AERON_BLIT2D_PIPELINE_CACHE_CAP];
	int                 pipeline_rr_count;
	AeronSampler*       sampler_nearest;
	AeronSampler*       sampler_linear;
	uint32_t            draw_list_count;
} G;

static int blit2d_ensure(void) {
	if (G.initialized) {
		return !G.failed;
	}
	G.initialized = 1;

	G.vs = Aeron_CreateShader(&(AeronShaderDesc){
		.name                 = "scene_blit.vert",
		.stage                = AERON_SHADER_STAGE_VERTEX,
		.sampler_count        = 0,
		.uniform_buffer_count = 1,
		.storage_buffer_count = 1,
	});
	G.fs = Aeron_CreateShader(&(AeronShaderDesc){
		.name                 = "scene_blit.frag",
		.stage                = AERON_SHADER_STAGE_FRAGMENT,
		.sampler_count        = 1,
		.uniform_buffer_count = 0,
	});
	G.vs4 = Aeron_CreateShader(&(AeronShaderDesc){
		.name                 = "scene_blit4.vert",
		.stage                = AERON_SHADER_STAGE_VERTEX,
		.sampler_count        = 0,
		.uniform_buffer_count = 1,
	});
	if (!G.vs || !G.fs || !G.vs4) {
		Aeron_LogError("aeron.scene", "blit shader load failed");
		G.failed = 1;
		return 0;
	}

	/* CLAMP_TO_EDGE on all axes so quads sampling slightly past the
	 * texture edge (sub-pixel rounding) reuse the edge texel instead of
	 * black. */
	AeronSamplerDesc s = (AeronSamplerDesc){
		.min_filter = AERON_FILTER_NEAREST,
		.mag_filter = AERON_FILTER_NEAREST,
		.mip_filter = AERON_FILTER_NEAREST,
		.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.min_lod    = 0.0f,
		.max_lod    = 1000.0f,
	};
	G.sampler_nearest = Aeron_CreateSampler(&s);
	s.min_filter      = AERON_FILTER_LINEAR;
	s.mag_filter      = AERON_FILTER_LINEAR;
	s.mip_filter      = AERON_FILTER_LINEAR;
	G.sampler_linear  = Aeron_CreateSampler(&s);
	if (!G.sampler_nearest || !G.sampler_linear) {
		Aeron_LogError("aeron.scene", "blit sampler creation failed");
		G.failed = 1;
		return 0;
	}
	return 1;
}

static int blit2d_rr_ensure(void) {
	if (G.rr_initialized) {
		return !G.rr_failed;
	}
	G.rr_initialized = 1;

	G.vs_rr = Aeron_CreateShader(&(AeronShaderDesc){
		.name                 = "scene_ui_rect.vert",
		.stage                = AERON_SHADER_STAGE_VERTEX,
		.sampler_count        = 0,
		.uniform_buffer_count = 1,
		.storage_buffer_count = 1,
	});
	G.fs_rr = Aeron_CreateShader(&(AeronShaderDesc){
		.name                 = "scene_ui_rect.frag",
		.stage                = AERON_SHADER_STAGE_FRAGMENT,
		.sampler_count        = 0,
		.uniform_buffer_count = 0,
	});
	if (!G.vs_rr || !G.fs_rr) {
		Aeron_LogError("aeron.scene", "rrect shader load failed");
		G.rr_failed = 1;
		return 0;
	}
	return 1;
}

static void blit2d_shutdown(void) {
	for (int i = 0; i < G.pipeline_count; i++) {
		if (G.pipelines[i].pipe) {
			Aeron_DestroyGraphicsPipeline(G.pipelines[i].pipe);
		}
	}
	for (int i = 0; i < G.pipeline4_count; i++) {
		if (G.pipelines4[i].pipe) {
			Aeron_DestroyGraphicsPipeline(G.pipelines4[i].pipe);
		}
	}
	for (int i = 0; i < G.pipeline_rr_count; i++) {
		if (G.pipelines_rr[i].pipe) {
			Aeron_DestroyGraphicsPipeline(G.pipelines_rr[i].pipe);
		}
	}
	if (G.sampler_nearest) Aeron_DestroySampler(G.sampler_nearest);
	if (G.sampler_linear) Aeron_DestroySampler(G.sampler_linear);
	if (G.vs) Aeron_DestroyShader(G.vs);
	if (G.fs) Aeron_DestroyShader(G.fs);
	if (G.vs4) Aeron_DestroyShader(G.vs4);
	if (G.vs_rr) Aeron_DestroyShader(G.vs_rr);
	if (G.fs_rr) Aeron_DestroyShader(G.fs_rr);
	memset(&G, 0, sizeof G);
}

static AeronSampler* blit2d_sampler(AeronBlit2DFilter filter) {
	if (!blit2d_ensure()) {
		return NULL;
	}
	return (filter == AERON_BLIT2D_FILTER_LINEAR) ? G.sampler_linear : G.sampler_nearest;
}

/* Premultiplied-alpha over: dst = src.rgb + (1-src.a) * dst.rgb;
 * dst.a = src.a + (1-src.a)*dst.a. */
static AeronBlendStateDesc blit_blend_state(AeronBlit2DBlend blend) {
	AeronBlendStateDesc bs = { 0 };
	if (blend == AERON_BLIT2D_BLEND_PMA) {
		bs.enabled   = 1;
		bs.src_color = AERON_BLEND_ONE;
		bs.dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA;
		bs.color_op  = AERON_BLEND_OP_ADD;
		bs.src_alpha = AERON_BLEND_ONE;
		bs.dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA;
		bs.alpha_op  = AERON_BLEND_OP_ADD;
	}
	return bs;
}

static AeronGraphicsPipeline* cache_lookup(Blit2DPipelineEntry* entries, int* count,
										   AeronShader* vs, AeronShader* fs,
										   AeronTextureFormat color_format,
										   AeronBlit2DBlend blend, AeronTextureFormat depth_format,
										   AeronSampleCount sample_count, int depth_test, const char* what) {
	for (int i = 0; i < *count; i++) {
		Blit2DPipelineEntry* e = &entries[i];
		if (e->format == color_format && e->blend == blend && e->depth_format == depth_format &&
			e->sample_count == sample_count && e->depth_test == depth_test) {
			return e->pipe;
		}
	}
	if (*count >= AERON_BLIT2D_PIPELINE_CACHE_CAP) {
		Aeron_LogWarn("aeron.scene", "%s cache full", what);
		return NULL;
	}
	if (depth_test && depth_format == AERON_TEXTURE_FORMAT_UNKNOWN) {
		return NULL;
	}
	AeronGraphicsPipeline* p = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc){
		.vertex_shader   = vs,
		.fragment_shader = fs,
		.primitive_type  = AERON_PRIMITIVE_TRIANGLE_STRIP,
		.cull_mode       = AERON_CULL_NONE,
		.color_format    = color_format,
		.depth_format    = depth_format,
		.depth           = depth_test ? (AeronDepthStateDesc){ .depth_test = 1,
															   .depth_write = 0,
															   .compare = AERON_COMPARE_GREATER_EQUAL }
									 : (AeronDepthStateDesc){ 0 },
		.blend           = blit_blend_state(blend),
		.sample_count    = sample_count,
	});
	if (!p) {
		Aeron_LogError("aeron.scene", "%s creation failed (fmt=%d blend=%d depth=%d)", what,
					   (int)color_format, (int)blend, (int)depth_format);
		return NULL;
	}
	entries[(*count)++] = (Blit2DPipelineEntry){
		.format       = color_format,
		.blend        = blend,
		.depth_format = depth_format,
		.sample_count = sample_count,
		.depth_test   = depth_test,
		.pipe         = p,
	};
	return p;
}

static AeronGraphicsPipeline* blit2d_pipeline(AeronTextureFormat color_format,
											  AeronBlit2DBlend blend,
											  AeronTextureFormat depth_format,
											  AeronSampleCount sample_count) {
	if (!blit2d_ensure()) {
		return NULL;
	}
	return cache_lookup(G.pipelines, &G.pipeline_count, G.vs, G.fs, color_format, blend,
						depth_format, sample_count, 0, "blit pipeline");
}

static AeronGraphicsPipeline* blit2d_pipeline4(AeronTextureFormat color_format,
											   AeronBlit2DBlend blend,
											   AeronTextureFormat depth_format,
											   AeronSampleCount sample_count, int depth_test) {
	if (!blit2d_ensure()) {
		return NULL;
	}
	return cache_lookup(G.pipelines4, &G.pipeline4_count, G.vs4, G.fs, color_format, blend,
						depth_format, sample_count, depth_test, "blit4 pipeline");
}

/* Rounded rects always blend PMA and never depth-test. */
static AeronGraphicsPipeline* blit2d_pipeline_rr(AeronTextureFormat color_format,
												 AeronTextureFormat depth_format,
												 AeronSampleCount sample_count) {
	if (!blit2d_rr_ensure()) {
		return NULL;
	}
	return cache_lookup(G.pipelines_rr, &G.pipeline_rr_count, G.vs_rr, G.fs_rr, color_format,
						AERON_BLIT2D_BLEND_PMA, depth_format, sample_count, 0, "rrect pipeline");
}

typedef struct Blit2DInstance {
	float dst_x, dst_y, dst_w, dst_h;
	float src_u0, src_v0, src_u1, src_v1;
	float tint_r, tint_g, tint_b, tint_a;
	float bias_r, bias_g, bias_b, bias_a;
	float trap_top_dx_left, trap_top_dx_right, trap_top_w, _trap_pad;
} Blit2DInstance;

typedef struct Blit2DQuad4Uniform {
	float corners[4][4];
	float q[4];
	float ndc_depth[4];
	float tint_r, tint_g, tint_b, tint_a;
	float bias_r, bias_g, bias_b, bias_a;
} Blit2DQuad4Uniform;

typedef struct Blit2DRunUniform {
	float    ndc_scale[2];
	float    output_rgb_scale;
	uint32_t base_instance;
} Blit2DRunUniform;

/* Storage layout MUST match RRectInstance in scene_ui_rect.vert.hlsl. */
typedef struct Blit2DRRectInstance {
	float dst_x, dst_y, dst_w, dst_h;
	float radius, border, bevel, soft;
	float fill_top[4];
	float fill_bottom[4];
	float border_col[4];
	float bevel_hi[4];
	float bevel_lo[4];
} Blit2DRRectInstance;

typedef char Blit2DInstanceSizeCheck[sizeof(Blit2DInstance) == 80 ? 1 : -1];
typedef char Blit2DQuad4UniformSizeCheck[sizeof(Blit2DQuad4Uniform) == 128 ? 1 : -1];
typedef char Blit2DRunUniformSizeCheck[sizeof(Blit2DRunUniform) == 16 ? 1 : -1];
typedef char Blit2DRRectInstanceSizeCheck[sizeof(Blit2DRRectInstance) == 112 ? 1 : -1];

static void blit2d_draw_quad4(AeronRenderPass* pass, AeronGraphicsPipeline* pipe,
							 AeronTexture* tex, AeronSampler* sampler,
							 const Blit2DQuad4Uniform* u) {
	if (!pass || !pipe || !tex || !sampler || !u) {
		return;
	}
	Aeron_BindGraphicsPipeline(pass, pipe);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0, tex, sampler);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, u, sizeof *u);
	Aeron_Draw(pass, 4, 0);
}

/* ===================================================================
 * AeronDrawList2D — retained record list on top of the batcher.
 * ================================================================= */

#include "internal.h" /* AeronSceneInternal_WhiteTexture */

#include <stdlib.h>

#define AERON_DRAWLIST2D_DEFAULT_CAP 4096

typedef enum {
	DL2D_SPRITE = 0,
	DL2D_QUAD4  = 1,
	DL2D_RRECT  = 2,
} Dl2dKind;

typedef struct Dl2dRecord {
	uint8_t           kind;
	uint8_t           depth_test;
	AeronBlit2DBlend  blend;
	AeronBlit2DFilter filter;
	AeronTexture*     texture;
	AeronRectI        scissor; /* zero w/h = none */
	uint32_t          instance_index;
	union {
		Blit2DInstance      quad;
		Blit2DQuad4Uniform  quad4;
		Blit2DRRectInstance rrect;
	} u;
} Dl2dRecord;

struct AeronDrawList2D {
	Dl2dRecord*              records;
	Blit2DInstance*          instances;
	AeronBuffer*             instance_buffer;
	uint32_t                 instance_buffer_capacity;
	uint32_t                 instance_count;
	/* Rounded-rect instances — allocated lazily on the first AddRRect so
	 * sprite-only consumers pay nothing. */
	Blit2DRRectInstance*     rr_instances;
	AeronBuffer*             rr_instance_buffer;
	uint32_t                 rr_instance_buffer_capacity;
	uint32_t                 rr_instance_count;
	int                      cap;
	int                      count;
	int                      dropped;
	int                      active;
	int                      prepared;
	int                      storage_error_logged;
	AeronRenderTarget*       target;
	int                      target_w, target_h;
	AeronDrawList2DClearMode clear_mode;
	float                    clear_rgba[4];
};

AeronDrawList2D* AeronDrawList_Create(int record_cap) {
	AeronDrawList2D* l = (AeronDrawList2D*)calloc(1, sizeof *l);
	if (!l) {
		return NULL;
	}
	l->cap     = record_cap > 0 ? record_cap : AERON_DRAWLIST2D_DEFAULT_CAP;
	l->records = (Dl2dRecord*)calloc((size_t)l->cap, sizeof *l->records);
	l->instances = (Blit2DInstance*)calloc((size_t)l->cap, sizeof *l->instances);
	if (!l->records || !l->instances) {
		free(l->instances);
		free(l->records);
		free(l);
		return NULL;
	}
	G.draw_list_count++;
	return l;
}

void AeronDrawList_Destroy(AeronDrawList2D* l) {
	if (!l) {
		return;
	}
	Aeron_DestroyBuffer(l->instance_buffer);
	Aeron_DestroyBuffer(l->rr_instance_buffer);
	free(l->rr_instances);
	free(l->instances);
	free(l->records);
	free(l);
	if (G.draw_list_count > 0 && --G.draw_list_count == 0) {
		blit2d_shutdown();
	}
}

void AeronDrawList_Begin(AeronDrawList2D* l, AeronRenderTarget* target, int target_w,
						 int target_h, AeronDrawList2DClearMode clear_mode,
						 const float clear_rgba[4]) {
	if (!l) {
		return;
	}
	l->target     = target;
	l->target_w   = target_w > 0 ? target_w : 1;
	l->target_h   = target_h > 0 ? target_h : 1;
	l->clear_mode = clear_mode;
	if (clear_rgba) {
		memcpy(l->clear_rgba, clear_rgba, sizeof l->clear_rgba);
	} else {
		memset(l->clear_rgba, 0, sizeof l->clear_rgba);
	}
	l->count   = 0;
	l->dropped = 0;
	l->active  = 1;
	l->prepared = 0;
}

static Dl2dRecord* dl2d_alloc(AeronDrawList2D* l) {
	if (!l || !l->active) {
		return NULL;
	}
	if (l->count >= l->cap) {
		if (!l->dropped) {
			Aeron_LogWarn("aeron.scene", "drawlist2d record cap (%d) hit; dropping", l->cap);
		}
		l->dropped++;
		return NULL;
	}
	Dl2dRecord* r = &l->records[l->count++];
	memset(r, 0, sizeof *r);
	l->prepared = 0;
	return r;
}

void AeronDrawList_AddSprite(AeronDrawList2D* l, const AeronDrawList2DSprite* s) {
	if (!s || !s->texture) {
		return;
	}
	Dl2dRecord* r = dl2d_alloc(l);
	if (!r) {
		return;
	}
	r->kind    = DL2D_SPRITE;
	r->blend   = s->blend;
	r->filter  = s->filter;
	r->texture = s->texture;
	r->scissor = s->scissor;

	Blit2DInstance* q = &r->u.quad;
	q->dst_x           = s->dst_x; /* px; converted to NDC at Render */
	q->dst_y           = s->dst_y;
	q->dst_w           = s->dst_w;
	q->dst_h           = s->dst_h;
	q->src_u0          = s->src_u0;
	q->src_v0          = s->src_v0;
	q->src_u1          = s->src_u1;
	q->src_v1          = s->src_v1;
	q->tint_r = s->tint[0];
	q->tint_g = s->tint[1];
	q->tint_b = s->tint[2];
	q->tint_a = s->tint[3];
	q->bias_r = s->bias[0];
	q->bias_g = s->bias[1];
	q->bias_b = s->bias[2];
	q->bias_a = s->bias[3];
	/* Pixel insets; converted to NDC deltas at Render. trap_top_w
	 * passes through (0 → shader treats as 1.0). */
	q->trap_top_dx_left  = s->trap_top_dx_left_px;
	q->trap_top_dx_right = s->trap_top_dx_right_px;
	q->trap_top_w        = s->trap_top_w;
}

void AeronDrawList_AddQuad4(AeronDrawList2D* l, const AeronDrawList2DQuad4* in) {
	if (!in || !in->texture) {
		return;
	}
	Dl2dRecord* r = dl2d_alloc(l);
	if (!r) {
		return;
	}
	r->kind    = DL2D_QUAD4;
	r->blend   = in->blend;
	r->filter  = in->filter;
	r->texture = in->texture;
	r->scissor = in->scissor;
	r->depth_test = in->depth_test != 0;

	Blit2DQuad4Uniform* q = &r->u.quad4;
	/* Header order TL,TR,BL,BR happens to BE the strip zig-zag order
	 * the shader expects — copy through. */
	memcpy(q->corners, in->corners, sizeof q->corners);
	for (int i = 0; i < 4; i++) {
		q->q[i] = in->q[i] != 0.0f ? in->q[i] : 1.0f;
		q->ndc_depth[i] = in->ndc_depth[i];
	}
	q->tint_r = in->tint[0];
	q->tint_g = in->tint[1];
	q->tint_b = in->tint[2];
	q->tint_a = in->tint[3];
	q->bias_r = in->bias[0];
	q->bias_g = in->bias[1];
	q->bias_b = in->bias[2];
	q->bias_a = in->bias[3];
}

void AeronDrawList_AddRRect(AeronDrawList2D* l, const AeronDrawList2DRRect* in) {
	if (!l || !in || in->dst_w <= 0.0f || in->dst_h <= 0.0f) {
		return;
	}
	if (!l->rr_instances) {
		l->rr_instances = (Blit2DRRectInstance*)calloc((size_t)l->cap, sizeof *l->rr_instances);
		if (!l->rr_instances) {
			Aeron_LogError("aeron.scene", "drawlist2d rrect instance allocation failed");
			return;
		}
	}
	Dl2dRecord* r = dl2d_alloc(l);
	if (!r) {
		return;
	}
	r->kind    = DL2D_RRECT;
	r->blend   = AERON_BLIT2D_BLEND_PMA;
	r->scissor = in->scissor;

	Blit2DRRectInstance* q = &r->u.rrect;
	q->dst_x  = in->dst_x;
	q->dst_y  = in->dst_y;
	q->dst_w  = in->dst_w;
	q->dst_h  = in->dst_h;
	q->radius = in->radius_px > 0.0f ? in->radius_px : 0.0f;
	q->border = in->border_px > 0.0f ? in->border_px : 0.0f;
	q->bevel  = in->bevel_px > 0.0f ? in->bevel_px : 0.0f;
	q->soft   = in->soft_px > 1.0f ? in->soft_px : 1.0f;
	memcpy(q->fill_top, in->fill_top, sizeof q->fill_top);
	memcpy(q->fill_bottom, in->fill_bottom, sizeof q->fill_bottom);
	memcpy(q->border_col, in->border, sizeof q->border_col);
	memcpy(q->bevel_hi, in->bevel_hi, sizeof q->bevel_hi);
	memcpy(q->bevel_lo, in->bevel_lo, sizeof q->bevel_lo);
}

static void dl2d_add_line(AeronDrawList2D* l, float x0, float y0, float x1, float y1,
						  float thickness_px, const float rgba[4], AeronBlit2DBlend blend,
						  const AeronRectI* scissor, float clip_w0, float clip_w1, float clip_z,
						  int depth_test) {
	AeronTexture* white = AeronSceneInternal_WhiteTexture();
	if (!white) {
		return;
	}
	/* Segment direction; degenerate segments render as a stroke-sized
	 * dot. Endpoints are texel centers, so extend both ends by half a
	 * stroke — matches the inclusive pixel span a Bresenham line
	 * covers. */
	float dx  = x1 - x0;
	float dy  = y1 - y0;
	float len = sqrtf(dx * dx + dy * dy);
	float ux, uy;
	if (len < 1e-4f) {
		ux = 1.0f;
		uy = 0.0f;
	} else {
		ux = dx / len;
		uy = dy / len;
	}
	const float h  = thickness_px * 0.5f;
	const float ax = x0 - ux * h, ay = y0 - uy * h;
	const float bx = x1 + ux * h, by = y1 + uy * h;
	const float nx = -uy * h, ny = ux * h;

	AeronDrawList2DQuad4 q = { 0 };
	q.texture              = white;
	q.blend                = blend;
	q.depth_test           = depth_test;
	/* Strip order TL, TR, BL, BR; white-texel center UVs. */
	q.corners[0][0] = ax + nx; q.corners[0][1] = ay + ny; q.corners[0][2] = 0.5f; q.corners[0][3] = 0.5f;
	q.corners[1][0] = bx + nx; q.corners[1][1] = by + ny; q.corners[1][2] = 0.5f; q.corners[1][3] = 0.5f;
	q.corners[2][0] = ax - nx; q.corners[2][1] = ay - ny; q.corners[2][2] = 0.5f; q.corners[2][3] = 0.5f;
	q.corners[3][0] = bx - nx; q.corners[3][1] = by - ny; q.corners[3][2] = 0.5f; q.corners[3][3] = 0.5f;
	if (depth_test) {
		q.q[0] = q.q[2] = clip_w0;
		q.q[1] = q.q[3] = clip_w1;
		q.ndc_depth[0] = q.ndc_depth[2] = clip_z / clip_w0;
		q.ndc_depth[1] = q.ndc_depth[3] = clip_z / clip_w1;
	}
	memcpy(q.tint, rgba, sizeof q.tint);
	if (scissor) {
		q.scissor = *scissor;
	}
	AeronDrawList_AddQuad4(l, &q);
}

void AeronDrawList_AddLine(AeronDrawList2D* l, float x0, float y0, float x1, float y1,
						   float thickness_px, const float rgba[4], AeronBlit2DBlend blend,
						   const AeronRectI* scissor) {
	if (!rgba || thickness_px <= 0.0f) {
		return;
	}
	dl2d_add_line(l, x0, y0, x1, y1, thickness_px, rgba, blend, scissor, 1.0f, 1.0f, 0.0f, 0);
}

void AeronDrawList_AddProjectedLine(AeronDrawList2D* l, float x0, float y0, float clip_w0,
									float x1, float y1, float clip_w1, float clip_z,
									float thickness_px, const float rgba[4], AeronBlit2DBlend blend,
									const AeronRectI* scissor) {
	if (!rgba || thickness_px <= 0.0f || clip_w0 <= 0.0f || clip_w1 <= 0.0f || clip_z < 0.0f) {
		return;
	}
	dl2d_add_line(l, x0, y0, x1, y1, thickness_px, rgba, blend, scissor, clip_w0, clip_w1, clip_z, 1);
}

void AeronDrawList_AddFill(AeronDrawList2D* l, float x, float y, float w, float h,
						   const float rgba[4], AeronBlit2DBlend blend,
						   const AeronRectI* scissor) {
	if (!rgba || w <= 0.0f || h <= 0.0f) {
		return;
	}
	AeronTexture* white = AeronSceneInternal_WhiteTexture();
	if (!white) {
		return;
	}
	AeronDrawList2DSprite s = { 0 };
	s.texture               = white;
	s.src_u1                = 1.0f;
	s.src_v1                = 1.0f;
	s.dst_x                 = x;
	s.dst_y                 = y;
	s.dst_w                 = w;
	s.dst_h                 = h;
	memcpy(s.tint, rgba, sizeof s.tint);
	s.blend  = blend;
	s.filter = AERON_BLIT2D_FILTER_NEAREST;
	if (scissor) {
		s.scissor = *scissor;
	}
	AeronDrawList_AddSprite(l, &s);
}

void AeronDrawList_AddFrame(AeronDrawList2D* l, float x, float y, float w, float h,
							float thickness_px, const float rgba[4], AeronBlit2DBlend blend,
							const AeronRectI* scissor) {
	if (thickness_px <= 0.0f || w <= 0.0f || h <= 0.0f) {
		return;
	}
	float t = thickness_px;
	if (t * 2.0f > w) t = w * 0.5f;
	if (t * 2.0f > h) t = h * 0.5f;
	AeronDrawList_AddFill(l, x, y, w, t, rgba, blend, scissor);                  /* top */
	AeronDrawList_AddFill(l, x, y + h - t, w, t, rgba, blend, scissor);          /* bottom */
	AeronDrawList_AddFill(l, x, y + t, t, h - 2.0f * t, rgba, blend, scissor);   /* left */
	AeronDrawList_AddFill(l, x + w - t, y + t, t, h - 2.0f * t, rgba, blend,
						  scissor);                                              /* right */
}

/* Grows (never shrinks) one storage buffer and uploads `required` bytes. */
static int dl2d_upload_storage(AeronDrawList2D* l, AeronCommandBuffer* cmd, AeronBuffer** buffer,
							   uint32_t* buffer_capacity, const void* data, uint32_t required,
							   const char* debug_name) {
	if (!*buffer || *buffer_capacity < required) {
		uint32_t capacity = *buffer_capacity ? *buffer_capacity : 4096u;
		while (capacity < required) {
			if (capacity > UINT32_MAX / 2u) {
				capacity = required;
				break;
			}
			capacity *= 2u;
		}
		AeronBuffer* replacement = Aeron_CreateBuffer(&(AeronBufferDesc){
			.size         = capacity,
			.usage        = AERON_BUFFER_USAGE_STORAGE,
			.memory_usage = AERON_MEMORY_USAGE_DYNAMIC,
			.debug_name   = debug_name,
		});
		if (!replacement) {
			if (!l->storage_error_logged) {
				Aeron_LogError("aeron.scene", "drawlist2d storage allocation failed (%u bytes): %s", capacity,
							   SDL_GetError());
				l->storage_error_logged = 1;
			}
			Aeron_CommandBufferSetFailure(cmd, "2D draw-list storage allocation failed");
			return 0;
		}
		Aeron_DestroyBuffer(*buffer);
		*buffer          = replacement;
		*buffer_capacity = capacity;
	}
	if (!Aeron_UploadBufferDataCmd(cmd, *buffer, 0, data, required)) {
		if (!l->storage_error_logged) {
			Aeron_LogError("aeron.scene", "drawlist2d storage upload failed (%u bytes): %s", required,
						   SDL_GetError());
			l->storage_error_logged = 1;
		}
		return 0;
	}
	return 1;
}

int AeronDrawList_Prepare(AeronDrawList2D* l, AeronCommandBuffer* cmd) {
	if (!l || !cmd || !l->active) {
		return 0;
	}
	l->instance_count    = 0;
	l->rr_instance_count = 0;
	for (int i = 0; i < l->count; ++i) {
		Dl2dRecord* r = &l->records[i];
		if (r->kind == DL2D_SPRITE) {
			r->instance_index                 = l->instance_count;
			l->instances[l->instance_count++] = r->u.quad;
		} else if (r->kind == DL2D_RRECT) {
			r->instance_index                       = l->rr_instance_count;
			l->rr_instances[l->rr_instance_count++] = r->u.rrect;
		}
	}
	l->prepared = 0;
	if (l->instance_count > 0 &&
		!dl2d_upload_storage(l, cmd, &l->instance_buffer, &l->instance_buffer_capacity,
							 l->instances, l->instance_count * (uint32_t)sizeof *l->instances,
							 "scene.draw_list2d.instances")) {
		return 0;
	}
	if (l->rr_instance_count > 0 &&
		!dl2d_upload_storage(l, cmd, &l->rr_instance_buffer, &l->rr_instance_buffer_capacity,
							 l->rr_instances,
							 l->rr_instance_count * (uint32_t)sizeof *l->rr_instances,
							 "scene.draw_list2d.rrect_instances")) {
		return 0;
	}
	l->prepared             = 1;
	l->storage_error_logged = 0;
	return 1;
}

static int dl2d_same_scissor(const AeronRectI* a, const AeronRectI* b) {
	return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}

static int dl2d_same_sprite_state(const Dl2dRecord* a, const Dl2dRecord* b) {
	return b->kind == DL2D_SPRITE && a->blend == b->blend && a->filter == b->filter &&
		   a->texture == b->texture && dl2d_same_scissor(&a->scissor, &b->scissor);
}

static int dl2d_same_rrect_state(const Dl2dRecord* a, const Dl2dRecord* b) {
	return b->kind == DL2D_RRECT && dl2d_same_scissor(&a->scissor, &b->scissor);
}

static void dl2d_set_scissor(AeronDrawList2D* l, AeronRenderPass* pass, const AeronRectI* scissor) {
	if (scissor->width > 0 && scissor->height > 0) {
		Aeron_SetScissor(pass, scissor);
	} else {
		const AeronRectI full = { 0, 0, l->target_w, l->target_h };
		Aeron_SetScissor(pass, &full);
	}
}

static int draw_list_encode(AeronDrawList2D* l, AeronRenderPass* pass,
							AeronRenderTarget* target) {
	const AeronTextureFormat fmt = Aeron_TextureGetFormat(Aeron_RenderTargetGetTexture(target));
	const AeronTextureFormat depth_format = Aeron_RenderPassGetDepthFormat(pass);
	const AeronSampleCount sample_count = Aeron_RenderPassGetSampleCount(pass);
	const float output_rgb_scale = Aeron_RenderPassOutputRgbScale(pass);
	const Blit2DRunUniform run_base = {
		.ndc_scale       = { 2.0f / (float)l->target_w, 2.0f / (float)l->target_h },
		.output_rgb_scale = output_rgb_scale,
		.base_instance   = 0,
	};

	const AeronRectI full_scissor = { 0, 0, l->target_w, l->target_h };
	Aeron_SetScissor(pass, &full_scissor);

	for (int i = 0; i < l->count;) {
		const Dl2dRecord* r = &l->records[i];
		dl2d_set_scissor(l, pass, &r->scissor);
		if (r->kind == DL2D_SPRITE) {
			int end = i + 1;
			while (end < l->count && dl2d_same_sprite_state(r, &l->records[end])) {
				++end;
			}
			AeronGraphicsPipeline* pipe =
				blit2d_pipeline(fmt, r->blend, depth_format, sample_count);
			AeronSampler* sampler = blit2d_sampler(r->filter);
			if (!pipe || !sampler || !l->instance_buffer) {
				return 0;
			}
			Blit2DRunUniform run = run_base;
			run.base_instance = r->instance_index;
			Aeron_BindGraphicsPipeline(pass, pipe);
			Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0, r->texture, sampler);
			Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, l->instance_buffer);
			Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &run, sizeof run);
			Aeron_DrawInstanced(pass, 4, (uint32_t)(end - i), 0);
			i = end;
			continue;
		}
		if (r->kind == DL2D_RRECT) {
			int end = i + 1;
			while (end < l->count && dl2d_same_rrect_state(r, &l->records[end])) {
				++end;
			}
			AeronGraphicsPipeline* pipe = blit2d_pipeline_rr(fmt, depth_format, sample_count);
			if (!pipe || !l->rr_instance_buffer) {
				return 0;
			}
			Blit2DRunUniform run = run_base;
			run.base_instance = r->instance_index;
			Aeron_BindGraphicsPipeline(pass, pipe);
			Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, l->rr_instance_buffer);
			Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &run, sizeof run);
			Aeron_DrawInstanced(pass, 4, (uint32_t)(end - i), 0);
			i = end;
			continue;
		}

		AeronGraphicsPipeline* pipe =
			blit2d_pipeline4(fmt, r->blend, depth_format, sample_count, r->depth_test);
		AeronSampler* sampler = blit2d_sampler(r->filter);
		if (!pipe || !sampler) {
			return 0;
		}
		Blit2DQuad4Uniform q = r->u.quad4;
		for (int c = 0; c < 4; c++) {
			q.corners[c][0] = q.corners[c][0] * run_base.ndc_scale[0] - 1.0f;
			q.corners[c][1] = 1.0f - q.corners[c][1] * run_base.ndc_scale[1];
		}
		q.tint_r *= output_rgb_scale;
		q.tint_g *= output_rgb_scale;
		q.tint_b *= output_rgb_scale;
		q.bias_r *= output_rgb_scale;
		q.bias_g *= output_rgb_scale;
		q.bias_b *= output_rgb_scale;
		blit2d_draw_quad4(pass, pipe, r->texture, sampler, &q);
		++i;
	}
	return 1;
}

void AeronDrawList_RenderIntoPass(AeronDrawList2D* l, AeronCommandBuffer* cmd,
								  AeronRenderPass* pass, AeronRenderTarget* target) {
	if (!l || !cmd || !pass || !target || l->count == 0) {
		return;
	}
	if (!l->prepared) {
		Aeron_CommandBufferSetFailure(cmd, "2D draw list was not prepared before rendering");
		return;
	}
	AeronTexture* texture = Aeron_RenderTargetGetTexture(target);
	if (!texture || Aeron_TextureGetWidth(texture) != l->target_w ||
		Aeron_TextureGetHeight(texture) != l->target_h || !blit2d_ensure()) {
		Aeron_CommandBufferSetFailure(cmd, "2D draw-list pass preparation failed");
		return;
	}
	if (!draw_list_encode(l, pass, target)) {
		Aeron_CommandBufferSetFailure(cmd, "2D draw-list encoding failed");
	}
}

void AeronDrawList_Render(AeronDrawList2D* l, AeronCommandBuffer* cmd) {
	if (!l || !cmd || !l->target || !blit2d_ensure() || !AeronDrawList_Prepare(l, cmd)) {
		if (cmd && l && l->target) {
			Aeron_CommandBufferSetFailure(cmd, "2D draw-list preparation failed");
		}
		return;
	}
	AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc){
		.color_target     = l->target,
		.clear_color      = l->clear_mode == AERON_DRAWLIST2D_CLEAR ? 1 : 0,
		.clear_color_rgba = { l->clear_rgba[0], l->clear_rgba[1], l->clear_rgba[2],
							  l->clear_rgba[3] },
		.command_buffer   = cmd,
		.debug_label      = "2D draw list",
	});
	if (!pass) {
		return;
	}
	if (!draw_list_encode(l, pass, l->target)) {
		Aeron_CommandBufferSetFailure(cmd, "2D draw-list encoding failed");
	}
	Aeron_EndRenderPass(pass);
}
