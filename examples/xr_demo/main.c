/*
 * xr_demo — a small test program for the Aeron XR layer.
 *
 * It draws a ring of spinning cubes once for each view of a view rig. When
 * Aeron has a running OpenXR session, the views are the eyes of the headset
 * and the cubes are drawn into the eye images. Otherwise the views come
 * from a fixed stereo rig and are drawn side by side in the window.
 *
 * With a headset, the program also tests the virtual screen and the mirror:
 *   - It submits a small panel with one cube as a layer. Aeron shows the
 *     layers on the virtual screen, a flat screen in front of the user.
 *   - It draws the left eye's view in the window, as the eye mirror. Aeron
 *     draws its copy of the virtual screen over it.
 *
 * The program logs its frame rate every five seconds, and the session's
 * state when it changes. The R key recentres the view.
 *
 * Usage: xr_demo [--vr | --no-vr] [--screen <panel|full|off>] [--frames <count>]
 *                [--capture <file.ppm>]
 *   --vr                  Fail when VR can't start. Without it, VR is used
 *                         when a headset is found.
 *   --no-vr               Don't use VR.
 *   --screen <mode>       What the virtual screen shows in the headset:
 *                           panel  the eye images, and the panel (default);
 *                           full   no eye images: the picture that the
 *                                  window shows without a headset fills the
 *                                  virtual screen, as a 2D game screen does;
 *                           off    the eye images only.
 *   --frames <count>      Quit after this many frames. For automated runs.
 *   --capture <file.ppm>  Before quitting, save the window image of the last
 *                         frame. It only works without a headset.
 */
#include "aeron/main.h"

#include "aeron/aeron.h"
#include "aeron/scene/view_rig.h"
#include "aeron/xr.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_LOGICAL_W 1920
#define DEMO_LOGICAL_H 1080
#define DEMO_PANEL_W 480
#define DEMO_PANEL_H 270
#define DEMO_CUBE_COUNT 8
#define DEMO_NEAR_M 0.05f
#define DEMO_FAR_M 100.0f
#define DEMO_PI 3.14159265358979f

typedef struct DemoVertex {
	float position[3];
	float normal[3];
} DemoVertex;

typedef struct DemoCubeUniforms {
	float clip_rows[4][4];
	float rotation[3][4];
	float color[4];
} DemoCubeUniforms;

typedef enum DemoScreen { DEMO_SCREEN_PANEL, DEMO_SCREEN_FULL, DEMO_SCREEN_OFF } DemoScreen;

typedef struct Demo {
	AeronShader*           vertex_shader;
	AeronShader*           fragment_shader;
	AeronGraphicsPipeline* pipeline;
	/* For the eye mirror, which is drawn in the window's own pass. That
	 * pass has no depth buffer, so the pipeline culls back faces instead. */
	AeronGraphicsPipeline* mirror_pipeline;
	AeronBuffer*           cube_vertices;
	DemoScreen             screen;
	/* The panel shown on the virtual screen. */
	AeronRenderTarget* panel_rt;
	AeronDepthTarget*  panel_depth;
	/* The left eye of this frame, which the mirror draws again. */
	AeronRigView mirror_view;
	/* The window image when there is no headset: both views side by side. */
	AeronRenderTarget* window_rt;
	AeronDepthTarget*  depth;
	int                depth_w, depth_h;
	double             seconds;
} Demo;

/* The six faces of a cube with a side of 1, two triangles each. */
static void demo_build_cube(DemoVertex out[36]) {
	static const float faces[6][3][3] = {
		/* normal, first edge, second edge */
		{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
		{ { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
		{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } }, { { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 } },
	};
	static const float corners[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
	for (int face = 0; face < 6; face++) {
		for (int corner = 0; corner < 6; corner++) {
			DemoVertex* v = &out[face * 6 + corner];
			for (int axis = 0; axis < 3; axis++) {
				v->position[axis] = 0.5f * (faces[face][0][axis] + corners[corner][0] * faces[face][1][axis] +
											corners[corner][1] * faces[face][2][axis]);
				v->normal[axis]   = faces[face][0][axis];
			}
		}
	}
}

static int demo_create(Demo* demo, AeronTextureFormat color_format) {
	demo->vertex_shader   = Aeron_CreateShader(&(AeronShaderDesc) {
		.name                 = "xr_demo_cube.vert",
		.stage                = AERON_SHADER_STAGE_VERTEX,
		.uniform_buffer_count = 1,
	});
	demo->fragment_shader = Aeron_CreateShader(&(AeronShaderDesc) {
		.name  = "xr_demo_cube.frag",
		.stage = AERON_SHADER_STAGE_FRAGMENT,
	});
	if (!demo->vertex_shader || !demo->fragment_shader)
		return 0;

	static const AeronVertexBufferLayoutDesc layout       = { .slot = 0, .stride = sizeof(DemoVertex) };
	static const AeronVertexAttributeDesc    attributes[] = {
		{ 0, 0, AERON_VERTEX_FORMAT_FLOAT3, offsetof(DemoVertex, position) },
		{ 1, 0, AERON_VERTEX_FORMAT_FLOAT3, offsetof(DemoVertex, normal) },
	};
	demo->pipeline = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader       = demo->vertex_shader,
		.fragment_shader     = demo->fragment_shader,
		.primitive_type      = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode           = AERON_CULL_NONE,
		.vertex_buffers      = &layout,
		.vertex_buffer_count = 1,
		.attributes          = attributes,
		.attribute_count     = 2,
		.color_format        = color_format,
		.depth_format        = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.depth               = { .depth_test = 1, .depth_write = 1, .compare = AERON_COMPARE_LESS },
		.sample_count        = AERON_SAMPLE_COUNT_1,
	});
	demo->mirror_pipeline = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader       = demo->vertex_shader,
		.fragment_shader     = demo->fragment_shader,
		.primitive_type      = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode           = AERON_CULL_BACK,
		.vertex_buffers      = &layout,
		.vertex_buffer_count = 1,
		.attributes          = attributes,
		.attribute_count     = 2,
		.color_format        = Aeron_SwapchainFormat(),
		.sample_count        = AERON_SAMPLE_COUNT_1,
	});
	demo->panel_rt        = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width      = DEMO_PANEL_W,
		.height     = DEMO_PANEL_H,
		.format     = color_format,
		.debug_name = "xr_demo panel",
	});
	demo->panel_depth     = Aeron_CreateDepthTarget(&(AeronDepthTargetDesc) {
		.width      = DEMO_PANEL_W,
		.height     = DEMO_PANEL_H,
		.format     = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.debug_name = "xr_demo panel depth",
	});
	if (!demo->pipeline || !demo->mirror_pipeline || !demo->panel_rt || !demo->panel_depth)
		return 0;

	DemoVertex cube[36];
	demo_build_cube(cube);
	demo->cube_vertices = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size       = sizeof cube,
		.usage      = AERON_BUFFER_USAGE_VERTEX,
		.debug_name = "xr_demo cube",
	});
	return demo->cube_vertices && Aeron_UploadBufferData(demo->cube_vertices, 0, cube, sizeof cube);
}

static void demo_destroy(Demo* demo) {
	if (demo->depth)
		Aeron_DestroyDepthTarget(demo->depth);
	if (demo->panel_depth)
		Aeron_DestroyDepthTarget(demo->panel_depth);
	if (demo->panel_rt)
		Aeron_DestroyRenderTarget(demo->panel_rt);
	if (demo->mirror_pipeline)
		Aeron_DestroyGraphicsPipeline(demo->mirror_pipeline);
	if (demo->window_rt)
		Aeron_DestroyRenderTarget(demo->window_rt);
	if (demo->cube_vertices)
		Aeron_DestroyBuffer(demo->cube_vertices);
	if (demo->pipeline)
		Aeron_DestroyGraphicsPipeline(demo->pipeline);
	if (demo->fragment_shader)
		Aeron_DestroyShader(demo->fragment_shader);
	if (demo->vertex_shader)
		Aeron_DestroyShader(demo->vertex_shader);
	memset(demo, 0, sizeof *demo);
}

/* One depth target is used for every view, so it has the size of the
 * largest target. */
static int demo_ensure_depth(Demo* demo, int width, int height) {
	if (demo->depth && demo->depth_w == width && demo->depth_h == height)
		return 1;
	if (demo->depth)
		Aeron_DestroyDepthTarget(demo->depth);
	demo->depth   = Aeron_CreateDepthTarget(&(AeronDepthTargetDesc) {
		.width      = width,
		.height     = height,
		.format     = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.debug_name = "xr_demo depth",
	});
	demo->depth_w = width;
	demo->depth_h = height;
	return demo->depth != NULL;
}

/* Row-major 4x4 product: out = a * b. */
static void demo_mul4(float a[4][4], float b[4][4], float out[4][4]) {
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + a[r][3] * b[3][c];
}

/* Rig space to clip space for one view. Rig and view space use the OpenXR
 * axes (-Z forward). Clip space is SDL GPU's: depth from 0 at the near plane
 * to 1 at the far plane. */
static void demo_view_clip(const AeronRigView* view, float out[4][4]) {
	float r[9];
	AeronViewPose_Rotation(&view->pose, r);
	/* The pose goes from view to rig. Its inverse is the transpose of the
	 * rotation, and the position turned back. */
	float        to_view[4][4] = { { 0 } };
	const float* p             = view->pose.position_m;
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++)
			to_view[row][col] = r[col * 3 + row];
		to_view[row][3] = -(r[0 * 3 + row] * p[0] + r[1 * 3 + row] * p[1] + r[2 * 3 + row] * p[2]);
	}
	to_view[3][3] = 1.0f;

	const float tan_left      = tanf(view->fov.angle_left);
	const float tan_right     = tanf(view->fov.angle_right);
	const float tan_up        = tanf(view->fov.angle_up);
	const float tan_down      = tanf(view->fov.angle_down);
	const float n             = DEMO_NEAR_M;
	const float f             = DEMO_FAR_M;
	float       to_clip[4][4] = {
		{ 2.0f / (tan_right - tan_left), 0.0f, (tan_right + tan_left) / (tan_right - tan_left), 0.0f },
		{ 0.0f, 2.0f / (tan_up - tan_down), (tan_up + tan_down) / (tan_up - tan_down), 0.0f },
		{ 0.0f, 0.0f, f / (n - f), n * f / (n - f) },
		{ 0.0f, 0.0f, -1.0f, 0.0f },
	};
	demo_mul4(to_clip, to_view, out);
}

static void demo_draw_cubes(const Demo* demo, AeronRenderPass* pass, AeronGraphicsPipeline* pipeline,
							float view_clip[4][4], int cube_count) {
	Aeron_BindGraphicsPipeline(pass, pipeline);
	Aeron_BindVertexBuffer(pass, 0, demo->cube_vertices, 0);
	for (int i = 0; i < cube_count; i++) {
		/* A ring around the viewer, 2 m away, at eye height. */
		const float around = 2.0f * DEMO_PI * (float)i / DEMO_CUBE_COUNT;
		const float spin   = (float)demo->seconds * (0.4f + 0.1f * (float)i);
		const float c = cosf(spin), s = sinf(spin);
		const float size        = 0.4f;
		float       model[4][4] = {
			{ c * size, 0.0f, s * size, 2.0f * sinf(around) },
			{ 0.0f, size, 0.0f, 0.3f * sinf((float)demo->seconds + around) },
			{ -s * size, 0.0f, c * size, -2.0f * cosf(around) },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
		};
		DemoCubeUniforms u = {
			.rotation = { { c, 0.0f, s, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { -s, 0.0f, c, 0.0f } },
			.color    = { 0.25f + 0.75f * (float)(i & 1), 0.25f + 0.75f * (float)((i >> 1) & 1),
						  0.25f + 0.75f * (float)((i >> 2) & 1), 1.0f },
		};
		demo_mul4(view_clip, model, u.clip_rows);
		Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &u, sizeof u);
		Aeron_Draw(pass, 36, 0);
	}
}

/* Draws every view of the rig. `targets[i]` is the target of view i; the
 * views can share one target and use different rectangles of it. */
static int demo_render_views(Demo* demo, const AeronViewRig* rig, AeronRenderTarget* const* targets) {
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd)
		return 0;
	for (int i = 0; i < rig->count; i++) {
		const AeronRigView* view = &rig->views[i];
		/* A target is cleared by the first view that draws into it. */
		int first_use = 1;
		for (int j = 0; j < i; j++)
			first_use = first_use && targets[j] != targets[i];
		AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target      = targets[i],
			.depth_target      = demo->depth,
			.viewport          = view->rect,
			.scissor           = view->rect,
			.clear_color       = first_use,
			.clear_color_rgba  = { 0.01f, 0.012f, 0.02f, 1.0f },
			.clear_depth       = first_use,
			.clear_depth_value = 1.0f,
			.command_buffer    = cmd,
			.debug_label       = "xr_demo view",
		});
		if (!pass) {
			Aeron_CancelCommandBuffer(cmd);
			return 0;
		}
		float view_clip[4][4];
		demo_view_clip(view, view_clip);
		demo_draw_cubes(demo, pass, demo->pipeline, view_clip, DEMO_CUBE_COUNT);
		Aeron_EndRenderPass(pass);
	}
	return Aeron_SubmitCommandBuffer(cmd);
}

/* Draws the panel, with the first cube seen from the front, and submits it
 * as a layer in the lower part of the logical frame. The panel's background
 * is half transparent, so the eye images show through it in the headset. */
static int demo_submit_panel(Demo* demo) {
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd)
		return 0;
	AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target      = demo->panel_rt,
		.depth_target      = demo->panel_depth,
		.clear_color       = 1,
		.clear_color_rgba  = { 0.02f, 0.05f, 0.12f, 0.6f },
		.clear_depth       = 1,
		.clear_depth_value = 1.0f,
		.command_buffer    = cmd,
		.debug_label       = "xr_demo panel",
	});
	if (!pass) {
		Aeron_CancelCommandBuffer(cmd);
		return 0;
	}
	const float        half_v = 0.2f;
	const float        half_h = atanf(tanf(half_v) * (float)DEMO_PANEL_W / (float)DEMO_PANEL_H);
	const AeronRigView view   = {
		.pose = { .orientation = { 0.0f, 0.0f, 0.0f, 1.0f } },
		.fov  = { -half_h, half_h, half_v, -half_v },
	};
	float view_clip[4][4];
	demo_view_clip(&view, view_clip);
	demo_draw_cubes(demo, pass, demo->pipeline, view_clip, 1);
	Aeron_EndRenderPass(pass);
	if (!Aeron_SubmitCommandBuffer(cmd))
		return 0;
	return Aeron_SubmitTextureLayer(&(AeronTextureLayerDesc) {
			   .texture      = Aeron_RenderTargetGetTexture(demo->panel_rt),
			   .logical_rect = { (DEMO_LOGICAL_W - 2 * DEMO_PANEL_W) / 2, DEMO_LOGICAL_H - 2 * DEMO_PANEL_H - 60,
								 2 * DEMO_PANEL_W, 2 * DEMO_PANEL_H },
			   .blend_mode   = AERON_LAYER_BLEND_ALPHA,
			   .color_space  = AERON_COLOR_SPACE_SRGB,
		   }) != 0;
}

/* The eye mirror: draws the left eye's view in the window. Aeron calls it
 * while it draws the window, inside the window's render pass. The view keeps
 * its shape: it is fitted into the middle of the window. */
static void demo_draw_mirror(AeronCommandBuffer* command_buffer, AeronRenderPass* pass, AeronRenderTarget* target,
							 int target_width, int target_height, void* userdata) {
	const Demo*         demo = userdata;
	const AeronRigView* view = &demo->mirror_view;
	(void)command_buffer;
	(void)target;
	if (view->rect.width <= 0 || view->rect.height <= 0)
		return;
	AeronRectI rect = { 0, 0, target_width, target_height };
	if ((int64_t)target_width * view->rect.height > (int64_t)target_height * view->rect.width)
		rect.width = (int)((int64_t)target_height * view->rect.width / view->rect.height);
	else
		rect.height = (int)((int64_t)target_width * view->rect.height / view->rect.width);
	rect.x = (target_width - rect.width) / 2;
	rect.y = (target_height - rect.height) / 2;
	Aeron_SetViewport(pass, &rect);
	Aeron_SetScissor(pass, &rect);
	float view_clip[4][4];
	demo_view_clip(view, view_clip);
	demo_draw_cubes(demo, pass, demo->mirror_pipeline, view_clip, DEMO_CUBE_COUNT);
}

/* The headset path: the rig and the eye images come from Aeron. Returns 0
 * when this frame has no eye images. */
static int demo_frame_headset(Demo* demo) {
	AeronXrFrame frame;
	if (demo->screen == DEMO_SCREEN_FULL || !Aeron_XrBeginViews(&frame))
		return 0;
	if (!demo_ensure_depth(demo, frame.rig.width, frame.rig.height) ||
		!demo_render_views(demo, &frame.rig, frame.color))
		return 0;
	Aeron_XrSubmitViews();
	demo->mirror_view = frame.rig.views[0];
	Aeron_XrSubmitMirror(&(AeronSwapchainRenderLayerDesc) {
		.callback    = demo_draw_mirror,
		.userdata    = demo,
		.debug_label = "xr_demo mirror",
	});
	if (demo->screen == DEMO_SCREEN_PANEL && !demo_submit_panel(demo))
		return 0;
	return 1;
}

/* The window path: a fixed stereo rig, with the two views side by side. */
static int demo_frame_window(Demo* demo) {
	const int           view_w = DEMO_LOGICAL_W / 2;
	const int           view_h = DEMO_LOGICAL_H;
	const float         half_h = 50.0f * DEMO_PI / 180.0f;
	const float         half_v = atanf(tanf(half_h) * (float)view_h / (float)view_w);
	const AeronViewFov  fov    = { -half_h, half_h, half_v, -half_v };
	const AeronViewPose head   = { .orientation = { 0.0f, 0.0f, 0.0f, 1.0f } };
	AeronViewRig        rig;
	if (!AeronViewRig_DebugStereo(0.064f, &fov, &head, view_w, view_h, &rig))
		return 0;
	if (!demo_ensure_depth(demo, DEMO_LOGICAL_W, DEMO_LOGICAL_H))
		return 0;
	AeronRenderTarget* targets[AERON_MAX_VIEWS];
	for (int i = 0; i < rig.count; i++)
		targets[i] = demo->window_rt;
	if (!demo_render_views(demo, &rig, targets))
		return 0;
	return Aeron_SubmitTextureLayer(&(AeronTextureLayerDesc) {
			   .texture      = Aeron_RenderTargetGetTexture(demo->window_rt),
			   .logical_rect = { 0, 0, DEMO_LOGICAL_W, DEMO_LOGICAL_H },
			   .blend_mode   = AERON_LAYER_BLEND_OPAQUE,
			   .color_space  = AERON_COLOR_SPACE_SRGB,
		   }) != 0;
}

/* Saves the window image as a binary PPM file. */
static int demo_capture(Demo* demo, const char* path) {
	const size_t pitch  = (size_t)DEMO_LOGICAL_W * 4;
	const size_t size   = pitch * DEMO_LOGICAL_H;
	uint8_t*     pixels = malloc(size);
	FILE*        file   = NULL;
	int          ok     = 0;
	if (pixels && Aeron_ReadRenderTargetRawPixels(demo->window_rt, pixels, size, pitch))
		file = fopen(path, "wb");
	if (file) {
		fprintf(file, "P6\n%d %d\n255\n", DEMO_LOGICAL_W, DEMO_LOGICAL_H);
		for (size_t i = 0; i < size; i += 4)
			fwrite(pixels + i, 1, 3, file);
		ok = fclose(file) == 0;
	}
	free(pixels);
	return ok;
}

static void demo_startup_options(AeronVfs* vfs, void* userdata, AeronStartupOptions* options) {
	(void)vfs;
	options->xr_mode = *(const AeronXrMode*)userdata;
}

int main(int argc, char* argv[]) {
	long        frame_limit  = 0;
	const char* capture_path = NULL;
	AeronXrMode xr_mode      = AERON_XR_ON;
	int         require_vr   = 0;
	DemoScreen  screen       = DEMO_SCREEN_PANEL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--vr") == 0) {
			require_vr = 1;
		} else if (strcmp(argv[i], "--no-vr") == 0) {
			xr_mode = AERON_XR_OFF;
		} else if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc && strcmp(argv[i + 1], "panel") == 0) {
			screen = DEMO_SCREEN_PANEL;
			i++;
		} else if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc && strcmp(argv[i + 1], "full") == 0) {
			screen = DEMO_SCREEN_FULL;
			i++;
		} else if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc && strcmp(argv[i + 1], "off") == 0) {
			screen = DEMO_SCREEN_OFF;
			i++;
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frame_limit = strtol(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
			capture_path = argv[++i];
		} else {
			fprintf(stderr, "Usage: xr_demo [--vr | --no-vr] [--screen <panel|full|off>] [--frames <count>] "
							"[--capture <file.ppm>]\n");
			return 2;
		}
	}

	AeronConfig config;
	memset(&config, 0, sizeof config);
	config.org_name          = "Aeron";
	config.app_name          = "AeronXrDemo";
	config.shader_path       = XR_DEMO_SHADER_RELATIVE_DIR;
	config.window_title      = "Aeron XR Demo";
	config.window_width      = 1280;
	config.window_height     = 720;
	config.logical_width     = DEMO_LOGICAL_W;
	config.logical_height    = DEMO_LOGICAL_H;
	config.presentation_mode = AERON_PRESENTATION_ASPECT_FIT;
	config.startup_options   = demo_startup_options;
	config.startup_userdata  = &xr_mode;
	if (!Aeron_Init(&config)) {
		fprintf(stderr, "xr_demo: Aeron_Init failed\n");
		return 1;
	}
	if (require_vr && !Aeron_XrEnabled()) {
		const char* reason = Aeron_XrStartError();
		fprintf(stderr, "xr_demo: VR could not start: %s\n", reason ? reason : "unknown reason");
		Aeron_Shutdown();
		return 1;
	}

	int eye_w = 0, eye_h = 0;
	if (Aeron_XrViewSize(&eye_w, &eye_h))
		Aeron_LogInfo("xr_demo", "VR is on, with eye images of %dx%d", eye_w, eye_h);
	else
		Aeron_LogInfo("xr_demo", "VR is off, drawing the views in the window");

	Demo demo;
	memset(&demo, 0, sizeof demo);
	demo.screen = screen;
	/* An sRGB format, as the eye images have. The shader writes linear values. */
	const AeronTextureFormat color_format = AERON_TEXTURE_FORMAT_RGBA8_SRGB;
	demo.window_rt                        = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width      = DEMO_LOGICAL_W,
		.height     = DEMO_LOGICAL_H,
		.format     = color_format,
		.debug_name = "xr_demo window",
	});
	if (!demo.window_rt || !demo_create(&demo, color_format)) {
		fprintf(stderr, "xr_demo: could not create the GPU resources: %s\n", Aeron_RenderLastError());
		demo_destroy(&demo);
		Aeron_Shutdown();
		return 1;
	}

	long     frames        = 0;
	int      window_frames = 0;
	long     rate_frames   = 0;
	uint64_t rate_start_us = Aeron_NowUs();
	int      was_running = -1, was_focused = -1;
	while (!Aeron_QuitRequested() && !Aeron_FatalErrorRequested()) {
		const int32_t delta_us = Aeron_BeginFrame();
		if (Aeron_FatalErrorRequested())
			break;
		demo.seconds += (double)delta_us * 1e-6;
		const AeronInputSnapshot* input = Aeron_InputSnapshot();
		/* The key codes are USB usage codes: 4 is A, so R is 4 + 17. */
		if (input && input->key_pressed[AERON_KEY_A + ('R' - 'A')])
			Aeron_XrRecenter();

		if (!demo_frame_headset(&demo)) {
			if (demo_frame_window(&demo))
				window_frames++;
			else
				Aeron_RequestFatalRendererError("xr_demo frame");
		}
		if (!Aeron_Present())
			Aeron_RequestFatalRendererError("frame presentation");
		Aeron_WaitForNextFrame(0);

		const int running = Aeron_XrSessionRunning(), focused = Aeron_XrFocused();
		if (running != was_running || focused != was_focused) {
			Aeron_LogInfo("xr_demo", "VR session: %s, %s", running ? "running" : "not running",
						  focused ? "focused" : "not focused");
			was_running = running;
			was_focused = focused;
		}
		rate_frames++;
		const uint64_t now_us = Aeron_NowUs();
		if (now_us - rate_start_us >= 5000000u) {
			Aeron_LogInfo("xr_demo", "%.1f frames per second, paced by the %s",
						  (double)rate_frames * 1e6 / (double)(now_us - rate_start_us),
						  running ? "headset" : "window");
			rate_frames   = 0;
			rate_start_us = now_us;
		}
		if (frame_limit > 0 && ++frames >= frame_limit)
			Aeron_RequestQuit();
	}

	int exit_status = Aeron_FatalErrorRequested() ? 1 : 0;
	Aeron_WaitForGpuIdle();
	if (capture_path && !exit_status && (!window_frames || !demo_capture(&demo, capture_path))) {
		fprintf(stderr, "xr_demo: could not save %s\n", capture_path);
		exit_status = 1;
	}
	demo_destroy(&demo);
	Aeron_Shutdown();
	return exit_status;
}
