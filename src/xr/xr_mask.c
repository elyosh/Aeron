/* The hidden-area mesh of each eye, from XR_KHR_visibility_mask: the part
 * of an eye image that the headset's lens never shows. The scene shades no
 * pixel under it (src/scene/hidden_area.c). */

#include "../internal.h"
#include "xr_internal.h"

#include <math.h>

void AeronXr_InitHiddenAreas(void) {
	PFN_xrGetInstanceProcAddr get_proc = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
	g_xr.get_visibility_mask           = NULL;
	/* The function only exists when the extension was asked for. */
	if (g_xr.extension_count == 0 || !get_proc ||
		XR_FAILED(get_proc(g_xr.instance, "xrGetVisibilityMaskKHR",
						   (PFN_xrVoidFunction*)&g_xr.get_visibility_mask))) {
		g_xr.get_visibility_mask = NULL;
	}
	for (int eye = 0; eye < 2; eye++)
		g_xr.hidden[eye].stale = 1;
}

void AeronXr_FreeHiddenAreas(void) {
	for (int eye = 0; eye < 2; eye++) {
		SDL_free(g_xr.hidden[eye].xy);
		SDL_zero(g_xr.hidden[eye]);
	}
	g_xr.get_visibility_mask = NULL;
}

void AeronXr_HiddenAreaChanged(uint32_t view_index) {
	if (view_index < 2)
		g_xr.hidden[view_index].stale = 1;
}

/* Reads one eye's mesh from the runtime, as a list of triangles. An eye
 * without a mesh has no vertices. `fov` is the eye's field of view, for the
 * log: the share of the eye image that the mesh covers. */
static void xr_read_hidden_area(int eye, const XrFovf* fov) {
	AeronXrHiddenAreaState* state = &g_xr.hidden[eye];
	XrVector2f*             vertices = NULL;
	uint32_t*               indices  = NULL;
	float*                  xy       = NULL;
	uint32_t                count    = 0;

	XrVisibilityMaskKHR mask   = { .type = XR_TYPE_VISIBILITY_MASK_KHR };
	XrResult            result = g_xr.get_visibility_mask(g_xr.machine.session,
														  XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, (uint32_t)eye,
														  XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);
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
										  (uint32_t)eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);
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
	state->stale = 0;
	/* The triangles' area on the plane one unit in front of the eye, over
	 * the area of the eye image on that plane. */
	double hidden = 0.0;
	for (uint32_t i = 0; i + 2 < count; i += 3) {
		const float* a = &xy[2 * i];
		hidden += 0.5 * fabs((double)(a[2] - a[0]) * (a[5] - a[1]) - (double)(a[4] - a[0]) * (a[3] - a[1]));
	}
	const double image = ((double)tanf(fov->angleRight) - tanf(fov->angleLeft)) *
						 ((double)tanf(fov->angleUp) - tanf(fov->angleDown));
	Aeron_LogInfo("aeron.xr", "hidden area of the %s eye: %u triangles, %.0f%% of the image",
				  eye == 0 ? "left" : "right", (unsigned)(count / 3), image > 0.0 ? 100.0 * hidden / image : 0.0);
}

void AeronXr_GetHiddenAreas(AeronXrFrame* frame) {
	for (int eye = 0; eye < 2; eye++) {
		AeronXrHiddenAreaState* state = &g_xr.hidden[eye];
		if (state->stale && g_xr.get_visibility_mask)
			xr_read_hidden_area(eye, &g_xr.located_views[eye].fov);
		frame->hidden_area[eye].xy           = state->xy;
		frame->hidden_area[eye].vertex_count = state->vertex_count;
		frame->hidden_area[eye].generation   = state->generation;
	}
}
