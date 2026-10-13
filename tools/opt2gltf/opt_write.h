#ifndef OPT_WRITE_H
#define OPT_WRITE_H

/*
 * OPT image writer.
 *
 * Builds a node tree in memory and serializes it once, in the layout of the
 * original OPT files: the payload that follows the file's -version/size
 * header, with a 14-byte container (base address, reserved u16 = 2, root
 * count, root table address), the root table at offset 14, then every node
 * written top-down in pre-order and packed without alignment:
 *
 *   24-byte header | name | child table | payload | children...
 *
 * Addresses are 32-bit, absolute from base 0x10000000; 0 is NULL. Every value
 * is written explicitly as little-endian. Texture nodes keep the exact
 * header | name | child table | texture header | pixels | inline palette span
 * that game texture processing deletes or copies.
 *
 * A node may be listed as a child more than once; it is written at its first
 * pre-order position and referenced by address elsewhere.
 */

#include <stddef.h>
#include <stdint.h>

#include "aeron/asset/opt_payload.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPT_WRITE_BASE_ADDRESS 0x10000000u

typedef enum opt_write_profile {
	OPT_WRITE_PROFILE_V5, /* version 5; 8192-byte RGB565 palettes; ENGINEGLOW and TEXALPHA allowed */
	OPT_WRITE_PROFILE_V2, /* version 2; 12288-byte palettes; node types 0..0x19 only */
} opt_write_profile_t;

typedef struct opt_writer opt_writer_t;
typedef uint32_t          opt_wnode_t; /* writer-local handle; 0 = NULL child slot */

opt_writer_t* opt_writer_create(opt_write_profile_t profile);
void          opt_writer_free(opt_writer_t* writer);

/* Node constructors return 0 on allocation failure; finish then fails. */
opt_wnode_t opt_w_group(opt_writer_t* writer, const opt_wnode_t* children, int count);
opt_wnode_t opt_w_vertices(opt_writer_t* writer, const float (*vertices)[3], int count);
opt_wnode_t opt_w_vertex_normals(opt_writer_t* writer, const float (*normals)[3], int count);
opt_wnode_t opt_w_texcoords(opt_writer_t* writer, const float (*uvs)[2], int count);
opt_wnode_t opt_w_descriptor(opt_writer_t* writer, const AeronOptMeshDescriptor* descriptor);
opt_wnode_t opt_w_rot_scale(opt_writer_t* writer, const AeronOptRotScale* rot_scale);
opt_wnode_t opt_w_hardpoint(opt_writer_t* writer, const AeronOptHardpoint* hardpoint);
opt_wnode_t opt_w_engine_glow(opt_writer_t* writer, const AeronOptEngineGlow* glow); /* V5 only */
/* FaceData with no faces: param1 = 0, payload = int32 edgeCount 0. */
opt_wnode_t opt_w_face_data_stub(opt_writer_t* writer);
/* FaceGrouping: one inverse LOD distance per child, LOD 0 first. */
opt_wnode_t opt_w_face_grouping(opt_writer_t* writer, const float* inverse_distances, const opt_wnode_t* lods,
								int count);
/* Palettized texture: `mip_chain` holds chain_bytes of 8-bit indices, base
 * level first; `rgb565_16x256` holds 16 shade rows of 256 RGB565 colours,
 * written as the profile's inline palette block. */
opt_wnode_t opt_w_texture(opt_writer_t* writer, const char* name, int width, int height,
						  const uint8_t* mip_chain, int chain_bytes, const uint16_t* rgb565_16x256,
						  uint32_t texture_id);
opt_wnode_t opt_w_node_ref(opt_writer_t* writer, const char* name);

/* Serializes the tree. On success the caller frees *payload with free().
 * Fails on an earlier allocation failure, an invalid handle, a node type the
 * profile does not allow, or a payload larger than INT32_MAX bytes. */
int opt_writer_finish(opt_writer_t* writer, const opt_wnode_t* roots, int root_count, uint8_t** payload,
					  int* payload_size, char* error, size_t error_size);

/* Payload offset (address - OPT_WRITE_BASE_ADDRESS) of a node's payload, valid
 * after a successful finish; UINT32_MAX for an unknown node or one without a
 * payload. */
uint32_t opt_writer_payload_offset(const opt_writer_t* writer, opt_wnode_t node);

#ifdef __cplusplus
}
#endif

#endif
