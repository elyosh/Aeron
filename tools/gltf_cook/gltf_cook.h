/*
 * aeron_gltf_cook — cook an artist-authored .gltf into Aeron's runtime-ready
 * .glb dialect.
 *
 * Input: a .gltf produced by opt2gltf or another tool, and (optionally)
 * edited by an artist. Materials carry external PNG textures via
 * image.uri; the file may use extensions such as KHR_materials_variants,
 * which the cooker preserves, and AERON_flight_model on nodes and
 * materials (tools/extensions/AERON_flight_model), which it preserves (the
 * aeron_gltf_cook tool checks the materials' first, animations.h).
 *
 * Output: a single .glb file that consolidates every material's
 * textures into per-channel atlases (base_color / normal /
 * metallic_roughness / emissive): KTX2 texture arrays embedded in the GLB
 * BIN chunk, one image per channel and set of variants using its pages (the
 * core atlas, the pages only some variants use apart). Each material's
 * texture bindings then point at a texture whose AERON_flight_model object
 * names its image, page and deepest mip level to sample, and carry a
 * KHR_texture_transform that remaps material-local UVs into the material's
 * sub-rect of that page. Materials binding the same image share one
 * sub-rect. A channel can instead give each of its images a page of its
 * own, with every mip level, when that takes little more memory
 * (AeronGltfCookOptions.separate_pages_budget). The output lists
 * AERON_flight_model in extensionsRequired.
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
/* Optional: an image's size without its pixels, and the end of the
 * cooker's use of the pixels that the provider gave. */
typedef bool (*AeronGltfCookImageSize)(void* context, const struct cgltf_image* image, int* width,
									   int* height);
typedef void (*AeronGltfCookImageRelease)(void* context, const struct cgltf_image* image);
typedef bool (*AeronGltfCookConsumer)(void* context, const struct cgltf_data* cooked_data);
/* The pixels of the image with URI `uri`. They stay valid until the cook
 * returns. */
typedef bool (*AeronGltfCookUriImageSource)(void* context, const char* uri, AeronGltfCookImageView* out_view);

/* A glTF to cook: its JSON or GLB bytes, and where its .bin and images come
 * from. Without `bin`, buffer 0 is loaded as the file names it, next to
 * `label`. Without `images`, the images are the PNG files next to `label`. */
typedef struct AeronGltfCookInput {
	const char*                 json;
	size_t                      json_size;
	const char*                 label; /* the .gltf path, also used in messages */
	const void*                 bin;   /* the bytes of buffer 0, or NULL */
	size_t                      bin_size;
	AeronGltfCookUriImageSource images;
	void*                       images_context;
} AeronGltfCookInput;

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
	/* One page per image. A channel (its core atlas and variant groups) gives
	 * each image its own page, resampled to fill it, with no gutter, when
	 * that takes at most `separate_pages_budget` more texels at level 0 than
	 * packing, and at most `separate_pages_max_layers` layers. Its images
	 * then sample every mip level, so they don't alias when small on screen.
	 * A negative budget turns this off, except for a channel with one image
	 * per atlas. Defaults: 1048576 texels and 256 layers (the number Vulkan
	 * guarantees). */
	int64_t separate_pages_budget;
	int     separate_pages_max_layers;
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

/* Cooks `input`, expanding its skin layers (skin_layers.h), and passes the
 * cooked graph to `consumer`. aeron_gltf_cook is this function for a file. */
bool aeron_gltf_cook_input(const AeronGltfCookInput* input, AeronGltfCookConsumer consumer,
						   void* consumer_context, const AeronGltfCookOptions* opts);

/* Cooks `input` into GLB bytes, as aeron_gltf_cook writes them. The caller
 * frees `*glb`. */
bool aeron_gltf_cook_glb(const AeronGltfCookInput* input, const AeronGltfCookOptions* opts, uint8_t** glb,
						 size_t* glb_size);

/* Apply the same cook transformation to an already-loaded graph, invoke the
 * consumer while the cooked graph is valid, then restore the source graph. */
bool aeron_gltf_cook_data(struct cgltf_data* data, const char* source_label,
						  AeronGltfCookImageProvider image_provider, void* image_context,
						  AeronGltfCookConsumer consumer, void* consumer_context,
						  const AeronGltfCookOptions* opts);

/* aeron_gltf_cook_data, asking the provider for each image's pixels only
 * when its atlas page is built, and releasing them right after. With
 * `image_size`, the cooker holds one page's sources at a time instead of
 * every image of the model. `image_release` may be NULL. */
bool aeron_gltf_cook_data_lazy(struct cgltf_data* data, const char* source_label,
							   AeronGltfCookImageProvider image_provider, AeronGltfCookImageSize image_size,
							   AeronGltfCookImageRelease image_release, void* image_context,
							   AeronGltfCookConsumer consumer, void* consumer_context,
							   const AeronGltfCookOptions* opts);

#ifdef __cplusplus
}
#endif

#endif
