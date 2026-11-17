/* The area meshes of each eye, from XR_KHR_visibility_mask. The hidden-area
 * mesh is the part of an eye image that the headset's lens never shows, and
 * the visible-area mesh is the rest. The scene shades no pixel under the
 * hidden one, and its full-screen passes draw the visible one
 * (src/scene/view_area.c). */

#include "../internal.h"
#include "xr_internal.h"

#include <math.h>

void AeronXr_InitAreaMeshes(void) {
	PFN_xrGetInstanceProcAddr get_proc = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
	g_xr.get_visibility_mask           = NULL;
	/* The function only exists when the extension was asked for. */
	if (!g_xr.extension_enabled[AERON_XR_EXT_VISIBILITY_MASK] || !get_proc ||
		XR_FAILED(get_proc(g_xr.instance, "xrGetVisibilityMaskKHR",
						   (PFN_xrVoidFunction*)&g_xr.get_visibility_mask))) {
		g_xr.get_visibility_mask = NULL;
	}
	for (int eye = 0; eye < 2; eye++)
		g_xr.areas[eye].stale = 1;
}

void AeronXr_FreeAreaMeshes(void) {
	for (int eye = 0; eye < 2; eye++) {
		SDL_free(g_xr.areas[eye].hidden.xy);
		SDL_free(g_xr.areas[eye].visible.xy);
		SDL_zero(g_xr.areas[eye]);
	}
	g_xr.get_visibility_mask = NULL;
}

void AeronXr_AreaMeshChanged(uint32_t view_index) {
	if (view_index < 2)
		g_xr.areas[view_index].stale = 1;
}

/* Reads one mesh of an eye from the runtime, as a list of triangles. An eye
 * without that mesh has no vertices. Returns the triangles' area on the
 * plane one unit in front of the eye. */
static double xr_read_area_mesh(int eye, XrVisibilityMaskTypeKHR type, AeronXrAreaMeshState* state) {
	XrVector2f* vertices = NULL;
	uint32_t*   indices  = NULL;
	float*      xy       = NULL;
	uint32_t    count    = 0;

	XrVisibilityMaskKHR mask   = { .type = XR_TYPE_VISIBILITY_MASK_KHR };
	XrResult            result = g_xr.get_visibility_mask(
		g_xr.machine.session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, (uint32_t)eye, type, &mask);
	if (XR_SUCCEEDED(result) && mask.vertexCountOutput > 0 && mask.indexCountOutput >= 3) {
		vertices = (XrVector2f*)SDL_calloc(mask.vertexCountOutput, sizeof *vertices);
		indices  = (uint32_t*)SDL_calloc(mask.indexCountOutput, sizeof *indices);
		xy       = (float*)SDL_calloc(mask.indexCountOutput, 2 * sizeof *xy);
	}
	if (vertices && indices && xy) {
		mask.vertexCapacityInput = mask.vertexCountOutput;
		mask.vertices            = vertices;
		mask.indexCapacityInput  = mask.indexCountOutput;
		mask.indices             = indices;
		result = g_xr.get_visibility_mask(g_xr.machine.session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
										  (uint32_t)eye, type, &mask);
		if (XR_SUCCEEDED(result)) {
			const uint32_t triangles = mask.indexCountOutput / 3;
			for (uint32_t i = 0; i < triangles * 3; i++) {
				const uint32_t index = indices[i];
				if (index >= mask.vertexCountOutput) {
					count = 0;
					break;
				}
				/* The mesh is on the plane one unit in front of the eye,
				 * with X to the right and Y up. */
				xy[2 * count + 0] = vertices[index].x;
				xy[2 * count + 1] = vertices[index].y;
				count++;
			}
		}
	}
	SDL_free(vertices);
	SDL_free(indices);
	if (count == 0) {
		SDL_free(xy);
		xy = NULL;
	}
	SDL_free(state->xy);
	state->xy           = xy;
	state->vertex_count = (int)count;
	state->generation++;

	double area = 0.0;
	for (uint32_t i = 0; i + 2 < count; i += 3) {
		const float* a = &xy[2 * i];
		area += 0.5 * fabs((double)(a[2] - a[0]) * (a[5] - a[1]) - (double)(a[4] - a[0]) * (a[3] - a[1]));
	}
	return area;
}

/* Reads both meshes of an eye. `fov` is the eye's field of view, for the
 * log: the share of the eye image that each mesh covers. */
static void xr_read_area_meshes(int eye, const XrFovf* fov) {
	AeronXrAreaState* state = &g_xr.areas[eye];
	const double      hidden =
		xr_read_area_mesh(eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &state->hidden);
	const double visible =
		xr_read_area_mesh(eye, XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, &state->visible);
	state->stale = 0;

	const double image = ((double)tanf(fov->angleRight) - tanf(fov->angleLeft)) *
						 ((double)tanf(fov->angleUp) - tanf(fov->angleDown));
	const char*  name          = eye == 0 ? "left" : "right";
	const double hidden_share  = image > 0.0 ? 100.0 * hidden / image : 0.0;
	const double visible_share = image > 0.0 ? 100.0 * visible / image : 0.0;
	Aeron_LogInfo("aeron.xr", "hidden area of the %s eye: %d triangles, %.0f%% of the image", name,
				  state->hidden.vertex_count / 3, hidden_share);
	Aeron_LogInfo("aeron.xr", "visible area of the %s eye: %d triangles, %.0f%% of the image", name,
				  state->visible.vertex_count / 3, visible_share);
	/* Together, the two meshes should cover the image. Where they don't, a
	 * pixel is skipped as hidden by one mesh and not drawn by the other. A
	 * visible mesh may reach past the image, so only a low sum is wrong. */
	if (state->hidden.vertex_count > 0 && state->visible.vertex_count > 0 &&
		hidden_share + visible_share < 98.0) {
		Aeron_LogWarn("aeron.xr",
					  "the hidden and visible areas of the %s eye only cover %.0f%% of the image; the "
					  "visible area is not used",
					  name, hidden_share + visible_share);
		SDL_free(state->visible.xy);
		state->visible.xy           = NULL;
		state->visible.vertex_count = 0;
	}
}

void AeronXr_GetAreaMeshes(AeronXrFrame* frame) {
	for (int eye = 0; eye < 2; eye++) {
		AeronXrAreaState* state = &g_xr.areas[eye];
		if (state->stale && g_xr.get_visibility_mask)
			xr_read_area_meshes(eye, &g_xr.located_views[eye].fov);
		frame->hidden_area[eye].xy            = state->hidden.xy;
		frame->hidden_area[eye].vertex_count  = state->hidden.vertex_count;
		frame->hidden_area[eye].generation    = state->hidden.generation;
		frame->visible_area[eye].xy           = state->visible.xy;
		frame->visible_area[eye].vertex_count = state->visible.vertex_count;
		frame->visible_area[eye].generation   = state->visible.generation;
	}
}
