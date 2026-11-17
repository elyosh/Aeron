/* The runtime's models of the controllers that the hands hold, from
 * XR_EXT_render_model and XR_EXT_interaction_render_model. A model is a
 * binary glTF file, with named nodes that the runtime moves: the trigger,
 * the buttons, the stick.
 *
 * For now this file only logs what the runtime offers: for each model its
 * hand, the size of its file and the names of its moving nodes. Nothing is
 * loaded or drawn. */

#include "../internal.h"
#include "xr_internal.h"

#define XR_MODEL_MAX 16
#define XR_MODEL_NODE_MAX 64

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

/* Which hand the model is drawn for, as text for the log. */
static const char* xr_model_hand(XrRenderModelEXT model) {
	const XrInteractionRenderModelTopLevelUserPathGetInfoEXT info = {
		.type                  = XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT,
		.topLevelUserPathCount = 2,
		.topLevelUserPaths     = g_xr.input.hand_paths,
	};
	XrPath path = XR_NULL_PATH;
	if (XR_FAILED(g_xr.model_fn.xrGetRenderModelPoseTopLevelUserPathEXT(model, &info, &path)))
		return "unknown hand";
	if (path == g_xr.input.hand_paths[0])
		return "left hand";
	if (path == g_xr.input.hand_paths[1])
		return "right hand";
	return "no hand";
}

static void xr_model_log(XrRenderModelIdEXT id) {
	XrRenderModelEXT model;
	if (!xr_model_create(id, &model))
		return;

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
	result = g_xr.model_fn.xrGetRenderModelAssetDataEXT(asset, &data_info, &data);
	if (XR_FAILED(result))
		xr_model_warn("could not read the size of the model's file", result);

	Aeron_LogInfo("aeron.xr", "controller model (%s): %u bytes, %u moving nodes", xr_model_hand(model),
				  (unsigned)data.bufferCountOutput, (unsigned)properties.animatableNodeCount);

	XrRenderModelAssetNodePropertiesEXT nodes[XR_MODEL_NODE_MAX];
	const uint32_t node_count = SDL_min(properties.animatableNodeCount, (uint32_t)XR_MODEL_NODE_MAX);
	const XrRenderModelAssetPropertiesGetInfoEXT nodes_info = {
		.type = XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT,
	};
	XrRenderModelAssetPropertiesEXT asset_properties = {
		.type              = XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT,
		.nodePropertyCount = node_count,
		.nodeProperties    = nodes,
	};
	SDL_zero(nodes);
	if (node_count > 0) {
		result = g_xr.model_fn.xrGetRenderModelAssetPropertiesEXT(asset, &nodes_info, &asset_properties);
		if (XR_FAILED(result)) {
			xr_model_warn("could not read the names of the model's nodes", result);
		} else {
			for (uint32_t i = 0; i < node_count; i++) {
				nodes[i].uniqueName[sizeof nodes[i].uniqueName - 1] = '\0';
				Aeron_LogInfo("aeron.xr", "  node %u: %s", (unsigned)i, nodes[i].uniqueName);
			}
		}
	}
	g_xr.model_fn.xrDestroyRenderModelAssetEXT(asset);
	g_xr.model_fn.xrDestroyRenderModelEXT(model);
}

void AeronXrModels_Update(void) {
	if (!g_xr.models_available || !g_xr.models_stale || !g_xr.input.ready)
		return;
	g_xr.models_stale = 0;

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
		xr_model_log(ids[i]);
}
