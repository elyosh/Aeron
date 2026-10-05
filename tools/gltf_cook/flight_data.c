/*
 * aeron_gltf_cook — cooked AERON_flight_model data.
 *
 * Runs on the cooked GLB. For each component it writes `simulationData` (the
 * face BVH and the 256-entry farthest-point vertex sample over the simulation
 * topology), and for the model root `renderData` (a box-only BVH over the
 * merged render triangles). The data is appended to the GLB's binary chunk as
 * new buffer views; existing references are replaced. The result is reloaded
 * and verified exactly as a game loads it.
 */

#include "flight_data.h"

#include "aeron/asset/flight_model.h"
#include "aeron/asset/opt_sim_model.h"
#include "aeron/collision/face_bvh.h"
#include "cJSON.h"
#include "cgltf.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host log for the Aeron flight-model libraries. */
void SDL_Log(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fputs("[aeron_gltf_cook] ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	va_end(args);
}

static bool cook_error(char* error, size_t error_size, const char* message) {
	if (error && error_size)
		snprintf(error, error_size, "%s", message);
	return false;
}

/* ===== GLB container ================================================= */

enum { GLB_MAGIC = 0x46546C67u, GLB_JSON = 0x4E4F534Au, GLB_BIN = 0x004E4942u };

typedef struct Glb {
	cJSON*   json;
	uint8_t* bin;
	size_t   bin_size;
	size_t   bin_capacity;
} Glb;

static uint32_t read_u32(const uint8_t* in) {
	return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

static void write_u32(uint8_t* out, uint32_t value) {
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

static bool glb_parse(const uint8_t* bytes, size_t size, Glb* glb) {
	memset(glb, 0, sizeof *glb);
	if (size < 20 || read_u32(bytes) != GLB_MAGIC || read_u32(bytes + 4) != 2 || read_u32(bytes + 8) != size)
		return false;
	const size_t json_size = read_u32(bytes + 12);
	if (read_u32(bytes + 16) != GLB_JSON || json_size > size - 20)
		return false;
	glb->json = cJSON_ParseWithLength((const char*)bytes + 20, json_size);
	if (!cJSON_IsObject(glb->json))
		return false;
	size_t at = 20 + json_size;
	if (at + 8 <= size && read_u32(bytes + at + 4) == GLB_BIN) {
		const size_t bin_size = read_u32(bytes + at);
		if (bin_size > size - at - 8)
			return false;
		glb->bin = malloc(bin_size ? bin_size : 1);
		if (!glb->bin)
			return false;
		memcpy(glb->bin, bytes + at + 8, bin_size);
		glb->bin_size     = bin_size;
		glb->bin_capacity = bin_size;
	}
	return true;
}

static void glb_free(Glb* glb) {
	cJSON_Delete(glb->json);
	free(glb->bin);
	memset(glb, 0, sizeof *glb);
}

/* Appends `bytes` 4-byte aligned as a new buffer view of buffer 0. */
static bool glb_append_view(Glb* glb, const uint8_t* bytes, size_t size, uint32_t* view_index) {
	const size_t offset = (glb->bin_size + 3) & ~(size_t)3;
	if (offset + size > glb->bin_capacity) {
		size_t capacity = glb->bin_capacity ? glb->bin_capacity : 4096;
		while (capacity < offset + size)
			capacity *= 2;
		uint8_t* grown = realloc(glb->bin, capacity);
		if (!grown)
			return false;
		glb->bin          = grown;
		glb->bin_capacity = capacity;
	}
	memset(glb->bin + glb->bin_size, 0, offset - glb->bin_size);
	memcpy(glb->bin + offset, bytes, size);
	glb->bin_size = offset + size;

	cJSON* buffers = cJSON_GetObjectItemCaseSensitive(glb->json, "buffers");
	cJSON* views   = cJSON_GetObjectItemCaseSensitive(glb->json, "bufferViews");
	if (!buffers) {
		buffers = cJSON_AddArrayToObject(glb->json, "buffers");
		cJSON_AddItemToArray(buffers, cJSON_CreateObject());
	}
	if (!views)
		views = cJSON_AddArrayToObject(glb->json, "bufferViews");
	cJSON* buffer = cJSON_GetArrayItem(buffers, 0);
	cJSON* view   = cJSON_CreateObject();
	if (!buffer || !view || cJSON_GetObjectItemCaseSensitive(buffer, "uri"))
		return false;
	cJSON_DeleteItemFromObjectCaseSensitive(buffer, "byteLength");
	cJSON_AddNumberToObject(buffer, "byteLength", (double)glb->bin_size);
	cJSON_AddNumberToObject(view, "buffer", 0);
	cJSON_AddNumberToObject(view, "byteOffset", (double)offset);
	cJSON_AddNumberToObject(view, "byteLength", (double)size);
	*view_index = (uint32_t)cJSON_GetArraySize(views);
	cJSON_AddItemToArray(views, view);
	return true;
}

static bool glb_serialize(const Glb* glb, uint8_t** out, size_t* out_size) {
	char* json = cJSON_PrintUnformatted(glb->json);
	if (!json)
		return false;
	const size_t json_size   = strlen(json);
	const size_t json_padded = (json_size + 3) & ~(size_t)3;
	const size_t bin_padded  = (glb->bin_size + 3) & ~(size_t)3;
	const size_t total       = 12 + 8 + json_padded + (glb->bin ? 8 + bin_padded : 0);
	uint8_t*     bytes       = malloc(total);
	if (!bytes || total > UINT32_MAX) {
		free(json);
		free(bytes);
		return false;
	}
	write_u32(bytes, GLB_MAGIC);
	write_u32(bytes + 4, 2);
	write_u32(bytes + 8, (uint32_t)total);
	write_u32(bytes + 12, (uint32_t)json_padded);
	write_u32(bytes + 16, GLB_JSON);
	memcpy(bytes + 20, json, json_size);
	memset(bytes + 20 + json_size, ' ', json_padded - json_size);
	free(json);
	if (glb->bin) {
		uint8_t* chunk = bytes + 20 + json_padded;
		write_u32(chunk, (uint32_t)bin_padded);
		write_u32(chunk + 4, GLB_BIN);
		memcpy(chunk + 8, glb->bin, glb->bin_size);
		memset(chunk + 8 + glb->bin_size, 0, bin_padded - glb->bin_size);
	}
	*out      = bytes;
	*out_size = total;
	return true;
}

/* ===== Flight-model hierarchy ======================================== */

static cJSON* node_flight_extension(cJSON* json, int node_index) {
	cJSON* node = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "nodes"), node_index);
	return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(node, "extensions"),
											AERON_FLIGHT_MODEL_EXTENSION);
}

/* Resolves the model root and its ordered component node indices. */
static bool flight_hierarchy(cJSON* json, int* root, int** components, int* component_count) {
	const cJSON* scene_value = cJSON_GetObjectItemCaseSensitive(json, "scene");
	const int    scene       = cJSON_IsNumber(scene_value) ? scene_value->valueint : 0;
	cJSON* scene_nodes = cJSON_GetObjectItemCaseSensitive(
		cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "scenes"), scene), "nodes");
	const cJSON* root_value = cJSON_GetArrayItem(scene_nodes, 0);
	if (!cJSON_IsNumber(root_value))
		return false;
	*root           = root_value->valueint;
	cJSON* children = cJSON_GetObjectItemCaseSensitive(
		cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "nodes"), *root), "children");
	*component_count = cJSON_GetArraySize(children);
	*components      = malloc((size_t)(*component_count ? *component_count : 1) * sizeof **components);
	if (!*components)
		return false;
	for (int index = 0; index < *component_count; ++index) {
		const cJSON* child = cJSON_GetArrayItem(children, index);
		if (!cJSON_IsNumber(child))
			return false;
		(*components)[index] = child->valueint;
	}
	return true;
}

static bool uses_flight_extension(const cJSON* json) {
	const cJSON* used = cJSON_GetObjectItemCaseSensitive(json, "extensionsUsed");
	const cJSON* item = NULL;
	cJSON_ArrayForEach(item, used) {
		if (cJSON_IsString(item) && strcmp(item->valuestring, AERON_FLIGHT_MODEL_EXTENSION) == 0)
			return true;
	}
	return false;
}

static void strip_cooked_references(cJSON* json) {
	cJSON* nodes = cJSON_GetObjectItemCaseSensitive(json, "nodes");
	cJSON* node  = NULL;
	cJSON_ArrayForEach(node, nodes) {
		cJSON* extension = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(node, "extensions"),
															 AERON_FLIGHT_MODEL_EXTENSION);
		cJSON_DeleteItemFromObjectCaseSensitive(extension, "simulationData");
		cJSON_DeleteItemFromObjectCaseSensitive(extension, "renderData");
	}
}

static bool add_reference(cJSON* extension, const char* field, const char* hash_field, uint32_t view,
						  uint64_t hash) {
	char text[17];
	snprintf(text, sizeof text, "%016" PRIx64, hash);
	cJSON* reference = cJSON_CreateObject();
	if (!extension || !reference)
		return false;
	cJSON_AddNumberToObject(reference, "version", 1);
	cJSON_AddNumberToObject(reference, "bufferView", view);
	cJSON_AddStringToObject(reference, hash_field, text);
	cJSON_AddItemToObject(extension, field, reference);
	return true;
}

/* ===== Cooked data =================================================== */

static bool cook_component(const AeronFlightComponent* component, uint8_t** bytes, size_t* size, uint64_t* hash) {
	const AeronFlightTopology* topology = &component->topology;
	uint32_t*                  remap    = malloc((size_t)topology->position_count * sizeof *remap);
	uint32_t*                  unique   = malloc((size_t)topology->position_count * sizeof *unique);
	uint32_t                   sample[AERON_FACE_BVH_SAMPLE_MAX];
	AeronFaceBvh*              bvh = NULL;
	bool                       ok  = false;
	if (!remap || !unique)
		goto done;
	const uint32_t unique_count = Aeron_FlightTopologyDeduplicate(topology, remap, unique);
	const uint32_t sample_count =
		unique_count ? Aeron_FlightTopologySample(topology, unique, unique_count, AERON_FACE_BVH_SAMPLE_MAX, sample)
					 : UINT32_MAX;
	if (sample_count == UINT32_MAX)
		goto done;
	const AeronFaceBvhGeometry geometry = {
		.positions       = &topology->positions[0].x,
		.position_stride = sizeof *topology->positions,
		.position_count  = topology->position_count,
		.triangles       = topology->faces[0].indices,
		.triangle_stride = sizeof *topology->faces,
		.triangle_count  = topology->face_count,
		.normals         = &topology->faces[0].normal.x,
		.normal_stride   = sizeof *topology->faces,
	};
	bvh = AeronFaceBvh_Build(&geometry);
	ok  = bvh && AeronFaceBvh_WriteCooked(bvh, unique_count, sample, sample_count, bytes, size);
	*hash = AeronFaceBvh_TopologyHash(&geometry);
done:
	AeronFaceBvh_Free(bvh);
	free(remap);
	free(unique);
	return ok;
}

static bool cook_render(const AeronGltfModel* render, uint8_t** bytes, size_t* size, uint64_t* hash) {
	const AeronFaceBvhGeometry geometry = {
		.positions       = render->vertices[0].pos,
		.position_stride = sizeof *render->vertices,
		.position_count  = render->vertex_count,
		.triangles       = render->indices,
		.triangle_stride = 3 * sizeof *render->indices,
		.triangle_count  = render->index_count / 3,
	};
	AeronFaceBvh* bvh = AeronFaceBvh_BuildBoxes(&geometry);
	const bool    ok  = bvh && AeronFaceBvh_WriteCooked(bvh, render->vertex_count, NULL, 0, bytes, size);
	*hash             = AeronFaceBvh_MeshHash(&geometry);
	AeronFaceBvh_Free(bvh);
	return ok;
}

/* Builds the semantic model and the merged render arrays without the render
 * BVH the full build would construct (and log) for data not cooked yet. */
static bool load_uncooked(const uint8_t* bytes, size_t size, AeronFlightModel* model) {
	if (!Aeron_FlightModelBuildSemanticsMemory(bytes, size, "cooked GLB", model))
		return false;
	cgltf_options options = { 0 };
	cgltf_data*   data    = NULL;
	bool          ok      = cgltf_parse(&options, bytes, size, &data) == cgltf_result_success &&
				  cgltf_load_buffers(&options, data, NULL) == cgltf_result_success &&
				  Aeron_GltfMeshBuildData(data, "cooked GLB", &model->render);
	cgltf_free(data);
	if (!ok)
		Aeron_FlightModelFree(model);
	return ok;
}

/* Loads the result the way a game does; every BVH must come from cooked
 * data. */
static bool verify(const uint8_t* bytes, size_t size, bool expect_render_data, char* error, size_t error_size) {
	AeronFlightModel model;
	if (!Aeron_FlightModelBuildMemory(bytes, size, "cooked flight data", &model))
		return cook_error(error, error_size, "cooked GLB failed to load");
	AeronOptSimModel         sim;
	const AeronOptSimOptions options = { 0, AERON_OPT_TRAILING_BOX_GEOMETRY };
	char                     detail[256];
	bool                     ok = Aeron_OptSimModelBuild(&model, &options, &sim, detail, sizeof detail);
	if (!ok)
		cook_error(error, error_size, detail);
	for (uint32_t index = 0; ok && index < sim.mesh_count; ++index)
		if (sim.meshes[index].face_count && !sim.meshes[index].bvh_cooked)
			ok = cook_error(error, error_size, "a component still loads without cooked simulation data");
	if (ok && expect_render_data && (!model.has_render_data || !model.render_bvh))
		ok = cook_error(error, error_size, "the model still loads without cooked render data");
	if (sim.meshes)
		Aeron_OptSimModelFree(&sim);
	Aeron_FlightModelFree(&model);
	return ok;
}

bool aeron_gltf_cook_flight_data(const uint8_t* in, size_t in_size, uint8_t** out, size_t* out_size, bool* applied,
								 char* error, size_t error_size) {
	Glb              glb        = { 0 };
	int*             components = NULL;
	uint8_t*         stripped   = NULL;
	size_t           stripped_size = 0;
	AeronFlightModel model;
	bool             have_model = false;
	bool             ok         = false;
	*applied                    = false;
	if (!glb_parse(in, in_size, &glb))
		return cook_error(error, error_size, "output is not a valid GLB");
	if (!uses_flight_extension(glb.json)) {
		glb_free(&glb);
		return true;
	}
	/* Drop stale references before loading: re-cooking a cooked asset carries
	 * old bufferView indices through. */
	strip_cooked_references(glb.json);
	if (!glb_serialize(&glb, &stripped, &stripped_size)) {
		cook_error(error, error_size, "out of memory serializing GLB");
		goto done;
	}
	if (!load_uncooked(stripped, stripped_size, &model)) {
		cook_error(error, error_size, "cooked GLB has an invalid AERON_flight_model contract");
		goto done;
	}
	have_model = true;
	int root = 0, component_count = 0;
	if (!flight_hierarchy(glb.json, &root, &components, &component_count) ||
		(uint32_t)component_count != model.component_count) {
		cook_error(error, error_size, "flight-model hierarchy does not match the loaded model");
		goto done;
	}
	for (int index = 0; index < component_count; ++index) {
		uint8_t* bytes = NULL;
		size_t   size  = 0;
		uint64_t hash  = 0;
		uint32_t view  = 0;
		/* A component without collision faces has no BVH to cook. */
		if (!model.components[index].topology.face_count)
			continue;
		const bool added = cook_component(&model.components[index], &bytes, &size, &hash) &&
						   glb_append_view(&glb, bytes, size, &view) &&
						   add_reference(node_flight_extension(glb.json, components[index]), "simulationData",
										 "topologyHash", view, hash);
		free(bytes);
		if (!added) {
			cook_error(error, error_size, "cooking simulation data failed");
			goto done;
		}
	}
	const bool has_render = model.render.vertex_count && model.render.index_count >= 3;
	if (has_render) {
		uint8_t* bytes = NULL;
		size_t   size  = 0;
		uint64_t hash  = 0;
		uint32_t view  = 0;
		const bool added = cook_render(&model.render, &bytes, &size, &hash) &&
						   glb_append_view(&glb, bytes, size, &view) &&
						   add_reference(node_flight_extension(glb.json, root), "renderData", "meshHash", view, hash);
		free(bytes);
		if (!added) {
			cook_error(error, error_size, "cooking render data failed");
			goto done;
		}
	}
	if (!glb_serialize(&glb, out, out_size)) {
		cook_error(error, error_size, "out of memory serializing GLB");
		goto done;
	}
	if (!verify(*out, *out_size, has_render, error, error_size)) {
		free(*out);
		*out = NULL;
		goto done;
	}
	*applied = true;
	ok       = true;
done:
	if (have_model)
		Aeron_FlightModelFree(&model);
	free(components);
	free(stripped);
	glb_free(&glb);
	return ok;
}
