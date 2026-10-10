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
 * normal / metallic_roughness / emissive), each a KTX2 texture array
 * embedded in the GLB BIN chunk and referenced via
 * KHR_texture_basisu. Each material's texture bindings then point at
 * a texture `tex_<channel>_p<page>_l<max_lod>` of the corresponding
 * atlas, naming the array layer and the deepest mip level to sample,
 * and carry a KHR_texture_transform that remaps material-local UVs
 * into the material's sub-rect of that layer. Materials binding the
 * same image share one sub-rect.
 *
 * Games then load the cooked .glb with Aeron_FlightModelBuildMemory — no PNG
 * decode, software atlas packing, or runtime mip generation.
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
	AERON_GLTF_COOK_ENCODING_BC    = 0,
	AERON_GLTF_COOK_ENCODING_RGBA8 = 1,
} AeronGltfCookEncoding;

typedef struct AeronGltfCookImageView {
	const uint8_t* rgba;
	int            width;
	int            height;
} AeronGltfCookImageView;

typedef bool (*AeronGltfCookImageProvider)(void* context, const struct cgltf_image* image,
										   AeronGltfCookImageView* out_view);
typedef bool (*AeronGltfCookConsumer)(void* context, const struct cgltf_data* cooked_data);

/* Cooker tunables. Pass NULL for all-defaults; partial structs are not
 * supported — caller fills every field. */
typedef struct AeronGltfCookOptions {
	/* Atlas page size (per axis, in texels, power of two ≥ 128), raised
	 * to the next power of two of a channel's largest source. A channel
	 * that fits one shared page is trimmed to its occupied bounds; any
	 * other channel is an array of square pages. Default: 2048. */
	int atlas_page_size;

	/* Largest source dimension. Larger sources are box-downscaled, aspect
	 * ratio kept, before packing. Default: 4096. */
	int max_texture_size;

	/* Gutter texels around each packed sub-rect, a power of two in
	 * [4, 32]. Footprints align to it, so a sub-rect samples mip levels up
	 * to log2 of it without bleeding. Default: 16. */
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
	/* Its level, 1 to 22. Default: 9 (KTX2_ZSTD_LEVEL_DEFAULT): 19 is
	 * about 14x slower for payloads about 6% smaller. */
	int zstd_level;

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
