/* The OpenXR swapchains: one for each eye and one for the virtual screen.
 * SDL creates their images as GPU textures. Aeron wraps each image as an
 * external render target, so the application draws into it like into any
 * other target. This file also has the application's view functions. */

#include "../internal.h"
#include "xr_internal.h"

#include <math.h>
#include <string.h>

static int xr_create_swapchain(SDL_GPUDevice* device, int width, int height, const char* name,
							   AeronXrSwapchain* out, char* error, size_t error_size) {
	const XrSwapchainCreateInfo info = {
		.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
		.sampleCount = 1,
		.width       = (uint32_t)width,
		.height      = (uint32_t)height,
		.faceCount   = 1,
		.arraySize   = 1,
		.mipCount    = 1,
	};
	char     what[96];
	XrResult result = SDL_CreateGPUXRSwapchain(device, g_xr.machine.session, &info, g_xr.color_format,
											   &out->tracker.swapchain, &out->textures);
	if (XR_FAILED(result) || !out->textures) {
		out->tracker.swapchain = XR_NULL_HANDLE;
		out->textures          = NULL;
		SDL_snprintf(what, sizeof what, "could not create the %s swapchain (%dx%d)", name, width, height);
		AeronXr_ResultError(error, error_size, what, result);
		return 0;
	}
	result = g_xr.fn.xrEnumerateSwapchainImages(out->tracker.swapchain, 0, &out->image_count, NULL);
	if (XR_FAILED(result) || out->image_count == 0) {
		SDL_snprintf(what, sizeof what, "could not count the images of the %s swapchain", name);
		AeronXr_ResultError(error, error_size, what, result);
		return 0;
	}
	out->targets = (AeronRenderTarget*)SDL_calloc(out->image_count, sizeof *out->targets);
	if (!out->targets) {
		SDL_snprintf(error, error_size, "out of memory");
		return 0;
	}
	out->width  = width;
	out->height = height;
	for (uint32_t i = 0; i < out->image_count; i++) {
		AeronTexture* texture = &out->targets[i].color;
		texture->texture      = out->textures[i];
		texture->width        = width;
		texture->height       = height;
		texture->mip_count    = 1;
		texture->layer_count  = 1;
		texture->format       = g_xr.color_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
									? AERON_TEXTURE_FORMAT_RGBA8_SRGB
									: AERON_TEXTURE_FORMAT_BGRA8_SRGB;
		texture->usage        = AERON_TEXTURE_USAGE_COLOR_TARGET;
		texture->sample_count = AERON_SAMPLE_COUNT_1;
		texture->external     = 1;
		SDL_snprintf(texture->debug_name, sizeof texture->debug_name, "xr %s %u", name, (unsigned)i);
	}
	return 1;
}

static void xr_destroy_swapchain(SDL_GPUDevice* device, AeronXrSwapchain* swapchain) {
	/* SDL waits for the GPU, then destroys the textures and the swapchain. */
	if (swapchain->tracker.swapchain && swapchain->textures)
		SDL_DestroyGPUXRSwapchain(device, swapchain->tracker.swapchain, swapchain->textures);
	SDL_free(swapchain->targets);
	SDL_zerop(swapchain);
}

int AeronXr_CreateSwapchains(SDL_GPUDevice* device, char* error, size_t error_size) {
	const AeronStartupOptions* options = &g_aeron.xr_options;

	/* Both eyes get the same size. */
	const float scale = options->xr_render_scale > 0.0f ? options->xr_render_scale : 1.0f;
	g_xr.view_width   = SDL_clamp((int)lroundf((float)g_xr.view_width * scale), 1, g_xr.view_max_width);
	g_xr.view_height  = SDL_clamp((int)lroundf((float)g_xr.view_height * scale), 1, g_xr.view_max_height);

	const int screen_width  = options->xr_screen_width > 0 ? options->xr_screen_width
														   : AERON_XR_SCREEN_DEFAULT_WIDTH;
	const int screen_height = options->xr_screen_height > 0 ? options->xr_screen_height
															: AERON_XR_SCREEN_DEFAULT_HEIGHT;
	return xr_create_swapchain(device, g_xr.view_width, g_xr.view_height, "left eye", &g_xr.eyes[0], error,
							   error_size) &&
		   xr_create_swapchain(device, g_xr.view_width, g_xr.view_height, "right eye", &g_xr.eyes[1], error,
							   error_size) &&
		   xr_create_swapchain(device, screen_width, screen_height, "screen", &g_xr.screen, error,
							   error_size);
}

void AeronXr_DestroySwapchains(SDL_GPUDevice* device) {
	xr_destroy_swapchain(device, &g_xr.panel);
	g_xr.panel_begun = 0;
	xr_destroy_swapchain(device, &g_xr.eyes[0]);
	xr_destroy_swapchain(device, &g_xr.eyes[1]);
	xr_destroy_swapchain(device, &g_xr.screen);
}

void AeronXr_ReleaseImages(void) {
	/* A swapchain that doesn't exist holds no image. */
	AeronXrSwapchain* swapchains[] = { &g_xr.eyes[0], &g_xr.eyes[1], &g_xr.screen, &g_xr.panel };
	char              error[256];
	for (size_t i = 0; i < SDL_arraysize(swapchains); i++) {
		if (!g_xr.failed && !AeronXrImage_Release(&swapchains[i]->tracker, &g_xr.fn, error, sizeof error))
			AeronXr_Fail(error);
	}
}

/* Whether the application may draw for the headset in this frame. */
static int xr_frame_renders(void) {
	return g_xr.machine.running && !g_xr.failed && g_xr.frame_open && g_xr.frame_should_render;
}

static void xr_pose_to_view(const XrPosef* pose, AeronViewPose* out) {
	out->position_m[0]  = pose->position.x;
	out->position_m[1]  = pose->position.y;
	out->position_m[2]  = pose->position.z;
	out->orientation[0] = pose->orientation.x;
	out->orientation[1] = pose->orientation.y;
	out->orientation[2] = pose->orientation.z;
	out->orientation[3] = pose->orientation.w;
}

int Aeron_XrBeginViews(AeronXrFrame* out) {
	char error[256];

	if (!out)
		return 0;
	memset(out, 0, sizeof *out);
	if (!xr_frame_renders() || g_xr.views_begun)
		return 0;

	const XrViewLocateInfo locate_info = {
		.type                  = XR_TYPE_VIEW_LOCATE_INFO,
		.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		.displayTime           = g_xr.frame_display_time,
		.space                 = g_xr.rig_space,
	};
	XrViewState view_state = { .type = XR_TYPE_VIEW_STATE };
	XrView      views[2]   = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
	uint32_t    view_count = 0;
	const XrResult result =
		g_xr.fn.xrLocateViews(g_xr.machine.session, &locate_info, &view_state, 2, &view_count, views);
	if (XR_FAILED(result)) {
		AeronXr_ResultError(error, sizeof error, "could not locate the eye views", result);
		AeronXr_Fail(error);
		return 0;
	}
	/* The runtime doesn't know where the head is, for example before the
	 * tracking starts. Nothing can be rendered for the eyes then. */
	const XrViewStateFlags needed = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
	if (view_count != 2 || (view_state.viewStateFlags & needed) != needed)
		return 0;

	for (int eye = 0; eye < 2; eye++) {
		if (!AeronXrImage_Acquire(&g_xr.eyes[eye].tracker, &g_xr.fn, error, sizeof error)) {
			AeronXr_Fail(error);
			return 0;
		}
	}
	g_xr.views_begun      = 1;
	g_xr.located_views[0] = views[0];
	g_xr.located_views[1] = views[1];

	AeronViewRig* rig = &out->rig;
	rig->count        = 2;
	for (int eye = 0; eye < 2; eye++) {
		AeronRigView* view = &rig->views[eye];
		xr_pose_to_view(&views[eye].pose, &view->pose);
		view->fov.angle_left  = views[eye].fov.angleLeft;
		view->fov.angle_right = views[eye].fov.angleRight;
		view->fov.angle_up    = views[eye].fov.angleUp;
		view->fov.angle_down  = views[eye].fov.angleDown;
		view->rect            = (AeronRectI) { 0, 0, g_xr.view_width, g_xr.view_height };
		out->color[eye]       = &g_xr.eyes[eye].targets[g_xr.eyes[eye].tracker.index];
	}
	/* The head is between the eyes. Its orientation comes from the head's
	 * own space, and from the left eye when that isn't known. */
	XrSpaceLocation            head         = { .type = XR_TYPE_SPACE_LOCATION };
	const XrSpaceLocationFlags head_needed  = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	XrPosef                    center       = views[0].pose;
	const XrResult             head_result =
		g_xr.fn.xrLocateSpace(g_xr.view_space, g_xr.rig_space, g_xr.frame_display_time, &head);
	if (XR_SUCCEEDED(head_result) && (head.locationFlags & head_needed) == head_needed)
		center.orientation = head.pose.orientation;
	center.position.x = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
	center.position.y = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
	center.position.z = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);
	xr_pose_to_view(&center, &rig->center);
	rig->width          = g_xr.view_width;
	rig->height         = g_xr.view_height;
	rig->primary        = 0;
	rig->tracked        = 1;
	rig->shared_sprites = 1;
	rig->generation     = g_xr.machine.generation;
	AeronXr_GetAreaMeshes(out);
	return 1;
}

void Aeron_XrSubmitViews(void) {
	if (g_xr.views_begun)
		g_xr.views_submitted = 1;
}

void Aeron_XrSubmitViewsOver(void) {
	if (g_xr.views_begun) {
		g_xr.views_submitted = 1;
		g_xr.views_over      = 1;
	}
}

/* Eye images that go over the virtual screen don't count: the screen is
 * then drawn as on a frame without eye images. */
int AeronXr_ViewsSubmitted(void) { return g_xr.views_submitted && !g_xr.views_over; }

uint64_t AeronXr_FramePeriodUs(void) { return (uint64_t)g_xr.frame_period / 1000u; }

SDL_GPUTexture* AeronXr_AcquireScreenImage(SDL_GPUTextureFormat* format, int* width, int* height) {
	char error[256];

	if (!xr_frame_renders())
		return NULL;
	if (!g_xr.screen.tracker.held &&
		!AeronXrImage_Acquire(&g_xr.screen.tracker, &g_xr.fn, error, sizeof error)) {
		AeronXr_Fail(error);
		return NULL;
	}
	*format = g_xr.color_format;
	*width  = g_xr.screen.width;
	*height = g_xr.screen.height;
	return g_xr.screen.textures[g_xr.screen.tracker.index];
}

AeronRenderTarget* Aeron_XrPanelBegin(const AeronXrPanelDesc* desc) {
	char error[256];

	if (!desc || desc->width_px <= 0 || desc->height_px <= 0 || !(desc->width_m > 0.0f) ||
		!xr_frame_renders())
		return NULL;
	if (g_xr.panel.tracker.swapchain &&
		(g_xr.panel.width != desc->width_px || g_xr.panel.height != desc->height_px)) {
		/* An image of the old size that is still held can't be shown. */
		if (g_xr.panel.tracker.held)
			return NULL;
		xr_destroy_swapchain(g_aeron.gpu_device, &g_xr.panel);
	}
	if (!g_xr.panel.tracker.swapchain &&
		!xr_create_swapchain(g_aeron.gpu_device, desc->width_px, desc->height_px, "panel", &g_xr.panel, error,
							 sizeof error)) {
		xr_destroy_swapchain(g_aeron.gpu_device, &g_xr.panel);
		Aeron_LogWarn("aeron.xr", "%s", error);
		return NULL;
	}
	if (!g_xr.panel.tracker.held &&
		!AeronXrImage_Acquire(&g_xr.panel.tracker, &g_xr.fn, error, sizeof error)) {
		AeronXr_Fail(error);
		return NULL;
	}
	g_xr.panel_desc  = *desc;
	g_xr.panel_begun = 1;
	AeronXr_SetPanel(desc);
	return &g_xr.panel.targets[g_xr.panel.tracker.index];
}

void AeronXr_SetScreenLayer(int visible, int transparent) {
	g_xr.screen_visible     = visible;
	g_xr.screen_transparent = transparent;
}
