/* The runtime's models of the controllers that the hands hold, from
 * XR_EXT_render_model and XR_EXT_interaction_render_model. A model is a
 * binary glTF file, with named nodes that the runtime moves: the trigger,
 * the buttons, the stick.
 *
 * This file keeps one model for each hand: its file, the names of its
 * moving nodes and the space that gives its pose. The application reads the
 * file once (Aeron_XrControllerModel), and the model's pose and the poses
 * of its moving nodes each frame (Aeron_XrControllerModelState). */

#include "../internal.h"
#include "xr_internal.h"

#include <string.h>

#define XR_MODEL_MAX 16

/* The model that a hand holds. */
typedef struct XrHandModel {
	XrRenderModelEXT model;
	XrSpace          space;
	uint8_t*         glb;
	size_t           glb_size;
	uint32_t         node_count;
	char             node_names[AERON_XR_MODEL_NODE_MAX][XR_MAX_RENDER_MODEL_ASSET_NODE_NAME_SIZE_EXT];
} XrHandModel;

static XrHandModel g_hand_models[2];
/* Changes each time the models are read again. */
static uint32_t g_model_generation;

static void xr_models_release(void) {
	for (int hand = 0; hand < 2; hand++) {
		XrHandModel* slot = &g_hand_models[hand];
		if (slot->space)
			g_xr.fn.xrDestroySpace(slot->space);
		if (slot->model && g_xr.model_fn.xrDestroyRenderModelEXT)
			g_xr.model_fn.xrDestroyRenderModelEXT(slot->model);
		SDL_free(slot->glb);
		SDL_zerop(slot);
	}
	g_model_generation++;
}

void AeronXrModels_Shutdown(void) {
	xr_models_release();
	g_xr.models_available = 0;
}

void AeronXrModels_Init(void) {
	PFN_xrGetInstanceProcAddr get_proc = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
	int                       found    = get_proc != NULL;

	SDL_zero(g_xr.model_fn);
	g_xr.models_available = 0;
	g_xr.models_stale     = 0;
	/* The functions only exist when the extensions were requested. */
	if (!g_xr.extension_enabled[AERON_XR_EXT_RENDER_MODEL] ||
		!g_xr.extension_enabled[AERON_XR_EXT_INTERACTION_RENDER_MODEL])
		found = 0;
#define XR_MODEL_FUNCTION(name)                                                                              \
	if (found && (XR_FAILED(get_proc(g_xr.instance, #name, (PFN_xrVoidFunction*)&g_xr.model_fn.name)) ||     \
				  !g_xr.model_fn.name))                                                                      \
		found = 0;
	XR_MODEL_FUNCTION(xrEnumerateInteractionRenderModelIdsEXT)
	XR_MODEL_FUNCTION(xrGetRenderModelPoseTopLevelUserPathEXT)
	XR_MODEL_FUNCTION(xrCreateRenderModelEXT)
	XR_MODEL_FUNCTION(xrDestroyRenderModelEXT)
	XR_MODEL_FUNCTION(xrGetRenderModelPropertiesEXT)
	XR_MODEL_FUNCTION(xrCreateRenderModelAssetEXT)
	XR_MODEL_FUNCTION(xrDestroyRenderModelAssetEXT)
	XR_MODEL_FUNCTION(xrGetRenderModelAssetDataEXT)
	XR_MODEL_FUNCTION(xrGetRenderModelAssetPropertiesEXT)
	XR_MODEL_FUNCTION(xrCreateRenderModelSpaceEXT)
	XR_MODEL_FUNCTION(xrGetRenderModelStateEXT)
#undef XR_MODEL_FUNCTION
	if (!found)
		SDL_zero(g_xr.model_fn);
	g_xr.models_available = found;
	g_xr.models_stale     = found;
	Aeron_LogInfo("aeron.xr", "controller models: %s",
				  found ? "available"
						: "not available (the runtime has no XR_EXT_render_model or "
						  "XR_EXT_interaction_render_model)");
}

void AeronXrModels_Changed(void) {
	if (g_xr.models_available)
		g_xr.models_stale = 1;
}

static void xr_model_warn(const char* what, XrResult result) {
	char error[256];
	AeronXr_ResultError(error, sizeof error, what, result);
	Aeron_LogWarn("aeron.xr", "controller model: %s", error);
}

/* Creates the model with this ID. The application says which glTF
 * extensions it can read, and the runtime refuses when the model needs
 * another. Nothing reads the file yet, so this tries with none first and
 * then with the extensions that such files often use, to learn from the
 * log what a loader will need. */
static int xr_model_create(XrRenderModelIdEXT id, XrRenderModelEXT* model) {
	static const char* const extensions[] = {
		"KHR_texture_basisu",      "KHR_texture_transform",
		"KHR_mesh_quantization",   "KHR_materials_emissive_strength",
		"EXT_meshopt_compression", "KHR_draco_mesh_compression",
	};
	XrRenderModelCreateInfoEXT info = { .type = XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT, .renderModelId = id };
	XrResult result = g_xr.model_fn.xrCreateRenderModelEXT(g_xr.machine.session, &info, model);
	if (result == XR_ERROR_RENDER_MODEL_GLTF_EXTENSION_REQUIRED_EXT) {
		Aeron_LogInfo("aeron.xr", "controller model: the model needs glTF extensions");
		info.gltfExtensionCount = SDL_arraysize(extensions);
		info.gltfExtensions     = extensions;
		result                  = g_xr.model_fn.xrCreateRenderModelEXT(g_xr.machine.session, &info, model);
	}
	if (XR_FAILED(result)) {
		*model = XR_NULL_HANDLE;
		xr_model_warn("could not create the model", result);
		return 0;
	}
	return 1;
}

/* Which hand the model is drawn for: 0 or 1, or -1. */
static int xr_model_hand(XrRenderModelEXT model) {
	const XrInteractionRenderModelTopLevelUserPathGetInfoEXT info = {
		.type                  = XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT,
		.topLevelUserPathCount = 2,
		.topLevelUserPaths     = g_xr.input.hand_paths,
	};
	XrPath path = XR_NULL_PATH;
	if (XR_FAILED(g_xr.model_fn.xrGetRenderModelPoseTopLevelUserPathEXT(model, &info, &path)))
		return -1;
	return path == g_xr.input.hand_paths[0] ? 0 : path == g_xr.input.hand_paths[1] ? 1 : -1;
}

/* Reads the model with this ID and keeps it for its hand. A hand keeps the
 * first model that the runtime lists for it. */
static void xr_model_read(XrRenderModelIdEXT id) {
	XrRenderModelEXT model;
	if (!xr_model_create(id, &model))
		return;
	const int    hand = xr_model_hand(model);
	XrHandModel* slot = hand >= 0 ? &g_hand_models[hand] : NULL;
	if (!slot || slot->model) {
		Aeron_LogInfo("aeron.xr", "controller model: %s, left out",
					  slot ? "a second model for a hand" : "a model for no hand");
		g_xr.model_fn.xrDestroyRenderModelEXT(model);
		return;
	}

	const XrRenderModelPropertiesGetInfoEXT get_info   = { .type =
															   XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT };
	XrRenderModelPropertiesEXT              properties = { .type = XR_TYPE_RENDER_MODEL_PROPERTIES_EXT };
	XrResult result = g_xr.model_fn.xrGetRenderModelPropertiesEXT(model, &get_info, &properties);
	if (XR_FAILED(result)) {
		xr_model_warn("could not read the model's properties", result);
		g_xr.model_fn.xrDestroyRenderModelEXT(model);
		return;
	}

	/* The model's file belongs to an asset, which several models can share. */
	const XrRenderModelAssetCreateInfoEXT asset_info = {
		.type    = XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT,
		.cacheId = properties.cacheId,
	};
	XrRenderModelAssetEXT asset = XR_NULL_HANDLE;
	result = g_xr.model_fn.xrCreateRenderModelAssetEXT(g_xr.machine.session, &asset_info, &asset);
	if (XR_FAILED(result)) {
		xr_model_warn("could not open the model's file", result);
		g_xr.model_fn.xrDestroyRenderModelEXT(model);
		return;
	}
	const XrRenderModelAssetDataGetInfoEXT data_info = { .type =
															 XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT };
	XrRenderModelAssetDataEXT              data      = { .type = XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT };
	uint8_t*                               glb       = NULL;
	result = g_xr.model_fn.xrGetRenderModelAssetDataEXT(asset, &data_info, &data);
	if (XR_SUCCEEDED(result) && data.bufferCountOutput > 0) {
		glb                      = (uint8_t*)SDL_malloc(data.bufferCountOutput);
		data.bufferCapacityInput = data.bufferCountOutput;
		data.buffer              = glb;
		result                   = glb ? g_xr.model_fn.xrGetRenderModelAssetDataEXT(asset, &data_info, &data)
									   : XR_ERROR_OUT_OF_MEMORY;
	}
	if (XR_FAILED(result) || !glb) {
		xr_model_warn("could not read the model's file", result);
		SDL_free(glb);
		g_xr.model_fn.xrDestroyRenderModelAssetEXT(asset);
		g_xr.model_fn.xrDestroyRenderModelEXT(model);
		return;
	}

	XrRenderModelAssetNodePropertiesEXT nodes[AERON_XR_MODEL_NODE_MAX];
	const uint32_t node_count = SDL_min(properties.animatableNodeCount, (uint32_t)AERON_XR_MODEL_NODE_MAX);
	const XrRenderModelAssetPropertiesGetInfoEXT nodes_info = {
		.type = XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT,
	};
	XrRenderModelAssetPropertiesEXT asset_properties = {
		.type              = XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT,
		.nodePropertyCount = node_count,
		.nodeProperties    = nodes,
	};
	SDL_zero(nodes);
	/* The states are given for every moving node, in this order, so a model
	 * with more of them than Aeron keeps can't be animated. */
	if (properties.animatableNodeCount > AERON_XR_MODEL_NODE_MAX) {
		Aeron_LogWarn("aeron.xr", "controller model: %u moving nodes are too many; the model stays still",
					  (unsigned)properties.animatableNodeCount);
	} else if (node_count > 0) {
		result = g_xr.model_fn.xrGetRenderModelAssetPropertiesEXT(asset, &nodes_info, &asset_properties);
		if (XR_FAILED(result))
			xr_model_warn("could not read the names of the model's nodes", result);
		else
			slot->node_count = node_count;
	}
	for (uint32_t i = 0; i < slot->node_count; i++)
		SDL_strlcpy(slot->node_names[i], nodes[i].uniqueName, sizeof slot->node_names[i]);
	g_xr.model_fn.xrDestroyRenderModelAssetEXT(asset);

	const XrRenderModelSpaceCreateInfoEXT space_info = {
		.type        = XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT,
		.renderModel = model,
	};
	result = g_xr.model_fn.xrCreateRenderModelSpaceEXT(g_xr.machine.session, &space_info, &slot->space);
	if (XR_FAILED(result)) {
		xr_model_warn("could not create the model's space", result);
		SDL_free(glb);
		g_xr.model_fn.xrDestroyRenderModelEXT(model);
		SDL_zerop(slot);
		return;
	}
	slot->model    = model;
	slot->glb      = glb;
	slot->glb_size = data.bufferCountOutput;
	Aeron_LogInfo("aeron.xr", "controller model (%s hand): %u bytes, %u moving nodes",
				  hand ? "right" : "left", (unsigned)slot->glb_size, (unsigned)slot->node_count);
	for (uint32_t i = 0; i < slot->node_count; i++)
		Aeron_LogInfo("aeron.xr", "  node %u: %s", (unsigned)i, slot->node_names[i]);
}

void AeronXrModels_Update(void) {
	if (!g_xr.models_available || !g_xr.models_stale || !g_xr.input.ready)
		return;
	g_xr.models_stale = 0;
	xr_models_release();

	const XrInteractionRenderModelIdsEnumerateInfoEXT info = {
		.type = XR_TYPE_INTERACTION_RENDER_MODEL_IDS_ENUMERATE_INFO_EXT,
	};
	XrRenderModelIdEXT ids[XR_MODEL_MAX];
	uint32_t           count = 0;
	const XrResult result = g_xr.model_fn.xrEnumerateInteractionRenderModelIdsEXT(g_xr.machine.session, &info,
																				  XR_MODEL_MAX, &count, ids);
	if (XR_FAILED(result)) {
		xr_model_warn("could not list the models", result);
		return;
	}
	Aeron_LogInfo("aeron.xr", "the runtime lists %u controller models", (unsigned)count);
	for (uint32_t i = 0; i < count && i < XR_MODEL_MAX; i++)
		xr_model_read(ids[i]);
}

int Aeron_XrControllerModel(int hand, AeronXrControllerModel* out) {
	if (!out || hand < 0 || hand > 1 || !g_hand_models[hand].model)
		return 0;
	const XrHandModel* slot = &g_hand_models[hand];
	SDL_zerop(out);
	out->generation = g_model_generation;
	out->glb        = slot->glb;
	out->glb_size   = slot->glb_size;
	out->node_count = (int)slot->node_count;
	for (uint32_t i = 0; i < slot->node_count; i++)
		out->node_names[i] = slot->node_names[i];
	return 1;
}

int Aeron_XrControllerModelState(int hand, AeronXrControllerModelState* out) {
	if (!out || hand < 0 || hand > 1 || !g_hand_models[hand].model || !g_xr.machine.running || g_xr.failed)
		return 0;
	const XrHandModel*         slot     = &g_hand_models[hand];
	XrSpaceLocation            location = { .type = XR_TYPE_SPACE_LOCATION };
	const XrSpaceLocationFlags needed =
		XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if (XR_FAILED(g_xr.fn.xrLocateSpace(slot->space, g_xr.rig_space, g_xr.frame_display_time, &location)) ||
		(location.locationFlags & needed) != needed)
		return 0;
	SDL_zerop(out);
	out->pose.position_m[0]  = location.pose.position.x;
	out->pose.position_m[1]  = location.pose.position.y;
	out->pose.position_m[2]  = location.pose.position.z;
	out->pose.orientation[0] = location.pose.orientation.x;
	out->pose.orientation[1] = location.pose.orientation.y;
	out->pose.orientation[2] = location.pose.orientation.z;
	out->pose.orientation[3] = location.pose.orientation.w;

	XrRenderModelNodeStateEXT          states[AERON_XR_MODEL_NODE_MAX];
	const XrRenderModelStateGetInfoEXT info = {
		.type        = XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT,
		.displayTime = g_xr.frame_display_time,
	};
	XrRenderModelStateEXT state = {
		.type           = XR_TYPE_RENDER_MODEL_STATE_EXT,
		.nodeStateCount = slot->node_count,
		.nodeStates     = states,
	};
	/* Without the nodes' states the model is still drawn, at rest. */
	if (slot->node_count == 0 ||
		XR_FAILED(g_xr.model_fn.xrGetRenderModelStateEXT(slot->model, &info, &state)))
		return 1;
	out->node_count = (int)slot->node_count;
	for (uint32_t i = 0; i < slot->node_count; i++) {
		out->nodes[i].position_m[0]  = states[i].nodePose.position.x;
		out->nodes[i].position_m[1]  = states[i].nodePose.position.y;
		out->nodes[i].position_m[2]  = states[i].nodePose.position.z;
		out->nodes[i].orientation[0] = states[i].nodePose.orientation.x;
		out->nodes[i].orientation[1] = states[i].nodePose.orientation.y;
		out->nodes[i].orientation[2] = states[i].nodePose.orientation.z;
		out->nodes[i].orientation[3] = states[i].nodePose.orientation.w;
		out->node_visible[i]         = states[i].isVisible == XR_TRUE;
	}
	return 1;
}
