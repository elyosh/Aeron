#include "opt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPT_V2_BASE_ADDRESS 0x10000000u
#define OPT_NODE_BYTES 24u

typedef struct OptImageBuilder {
    uint8_t *data;
    size_t size;
    size_t capacity;
    opt_error_t *error;
    int failed;
} OptImageBuilder;

typedef struct OptCanonicalMesh {
    opt_vec3_t *vertices;
    opt_vec3_t *normals;
    opt_vec2_t *uvs;
    int32_t *vertex_map;
    int32_t *normal_map;
    int32_t *uv_map;
    int32_t vertex_count;
    int32_t normal_count;
    int32_t uv_count;
} OptCanonicalMesh;

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t read_i32(const uint8_t *p) { return (int32_t)read_u32(p); }

static void write_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void upgrade_error(opt_error_t *error, const char *format, ...) {
    if (!error || error->msg[0]) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error->msg, sizeof error->msg, format, arguments);
    va_end(arguments);
}

static void builder_fail(OptImageBuilder *builder, const char *format, ...) {
    if (builder->failed) return;
    builder->failed = 1;
    if (!builder->error) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(builder->error->msg, sizeof builder->error->msg, format, arguments);
    va_end(arguments);
}

static size_t builder_reserve(OptImageBuilder *builder, size_t bytes, size_t alignment) {
    if (builder->failed) return 0;
    const size_t aligned = (builder->size + alignment - 1) & ~(alignment - 1);
    if (aligned > UINT32_MAX - OPT_V2_BASE_ADDRESS ||
        bytes > UINT32_MAX - OPT_V2_BASE_ADDRESS - aligned) {
        builder_fail(builder, "canonical OPT is too large");
        return 0;
    }
    const size_t needed = aligned + bytes;
    if (needed > builder->capacity) {
        size_t capacity = builder->capacity ? builder->capacity : 4096;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                builder_fail(builder, "canonical OPT allocation overflow");
                return 0;
            }
            capacity *= 2;
        }
        uint8_t *data = (uint8_t *)realloc(builder->data, capacity);
        if (!data) {
            builder_fail(builder, "out of memory building canonical OPT");
            return 0;
        }
        builder->data = data;
        builder->capacity = capacity;
    }
    memset(builder->data + builder->size, 0, needed - builder->size);
    builder->size = needed;
    return aligned;
}

static uint32_t builder_address(size_t offset) {
    return OPT_V2_BASE_ADDRESS + (uint32_t)offset;
}

static uint32_t builder_blob(OptImageBuilder *builder, const void *data,
                             size_t bytes, size_t alignment) {
    if (!bytes) return 0;
    const size_t offset = builder_reserve(builder, bytes, alignment);
    if (builder->failed) return 0;
    memcpy(builder->data + offset, data, bytes);
    return builder_address(offset);
}

static uint32_t builder_node(OptImageBuilder *builder, const char *name,
                             int32_t type, int32_t param1, uint32_t param2,
                             const uint32_t *children, int32_t child_count) {
    uint32_t name_address = 0;
    uint32_t children_address = 0;
    if (name && name[0])
        name_address = builder_blob(builder, name, strlen(name) + 1, 1);
    if (child_count > 0)
        children_address = builder_blob(builder, children,
            (size_t)child_count * sizeof *children, 4);
    const size_t offset = builder_reserve(builder, OPT_NODE_BYTES, 4);
    if (builder->failed) return 0;
    uint8_t *node = builder->data + offset;
    write_u32(node, name_address);
    write_u32(node + 4, (uint32_t)type);
    write_u32(node + 8, (uint32_t)child_count);
    write_u32(node + 12, children_address);
    write_u32(node + 16, (uint32_t)param1);
    write_u32(node + 20, param2);
    return builder_address(offset);
}

static int source_node_type_supported(int32_t type) {
    switch (type) {
        case OPT_NODE_GROUP:
        case OPT_NODE_FACE_DATA:
        case OPT_NODE_MESH_VERTICES:
        case OPT_NODE_NODE_REFERENCE:
        case OPT_NODE_VERTEX_NORMALS:
        case OPT_NODE_TEXTURE_COORDINATES:
        case OPT_NODE_TEXTURE:
        case OPT_NODE_FACE_GROUPING:
        case OPT_NODE_HARDPOINT:
        case OPT_NODE_ROTATION_SCALE:
        case OPT_NODE_NODE_SWITCH:
        case OPT_NODE_MESH_DESCRIPTOR:
            return 1;
        default:
            return 0;
    }
}

static int validate_v1_graph(const uint8_t *source, size_t size, opt_error_t *error) {
    if (size < 22 || read_i32(source) != -1 || read_i32(source + 4) != (int32_t)(size - 8)) {
        upgrade_error(error, "input is not a valid version-1 OPT image");
        return 0;
    }
    const uint32_t serialized_base = read_u32(source + 8);
    const int32_t root_count = read_i32(source + 14);
    const uint32_t root_address = read_u32(source + 18);
    const size_t max_nodes = (size - 8) / OPT_NODE_BYTES + 1;
    if (root_count < 0 || (size_t)root_count > max_nodes || root_address < serialized_base) {
        upgrade_error(error, "invalid version-1 OPT root table");
        return 0;
    }
    const size_t root_offset = 8u + (size_t)(root_address - serialized_base);
    if (root_offset > size || (size_t)root_count * 4 > size - root_offset) {
        upgrade_error(error, "version-1 OPT root table is out of bounds");
        return 0;
    }

    uint32_t *stack = (uint32_t *)malloc(max_nodes * sizeof *stack);
    uint32_t *visited = (uint32_t *)malloc(max_nodes * sizeof *visited);
    if (!stack || !visited) {
        free(stack);
        free(visited);
        upgrade_error(error, "out of memory validating version-1 OPT");
        return 0;
    }
    size_t stack_count = 0;
    size_t visited_count = 0;
    for (int32_t i = 0; i < root_count; ++i)
        stack[stack_count++] = read_u32(source + root_offset + (size_t)i * 4);

    while (stack_count) {
        const uint32_t address = stack[--stack_count];
        if (!address) continue;
        size_t i = 0;
        while (i < visited_count && visited[i] != address) ++i;
        if (i != visited_count) continue;
        if (visited_count == max_nodes || address < serialized_base) {
            upgrade_error(error, "invalid version-1 OPT node graph");
            goto fail;
        }
        const size_t offset = 8u + (size_t)(address - serialized_base);
        if (offset > size || OPT_NODE_BYTES > size - offset) {
            upgrade_error(error, "version-1 OPT node is out of bounds");
            goto fail;
        }
        visited[visited_count++] = address;
        const int32_t type = read_i32(source + offset + 4);
        const int32_t child_count = read_i32(source + offset + 8);
        const uint32_t child_address = read_u32(source + offset + 12);
        if (!source_node_type_supported(type)) {
            upgrade_error(error, "unsupported version-1 OPT node type %d", type);
            goto fail;
        }
        if (child_count < 0 || (size_t)child_count > max_nodes ||
            (child_count && child_address < serialized_base)) {
            upgrade_error(error, "invalid version-1 OPT child table");
            goto fail;
        }
        if (child_count) {
            const size_t table = 8u + (size_t)(child_address - serialized_base);
            if (table > size || (size_t)child_count * 4 > size - table ||
                stack_count + (size_t)child_count > max_nodes) {
                upgrade_error(error, "version-1 OPT child table is out of bounds");
                goto fail;
            }
            for (int32_t child = 0; child < child_count; ++child)
                stack[stack_count++] = read_u32(source + table + (size_t)child * 4);
        }
    }
    free(stack);
    free(visited);
    return 1;

fail:
    free(stack);
    free(visited);
    return 0;
}

static uint32_t build_data_node(OptImageBuilder *builder, int32_t type,
                                const void *data, int32_t count, size_t stride) {
    const uint32_t payload = count > 0
        ? builder_blob(builder, data, (size_t)count * stride, 4) : 0;
    return builder_node(builder, NULL, type, count, payload, NULL, 0);
}

static int vec3_equal(opt_vec3_t left, opt_vec3_t right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

static int vec2_equal(opt_vec2_t left, opt_vec2_t right) {
    return left.u == right.u && left.v == right.v;
}

static void canonical_mesh_free(OptCanonicalMesh *mesh) {
    free(mesh->vertices);
    free(mesh->normals);
    free(mesh->uvs);
    free(mesh->vertex_map);
    free(mesh->normal_map);
    free(mesh->uv_map);
    memset(mesh, 0, sizeof *mesh);
}

static int canonicalize_vec3(OptImageBuilder *builder, const opt_vec3_t *source,
                             int32_t source_count, int32_t extra_capacity,
                             opt_vec3_t **output, int32_t *output_count,
                             int32_t **index_map) {
    if (source_count < 0 || (source_count && !source)) {
        builder_fail(builder, "invalid version-1 OPT vector table");
        return 0;
    }
    const size_t capacity = (size_t)source_count + (size_t)extra_capacity;
    opt_vec3_t *values = capacity
        ? (opt_vec3_t *)malloc(capacity * sizeof *values) : NULL;
    int32_t *mapping = source_count
        ? (int32_t *)malloc((size_t)source_count * sizeof *mapping) : NULL;
    if ((capacity && !values) || (source_count && !mapping)) {
        free(values);
        free(mapping);
        builder_fail(builder, "out of memory canonicalizing OPT vectors");
        return 0;
    }
    int32_t count = 0;
    for (int32_t index = 0; index < source_count; ++index) {
        int32_t canonical = 0;
        while (canonical < count && !vec3_equal(values[canonical], source[index]))
            ++canonical;
        if (canonical == count)
            values[count++] = source[index];
        mapping[index] = canonical;
    }
    *output = values;
    *output_count = count;
    *index_map = mapping;
    return 1;
}

static int canonicalize_vec2(OptImageBuilder *builder, const opt_vec2_t *source,
                             int32_t source_count, opt_vec2_t **output,
                             int32_t *output_count, int32_t **index_map) {
    if (source_count < 0 || (source_count && !source)) {
        builder_fail(builder, "invalid version-1 OPT texture-coordinate table");
        return 0;
    }
    opt_vec2_t *values = source_count
        ? (opt_vec2_t *)malloc((size_t)source_count * sizeof *values) : NULL;
    int32_t *mapping = source_count
        ? (int32_t *)malloc((size_t)source_count * sizeof *mapping) : NULL;
    if (source_count && (!values || !mapping)) {
        free(values);
        free(mapping);
        builder_fail(builder, "out of memory canonicalizing OPT texture coordinates");
        return 0;
    }
    int32_t count = 0;
    for (int32_t index = 0; index < source_count; ++index) {
        int32_t canonical = 0;
        while (canonical < count && !vec2_equal(values[canonical], source[index]))
            ++canonical;
        if (canonical == count)
            values[count++] = source[index];
        mapping[index] = canonical;
    }
    *output = values;
    *output_count = count;
    *index_map = mapping;
    return 1;
}

static void append_vertex_extrema(OptCanonicalMesh *canonical) {
    if (canonical->vertex_count <= 0) return;
    opt_vec3_t minimum = canonical->vertices[0];
    opt_vec3_t maximum = canonical->vertices[0];
    for (int32_t index = 1; index < canonical->vertex_count; ++index) {
        const opt_vec3_t value = canonical->vertices[index];
        if (value.x < minimum.x) minimum.x = value.x;
        if (value.y < minimum.y) minimum.y = value.y;
        if (value.z < minimum.z) minimum.z = value.z;
        if (value.x > maximum.x) maximum.x = value.x;
        if (value.y > maximum.y) maximum.y = value.y;
        if (value.z > maximum.z) maximum.z = value.z;
    }
    const int32_t count = canonical->vertex_count;
    if (count < 2 || !vec3_equal(canonical->vertices[count - 2], minimum) ||
        !vec3_equal(canonical->vertices[count - 1], maximum)) {
        canonical->vertices[count] = minimum;
        canonical->vertices[count + 1] = maximum;
        canonical->vertex_count += 2;
    }
}

static int canonical_mesh_build(OptImageBuilder *builder, const opt_mesh_t *source,
                                OptCanonicalMesh *canonical) {
    memset(canonical, 0, sizeof *canonical);
    if (!canonicalize_vec3(builder, source->vertices, source->vertex_count, 2,
                           &canonical->vertices, &canonical->vertex_count,
                           &canonical->vertex_map) ||
        !canonicalize_vec3(builder, source->normals, source->normal_count, 0,
                           &canonical->normals, &canonical->normal_count,
                           &canonical->normal_map) ||
        !canonicalize_vec2(builder, source->uvs, source->uv_count,
                           &canonical->uvs, &canonical->uv_count,
                           &canonical->uv_map)) {
        canonical_mesh_free(canonical);
        return 0;
    }
    append_vertex_extrema(canonical);
    return 1;
}

static int32_t remap_index(OptImageBuilder *builder, int32_t index,
                           const int32_t *mapping, int32_t source_count,
                           const char *kind) {
    if (index == -1) return -1;
    if (index < 0 || index >= source_count || !mapping) {
        builder_fail(builder, "invalid version-1 OPT %s index %d", kind, index);
        return -1;
    }
    return mapping[index];
}

static uint32_t build_face_node(OptImageBuilder *builder,
                                const opt_face_group_t *group,
                                const opt_mesh_t *source_mesh,
                                const OptCanonicalMesh *canonical) {
    if (group->face_count < 0 || group->edges_count < 0 ||
        (group->face_count && !group->faces)) {
        builder_fail(builder, "invalid version-1 OPT face group");
        return 0;
    }
    const size_t face_count = (size_t)group->face_count;
    const size_t payload_size = 4 + face_count * (64 + 12 + 24);
    const size_t offset = builder_reserve(builder, payload_size, 4);
    if (builder->failed) return 0;
    uint8_t *payload = builder->data + offset;
    write_u32(payload, (uint32_t)group->edges_count);
    uint8_t *records = payload + 4;
    uint8_t *normals = records + face_count * 64;
    uint8_t *gradients = normals + face_count * 12;
    for (size_t face = 0; face < face_count; ++face) {
        const opt_face_t *input = &group->faces[face];
        int32_t indices[16];
        for (int corner = 0; corner < 4; ++corner)
            indices[corner] = remap_index(builder, input->verts[corner],
                canonical->vertex_map, source_mesh->vertex_count, "vertex");
        memcpy(indices + 4, input->edges, 4 * sizeof(int32_t));
        for (int corner = 0; corner < 4; ++corner) {
            indices[8 + corner] = remap_index(builder, input->uvs[corner],
                canonical->uv_map, source_mesh->uv_count, "texture coordinate");
            indices[12 + corner] = remap_index(builder, input->normals[corner],
                canonical->normal_map, source_mesh->normal_count, "normal");
        }
        if (builder->failed) return 0;
        memcpy(records + face * 64, indices, sizeof indices);
        memcpy(normals + face * 12, &input->face_normal, 12);
        memcpy(gradients + face * 24, &input->tex_direction, 12);
        memcpy(gradients + face * 24 + 12, &input->tex_magnitude, 12);
    }
    return builder_node(builder, NULL, OPT_NODE_FACE_DATA, group->face_count,
                        builder_address(offset), NULL, 0);
}

static uint32_t build_lod_node(OptImageBuilder *builder, const opt_lod_t *lod,
                               const uint32_t *texture_nodes, int32_t texture_count,
                               const opt_mesh_t *source_mesh,
                               const OptCanonicalMesh *canonical) {
    if (lod->group_count < 0 || (lod->group_count && !lod->groups)) {
        builder_fail(builder, "invalid version-1 OPT LOD");
        return 0;
    }
    const size_t capacity = (size_t)lod->group_count * 2;
    uint32_t *children = capacity ? (uint32_t *)malloc(capacity * sizeof *children) : NULL;
    if (capacity && !children) {
        builder_fail(builder, "out of memory building canonical OPT LOD");
        return 0;
    }
    int32_t child_count = 0;
    for (int32_t index = 0; index < lod->group_count && !builder->failed; ++index) {
        const opt_face_group_t *group = &lod->groups[index];
        if (group->state_count > 1) {
            uint32_t states[16];
            if (!group->state_textures || group->state_count > 16) {
                builder_fail(builder, "invalid version-1 OPT texture switch");
                break;
            }
            for (int32_t state = 0; state < group->state_count; ++state) {
                const int32_t texture = group->state_textures[state];
                if (texture < 0 || texture >= texture_count) {
                    builder_fail(builder, "invalid version-1 OPT texture index %d", texture);
                    break;
                }
                states[state] = texture_nodes[texture];
            }
            if (!builder->failed)
                children[child_count++] = builder_node(builder, NULL, OPT_NODE_NODE_SWITCH,
                    0, 0, states, group->state_count);
        } else if (group->texture_index >= 0) {
            if (group->texture_index >= texture_count) {
                builder_fail(builder, "invalid version-1 OPT texture index %d",
                             group->texture_index);
                break;
            }
            children[child_count++] = texture_nodes[group->texture_index];
        }
        children[child_count++] = build_face_node(builder, group, source_mesh, canonical);
    }
    const uint32_t result = builder->failed ? 0 : builder_node(builder, NULL,
        OPT_NODE_GROUP, 1, 0, children, child_count);
    free(children);
    return result;
}

static uint32_t build_face_group_node(OptImageBuilder *builder, const opt_mesh_t *mesh,
                                      const uint32_t *texture_nodes, int32_t texture_count,
                                      const OptCanonicalMesh *canonical) {
    if (mesh->lod_count < 0 || (mesh->lod_count && !mesh->lods)) {
        builder_fail(builder, "invalid version-1 OPT mesh LODs");
        return 0;
    }
    uint32_t *children = mesh->lod_count
        ? (uint32_t *)malloc((size_t)mesh->lod_count * sizeof *children) : NULL;
    float *thresholds = mesh->lod_count
        ? (float *)malloc((size_t)mesh->lod_count * sizeof *thresholds) : NULL;
    if (mesh->lod_count && (!children || !thresholds)) {
        free(children);
        free(thresholds);
        builder_fail(builder, "out of memory building canonical OPT face group");
        return 0;
    }
    for (int32_t lod = 0; lod < mesh->lod_count; ++lod) {
        thresholds[lod] = mesh->lods[lod].distance_threshold;
        children[lod] = build_lod_node(builder, &mesh->lods[lod],
                                       texture_nodes, texture_count, mesh, canonical);
    }
    const uint32_t payload = mesh->lod_count
        ? builder_blob(builder, thresholds,
            (size_t)mesh->lod_count * sizeof *thresholds, 4) : 0;
    const uint32_t result = builder->failed ? 0 : builder_node(builder, NULL,
        OPT_NODE_FACE_GROUPING, mesh->lod_count, payload, children, mesh->lod_count);
    free(children);
    free(thresholds);
    return result;
}

static uint32_t build_descriptor_node(OptImageBuilder *builder,
                                      const opt_mesh_descriptor_t *descriptor) {
    uint8_t payload[72];
    write_u32(payload, (uint32_t)descriptor->mesh_type);
    write_u32(payload + 4, (uint32_t)descriptor->explosion_type);
    memcpy(payload + 8, &descriptor->span, 12);
    memcpy(payload + 20, &descriptor->center, 12);
    memcpy(payload + 32, &descriptor->bbox_min, 12);
    memcpy(payload + 44, &descriptor->bbox_max, 12);
    write_u32(payload + 56, (uint32_t)descriptor->target_id);
    memcpy(payload + 60, &descriptor->target, 12);
    const uint32_t address = builder_blob(builder, payload, sizeof payload, 4);
    return builder_node(builder, NULL, OPT_NODE_MESH_DESCRIPTOR, 1, address, NULL, 0);
}

static uint32_t build_mesh_root(OptImageBuilder *builder, const opt_mesh_t *mesh,
                                const uint32_t *texture_nodes, int32_t texture_count) {
    if (mesh->hardpoint_count < 0 ||
        (mesh->hardpoint_count && !mesh->hardpoints) ||
        mesh->vertex_count < 0 || (mesh->vertex_count && !mesh->vertices) ||
        mesh->normal_count < 0 || (mesh->normal_count && !mesh->normals) ||
        mesh->uv_count < 0 || (mesh->uv_count && !mesh->uvs)) {
        builder_fail(builder, "invalid version-1 OPT mesh data");
        return 0;
    }
    OptCanonicalMesh canonical;
    if (!canonical_mesh_build(builder, mesh, &canonical)) return 0;
    const size_t child_capacity = 6u + (size_t)(mesh->hardpoint_count > 0
        ? mesh->hardpoint_count : 0);
    uint32_t *children = (uint32_t *)malloc(child_capacity * sizeof *children);
    if (!children) {
        canonical_mesh_free(&canonical);
        builder_fail(builder, "out of memory building canonical OPT mesh");
        return 0;
    }
    int32_t count = 0;
    if (mesh->has_descriptor)
        children[count++] = build_descriptor_node(builder, &mesh->descriptor);
    if (mesh->has_rotation_scale) {
        const uint32_t payload = builder_blob(builder, &mesh->rotation_scale, 48, 4);
        children[count++] = builder_node(builder, NULL, OPT_NODE_ROTATION_SCALE,
                                         1, payload, NULL, 0);
    }
    for (int32_t index = 0; index < mesh->hardpoint_count; ++index) {
        uint8_t payload[16];
        write_u32(payload, (uint32_t)mesh->hardpoints[index].type);
        memcpy(payload + 4, &mesh->hardpoints[index].pos, 12);
        const uint32_t address = builder_blob(builder, payload, sizeof payload, 4);
        children[count++] = builder_node(builder, NULL, OPT_NODE_HARDPOINT,
                                         1, address, NULL, 0);
    }
    children[count++] = build_data_node(builder, OPT_NODE_MESH_VERTICES,
        canonical.vertices, canonical.vertex_count, sizeof(opt_vec3_t));
    children[count++] = build_data_node(builder, OPT_NODE_VERTEX_NORMALS,
        canonical.normals, canonical.normal_count, sizeof(opt_vec3_t));
    children[count++] = build_data_node(builder, OPT_NODE_TEXTURE_COORDINATES,
        canonical.uvs, canonical.uv_count, sizeof(opt_vec2_t));
    children[count++] = build_face_group_node(builder, mesh, texture_nodes,
                                               texture_count, &canonical);
    const uint32_t result = builder->failed ? 0 : builder_node(builder, NULL,
        OPT_NODE_GROUP, 1, 0, children, count);
    free(children);
    canonical_mesh_free(&canonical);
    return result;
}

static int build_texture_nodes(OptImageBuilder *builder, const opt_file_t *model,
                               uint32_t *texture_nodes) {
    uint32_t *palettes = model->texture_count
        ? (uint32_t *)calloc((size_t)model->texture_count, sizeof *palettes) : NULL;
    if (model->texture_count && !palettes) {
        builder_fail(builder, "out of memory building canonical OPT palettes");
        return 0;
    }
    for (int32_t index = 0; index < model->texture_count; ++index) {
        const opt_texture_t *texture = &model->textures[index];
        if (!texture->native_shade_table) {
            builder_fail(builder, "texture '%s' has no native shade table", texture->name);
            break;
        }
        int32_t shared = 0;
        while (shared < index && memcmp(texture->native_shade_table,
            model->textures[shared].native_shade_table,
            OPT_NATIVE_SHADE_TABLE_BYTES) != 0) ++shared;
        palettes[index] = shared < index ? palettes[shared] : builder_blob(builder,
            texture->native_shade_table, OPT_NATIVE_SHADE_TABLE_BYTES, 4);
    }
    for (int32_t index = 0; index < model->texture_count && !builder->failed; ++index) {
        const opt_texture_t *texture = &model->textures[index];
        if (texture->width <= 0 || texture->height <= 0 ||
            texture->mip_chain_bytes <= 0 || !texture->pixels) {
            builder_fail(builder, "texture '%s' has invalid pixel data", texture->name);
            break;
        }
        const size_t payload_size = 24u + (size_t)texture->mip_chain_bytes;
        const size_t offset = builder_reserve(builder, payload_size, 4);
        if (builder->failed) break;
        uint8_t *payload = builder->data + offset;
        write_u32(payload, palettes[index]);
        write_u32(payload + 4, 0);
        write_u32(payload + 8, texture->mip_count > 1
            ? (uint32_t)(texture->width * texture->height) : 0);
        write_u32(payload + 12, (uint32_t)texture->mip_chain_bytes);
        write_u32(payload + 16, (uint32_t)texture->width);
        write_u32(payload + 20, (uint32_t)texture->height);
        memcpy(payload + 24, texture->pixels, (size_t)texture->mip_chain_bytes);
        texture_nodes[index] = builder_node(builder, texture->name, OPT_NODE_TEXTURE,
                                             1, builder_address(offset), NULL, 0);
    }
    free(palettes);
    return !builder->failed;
}

int opt_upgrade_v1_memory(const void *source, size_t source_size,
                          uint8_t **output, size_t *output_size,
                          opt_error_t *error) {
    if (output) *output = NULL;
    if (output_size) *output_size = 0;
    if (error) error->msg[0] = 0;
    if (!source || !output || !output_size) {
        upgrade_error(error, "invalid version-1 OPT conversion arguments");
        return 0;
    }
    if (!validate_v1_graph((const uint8_t *)source, source_size, error)) return 0;

    opt_file_t *model = opt_load_memory(source, source_size, error);
    if (!model) return 0;
    if (model->version != 1) {
        upgrade_error(error, "OPT parser returned version %d for version-1 input", model->version);
        opt_free(model);
        return 0;
    }

    OptImageBuilder builder = { .error = error };
    const size_t container = builder_reserve(&builder, 14, 1);
    const size_t root_table = builder_reserve(&builder,
        (size_t)model->mesh_count * sizeof(uint32_t), 4);
    uint32_t *texture_nodes = model->texture_count
        ? (uint32_t *)calloc((size_t)model->texture_count, sizeof *texture_nodes) : NULL;
    if (model->texture_count && !texture_nodes)
        builder_fail(&builder, "out of memory building canonical OPT textures");
    if (!builder.failed)
        build_texture_nodes(&builder, model, texture_nodes);
    for (int32_t mesh = 0; mesh < model->mesh_count && !builder.failed; ++mesh) {
        const uint32_t root = build_mesh_root(&builder, &model->meshes[mesh],
                                               texture_nodes, model->texture_count);
        write_u32(builder.data + root_table + (size_t)mesh * 4, root);
    }
    free(texture_nodes);

    if (!builder.failed) {
        write_u32(builder.data + container, OPT_V2_BASE_ADDRESS);
        write_u16(builder.data + container + 4, read_u16((const uint8_t *)source + 12));
        write_u32(builder.data + container + 6, (uint32_t)model->mesh_count);
        write_u32(builder.data + container + 10, builder_address(root_table));
        if (builder.size > INT32_MAX) {
            builder_fail(&builder, "canonical OPT payload exceeds the format limit");
        } else {
            uint8_t *image = (uint8_t *)malloc(builder.size + 8);
            if (!image) {
                builder_fail(&builder, "out of memory finalizing canonical OPT");
            } else {
                write_u32(image, (uint32_t)-2);
                write_u32(image + 4, (uint32_t)builder.size);
                memcpy(image + 8, builder.data, builder.size);
                *output = image;
                *output_size = builder.size + 8;
            }
        }
    }
    free(builder.data);
    opt_free(model);
    return !builder.failed;
}
