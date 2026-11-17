/*
 * planes_demo — a 2D draw list drawn on planes in 3D.
 *
 * It tests AeronDrawList_RenderPlanes (aeron/scene/draw_list2d.h). The
 * window shows one small drawing five times: a frame of fills, a filled
 * bar, a sprite and a line of text.
 *
 *   Top row, left:   the drawing, drawn flat (AeronDrawList_RenderIntoPass).
 *   Top row, right:  the same list drawn with RenderPlanes, on an identity
 *                    plane, through a matrix that takes pixels to the
 *                    target. The two must look the same.
 *   Bottom:          the drawing on three planes in front of a camera:
 *                    turned 40 degrees, straight ahead, and rolled 20
 *                    degrees. Lines must stay straight, the edges of the
 *                    fills smooth, and the text readable on all three.
 *
 * Usage: planes_demo <font_atlas_basename>
 *   The basename names a font atlas without extension: a .fnt file with a
 *   .png or a .ktx2 file.
 */

#include "aeron/main.h"
#include "aeron/aeron.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/text2d.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DEMO_LOGICAL_W 1920
#define DEMO_LOGICAL_H 1080
/* The drawing's size in its own pixels. */
#define DRAWING_W 480
#define DRAWING_H 270
/* The target of the three planes. */
#define SCENE_W 1920
#define SCENE_H 720

static const float k_pi = 3.14159265358979f;

/* Adds the drawing to `list`, in the drawing's pixels. */
static void demo_drawing(AeronDrawList2D* list, const AeronFontAtlas* font) {
	const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	const float green[4] = { 0.1f, 0.8f, 0.2f, 1.0f };
	const float amber[4] = { 0.9f, 0.6f, 0.1f, 1.0f };
	/* A frame of thin lines, as a target box has, and corner arms. */
	AeronDrawList_AddFrame(list, 20.0f, 20.0f, DRAWING_W - 40.0f, DRAWING_H - 40.0f, 3.0f, white,
						   AERON_BLIT2D_BLEND_PMA, NULL);
	AeronDrawList_AddFill(list, 60.0f, 60.0f, 80.0f, 3.0f, green, AERON_BLIT2D_BLEND_PMA, NULL);
	AeronDrawList_AddFill(list, 60.0f, 60.0f, 3.0f, 50.0f, green, AERON_BLIT2D_BLEND_PMA, NULL);
	/* A filled bar. */
	AeronDrawList_AddFill(list, 60.0f, 200.0f, 220.0f, 24.0f, amber, AERON_BLIT2D_BLEND_PMA, NULL);
	/* A sprite: a part of the font's texture. */
	AeronDrawList_AddSprite(list, &(AeronDrawList2DSprite) {
									  .texture = font->texture,
									  .src_u1  = 0.25f,
									  .src_v1  = 0.25f,
									  .dst_x   = 330.0f,
									  .dst_y   = 60.0f,
									  .dst_w   = 100.0f,
									  .dst_h   = 100.0f,
									  .tint    = { 1.0f, 1.0f, 1.0f, 1.0f },
									  .blend   = AERON_BLIT2D_BLEND_PMA,
									  .filter  = AERON_BLIT2D_FILTER_LINEAR,
								  });
	const AeronTextStyle style = {
		.font   = font,
		.scale  = 22.0f / (float)font->cell_h,
		.tint   = { 1.0f, 1.0f, 1.0f, 1.0f },
		.filter = AERON_BLIT2D_FILTER_LINEAR,
	};
	AeronText_Draw(list, &style, 60.0f, 130.0f, AERON_TEXT_LEFT, "Target 12.5 km", -1, NULL);
}

/* The plane of a drawing whose centre is at `center`, `width` units wide,
 * with the axes `right` and `down` (unit vectors). */
static void demo_plane(const float center[3], const float right[3], const float down[3], float width,
					   float out[9]) {
	const float s = width / (float)DRAWING_W;
	for (int axis = 0; axis < 3; axis++) {
		out[axis * 3 + 0] = right[axis] * s;
		out[axis * 3 + 1] = down[axis] * s;
		out[axis * 3 + 2] = center[axis] - 0.5f * (float)DRAWING_W * s * right[axis] -
							0.5f * (float)DRAWING_H * s * down[axis];
	}
}

static AeronRenderTarget* demo_target(int w, int h, const char* name) {
	return Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = w, .height = h, .format = AERON_TEXTURE_FORMAT_RGBA8_SRGB, .debug_name = name });
}

static AeronRenderPass* demo_pass(AeronCommandBuffer* cmd, AeronRenderTarget* target, const char* label) {
	return Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target     = target,
		.clear_color      = 1,
		.clear_color_rgba = { 0.02f, 0.02f, 0.05f, 1.0f },
		.command_buffer   = cmd,
		.debug_label      = label,
	});
}

int main(int argc, char* argv[]) {
	if (argc != 2) {
		fprintf(stderr, "Usage: planes_demo <font_atlas_basename>\n"
						"  Basename of a font atlas (no extension): .fnt with .png or .ktx2.\n");
		return 2;
	}

	AeronConfig config;
	memset(&config, 0, sizeof config);
	config.org_name          = "Aeron";
	config.app_name          = "AeronPlanesDemo";
	config.shader_path       = PLANES_DEMO_SHADER_RELATIVE_DIR;
	config.window_title      = "Aeron Planes Demo";
	config.logical_width     = DEMO_LOGICAL_W;
	config.logical_height    = DEMO_LOGICAL_H;
	config.presentation_mode = AERON_PRESENTATION_ASPECT_FIT;
	if (!Aeron_Init(&config)) {
		fprintf(stderr, "planes_demo: Aeron_Init failed\n");
		return 1;
	}

	AeronFontAtlas      atlas;
	AeronCommandBuffer* load = Aeron_AcquireCommandBuffer();
	if (!load ||
		(!AeronFontAtlas_Load(&atlas, load, argv[1]) && !AeronFontAtlas_LoadKtx2(&atlas, load, argv[1])) ||
		!Aeron_SubmitCommandBuffer(load)) {
		fprintf(stderr, "planes_demo: could not load font atlas '%s'\n", argv[1]);
		Aeron_Shutdown();
		return 1;
	}

	AeronRenderTarget* flat     = demo_target(DRAWING_W, DRAWING_H, "planes_demo.flat");
	AeronRenderTarget* identity = demo_target(DRAWING_W, DRAWING_H, "planes_demo.identity");
	AeronRenderTarget* scene    = demo_target(SCENE_W, SCENE_H, "planes_demo.scene");
	AeronDrawList2D*   list     = AeronDrawList_Create(256);
	int                ok       = flat && identity && scene && list;

	while (ok && !Aeron_QuitRequested() && !Aeron_FatalErrorRequested()) {
		Aeron_BeginFrame();
		if (Aeron_FatalErrorRequested())
			break;
		AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
		if (!cmd)
			break;

		/* The flat drawing, and the same list on an identity plane. */
		static const float k_identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		AeronDrawList_Begin(list, NULL, DRAWING_W, DRAWING_H, AERON_DRAWLIST2D_LOAD, NULL);
		AeronDrawList_UsePlane(list, AeronDrawList_AddPlane(list, k_identity, 0.0f));
		demo_drawing(list, &atlas);
		ok                    = AeronDrawList_Prepare(list, cmd);
		AeronRenderPass* pass = ok ? demo_pass(cmd, flat, "Flat drawing") : NULL;
		if (pass) {
			AeronDrawList_RenderIntoPass(list, cmd, pass, flat);
			Aeron_EndRenderPass(pass);
		}
		pass = ok ? demo_pass(cmd, identity, "Drawing on an identity plane") : NULL;
		if (pass) {
			/* v = (x, y, 1), w = 0: this matrix takes a pixel to NDC. */
			const float pixels[12] = { 2.0f / DRAWING_W,
									   0.0f,
									   -1.0f,
									   0.0f,
									   0.0f,
									   -2.0f / DRAWING_H,
									   1.0f,
									   0.0f,
									   0.0f,
									   0.0f,
									   1.0f,
									   0.0f };
			AeronDrawList_RenderPlanes(list, cmd, pass, identity, pixels, 1.0f);
			Aeron_EndRenderPass(pass);
		}

		/* The drawing on three planes. The camera is at the origin and
		 * looks along +Z, with X right and Y down. */
		AeronDrawList_Begin(list, NULL, DRAWING_W, DRAWING_H, AERON_DRAWLIST2D_LOAD, NULL);
		float       plane[9];
		const float turn = 40.0f * k_pi / 180.0f, roll = 20.0f * k_pi / 180.0f;
		const float left_center[3]  = { -2.6f, 0.0f, 4.0f };
		const float left_right[3]   = { cosf(turn), 0.0f, -sinf(turn) };
		const float mid_center[3]   = { 0.0f, 0.0f, 4.0f };
		const float right_center[3] = { 2.6f, 0.0f, 4.0f };
		const float rolled_right[3] = { cosf(roll), sinf(roll), 0.0f };
		const float rolled_down[3]  = { -sinf(roll), cosf(roll), 0.0f };
		const float x_axis[3] = { 1.0f, 0.0f, 0.0f }, y_axis[3] = { 0.0f, 1.0f, 0.0f };
		demo_plane(left_center, left_right, y_axis, 2.2f, plane);
		AeronDrawList_UsePlane(list, AeronDrawList_AddPlane(list, plane, 1.0f));
		demo_drawing(list, &atlas);
		demo_plane(mid_center, x_axis, y_axis, 2.2f, plane);
		AeronDrawList_UsePlane(list, AeronDrawList_AddPlane(list, plane, 1.0f));
		demo_drawing(list, &atlas);
		demo_plane(right_center, rolled_right, rolled_down, 2.2f, plane);
		AeronDrawList_UsePlane(list, AeronDrawList_AddPlane(list, plane, 1.0f));
		demo_drawing(list, &atlas);
		ok   = ok && AeronDrawList_Prepare(list, cmd);
		pass = ok ? demo_pass(cmd, scene, "Drawing on three planes") : NULL;
		if (pass) {
			/* A camera with a horizontal field of view of 90 degrees. */
			const float aspect   = (float)SCENE_W / (float)SCENE_H;
			const float view[12] = {
				1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -aspect, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f
			};
			AeronDrawList_RenderPlanes(list, cmd, pass, scene, view, 1.0f);
			Aeron_EndRenderPass(pass);
		}
		if (!Aeron_SubmitCommandBuffer(cmd))
			break;

		const int gap = (DEMO_LOGICAL_W - 2 * DRAWING_W) / 3;
		Aeron_SubmitTextureLayer(&(AeronTextureLayerDesc) {
			.texture      = Aeron_RenderTargetGetTexture(flat),
			.logical_rect = { gap, 40, DRAWING_W, DRAWING_H },
		});
		Aeron_SubmitTextureLayer(&(AeronTextureLayerDesc) {
			.texture      = Aeron_RenderTargetGetTexture(identity),
			.logical_rect = { 2 * gap + DRAWING_W, 40, DRAWING_W, DRAWING_H },
		});
		Aeron_SubmitTextureLayer(&(AeronTextureLayerDesc) {
			.texture      = Aeron_RenderTargetGetTexture(scene),
			.logical_rect = { 0, DEMO_LOGICAL_H - SCENE_H, SCENE_W, SCENE_H },
		});
		if (!Aeron_Present())
			Aeron_RequestFatalRendererError("frame presentation");
	}

	const int exit_status = ok && !Aeron_FatalErrorRequested() ? 0 : 1;
	AeronDrawList_Destroy(list);
	if (flat)
		Aeron_DestroyRenderTarget(flat);
	if (identity)
		Aeron_DestroyRenderTarget(identity);
	if (scene)
		Aeron_DestroyRenderTarget(scene);
	AeronFontAtlas_Release(&atlas);
	Aeron_Shutdown();
	return exit_status;
}
