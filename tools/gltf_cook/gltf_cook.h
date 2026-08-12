/*
 * aeron_gltf_cook — cook an artist-authored .gltf into Aeron's runtime-ready
 * .glb dialect.
 *
 * Input: a .gltf produced by opt2gltf and (optionally) edited by an
 * artist. Materials carry external PNG textures via image.uri; the
 * file may use extensions such as KHR_materials_variants and arbitrary
 * application-specific extras; the cooker preserves both unchanged.
 *
 * Output: a single .glb file that consolidates every material's
 * textures into 4 per-channel atlases (base_color /
 * normal / metallic_roughness / emissive), each shipped as a KTX2 payload
 * embedded in the GLB BIN chunk and referenced via
 * KHR_texture_basisu. Each material's texture bindings then point at
 * the corresponding atlas and carry a KHR_texture_transform that
 * remaps material-local UVs into the material's sub-rect of the
 * atlas.
 *
 * Aeron_FlightModelBuild then reads only the cooked .glb — no PNG decode,
 * software atlas packing, or runtime mip generation.
 */
#ifndef AERON_GLTF_COOK_H
#define AERON_GLTF_COOK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cgltf_data;
struct cgltf_image;

typedef enum AeronGltfCookEncoding {
	AERON_GLTF_COOK_ENCODING_BC = 0,
	AERON_GLTF_COOK_ENCODING_RGBA8 = 1,
} AeronGltfCookEncoding;

typedef struct AeronGltfCookImageView {
	const uint8_t* rgba;
	int width;
	int height;
} AeronGltfCookImageView;

typedef bool (*AeronGltfCookImageProvider)(void* context, const struct cgltf_image* image,
										   AeronGltfCookImageView* out_view);
typedef bool (*AeronGltfCookConsumer)(void* context, const struct cgltf_data* cooked_data);

/* Cooker tunables. Pass NULL for all-defaults; partial structs are not
 * supported — caller fills every field. */
typedef struct AeronGltfCookOptions {
	/* Max atlas dimension (per axis, in texels). The packer picks the
	 * smallest power-of-2 square that fits; values above this fail the
	 * cook with a clear error. Default: 2048. */
	int max_atlas_size;

	/* Smallest sub-rect dimension at the finest mip. Used to cap the
	 * generated mip count so cross-sub-rect bleed at distant LODs
	 * stays bounded. Default: 4 (matches BC7 4×4 block). */
	int mip_min_subrect;
	/* BC5/BC7 for shipped GLBs, or uncompressed RGBA8 for latency-sensitive
	 * in-memory conversion. Both modes generate mip chains. */
	AeronGltfCookEncoding encoding;

	/* BC7 encoder preset. 0 = fast, 1 = medium, 2 = uber. Default: 1
	 * (medium) — seconds per atlas, near-lossless. */
	int bc7_quality;

	/* Zstd supercompression on each KTX2 payload. Default: true.
	 * Disable when a downstream consumer rejects supercompressed
	 * KTX2. */
	bool zstd_supercompress;

	/* Verbose cook progress on stderr. */
	bool verbose;
} AeronGltfCookOptions;

void aeron_gltf_cook_default_options(AeronGltfCookOptions* out);

/* Cook `in_path` (.gltf) into `out_path` (.glb). Returns true on
 * success, false on any error (diagnostic on stderr). */
bool aeron_gltf_cook(const char* in_path, const char* out_path, const AeronGltfCookOptions* opts);

/* Apply the same cook transformation to an already-loaded graph, invoke the
 * consumer while the cooked graph is valid, then restore the source graph. */
bool aeron_gltf_cook_data(struct cgltf_data* data, const char* source_label,
						  AeronGltfCookImageProvider image_provider, void* image_context,
						  AeronGltfCookConsumer consumer, void* consumer_context,
						  const AeronGltfCookOptions* opts);

#ifdef __cplusplus
}
#endif

#endif
