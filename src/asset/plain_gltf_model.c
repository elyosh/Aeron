/* Builds a flight model from a plain glTF binary file
 * (aeron/asset/plain_gltf_model.h). */

#include "aeron/asset/plain_gltf_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aeron/image.h"
#include "cJSON.h"
#include "gltf_cook.h"

/* ---- 3x4 matrices, row-major, for column vectors -------------------- */

static const float kIdentity[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };

static void mat_multiply(const float a[12], const float b[12], float out[12]) {
	float result[12];
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 4; column++) {
			result[row * 4 + column] =
				a[row * 4 + 0] * b[0 * 4 + column] + a[row * 4 + 1] * b[1 * 4 + column] +
				a[row * 4 + 2] * b[2 * 4 + column] + (column == 3 ? a[row * 4 + 3] : 0.0f);
		}
	}
	memcpy(out, result, sizeof result);
}

/* Inverts a matrix made of a rotation, a uniform scale and a translation. */
static void mat_invert(const float m[12], float out[12]) {
	const float scale2 = m[0] * m[0] + m[4] * m[4] + m[8] * m[8];
	const float k      = scale2 > 0.0f ? 1.0f / scale2 : 0.0f;
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 3; column++)
			out[row * 4 + column] = m[column * 4 + row] * k;
		out[row * 4 + 3] = -(out[row * 4 + 0] * m[3] + out[row * 4 + 1] * m[7] + out[row * 4 + 2] * m[11]);
	}
}

static void mat_from_trs(const float t[3], const float q[4], float scale, float out[12]) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	out[0]  = scale * (1.0f - 2.0f * (y * y + z * z));
	out[1]  = scale * 2.0f * (x * y - w * z);
	out[2]  = scale * 2.0f * (x * z + w * y);
	out[3]  = t[0];
	out[4]  = scale * 2.0f * (x * y + w * z);
	out[5]  = scale * (1.0f - 2.0f * (x * x + z * z));
	out[6]  = scale * 2.0f * (y * z - w * x);
	out[7]  = t[1];
	out[8]  = scale * 2.0f * (x * z - w * y);
	out[9]  = scale * 2.0f * (y * z + w * x);
	out[10] = scale * (1.0f - 2.0f * (x * x + y * y));
	out[11] = t[2];
}

/* Splits a matrix into a translation, a rotation and a uniform scale.
 * Returns false when its scale isn't uniform and positive. */
static bool mat_to_trs(const float m[12], float t[3], float q[4], float* scale) {
	float lengths[3];
	for (int column = 0; column < 3; column++)
		lengths[column] =
			sqrtf(m[column] * m[column] + m[4 + column] * m[4 + column] + m[8 + column] * m[8 + column]);
	const float s           = (lengths[0] + lengths[1] + lengths[2]) / 3.0f;
	const float determinant = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
							  m[2] * (m[4] * m[9] - m[5] * m[8]);
	if (!(s > 1e-6f) || determinant <= 0.0f)
		return false;
	for (int column = 0; column < 3; column++)
		if (fabsf(lengths[column] - s) > 1e-3f * s)
			return false;
	const float r[9]  = { m[0] / s, m[1] / s, m[2] / s, m[4] / s, m[5] / s,
						  m[6] / s, m[8] / s, m[9] / s, m[10] / s };
	const float trace = r[0] + r[4] + r[8];
	if (trace > 0.0f) {
		const float k = sqrtf(trace + 1.0f) * 2.0f;
		q[3]          = 0.25f * k;
		q[0]          = (r[7] - r[5]) / k;
		q[1]          = (r[2] - r[6]) / k;
		q[2]          = (r[3] - r[1]) / k;
	} else if (r[0] > r[4] && r[0] > r[8]) {
		const float k = sqrtf(1.0f + r[0] - r[4] - r[8]) * 2.0f;
		q[3]          = (r[7] - r[5]) / k;
		q[0]          = 0.25f * k;
		q[1]          = (r[1] + r[3]) / k;
		q[2]          = (r[2] + r[6]) / k;
	} else if (r[4] > r[8]) {
		const float k = sqrtf(1.0f + r[4] - r[0] - r[8]) * 2.0f;
		q[3]          = (r[2] - r[6]) / k;
		q[0]          = (r[1] + r[3]) / k;
		q[1]          = 0.25f * k;
		q[2]          = (r[5] + r[7]) / k;
	} else {
		const float k = sqrtf(1.0f + r[8] - r[0] - r[4]) * 2.0f;
		q[3]          = (r[3] - r[1]) / k;
		q[0]          = (r[2] + r[6]) / k;
		q[1]          = (r[5] + r[7]) / k;
		q[2]          = 0.25f * k;
	}
	t[0]   = m[3];
	t[1]   = m[7];
	t[2]   = m[11];
	*scale = s;
	return true;
}

/* ---- Reading the file ----------------------------------------------- */

static bool fail(char* error, size_t error_size, const char* message) {
	if (error && error_size)
		snprintf(error, error_size, "%s", message);
	return false;
}

static uint32_t read_u32(const uint8_t* bytes) {
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
		   ((uint32_t)bytes[3] << 24);
}

static bool json_numbers(const cJSON* array, float* out, int count) {
	if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != count)
		return false;
	for (int i = 0; i < count; i++) {
		const cJSON* item = cJSON_GetArrayItem(array, i);
		if (!cJSON_IsNumber(item))
			return false;
		out[i] = (float)item->valuedouble;
	}
	return true;
}

/* A node's transform at rest: its `matrix`, or its translation, rotation
 * and scale. */
static void node_local(const cJSON* node, float out[12]) {
	float matrix[16];
	if (json_numbers(cJSON_GetObjectItemCaseSensitive(node, "matrix"), matrix, 16)) {
		/* glTF writes a matrix column by column. */
		for (int row = 0; row < 3; row++)
			for (int column = 0; column < 4; column++)
				out[row * 4 + column] = matrix[column * 4 + row];
		return;
	}
	float t[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };
	json_numbers(cJSON_GetObjectItemCaseSensitive(node, "translation"), t, 3);
	json_numbers(cJSON_GetObjectItemCaseSensitive(node, "rotation"), q, 4);
	json_numbers(cJSON_GetObjectItemCaseSensitive(node, "scale"), s, 3);
	mat_from_trs(t, q, 1.0f, out);
	for (int row = 0; row < 3; row++)
		for (int column = 0; column < 3; column++)
			out[row * 4 + column] *= s[column];
}

/* Removes every `extensions` member in `item` and below it. */
static void strip_extensions(cJSON* item) {
	if (cJSON_IsObject(item))
		cJSON_DeleteItemFromObjectCaseSensitive(item, "extensions");
	cJSON* child;
	cJSON_ArrayForEach(child, item) { strip_extensions(child); }
}

/* The images of the file, decoded when the cook asks for them. */
typedef struct PlainImages {
	const uint8_t* bin;
	size_t         bin_size;
	int            count;

	struct {
		size_t   offset, size;
		uint8_t* rgba;
	} images[64];
} PlainImages;

static bool plain_image(void* context, const char* uri, AeronGltfCookImageView* out) {
	PlainImages* images = context;
	int          index  = -1;
	if (sscanf(uri, "plain_image_%d.png", &index) != 1 || index < 0 || index >= images->count)
		return false;
	int width = 0, height = 0;
	if (!images->images[index].rgba)
		images->images[index].rgba = Aeron_ImageDecodeRgba8(images->bin + images->images[index].offset,
															images->images[index].size, &width, &height);
	else
		return false; /* the cook asks for each image once */
	if (!images->images[index].rgba)
		return false;
	out->rgba   = images->images[index].rgba;
	out->width  = width;
	out->height = height;
	return true;
}

static cJSON* flight_extension(const char* role, bool component) {
	cJSON* extensions = cJSON_CreateObject();
	cJSON* object     = cJSON_AddObjectToObject(extensions, "AERON_flight_model");
	cJSON_AddStringToObject(object, "role", role);
	if (component)
		cJSON_AddStringToObject(object, "meshType", "default");
	return extensions;
}

bool Aeron_FlightModelBuildPlainGlb(const uint8_t* glb, size_t size, const char* label, AeronFlightModel* out,
									AeronPlainGltfNodes* nodes, char* error, size_t error_size) {
	if (!glb || !out || !nodes || !label)
		return fail(error, error_size, "no file");
	memset(out, 0, sizeof *out);
	memset(nodes, 0, sizeof *nodes);
	/* The GLB container: a header, the JSON chunk, then the binary chunk. */
	if (size < 20 || read_u32(glb) != 0x46546c67u || read_u32(glb + 4) != 2)
		return fail(error, error_size, "the file isn't a glTF 2.0 binary file");
	const uint32_t json_size = read_u32(glb + 12);
	if (read_u32(glb + 16) != 0x4e4f534au || (size_t)json_size > size - 20)
		return fail(error, error_size, "the file has no JSON part");
	const uint8_t* bin      = NULL;
	size_t         bin_size = 0;
	const size_t   bin_at   = 20 + (size_t)json_size;
	if (bin_at + 8 <= size && read_u32(glb + bin_at + 4) == 0x004e4942u) {
		bin_size = read_u32(glb + bin_at);
		bin      = glb + bin_at + 8;
		if (bin_size > size - bin_at - 8)
			return fail(error, error_size, "the file's binary part is cut short");
	}
	cJSON* root = cJSON_ParseWithLength((const char*)glb + 20, json_size);
	if (!root)
		return fail(error, error_size, "the file's JSON part can't be read");

	bool         ok           = true;
	const char*  problem      = NULL;
	PlainImages* images       = (PlainImages*)calloc(1, sizeof *images);
	float (*rest)[12]         = (float (*)[12])calloc(AERON_PLAIN_GLTF_MAX_NODES, sizeof *rest);
	cJSON*       file_nodes   = cJSON_GetObjectItemCaseSensitive(root, "nodes");
	const cJSON* required     = cJSON_GetObjectItemCaseSensitive(root, "extensionsRequired");
	const cJSON* buffers      = cJSON_GetObjectItemCaseSensitive(root, "buffers");
	const cJSON* buffer_views = cJSON_GetObjectItemCaseSensitive(root, "bufferViews");
	const cJSON* scenes       = cJSON_GetObjectItemCaseSensitive(root, "scenes");
	const cJSON* scene_index  = cJSON_GetObjectItemCaseSensitive(root, "scene");
	const int    node_count   = cJSON_GetArraySize(file_nodes);
#define REQUIRE(condition, message)                                                                          \
	if (ok && !(condition)) {                                                                                \
		ok      = false;                                                                                     \
		problem = message;                                                                                   \
	}
	REQUIRE(images && rest, "out of memory");
	REQUIRE(!required || cJSON_GetArraySize(required) == 0, "the file needs a glTF extension");
	REQUIRE(cJSON_GetArraySize(buffers) == 1 && bin, "the file must have one buffer, in its binary part");
	REQUIRE(node_count > 0 && node_count <= AERON_PLAIN_GLTF_MAX_NODES, "the file has no nodes, or too many");

	/* The node tree: names, parents and transforms at rest. */
	for (int n = 0; ok && n < node_count; n++) {
		const cJSON*        node = cJSON_GetArrayItem(file_nodes, n);
		const cJSON*        name = cJSON_GetObjectItemCaseSensitive(node, "name");
		AeronPlainGltfNode* info = &nodes->nodes[n];
		snprintf(info->name, sizeof info->name, "%s", cJSON_IsString(name) ? name->valuestring : "");
		info->parent    = -1;
		info->component = -1;
		node_local(node, info->local);
		info->scale = sqrtf(info->local[0] * info->local[0] + info->local[4] * info->local[4] +
							info->local[8] * info->local[8]);
	}
	for (int n = 0; ok && n < node_count; n++) {
		const cJSON* child;
		cJSON_ArrayForEach(child,
						   cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(file_nodes, n), "children")) {
			if (cJSON_IsNumber(child) && child->valueint >= 0 && child->valueint < node_count)
				nodes->nodes[child->valueint].parent = n;
		}
	}
	nodes->count = ok ? node_count : 0;
	/* A parent can come after its child in the file, so the transforms to
	 * the model's space are built by walking up from each node. */
	for (int n = 0; ok && n < node_count; n++) {
		memcpy(rest[n], nodes->nodes[n].local, sizeof rest[n]);
		int depth = 0;
		for (int parent = nodes->nodes[n].parent; parent >= 0; parent = nodes->nodes[parent].parent) {
			mat_multiply(nodes->nodes[parent].local, rest[n], rest[n]);
			REQUIRE(++depth <= node_count, "the file's nodes form a loop");
			if (!ok)
				break;
		}
		mat_invert(rest[n], nodes->rest_inverse[n]);
	}

	/* The nodes of the scene that have a mesh become the parts. */
	const cJSON* scene = cJSON_GetArrayItem(scenes, cJSON_IsNumber(scene_index) ? scene_index->valueint : 0);
	uint8_t*     in_scene = (uint8_t*)calloc((size_t)(node_count > 0 ? node_count : 1), 1);
	REQUIRE(scene && in_scene, "the file has no scene");
	const cJSON* scene_root;
	cJSON_ArrayForEach(scene_root, cJSON_GetObjectItemCaseSensitive(scene, "nodes")) {
		if (ok && cJSON_IsNumber(scene_root) && scene_root->valueint >= 0 &&
			scene_root->valueint < node_count)
			in_scene[scene_root->valueint] = 1;
	}
	for (int n = 0; ok && n < node_count; n++) {
		/* A node is in the scene when one of its ancestors is a scene root. */
		for (int up = n; up >= 0 && !in_scene[n]; up = nodes->nodes[up].parent)
			in_scene[n] = in_scene[up];
	}
	cJSON* new_nodes = cJSON_CreateArray();
	cJSON* model     = cJSON_CreateObject();
	cJSON* children  = cJSON_AddArrayToObject(model, "children");
	cJSON_AddStringToObject(model, "name", label);
	cJSON_AddItemToObject(model, "extensions", flight_extension("model", false));
	cJSON_AddItemToArray(new_nodes, model);
	for (int n = 0; ok && n < node_count; n++) {
		const cJSON* mesh = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(file_nodes, n), "mesh");
		float        t[3], q[4], s;
		if (!in_scene[n] || !cJSON_IsNumber(mesh))
			continue;
		REQUIRE(nodes->component_count < AERON_MAX_MESH_COMPONENTS, "the file has too many meshes");
		REQUIRE(mat_to_trs(rest[n], t, q, &s), "a mesh of the file is under a scale that isn't uniform");
		if (!ok)
			break;
		char   name[32];
		cJSON* part = cJSON_CreateObject();
		snprintf(name, sizeof name, "part%d", nodes->component_count);
		cJSON_AddStringToObject(part, "name", name);
		cJSON_AddNumberToObject(part, "mesh", mesh->valueint);
		cJSON_AddItemToObject(part, "translation", cJSON_CreateFloatArray(t, 3));
		cJSON_AddItemToObject(part, "rotation", cJSON_CreateFloatArray(q, 4));
		const float scale[3] = { s, s, s };
		cJSON_AddItemToObject(part, "scale", cJSON_CreateFloatArray(scale, 3));
		cJSON_AddItemToObject(part, "extensions", flight_extension("component", true));
		cJSON_AddItemToArray(new_nodes, part);
		cJSON_AddItemToArray(children, cJSON_CreateNumber(nodes->component_count + 1));
		nodes->nodes[n].component = nodes->component_count++;
	}
	free(in_scene);
	REQUIRE(nodes->component_count > 0, "the file's scene has no mesh");

	/* The images: the cook reads them by name, so each gets one, and its
	 * bytes are found again when the cook asks. */
	cJSON* image;
	cJSON_ArrayForEach(image, cJSON_GetObjectItemCaseSensitive(root, "images")) {
		const cJSON* view_index = cJSON_GetObjectItemCaseSensitive(image, "bufferView");
		const cJSON* view =
			cJSON_IsNumber(view_index) ? cJSON_GetArrayItem(buffer_views, view_index->valueint) : NULL;
		const cJSON* offset = cJSON_GetObjectItemCaseSensitive(view, "byteOffset");
		const cJSON* length = cJSON_GetObjectItemCaseSensitive(view, "byteLength");
		REQUIRE(images->count < 64, "the file has too many images");
		REQUIRE(view && cJSON_IsNumber(length), "an image of the file isn't in its binary part");
		if (!ok)
			break;
		const size_t at = cJSON_IsNumber(offset) ? (size_t)offset->valuedouble : 0;
		REQUIRE(at <= bin_size && (size_t)length->valuedouble <= bin_size - at,
				"an image of the file is cut short");
		if (!ok)
			break;
		char uri[40];
		snprintf(uri, sizeof uri, "plain_image_%d.png", images->count);
		images->images[images->count].offset = at;
		images->images[images->count].size   = (size_t)length->valuedouble;
		images->count++;
		cJSON_DeleteItemFromObjectCaseSensitive(image, "bufferView");
		cJSON_DeleteItemFromObjectCaseSensitive(image, "mimeType");
		cJSON_DeleteItemFromObjectCaseSensitive(image, "uri");
		cJSON_AddStringToObject(image, "uri", uri);
	}

	/* What the game's models don't have is dropped. */
	cJSON* material;
	cJSON_ArrayForEach(material, cJSON_GetObjectItemCaseSensitive(root, "materials")) {
		strip_extensions(material);
		cJSON_DeleteItemFromObjectCaseSensitive(material, "occlusionTexture");
	}
	cJSON* mesh;
	cJSON_ArrayForEach(mesh, cJSON_GetObjectItemCaseSensitive(root, "meshes")) {
		strip_extensions(mesh);
		cJSON_DeleteItemFromObjectCaseSensitive(mesh, "weights");
		cJSON* primitive;
		cJSON_ArrayForEach(primitive, cJSON_GetObjectItemCaseSensitive(mesh, "primitives")) {
			cJSON_DeleteItemFromObjectCaseSensitive(primitive, "targets");
			cJSON* attributes = cJSON_GetObjectItemCaseSensitive(primitive, "attributes");
			cJSON_DeleteItemFromObjectCaseSensitive(attributes, "JOINTS_0");
			cJSON_DeleteItemFromObjectCaseSensitive(attributes, "WEIGHTS_0");
		}
	}
	static const char* const dropped[] = {
		"animations",         "skins",     "cameras", "scenes", "scene", "extensionsUsed",
		"extensionsRequired", "extensions"
	};
	for (size_t i = 0; i < sizeof dropped / sizeof dropped[0]; i++)
		cJSON_DeleteItemFromObjectCaseSensitive(root, dropped[i]);
	cJSON_ReplaceItemInObjectCaseSensitive(root, "nodes", new_nodes);
	cJSON* new_scenes = cJSON_AddArrayToObject(root, "scenes");
	cJSON* new_scene  = cJSON_CreateObject();
	cJSON* scene_list = cJSON_AddArrayToObject(new_scene, "nodes");
	cJSON_AddItemToArray(scene_list, cJSON_CreateNumber(0));
	cJSON_AddItemToArray(new_scenes, new_scene);
	cJSON_AddNumberToObject(root, "scene", 0);
	cJSON* used = cJSON_AddArrayToObject(root, "extensionsUsed");
	cJSON_AddItemToArray(used, cJSON_CreateString("AERON_flight_model"));
#undef REQUIRE

	char*    json        = ok ? cJSON_PrintUnformatted(root) : NULL;
	uint8_t* cooked      = NULL;
	size_t   cooked_size = 0;
	cJSON_Delete(root);
	if (ok && !json) {
		ok      = false;
		problem = "out of memory";
	}
	if (ok) {
		AeronGltfCookOptions options;
		aeron_gltf_cook_default_options(&options);
		options.encoding               = AERON_GLTF_COOK_ENCODING_RGBA8;
		options.zstd_supercompress     = false;
		options.verbose                = false;
		images->bin                    = bin;
		images->bin_size               = bin_size;
		const AeronGltfCookInput input = {
			.json           = json,
			.json_size      = strlen(json),
			.label          = label,
			.bin            = bin,
			.bin_size       = bin_size,
			.images         = plain_image,
			.images_context = images,
		};
		if (!aeron_gltf_cook_glb(&input, &options, &cooked, &cooked_size)) {
			ok      = false;
			problem = "the file couldn't be prepared for the game";
		} else if (!Aeron_FlightModelBuildMemory(cooked, cooked_size, label, out)) {
			ok      = false;
			problem = "the prepared file couldn't be loaded";
		}
	}
	for (int i = 0; images && i < images->count; i++)
		free(images->images[i].rgba);
	free(images);
	free(rest);
	free(cooked);
	cJSON_free(json);
	if (!ok) {
		memset(nodes, 0, sizeof *nodes);
		return fail(error, error_size, problem ? problem : "the file can't be used");
	}
	return true;
}

int AeronPlainGltf_FindNode(const AeronPlainGltfNodes* nodes, const char* name) {
	for (int n = 0; nodes && name && n < nodes->count; n++)
		if (strcmp(nodes->nodes[n].name, name) == 0)
			return n;
	return -1;
}

void AeronPlainGltf_Pose(const AeronPlainGltfNodes* nodes, const AeronViewPose* const* poses,
						 const uint8_t* hidden, AeronSceneMeshTable* table) {
	/* The mesh holds (-x, -z, y) of the file's (x, y, z). */
	static const float to_mesh[12]   = { -1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0 };
	static const float from_mesh[12] = { -1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0 };
	memset(table, 0, sizeof *table);
	table->component_count = (uint32_t)nodes->component_count;
	for (int n = 0; n < nodes->count; n++) {
		const int row = nodes->nodes[n].component;
		if (row < 0)
			continue;
		/* The node's transform to the model's space now, then what it adds
		 * to the transform at rest, which the mesh already holds. */
		float now[12];
		int   shown = 1;
		memcpy(now, kIdentity, sizeof now);
		for (int up = n; up >= 0; up = nodes->nodes[up].parent) {
			float local[12];
			if (poses && poses[up])
				mat_from_trs(poses[up]->position_m, poses[up]->orientation, nodes->nodes[up].scale, local);
			else
				memcpy(local, nodes->nodes[up].local, sizeof local);
			mat_multiply(local, now, now);
			if (hidden && hidden[up])
				shown = 0;
		}
		float moved[12], in_mesh[12];
		mat_multiply(now, nodes->rest_inverse[n], moved);
		mat_multiply(to_mesh, moved, in_mesh);
		mat_multiply(in_mesh, from_mesh, in_mesh);
		memcpy(table->rows[row], in_mesh, sizeof in_mesh);
		table->visibility_packed[row >> 2][row & 3] = shown ? 1.0f : 0.0f;
	}
}
