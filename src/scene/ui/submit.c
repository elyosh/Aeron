/*
 * AeronUi presentation — chooses between the direct swapchain layer
 * (native resolution, no intermediate target) and the content-rect
 * render-target fallback used when aspect-fit letterboxing keeps the
 * content rect from covering the drawable.
 */

#include "internal.h"

/* Borrowed-pass replay for the direct path. */
static void ui_swapchain_render(AeronCommandBuffer* command_buffer, AeronRenderPass* render_pass,
								AeronRenderTarget* target, int target_width, int target_height,
								void* userdata) {
	AeronUiContext* ctx = (AeronUiContext*)userdata;
	(void)target_width;
	(void)target_height;
	AeronDrawList_RenderIntoPass(ctx->list, command_buffer, render_pass, target);
}

/* Lazily (re)creates the fallback target at the content-rect size.
 * Returns NULL on failure. */
AeronRenderTarget* ui_submit_ensure_fallback_rt(AeronUiContext* ctx) {
	if (ctx->fallback_rt && ctx->fallback_w == ctx->out_w && ctx->fallback_h == ctx->out_h) {
		return ctx->fallback_rt;
	}
	if (ctx->fallback_rt) {
		Aeron_DestroyRenderTarget(ctx->fallback_rt);
		ctx->fallback_rt = NULL;
	}
	ctx->fallback_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width      = ctx->out_w,
		.height     = ctx->out_h,
		.format     = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
		.debug_name = "aeron.ui.fallback",
	});
	if (!ctx->fallback_rt) {
		Aeron_LogError("aeron.scene", "ui fallback target creation failed (%dx%d)", ctx->out_w, ctx->out_h);
		return NULL;
	}
	ctx->fallback_w = ctx->out_w;
	ctx->fallback_h = ctx->out_h;
	return ctx->fallback_rt;
}

/* Releases the fallback target while the direct path is active (the
 * dominant fullscreen case keeps no resident UI memory). */
void ui_submit_release_fallback_rt(AeronUiContext* ctx) {
	if (ctx->fallback_rt) {
		Aeron_DestroyRenderTarget(ctx->fallback_rt);
		ctx->fallback_rt = NULL;
		ctx->fallback_w  = 0;
		ctx->fallback_h  = 0;
	}
}

int AeronUi_Submit(AeronUiContext* ctx) {
	if (!ctx || ctx->frame_active) {
		if (ctx && ctx->frame_active) {
			Aeron_LogWarn("aeron.scene", "AeronUi_Submit before EndFrame");
		}
		return 0;
	}
	if (!ctx->any_window_prev) {
		return 0; /* nothing declared this frame */
	}

	if (ctx->direct_path) {
		ui_submit_release_fallback_rt(ctx);
		AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
		if (!cmd) {
			return 0;
		}
		if (!AeronDrawList_Prepare(ctx->list, cmd)) {
			Aeron_CancelCommandBuffer(cmd);
			return 0;
		}
		if (!Aeron_SubmitCommandBuffer(cmd)) {
			return 0;
		}
		return Aeron_SubmitSwapchainRenderLayer(&(AeronSwapchainRenderLayerDesc) {
			.callback        = ui_swapchain_render,
			.userdata        = ctx,
			.required_width  = ctx->out_w,
			.required_height = ctx->out_h,
			.debug_label     = "aeron.ui",
		});
	}

	/* Fallback: the target was latched by BeginFrame's draw-list Begin. */
	AeronRenderTarget* rt = ctx->fallback_rt;
	if (!rt) {
		return 0;
	}
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd) {
		return 0;
	}
	AeronDrawList_Render(ctx->list, cmd);
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		return 0;
	}

	int logical_w = 0;
	int logical_h = 0;
	if (!Aeron_GetLogicalSize(&logical_w, &logical_h)) {
		logical_w = ctx->out_w;
		logical_h = ctx->out_h;
	}
	return Aeron_SubmitTextureLayer(&(AeronTextureLayerDesc) {
			   .texture      = Aeron_RenderTargetGetTexture(rt),
			   .logical_rect = { 0, 0, logical_w, logical_h },
			   .blend_mode   = AERON_LAYER_BLEND_PREMULTIPLIED,
			   .color_space  = AERON_COLOR_SPACE_LINEAR_DISPLAY,
		   }) != 0;
}
