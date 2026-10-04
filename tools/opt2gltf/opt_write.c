#include "opt_write.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	NODE_GROUP          = 0,
	NODE_FACE_DATA      = 1,
	NODE_MESH_VERTICES  = 3,
	NODE_NODE_REFERENCE = 7,
	NODE_VERTEX_NORMALS = 11,
	NODE_TEXTURE_COORDS = 13,
	NODE_TEXTURE        = 20,
	NODE_FACE_GROUPING  = 21,
	NODE_HARDPOINT      = 22,
	NODE_ROTATION_SCALE = 23,
	NODE_MESH_DESCRIPTOR = 25,
	NODE_TEXTURE_ALPHA  = 26,
	NODE_ENGINE_GLOW    = 28,
};

enum { NODE_HEADER_BYTES = 24, CONTAINER_BYTES = 14, TEXTURE_HEADER_BYTES = 24 };

typedef struct write_node {
	int32_t      type;
	char*        name;
	opt_wnode_t* children;
	uint32_t     child_count;
	int32_t      param1;
	uint8_t*     payload;
	uint32_t     payload_size;
	uint32_t     palette_offset; /* texture: palette position inside the payload */
	uint32_t     offset;         /* node header offset after layout */
	bool         placed;
} write_node;

struct opt_writer {
	opt_write_profile_t profile;
	write_node*         nodes;
	uint32_t            count;
	uint32_t            capacity;
	bool                failed;
};

/* ===== Little-endian encoding ======================================== */

static void put_u32(uint8_t* out, uint32_t value) {
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

static void put_f32(uint8_t* out, float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	put_u32(out, bits);
}

static uint8_t* put_vec3(uint8_t* out, const float value[3]) {
	for (int axis = 0; axis < 3; ++axis)
		put_f32(out + axis * 4, value[axis]);
	return out + 12;
}

/* ===== Node construction ============================================= */

opt_writer_t* opt_writer_create(opt_write_profile_t profile) {
	opt_writer_t* writer = calloc(1, sizeof *writer);
	if (writer)
		writer->profile = profile;
	return writer;
}

void opt_writer_free(opt_writer_t* writer) {
	if (!writer)
		return;
	for (uint32_t index = 0; index < writer->count; ++index) {
		free(writer->nodes[index].name);
		free(writer->nodes[index].children);
		free(writer->nodes[index].payload);
	}
	free(writer->nodes);
	free(writer);
}

/* Adds a node and returns its handle; `payload` (owned) may be NULL. */
static opt_wnode_t add_node(opt_writer_t* writer, int32_t type, const char* name, const opt_wnode_t* children,
							int child_count, int32_t param1, uint8_t* payload, uint32_t payload_size) {
	if (!writer || writer->failed || child_count < 0 || (child_count && !children))
		goto fail;
	if (writer->count == writer->capacity) {
		const uint32_t capacity = writer->capacity ? writer->capacity * 2 : 64;
		write_node*    grown    = realloc(writer->nodes, (size_t)capacity * sizeof *grown);
		if (!grown)
			goto fail;
		writer->nodes    = grown;
		writer->capacity = capacity;
	}
	write_node* node = &writer->nodes[writer->count];
	memset(node, 0, sizeof *node);
	node->type         = type;
	node->param1       = param1;
	node->payload      = payload;
	node->payload_size = payload_size;
	node->child_count  = (uint32_t)child_count;
	if (name) {
		node->name = malloc(strlen(name) + 1);
		if (!node->name)
			goto fail;
		strcpy(node->name, name);
	}
	if (child_count) {
		node->children = malloc((size_t)child_count * sizeof *node->children);
		if (!node->children) {
			free(node->name);
			goto fail;
		}
		memcpy(node->children, children, (size_t)child_count * sizeof *children);
	}
	return ++writer->count;

fail:
	free(payload);
	if (writer)
		writer->failed = true;
	return 0;
}

static opt_wnode_t node_failed(opt_writer_t* writer) {
	if (writer)
		writer->failed = true;
	return 0;
}

static uint8_t* payload_alloc(opt_writer_t* writer, size_t size) {
	uint8_t* payload = malloc(size ? size : 1);
	if (!payload && writer)
		writer->failed = true;
	return payload;
}

opt_wnode_t opt_w_group(opt_writer_t* writer, const opt_wnode_t* children, int count) {
	return add_node(writer, NODE_GROUP, NULL, children, count, 1, NULL, 0);
}

static opt_wnode_t vector_node(opt_writer_t* writer, int32_t type, const float* values, int count, int width) {
	if (count < 0 || (count && !values) || (size_t)count > (size_t)INT32_MAX / ((size_t)width * 4))
		return node_failed(writer);
	const size_t size    = (size_t)count * (size_t)width * 4;
	uint8_t*     payload = payload_alloc(writer, size);
	if (!payload)
		return 0;
	for (size_t index = 0; index < (size_t)count * (size_t)width; ++index)
		put_f32(payload + index * 4, values[index]);
	return add_node(writer, type, NULL, NULL, 0, count, payload, (uint32_t)size);
}

opt_wnode_t opt_w_vertices(opt_writer_t* writer, const float (*vertices)[3], int count) {
	return vector_node(writer, NODE_MESH_VERTICES, vertices ? vertices[0] : NULL, count, 3);
}

opt_wnode_t opt_w_vertex_normals(opt_writer_t* writer, const float (*normals)[3], int count) {
	return vector_node(writer, NODE_VERTEX_NORMALS, normals ? normals[0] : NULL, count, 3);
}

opt_wnode_t opt_w_texcoords(opt_writer_t* writer, const float (*uvs)[2], int count) {
	return vector_node(writer, NODE_TEXTURE_COORDS, uvs ? uvs[0] : NULL, count, 2);
}

opt_wnode_t opt_w_descriptor(opt_writer_t* writer, const AeronOptMeshDescriptor* descriptor) {
	uint8_t* payload = descriptor ? payload_alloc(writer, 72) : NULL;
	if (!payload)
		return node_failed(writer);
	uint8_t* cursor = payload;
	put_u32(cursor, (uint32_t)descriptor->mesh_type);
	put_u32(cursor + 4, (uint32_t)descriptor->explosion_type);
	cursor = put_vec3(cursor + 8, descriptor->span);
	cursor = put_vec3(cursor, descriptor->center);
	cursor = put_vec3(cursor, descriptor->box_min);
	cursor = put_vec3(cursor, descriptor->box_max);
	put_u32(cursor, (uint32_t)descriptor->target_id);
	put_vec3(cursor + 4, descriptor->target);
	return add_node(writer, NODE_MESH_DESCRIPTOR, NULL, NULL, 0, 1, payload, 72);
}

opt_wnode_t opt_w_rot_scale(opt_writer_t* writer, const AeronOptRotScale* rot_scale) {
	uint8_t* payload = rot_scale ? payload_alloc(writer, 48) : NULL;
	if (!payload)
		return node_failed(writer);
	uint8_t* cursor = put_vec3(payload, rot_scale->pivot);
	cursor          = put_vec3(cursor, rot_scale->rotation_axis);
	cursor          = put_vec3(cursor, rot_scale->direction_axis);
	put_vec3(cursor, rot_scale->up_axis);
	return add_node(writer, NODE_ROTATION_SCALE, NULL, NULL, 0, 1, payload, 48);
}

opt_wnode_t opt_w_hardpoint(opt_writer_t* writer, const AeronOptHardpoint* hardpoint) {
	uint8_t* payload = hardpoint ? payload_alloc(writer, 16) : NULL;
	if (!payload)
		return node_failed(writer);
	put_u32(payload, (uint32_t)hardpoint->type);
	put_vec3(payload + 4, hardpoint->position);
	return add_node(writer, NODE_HARDPOINT, NULL, NULL, 0, 1, payload, 16);
}

opt_wnode_t opt_w_engine_glow(opt_writer_t* writer, const AeronOptEngineGlow* glow) {
	uint8_t* payload = glow ? payload_alloc(writer, 72) : NULL;
	if (!payload)
		return node_failed(writer);
	put_u32(payload, (uint32_t)glow->is_disabled);
	put_u32(payload + 4, glow->core_color);
	put_u32(payload + 8, glow->outer_color);
	uint8_t* cursor = put_vec3(payload + 12, glow->dimensions);
	cursor          = put_vec3(cursor, glow->position);
	cursor          = put_vec3(cursor, glow->look);
	cursor          = put_vec3(cursor, glow->up);
	put_vec3(cursor, glow->right);
	return add_node(writer, NODE_ENGINE_GLOW, NULL, NULL, 0, 1, payload, 72);
}

opt_wnode_t opt_w_face_data_stub(opt_writer_t* writer) {
	uint8_t* payload = payload_alloc(writer, 4);
	if (!payload)
		return 0;
	put_u32(payload, 0);
	return add_node(writer, NODE_FACE_DATA, NULL, NULL, 0, 0, payload, 4);
}

opt_wnode_t opt_w_face_grouping(opt_writer_t* writer, const float* inverse_distances, const opt_wnode_t* lods,
								int count) {
	if (count <= 0 || !inverse_distances || !lods)
		return node_failed(writer);
	uint8_t* payload = payload_alloc(writer, (size_t)count * 4);
	if (!payload)
		return 0;
	for (int index = 0; index < count; ++index)
		put_f32(payload + index * 4, inverse_distances[index]);
	return add_node(writer, NODE_FACE_GROUPING, NULL, lods, count, count, payload, (uint32_t)count * 4);
}

opt_wnode_t opt_w_texture(opt_writer_t* writer, const char* name, int width, int height, const uint8_t* mip_chain,
						  int chain_bytes, const uint16_t* rgb565_16x256, uint32_t texture_id) {
	if (!writer || !name || width <= 0 || height <= 0 || width > 16384 || height > 16384 || !mip_chain ||
		chain_bytes < width * height || !rgb565_16x256)
		return node_failed(writer);
	/* V2 palettes prefix the RGB565 rows with a 4096-byte 8-bit shade table,
	 * left at the 0xCD fill the original tools wrote. */
	const uint32_t prefix  = writer->profile == OPT_WRITE_PROFILE_V2 ? 4096u : 0u;
	const uint32_t palette = prefix + 16u * 256u * 2u;
	const uint32_t size    = TEXTURE_HEADER_BYTES + (uint32_t)chain_bytes + palette;
	uint8_t*       payload = payload_alloc(writer, size);
	if (!payload)
		return 0;
	put_u32(payload, 0); /* palette address, set at finish */
	put_u32(payload + 4, 0);
	put_u32(payload + 8, (uint32_t)(width * height));
	put_u32(payload + 12, (uint32_t)chain_bytes);
	put_u32(payload + 16, (uint32_t)width);
	put_u32(payload + 20, (uint32_t)height);
	memcpy(payload + TEXTURE_HEADER_BYTES, mip_chain, (size_t)chain_bytes);
	uint8_t* block = payload + TEXTURE_HEADER_BYTES + chain_bytes;
	memset(block, 0xCD, prefix);
	for (uint32_t entry = 0; entry < 16u * 256u; ++entry) {
		block[prefix + entry * 2]     = (uint8_t)rgb565_16x256[entry];
		block[prefix + entry * 2 + 1] = (uint8_t)(rgb565_16x256[entry] >> 8);
	}
	const opt_wnode_t node =
		add_node(writer, NODE_TEXTURE, name, NULL, 0, (int32_t)texture_id, payload, size);
	if (node)
		writer->nodes[node - 1].palette_offset = TEXTURE_HEADER_BYTES + (uint32_t)chain_bytes;
	return node;
}

opt_wnode_t opt_w_node_ref(opt_writer_t* writer, const char* name) {
	if (!name)
		return node_failed(writer);
	const size_t length  = strlen(name) + 1;
	uint8_t*     payload = payload_alloc(writer, length);
	if (!payload)
		return 0;
	memcpy(payload, name, length);
	return add_node(writer, NODE_NODE_REFERENCE, NULL, NULL, 0, 0, payload, (uint32_t)length);
}

/* ===== Serialization ================================================= */

static bool fail(char* error, size_t error_size, const char* message) {
	if (error && error_size)
		snprintf(error, error_size, "%s", message);
	return false;
}

static uint64_t node_bytes(const write_node* node) {
	return NODE_HEADER_BYTES + (node->name ? strlen(node->name) + 1 : 0) + (uint64_t)node->child_count * 4 +
		   node->payload_size;
}

/* Assigns pre-order offsets: each node is followed by its unplaced children. */
static bool place(opt_writer_t* writer, opt_wnode_t handle, uint64_t* cursor, int depth) {
	if (handle == 0)
		return true;
	if (handle > writer->count || depth > 256)
		return false;
	write_node* node = &writer->nodes[handle - 1];
	if (node->placed)
		return true;
	node->placed = true;
	node->offset = (uint32_t)*cursor;
	*cursor += node_bytes(node);
	if (*cursor > INT32_MAX)
		return false;
	for (uint32_t child = 0; child < node->child_count; ++child)
		if (!place(writer, node->children[child], cursor, depth + 1))
			return false;
	return true;
}

static uint32_t address_of(const opt_writer_t* writer, opt_wnode_t handle) {
	return handle ? OPT_WRITE_BASE_ADDRESS + writer->nodes[handle - 1].offset : 0;
}

int opt_writer_finish(opt_writer_t* writer, const opt_wnode_t* roots, int root_count, uint8_t** payload,
					  int* payload_size, char* error, size_t error_size) {
	if (!writer || !payload || !payload_size || root_count <= 0 || !roots)
		return fail(error, error_size, "invalid OPT writer arguments");
	if (writer->failed)
		return fail(error, error_size, "OPT writer node construction failed");
	for (uint32_t index = 0; index < writer->count; ++index) {
		const int32_t type = writer->nodes[index].type;
		writer->nodes[index].placed = false;
		if (writer->profile == OPT_WRITE_PROFILE_V2 && (type == NODE_ENGINE_GLOW || type == NODE_TEXTURE_ALPHA))
			return fail(error, error_size, "node type not allowed by the V2 profile");
	}
	uint64_t cursor = CONTAINER_BYTES + (uint64_t)root_count * 4;
	for (int index = 0; index < root_count; ++index) {
		if (roots[index] == 0 || !place(writer, roots[index], &cursor, 0))
			return fail(error, error_size, "invalid OPT node tree or payload over INT32_MAX bytes");
	}
	uint8_t* out = calloc(1, (size_t)cursor);
	if (!out)
		return fail(error, error_size, "out of memory serializing OPT image");
	put_u32(out, OPT_WRITE_BASE_ADDRESS);
	out[4] = 2;
	out[5] = 0;
	put_u32(out + 6, (uint32_t)root_count);
	put_u32(out + 10, OPT_WRITE_BASE_ADDRESS + CONTAINER_BYTES);
	for (int index = 0; index < root_count; ++index)
		put_u32(out + CONTAINER_BYTES + (size_t)index * 4, address_of(writer, roots[index]));
	for (uint32_t index = 0; index < writer->count; ++index) {
		const write_node* node = &writer->nodes[index];
		if (!node->placed)
			continue;
		uint8_t*       at          = out + node->offset;
		const uint32_t name_bytes  = node->name ? (uint32_t)strlen(node->name) + 1 : 0;
		const uint32_t names_at    = node->offset + NODE_HEADER_BYTES;
		const uint32_t children_at = names_at + name_bytes;
		const uint32_t payload_at  = children_at + node->child_count * 4;
		put_u32(at, node->name ? OPT_WRITE_BASE_ADDRESS + names_at : 0);
		put_u32(at + 4, (uint32_t)node->type);
		put_u32(at + 8, node->child_count);
		put_u32(at + 12, node->child_count ? OPT_WRITE_BASE_ADDRESS + children_at : 0);
		put_u32(at + 16, (uint32_t)node->param1);
		put_u32(at + 20, node->payload_size ? OPT_WRITE_BASE_ADDRESS + payload_at : 0);
		if (name_bytes)
			memcpy(out + names_at, node->name, name_bytes);
		for (uint32_t child = 0; child < node->child_count; ++child) {
			if (node->children[child] > writer->count) {
				free(out);
				return fail(error, error_size, "invalid OPT child handle");
			}
			put_u32(out + children_at + child * 4, address_of(writer, node->children[child]));
		}
		if (node->payload_size)
			memcpy(out + payload_at, node->payload, node->payload_size);
		if (node->type == NODE_TEXTURE)
			put_u32(out + payload_at, OPT_WRITE_BASE_ADDRESS + payload_at + node->palette_offset);
	}
	*payload      = out;
	*payload_size = (int)cursor;
	return 1;
}

uint32_t opt_writer_payload_offset(const opt_writer_t* writer, opt_wnode_t handle) {
	if (!writer || handle == 0 || handle > writer->count)
		return UINT32_MAX;
	const write_node* node = &writer->nodes[handle - 1];
	if (!node->placed || !node->payload_size)
		return UINT32_MAX;
	return node->offset + NODE_HEADER_BYTES + (node->name ? (uint32_t)strlen(node->name) + 1 : 0) +
		   node->child_count * 4;
}
