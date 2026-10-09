/*
 * aeron_gltf_cook — cooking orchestration.
 *
 * Read source .gltf via cgltf, decode every referenced PNG via imgbake's
 * png_read, build 4 per-channel atlases (KHR_texture_basisu /
 * KTX2-payload images embedded in the GLB BIN), repoint material
 * texture bindings to the atlas textures with KHR_texture_transform
 * sub-rects, write a single .glb via cgltf_write.
 */

#include "gltf_cook.h"
#include "channel_atlas.h"

#include "cgltf.h"
#include "cgltf_write.h"

#include "cJSON.h"
#include "ktx2_writer.h" /* write_ktx2_bc7_with_generated_mips_to_buffer */
#include "png_read.h"    /* png_read_rgba */

/* Per-channel block format. BC5 is restricted to the normal channel
 * (RG-only, no SRGB variant, linear-only); BC7 covers the other three
 * because they need either RGBA (base_color, emissive — both sRGB) or
 * three independent UNORM channels (metallic_roughness: AO/rough/metal
 * in R/G/B per glTF). The shader's normal sample uses
 *   max(ntex.z, sqrt(1 - x²-y²))
 * which collapses to the reconstructed Z when BC5 sets B=0 at sample
 * time — no shader change is needed when the channel flips formats. */
typedef enum { CH_FMT_BC7 = 0, CH_FMT_BC5 = 1 } ChannelFormat;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* cook_strdup(const char* source) {
	const size_t size = strlen(source) + 1;
	char*        copy = (char*)malloc(size);
	if (copy) {
		memcpy(copy, source, size);
	}
	return copy;
}

/* ---------- defaults ---------- */

void aeron_gltf_cook_default_options(AeronGltfCookOptions* out) {
	if (!out)
		return;
	out->atlas_page_size    = 2048;
	out->max_texture_size   = 4096;
	out->mip_min_subrect    = 16;
	out->encoding           = AERON_GLTF_COOK_ENCODING_BC;
	out->bc7_quality        = 1; /* KTX2_BC7_QUALITY_MED */
	out->zstd_supercompress = true;
	out->verbose            = false;
}

/* ---------- channel-binding accessor table ---------------------------
 *
 * The 4 PBR channels we atlas live in different cgltf_material slots.
 * This table abstracts the slot lookup so the cook loop can iterate
 * uniformly over channels. */

typedef struct ChannelBinding {
	int               channel;  /* AeronGltfCookChannel */
	const char*       name;     /* for diagnostics */
	Ktx2TransferFn    transfer; /* KTX2_TF_SRGB | LINEAR (BC7 only) */
	ChannelFormat     format;   /* BC7 | BC5 */
	Ktx2AlphaEncoding alpha_encoding;
} ChannelBinding;

static const ChannelBinding kChannels[AERON_GLTF_COOK_CHANNEL_COUNT] = {
	{ AERON_GLTF_COOK_CHANNEL_BASE_COLOR, "base_color", KTX2_TF_SRGB, CH_FMT_BC7, KTX2_ALPHA_STRAIGHT },
	{ AERON_GLTF_COOK_CHANNEL_NORMAL, "normal", KTX2_TF_LINEAR, CH_FMT_BC5, KTX2_ALPHA_PREMULTIPLIED },
	{ AERON_GLTF_COOK_CHANNEL_METALLIC_ROUGHNESS, "metallic_roughness", KTX2_TF_LINEAR, CH_FMT_BC7,
	  KTX2_ALPHA_PREMULTIPLIED },
	{ AERON_GLTF_COOK_CHANNEL_EMISSIVE, "emissive", KTX2_TF_SRGB, CH_FMT_BC7, KTX2_ALPHA_PREMULTIPLIED },
};

/* Per-channel KTX2 encode dispatch. Same in-buffer output shape for
 * both formats; the runtime KTX2 reader maps the vkFormat in the
 * header to the right SDL_GPU texture format. */
static bool encode_channel_ktx2(const ChannelBinding* cb, int w, int h, int layers, const uint8_t* rgba,
								AeronGltfCookEncoding encoding, Ktx2Bc7Quality bc7_q, bool zstd,
								int max_levels, uint8_t** out_buf, size_t* out_size) {
	if (encoding == AERON_GLTF_COOK_ENCODING_RGBA8) {
		return write_ktx2_rgba_with_generated_mips_to_buffer_limited_alpha(
			w, h, layers, rgba, cb->transfer, zstd, max_levels, cb->alpha_encoding, out_buf, out_size);
	}
	if (cb->format == CH_FMT_BC5) {
		return write_ktx2_bc5_with_generated_mips_to_buffer_limited(w, h, layers, rgba, zstd, max_levels,
																	out_buf, out_size);
	}
	return write_ktx2_bc7_with_generated_mips_to_buffer_limited_alpha(
		w, h, layers, rgba, bc7_q, cb->transfer, zstd, max_levels, cb->alpha_encoding, out_buf, out_size);
}

/* Return the texture_view for (material, channel), or NULL if not
 * authored or not bound. */
static cgltf_texture_view* material_channel_view(cgltf_material* m, int channel) {
	if (!m)
		return NULL;
	switch (channel) {
		case AERON_GLTF_COOK_CHANNEL_BASE_COLOR:
			if (!m->has_pbr_metallic_roughness)
				return NULL;
			return &m->pbr_metallic_roughness.base_color_texture;
		case AERON_GLTF_COOK_CHANNEL_METALLIC_ROUGHNESS:
			if (!m->has_pbr_metallic_roughness)
				return NULL;
			return &m->pbr_metallic_roughness.metallic_roughness_texture;
		case AERON_GLTF_COOK_CHANNEL_NORMAL:
			return &m->normal_texture;
		case AERON_GLTF_COOK_CHANNEL_EMISSIVE:
			return &m->emissive_texture;
		default:
			return NULL;
	}
}

/* Sources are shared per channel by image: the same cgltf_image, or the
 * same URI. */
static int find_channel_rect(const ChannelAtlas* ca, const cgltf_image* image) {
	for (int r = 0; r < ca->rect_count; r++) {
		const cgltf_image* other = (const cgltf_image*)ca->rects[r].key;
		if (other == image || (other->uri && image->uri && strcmp(other->uri, image->uri) == 0))
			return r;
	}
	return -1;
}

/* Only the four atlas channels are repointed; any other bound texture would
 * keep the index of a source texture the cooked graph no longer has, so it
 * is rejected. */
static bool validate_material_textures(const cgltf_data* data) {
	for (size_t mi = 0; mi < data->materials_count; mi++) {
		const cgltf_material* m    = &data->materials[mi];
		const char*           slot = NULL;
		if (m->occlusion_texture.texture)
			slot = "occlusionTexture";
		else if (m->has_specular && m->specular.specular_texture.texture)
			slot = "specularTexture";
		else if (m->has_specular && m->specular.specular_color_texture.texture)
			slot = "specularColorTexture";
		if (slot) {
			fprintf(stderr, "[aeron_gltf_cook] material '%s' has an unsupported %s\n",
					m->name ? m->name : "<unnamed>", slot);
			return false;
		}
	}
	return true;
}

/* The texture of one (channel, page, LOD limit), all on the channel's atlas
 * image. The runtime reads the page and LOD limit back from the name
 * `tex_<channel>_p<page>_l<max_lod>`. Appended to `textures` when new;
 * the caller sized the array for every rect. */
static cgltf_texture* atlas_texture(cgltf_texture* textures, size_t* count, cgltf_image* image,
									cgltf_sampler* sampler, int channel, const ChannelRect* rect) {
	char name[64];
	snprintf(name, sizeof name, "tex_%s_p%d_l%d", kChannels[channel].name, rect->page, rect->max_lod);
	for (size_t i = 0; i < *count; i++)
		if (strcmp(textures[i].name, name) == 0)
			return &textures[i];
	cgltf_texture* texture = &textures[*count];
	memset(texture, 0, sizeof *texture);
	texture->name = cook_strdup(name);
	if (!texture->name)
		return NULL;
	texture->sampler      = sampler;
	texture->has_basisu   = 1;
	texture->basisu_image = image;
	(*count)++;
	return texture;
}

/* ---------- source-image cache --------------------------------------
 *
 * Decode every (material, channel) PNG once up-front. Multiple
 * materials may share an image URI; we cache by URI so identical
 * source images don't pay for repeated stb_image decode. */

typedef struct CachedImage {
	char*    uri; /* owned */
	int      w, h;
	uint8_t* rgba; /* owned */
} CachedImage;

typedef struct ImageCache {
	CachedImage* items;
	int          count;
	int          capacity;
} ImageCache;

static void image_cache_init(ImageCache* c) { memset(c, 0, sizeof *c); }

static void image_cache_free(ImageCache* c) {
	if (!c)
		return;
	for (int i = 0; i < c->count; i++) {
		free(c->items[i].uri);
		free(c->items[i].rgba);
	}
	free(c->items);
	memset(c, 0, sizeof *c);
}

static const CachedImage* image_cache_get(const ImageCache* c, const char* uri) {
	for (int i = 0; i < c->count; i++)
		if (strcmp(c->items[i].uri, uri) == 0)
			return &c->items[i];
	return NULL;
}

static bool image_cache_add(ImageCache* c, const char* uri, int w, int h, uint8_t* rgba) {
	if (c->count == c->capacity) {
		int          cap   = c->capacity ? c->capacity * 2 : 8;
		CachedImage* grown = (CachedImage*)realloc(c->items, (size_t)cap * sizeof *grown);
		if (!grown)
			return false;
		c->items    = grown;
		c->capacity = cap;
	}
	CachedImage* it = &c->items[c->count++];
	it->uri         = cook_strdup(uri);
	it->w           = w;
	it->h           = h;
	it->rgba        = rgba;
	return it->uri != NULL;
}

/* Resolve `rel` against the directory containing `gltf_path` and write
 * the result into `out` (NUL-terminated). */
static void resolve_relative(char* out, size_t cap, const char* gltf_path, const char* rel) {
	const char* slash = strrchr(gltf_path, '/');
	if (!slash) {
		snprintf(out, cap, "%s", rel);
		return;
	}
	size_t dirlen = (size_t)(slash - gltf_path) + 1;
	if (dirlen >= cap)
		dirlen = cap - 1;
	memcpy(out, gltf_path, dirlen);
	out[dirlen] = '\0';
	strncat(out, rel, cap - dirlen - 1);
}

static const CachedImage* load_or_cache_image(ImageCache* cache, const char* gltf_path, const char* uri) {
	const CachedImage* hit = image_cache_get(cache, uri);
	if (hit)
		return hit;

	char abs[1024];
	resolve_relative(abs, sizeof abs, gltf_path, uri);

	int      w = 0, h = 0;
	uint8_t* rgba     = NULL;
	char     err[256] = { 0 };
	if (!read_png_rgba(abs, &rgba, &w, &h, err, sizeof err)) {
		fprintf(stderr, "[aeron_gltf_cook] PNG load failed: %s (%s)\n", abs, err);
		return NULL;
	}
	if (!image_cache_add(cache, uri, w, h, rgba)) {
		free(rgba);
		return NULL;
	}
	return &cache->items[cache->count - 1];
}

/* ---------- base-colour variant groups ------------------------------
 *
 * Base-colour images that list the variants using them (image extras
 * `aeronVariants`, written by opt2gltf for skin composites) are packed by
 * variant set into groups, each its own KTX2 block, so a runtime loads only
 * the groups of the variants it shows. Group pages have the core atlas's
 * size: they join its array after the core pages, in group order. The asset
 * extras `aeronVariantGroups` lists them:
 *   [{"bufferView": b, "layers": l, "variants": [v, ...]}, ...] */

typedef struct CookGroup {
	char*        variants; /* the aeronVariants array, printed */
	ChannelAtlas atlas;
	int          page_base; /* pages of the earlier groups */
	uint8_t*     ktx2;
	size_t       ktx2_size;
} CookGroup;

/* An object's extras as a JSON string (owned), or NULL without extras. */
static char* extras_json(const cgltf_data* data, const cgltf_extras* extras) {
	if (extras->data)
		return cook_strdup(extras->data);
	if (!data->json || extras->end_offset <= extras->start_offset)
		return NULL;
	const size_t length = (size_t)(extras->end_offset - extras->start_offset);
	char*        copy   = (char*)malloc(length + 1);
	if (copy) {
		memcpy(copy, data->json + extras->start_offset, length);
		copy[length] = '\0';
	}
	return copy;
}

/* The image's `aeronVariants` array, printed (owned), or NULL. */
static char* image_variants(const cgltf_data* data, const cgltf_image* image) {
	char* json = extras_json(data, &image->extras);
	if (!json)
		return NULL;
	cJSON* root = cJSON_Parse(json);
	free(json);
	const cJSON* variants = cJSON_GetObjectItemCaseSensitive(root, "aeronVariants");
	char*        printed  = cJSON_IsArray(variants) ? cJSON_PrintUnformatted(variants) : NULL;
	cJSON_Delete(root);
	return printed;
}

/* The asset extras with `aeronVariantGroups` added (owned), or NULL. */
static char* asset_extras_with_groups(const cgltf_data* data, const CookGroup* groups, int group_count,
									  size_t first_view) {
	char*  json = extras_json(data, &data->asset.extras);
	cJSON* root = json ? cJSON_Parse(json) : cJSON_CreateObject();
	free(json);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return NULL;
	}
	cJSON_DeleteItemFromObjectCaseSensitive(root, "aeronVariantGroups");
	cJSON* list = cJSON_AddArrayToObject(root, "aeronVariantGroups");
	for (int g = 0; list && g < group_count; g++) {
		cJSON* entry = cJSON_CreateObject();
		cJSON_AddItemToArray(list, entry);
		cJSON_AddNumberToObject(entry, "bufferView", (double)(first_view + (size_t)g));
		cJSON_AddNumberToObject(entry, "layers", groups[g].atlas.page_count);
		cJSON_AddItemToObject(entry, "variants", cJSON_Parse(groups[g].variants));
	}
	char* printed = list ? cJSON_PrintUnformatted(root) : NULL;
	cJSON_Delete(root);
	return printed;
}

/* The rect of material `mi`'s channel `c` binding with its final page (a
 * group's pages follow the core's); false when not bound. */
static bool binding_rect(const ChannelAtlas* atlases, const CookGroup* groups, const int* group_of,
						 int* const* rect_of, int c, size_t mi, ChannelRect* out) {
	if (rect_of[c][mi] < 0)
		return false;
	if (c == AERON_GLTF_COOK_CHANNEL_BASE_COLOR && group_of[mi] >= 0) {
		const CookGroup* group = &groups[group_of[mi]];
		*out                   = group->atlas.rects[rect_of[c][mi]];
		out->page += atlases[c].page_count + group->page_base;
		return true;
	}
	*out = atlases[c].rects[rect_of[c][mi]];
	return true;
}

/* ---------- cooking orchestration ----------------------------------- */

/* Round `v` up to multiple of 4 — KTX2 spec wants buffer_view byte
 * offsets aligned to the format's texel-block size (16 for BC7), and 4
 * is the minimum alignment cgltf_write enforces on GLB BIN chunks.
 * Picking 16 is the safer choice; KTX2 image headers don't depend on
 * the host-side embedding offset, so the 16-byte alignment of the GLB
 * BIN chunk doesn't propagate to the runtime — we just give it room. */
static size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

bool aeron_gltf_cook_data(cgltf_data* data, const char* source_label,
						  AeronGltfCookImageProvider image_provider, void* image_context,
						  AeronGltfCookConsumer consumer, void* consumer_context,
						  const AeronGltfCookOptions* opts_in) {
	if (!data || !consumer)
		return false;
	if (!source_label)
		source_label = "<memory glTF>";
	AeronGltfCookOptions opts;
	if (opts_in)
		opts = *opts_in;
	else
		aeron_gltf_cook_default_options(&opts);

	if (data->buffers_count != 1) {
		fprintf(stderr,
				"[aeron_gltf_cook] expected 1 source buffer, got %zu — opt2gltf "
				"should produce single-buffer .gltf files\n",
				data->buffers_count);
		return false;
	}

	ImageCache cache;
	image_cache_init(&cache);

	ChannelAtlas atlases[AERON_GLTF_COOK_CHANNEL_COUNT];
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++)
		channel_atlas_init(&atlases[c], c);

	/* Rect index of each (channel, material) binding, or -1. */
	int*          rect_of[AERON_GLTF_COOK_CHANNEL_COUNT]    = { 0 };
	uint8_t*      ktx2_bufs[AERON_GLTF_COOK_CHANNEL_COUNT]  = { 0 };
	size_t        ktx2_sizes[AERON_GLTF_COOK_CHANNEL_COUNT] = { 0 };
	cgltf_buffer* grown_src_buf                             = NULL;
	cgltf_size    source_base_size                          = 0;
	/* Base-colour variant groups, and each material's group or -1. */
	CookGroup* groups      = NULL;
	int        group_count = 0;
	int*       group_of    = NULL;

	bool ok  = validate_material_textures(data);
	group_of = (int*)malloc((data->materials_count ? data->materials_count : 1) * sizeof *group_of);
	ok       = ok && group_of != NULL;
	for (size_t mi = 0; mi < data->materials_count && ok; mi++)
		group_of[mi] = -1;
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT && ok; c++) {
		rect_of[c] = (int*)malloc((data->materials_count ? data->materials_count : 1) * sizeof *rect_of[c]);
		ok         = rect_of[c] != NULL;
		for (size_t mi = 0; mi < data->materials_count && ok; mi++)
			rect_of[c][mi] = -1;
	}

	/* ---- Stage A: decode + per-channel rect collection ---- */

	/* Track which (material, channel) pairs we plan to atlas. Materials
	 * that bind the same image share its rect. */
	for (size_t mi = 0; mi < data->materials_count && ok; mi++) {
		cgltf_material* m = &data->materials[mi];
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT && ok; c++) {
			cgltf_texture_view* tv = material_channel_view(m, c);
			if (!tv || !tv->texture || !tv->texture->image || !tv->texture->image->uri)
				continue;
			/* A base-colour image some variants use goes to their group. */
			ChannelAtlas* atlas = &atlases[c];
			if (c == AERON_GLTF_COOK_CHANNEL_BASE_COLOR) {
				char* variants = image_variants(data, tv->texture->image);
				if (variants) {
					int g = 0;
					while (g < group_count && strcmp(groups[g].variants, variants) != 0)
						g++;
					if (g == group_count) {
						CookGroup* grown =
							(CookGroup*)realloc(groups, (size_t)(group_count + 1) * sizeof *grown);
						if (!grown) {
							free(variants);
							ok = false;
							break;
						}
						groups = grown;
						memset(&groups[g], 0, sizeof groups[g]);
						channel_atlas_init(&groups[g].atlas, c);
						groups[g].variants = variants;
						group_count++;
					} else {
						free(variants);
					}
					group_of[mi] = g;
					atlas        = &groups[g].atlas;
				}
			}
			rect_of[c][mi] = find_channel_rect(atlas, tv->texture->image);
			if (rect_of[c][mi] >= 0)
				continue;
			AeronGltfCookImageView view = { 0 };
			if (image_provider) {
				if (!image_provider(image_context, tv->texture->image, &view)) {
					ok = false;
					break;
				}
			} else {
				const CachedImage* img = load_or_cache_image(&cache, source_label, tv->texture->image->uri);
				if (!img) {
					ok = false;
					break;
				}
				view.rgba   = img->rgba;
				view.width  = img->w;
				view.height = img->h;
			}
			rect_of[c][mi] = channel_atlas_add_rect(atlas, tv->texture->image, view.width, view.height,
													view.rgba, opts.max_texture_size);
			if (rect_of[c][mi] < 0) {
				ok = false;
				break;
			}
		}
	}

	if (!ok)
		goto cleanup;

	/* ---- Stage B: per-channel pack + materialize + KTX2 encode ---- */

	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT && ok; c++) {
		ChannelAtlas* ca = &atlases[c];
		if (!channel_atlas_pack(ca, opts.atlas_page_size, opts.mip_min_subrect)) {
			ok = false;
			break;
		}
		if (ca->rect_count == 0 && c == AERON_GLTF_COOK_CHANNEL_BASE_COLOR && group_count) {
			fprintf(stderr, "[aeron_gltf_cook] base_color: variant groups without a core atlas\n");
			ok = false;
			break;
		}
		if (ca->rect_count == 0) {
			/* No materials author this channel — emit a 4×4 stub
			 * KTX2 anyway so the runtime always finds 4 atlases.
			 * Materials with `scale==0` UV transform skip the
			 * sample in the FS. */
			uint8_t stub[4 * 4 * 4] = { 0 };
			if (!encode_channel_ktx2(&kChannels[c], 4, 4, 1, stub, opts.encoding,
									 (Ktx2Bc7Quality)opts.bc7_quality, opts.zstd_supercompress, ca->mip_count,
									 &ktx2_bufs[c], &ktx2_sizes[c])) {
				ok = false;
				break;
			}
			if (opts.verbose) {
				fprintf(stderr,
						"[aeron_gltf_cook] channel %s: stub 4×4 "
						"(%zu B)\n",
						kChannels[c].name, ktx2_sizes[c]);
			}
			continue;
		}
		/* Groups: pages of the core's size, and one mip count for all. */
		if (c == AERON_GLTF_COOK_CHANNEL_BASE_COLOR && group_count) {
			int pages = 0;
			for (int g = 0; g < group_count && ok; g++) {
				groups[g].atlas.fixed_width  = ca->width;
				groups[g].atlas.fixed_height = ca->height;
				groups[g].page_base          = pages;
				ok = channel_atlas_pack(&groups[g].atlas, ca->page, opts.mip_min_subrect);
				pages += groups[g].atlas.page_count;
				if (ok && groups[g].atlas.mip_count > ca->mip_count)
					ca->mip_count = groups[g].atlas.mip_count;
			}
			double used = 0.0;
			for (int g = 0; g < group_count && ok; g++) {
				ChannelAtlas* ga = &groups[g].atlas;
				ga->mip_count    = ca->mip_count;
				ok = channel_atlas_materialize(ga) &&
					 encode_channel_ktx2(&kChannels[c], ga->width, ga->height, ga->page_count, ga->rgba,
										 opts.encoding, (Ktx2Bc7Quality)opts.bc7_quality,
										 opts.zstd_supercompress, ga->mip_count, &groups[g].ktx2,
										 &groups[g].ktx2_size);
				for (int r = 0; r < ga->rect_count; r++) {
					const ChannelRect* rect = &ga->rects[r];
					used += rect->dedicated
								? (double)ga->width * ga->height
								: (double)(rect->src_w + 2 * ga->gutter) * (rect->src_h + 2 * ga->gutter);
				}
			}
			if (!ok)
				break;
			fprintf(
				stderr,
				"[aeron_gltf_cook] base_color: %d core page(s) of %d×%d, %d variant group(s) on %d page(s), "
				"%.0f%% of the group pages used\n",
				ca->page_count, ca->width, ca->height, group_count, pages,
				100.0 * used / ((double)pages * ca->width * ca->height));
		}
		if (!channel_atlas_materialize(ca)) {
			ok = false;
			break;
		}
		if (!encode_channel_ktx2(&kChannels[c], ca->width, ca->height, ca->page_count, ca->rgba,
								 opts.encoding, (Ktx2Bc7Quality)opts.bc7_quality, opts.zstd_supercompress,
								 ca->mip_count, &ktx2_bufs[c], &ktx2_sizes[c])) {
			ok = false;
			break;
		}
		if (opts.verbose) {
			fprintf(stderr,
					"[aeron_gltf_cook] channel %s: %d sub-rects → %d page(s) of %d×%d, "
					"%d level(s), %s KTX2 %zu B (zstd=%d)\n",
					kChannels[c].name, ca->rect_count, ca->page_count, ca->width, ca->height, ca->mip_count,
					opts.encoding == AERON_GLTF_COOK_ENCODING_RGBA8
						? "RGBA8"
						: (kChannels[c].format == CH_FMT_BC5 ? "BC5" : "BC7"),
					ktx2_sizes[c], (int)opts.zstd_supercompress);
		}
	}

	if (!ok)
		goto cleanup;

	/* ---- Stage C: extend the source buffer with the 4 KTX2 blobs ---- */

	cgltf_buffer* src_buf                              = &data->buffers[0];
	size_t        base_size                            = src_buf->size;
	grown_src_buf                                      = src_buf;
	source_base_size                                   = base_size;
	size_t ktx2_offsets[AERON_GLTF_COOK_CHANNEL_COUNT] = { 0 };
	size_t cursor                                      = base_size;
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
		cursor          = align_up(cursor, 16);
		ktx2_offsets[c] = cursor;
		cursor += ktx2_sizes[c];
	}
	size_t* group_offsets = (size_t*)calloc(group_count ? (size_t)group_count : 1, sizeof *group_offsets);
	if (!group_offsets) {
		ok = false;
		goto cleanup;
	}
	for (int g = 0; g < group_count; g++) {
		cursor           = align_up(cursor, 16);
		group_offsets[g] = cursor;
		cursor += groups[g].ktx2_size;
	}
	size_t new_buf_size = cursor;
	void*  new_buf_data = realloc(src_buf->data, new_buf_size);
	if (!new_buf_data) {
		free(group_offsets);
		ok = false;
		goto cleanup;
	}
	src_buf->data  = new_buf_data;
	src_buf->size  = new_buf_size;
	uint8_t* bytes = (uint8_t*)new_buf_data;
	/* Zero any padding so the GLB BIN chunk has deterministic bytes. */
	if (base_size < ktx2_offsets[0])
		memset(bytes + base_size, 0, ktx2_offsets[0] - base_size);
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
		size_t end = ktx2_offsets[c] + ktx2_sizes[c];
		if (c + 1 < AERON_GLTF_COOK_CHANNEL_COUNT && end < ktx2_offsets[c + 1])
			memset(bytes + end, 0, ktx2_offsets[c + 1] - end);
		memcpy(bytes + ktx2_offsets[c], ktx2_bufs[c], ktx2_sizes[c]);
	}
	size_t written =
		ktx2_offsets[AERON_GLTF_COOK_CHANNEL_COUNT - 1] + ktx2_sizes[AERON_GLTF_COOK_CHANNEL_COUNT - 1];
	for (int g = 0; g < group_count; g++) {
		memset(bytes + written, 0, group_offsets[g] - written);
		memcpy(bytes + group_offsets[g], groups[g].ktx2, groups[g].ktx2_size);
		written = group_offsets[g] + groups[g].ktx2_size;
	}

	/* ---- Stage D: build out-arrays (buffer_views / images / textures
	 *               / samplers / materials) and write GLB ---- */

	/* New buffer_views: original count + 4 atlas KTX2 views + one per
	 * variant group. */
	size_t             old_bv   = data->buffer_views_count;
	const size_t       new_bv_n = old_bv + AERON_GLTF_COOK_CHANNEL_COUNT + (size_t)group_count;
	cgltf_buffer_view* new_bvs  = (cgltf_buffer_view*)calloc(new_bv_n, sizeof *new_bvs);
	if (!new_bvs) {
		free(group_offsets);
		ok = false;
		goto cleanup;
	}
	for (size_t i = 0; i < old_bv; i++)
		new_bvs[i] = data->buffer_views[i];
	/* Re-point copied buffer_view->buffer at our (same) src_buf — the
	 * underlying pointer didn't change because realloc may have moved
	 * the .data block but `data->buffers[0]` itself is still at the
	 * same address. cgltf_write doesn't follow ->buffer beyond
	 * identifying which buffer index; it just emits `"buffer": N`. */
	cgltf_buffer_view* atlas_bvs[AERON_GLTF_COOK_CHANNEL_COUNT] = { 0 };
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
		cgltf_buffer_view* bv = &new_bvs[old_bv + c];
		memset(bv, 0, sizeof *bv);
		bv->buffer   = src_buf;
		bv->offset   = ktx2_offsets[c];
		bv->size     = ktx2_sizes[c];
		bv->stride   = 0;
		bv->type     = cgltf_buffer_view_type_invalid;
		atlas_bvs[c] = bv;
	}
	for (int g = 0; g < group_count; g++) {
		cgltf_buffer_view* bv = &new_bvs[old_bv + AERON_GLTF_COOK_CHANNEL_COUNT + (size_t)g];
		bv->buffer            = src_buf;
		bv->offset            = group_offsets[g];
		bv->size              = groups[g].ktx2_size;
		bv->type              = cgltf_buffer_view_type_invalid;
	}
	free(group_offsets);

	/* New images: 4 KTX2-backed atlas images. We OWN this array and
	 * the strings inside it. Source data->images entries are not
	 * carried over — every binding gets re-pointed below. */
	cgltf_image* new_imgs = (cgltf_image*)calloc(AERON_GLTF_COOK_CHANNEL_COUNT, sizeof *new_imgs);
	if (!new_imgs) {
		free(new_bvs);
		ok = false;
		goto cleanup;
	}
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
		char nm[64];
		snprintf(nm, sizeof nm, "atlas_%s", kChannels[c].name);
		new_imgs[c].name        = cook_strdup(nm);
		new_imgs[c].uri         = NULL;
		new_imgs[c].buffer_view = atlas_bvs[c];
		new_imgs[c].mime_type   = cook_strdup("image/ktx2");
	}

	/* New sampler: linear+linear with mipmaps, repeat-wrap. REPEAT is
	 * exact inside a sub-rect because the runtime wraps UVs into the rect
	 * and every footprint is filled by REPEAT. */
	cgltf_sampler* new_smp = (cgltf_sampler*)calloc(1, sizeof *new_smp);
	if (!new_smp) {
		free(new_bvs);
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
			free(new_imgs[c].name);
			free(new_imgs[c].mime_type);
		}
		free(new_imgs);
		ok = false;
		goto cleanup;
	}
	new_smp[0].name       = cook_strdup("atlas_sampler");
	new_smp[0].mag_filter = cgltf_filter_type_linear;
	new_smp[0].min_filter = cgltf_filter_type_linear_mipmap_linear;
	new_smp[0].wrap_s     = cgltf_wrap_mode_repeat;
	new_smp[0].wrap_t     = cgltf_wrap_mode_repeat;

	/* New textures: one per (channel, page, LOD limit) that a binding
	 * uses, with has_basisu=1 and basisu_image pointing at the channel's
	 * new_imgs entry. */
	size_t texture_capacity = 1;
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++)
		texture_capacity += (size_t)atlases[c].rect_count;
	for (int g = 0; g < group_count; g++)
		texture_capacity += (size_t)groups[g].atlas.rect_count;
	size_t         new_tex_count = 0;
	cgltf_texture* new_texs      = (cgltf_texture*)calloc(texture_capacity, sizeof *new_texs);
	bool           textures_ok   = new_texs != NULL;
	for (size_t mi = 0; mi < data->materials_count && textures_ok; mi++) {
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT && textures_ok; c++) {
			ChannelRect rect;
			if (binding_rect(atlases, groups, group_of, rect_of, c, mi, &rect))
				textures_ok =
					atlas_texture(new_texs, &new_tex_count, &new_imgs[c], &new_smp[0], c, &rect) != NULL;
		}
	}
	if (!textures_ok) {
		for (size_t i = 0; new_texs && i < new_tex_count; i++)
			free(new_texs[i].name);
		free(new_texs);
		free(new_bvs);
		free(new_smp[0].name);
		free(new_smp);
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
			free(new_imgs[c].name);
			free(new_imgs[c].mime_type);
		}
		free(new_imgs);
		ok = false;
		goto cleanup;
	}

	/* Preserve material bindings so the memory API can restore the caller's
	 * graph after the consumer returns. */
	const size_t        saved_view_count = data->materials_count * AERON_GLTF_COOK_CHANNEL_COUNT;
	cgltf_texture_view* saved_views =
		(cgltf_texture_view*)calloc(saved_view_count ? saved_view_count : 1, sizeof *saved_views);
	if (!saved_views) {
		free(new_bvs);
		free(new_smp[0].name);
		free(new_smp);
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
			free(new_imgs[c].name);
			free(new_imgs[c].mime_type);
		}
		for (size_t i = 0; i < new_tex_count; i++)
			free(new_texs[i].name);
		free(new_imgs);
		free(new_texs);
		ok = false;
		goto cleanup;
	}
	for (size_t mi = 0; mi < data->materials_count; mi++) {
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
			cgltf_texture_view* tv = material_channel_view(&data->materials[mi], c);
			if (tv)
				saved_views[mi * AERON_GLTF_COOK_CHANNEL_COUNT + (size_t)c] = *tv;
		}
	}

	/* Mutate each material's per-channel texture_view in place: clear
	 * the old `.texture` (and any transform), then set the new
	 * atlas texture + transform if the material authored this
	 * channel. */
	for (size_t mi = 0; mi < data->materials_count; mi++) {
		cgltf_material* m = &data->materials[mi];
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
			cgltf_texture_view* tv = material_channel_view(m, c);
			if (!tv)
				continue;
			ChannelRect        rect;
			const ChannelRect* match =
				binding_rect(atlases, groups, group_of, rect_of, c, mi, &rect) ? &rect : NULL;
			const ChannelAtlas* ca = c == AERON_GLTF_COOK_CHANNEL_BASE_COLOR && group_of[mi] >= 0
										 ? &groups[group_of[mi]].atlas
										 : &atlases[c];
			if (!match) {
				/* This material doesn't author this channel — clear
				 * the binding entirely so cgltf_write emits no
				 * texture reference. The FS falls back to the
				 * per-material factor. */
				tv->texture       = NULL;
				tv->has_transform = 0;
				memset(&tv->transform, 0, sizeof tv->transform);
				continue;
			}
			float off[2], scl[2];
			channel_atlas_rect_uv_transform(ca, match, off, scl);
			tv->texture       = atlas_texture(new_texs, &new_tex_count, &new_imgs[c], &new_smp[0], c, match);
			tv->texcoord      = 0;
			tv->has_transform = 1;
			tv->transform.offset[0]    = off[0];
			tv->transform.offset[1]    = off[1];
			tv->transform.scale[0]     = scl[0];
			tv->transform.scale[1]     = scl[1];
			tv->transform.rotation     = 0.0f;
			tv->transform.has_texcoord = 0;
			tv->transform.texcoord     = 0;
		}
	}

	/* Swap in the new arrays. The originals are released by
	 * cgltf_free(data) below — we leak nothing, the new arrays we
	 * own are freed at the cleanup label. */
	cgltf_buffer_view* orig_bvs        = data->buffer_views;
	cgltf_image*       orig_imgs       = data->images;
	cgltf_texture*     orig_texs       = data->textures;
	cgltf_sampler*     orig_smps       = data->samplers;
	size_t             orig_bvs_count  = data->buffer_views_count;
	size_t             orig_imgs_count = data->images_count;
	size_t             orig_texs_count = data->textures_count;
	size_t             orig_smps_count = data->samplers_count;
	(void)orig_bvs;
	(void)orig_imgs;
	(void)orig_texs;
	(void)orig_smps;
	(void)orig_bvs_count;
	(void)orig_imgs_count;
	(void)orig_texs_count;
	(void)orig_smps_count;

	data->buffer_views       = new_bvs;
	data->buffer_views_count = new_bv_n;
	data->images             = new_imgs;
	data->images_count       = AERON_GLTF_COOK_CHANNEL_COUNT;
	data->textures           = new_texs;
	data->textures_count     = new_tex_count;
	data->samplers           = new_smp;
	data->samplers_count     = 1;

	/* Existing material primitives' accessors -> buffer_view pointers
	 * still target slots in `orig_bvs`; we copied those entries into
	 * new_bvs at the same indices, so cgltf_write would emit the same
	 * indices either way. But the pointer comparison cgltf_write does
	 * uses pointer arithmetic against `data->buffer_views`, so we
	 * MUST repoint every accessor's ->buffer_view at the new array. */
	for (size_t ai = 0; ai < data->accessors_count; ai++) {
		cgltf_accessor* a = &data->accessors[ai];
		if (!a->buffer_view)
			continue;
		size_t idx = (size_t)(a->buffer_view - orig_bvs);
		if (idx < orig_bvs_count)
			a->buffer_view = &new_bvs[idx];
	}

	/* ---- Stage E: consume cooked graph ----
	 *
	 * cgltf_write_glb reads the BIN chunk payload from data->bin /
	 * data->bin_size (NOT from buffers[0]). And for GLB mode, the
	 * embedded buffer must have NO uri — cgltf_write would otherwise
	 * emit a "uri" property that contradicts the GLB BIN chunk
	 * convention. Save + clear both before write, restore after. */
	const void*  orig_bin      = data->bin;
	cgltf_size   orig_bin_size = data->bin_size;
	char*        orig_buf0_uri = src_buf->uri;
	cgltf_extras orig_extras   = data->asset.extras;
	char*        group_extras = group_count ? asset_extras_with_groups(data, groups, group_count,
																	   old_bv + AERON_GLTF_COOK_CHANNEL_COUNT)
											: NULL;
	data->bin                 = src_buf->data;
	data->bin_size            = src_buf->size;
	src_buf->uri              = NULL;
	if (group_extras)
		data->asset.extras.data = group_extras;

	ok = (!group_count || group_extras) && consumer(consumer_context, data);

	data->asset.extras = orig_extras;
	free(group_extras);

	data->bin      = orig_bin;
	data->bin_size = orig_bin_size;
	src_buf->uri   = orig_buf0_uri;
	src_buf->size  = base_size;

	/* Restore the original arrays so cgltf_free can release them
	 * without confusing our owned arrays. */
	data->buffer_views       = orig_bvs;
	data->buffer_views_count = orig_bvs_count;
	data->images             = orig_imgs;
	data->images_count       = orig_imgs_count;
	data->textures           = orig_texs;
	data->textures_count     = orig_texs_count;
	data->samplers           = orig_smps;
	data->samplers_count     = orig_smps_count;

	/* Repoint accessor->buffer_view back at the original array. */
	for (size_t ai = 0; ai < data->accessors_count; ai++) {
		cgltf_accessor* a = &data->accessors[ai];
		if (!a->buffer_view)
			continue;
		size_t idx = (size_t)(a->buffer_view - new_bvs);
		if (idx < old_bv)
			a->buffer_view = &orig_bvs[idx];
	}
	for (size_t mi = 0; mi < data->materials_count; mi++) {
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
			cgltf_texture_view* tv = material_channel_view(&data->materials[mi], c);
			if (tv)
				*tv = saved_views[mi * AERON_GLTF_COOK_CHANNEL_COUNT + (size_t)c];
		}
	}

	/* Free our owned arrays. */
	free(saved_views);
	free(new_bvs);
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
		free(new_imgs[c].name);
		free(new_imgs[c].mime_type);
	}
	for (size_t i = 0; i < new_tex_count; i++)
		free(new_texs[i].name);
	free(new_imgs);
	free(new_texs);
	free(new_smp[0].name);
	free(new_smp);

cleanup:
	if (grown_src_buf)
		grown_src_buf->size = source_base_size;
	for (int g = 0; g < group_count; g++) {
		free(groups[g].variants);
		free(groups[g].ktx2);
		channel_atlas_free(&groups[g].atlas);
	}
	free(groups);
	free(group_of);
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
		free(ktx2_bufs[c]);
		free(rect_of[c]);
		channel_atlas_free(&atlases[c]);
	}
	image_cache_free(&cache);
	return ok;
}

typedef struct FileCookConsumerContext {
	const char* out_path;
} FileCookConsumerContext;

static bool write_cooked_glb(void* context, const cgltf_data* data) {
	const FileCookConsumerContext* file    = (const FileCookConsumerContext*)context;
	cgltf_options                  options = { 0 };
	options.type                           = cgltf_file_type_glb;
	const cgltf_result result              = cgltf_write_file(&options, file->out_path, data);
	if (result != cgltf_result_success) {
		fprintf(stderr, "[aeron_gltf_cook] cgltf_write_file '%s' failed: %d\n", file->out_path, (int)result);
		return false;
	}
	return true;
}

bool aeron_gltf_cook(const char* in_path, const char* out_path, const AeronGltfCookOptions* opts) {
	if (!in_path || !out_path)
		return false;
	cgltf_options parse_options = { 0 };
	cgltf_data*   data          = NULL;
	cgltf_result  result        = cgltf_parse_file(&parse_options, in_path, &data);
	if (result != cgltf_result_success) {
		fprintf(stderr, "[aeron_gltf_cook] parse '%s' failed: %d\n", in_path, (int)result);
		return false;
	}
	result = cgltf_load_buffers(&parse_options, data, in_path);
	if (result != cgltf_result_success) {
		fprintf(stderr, "[aeron_gltf_cook] load_buffers '%s' failed: %d\n", in_path, (int)result);
		cgltf_free(data);
		return false;
	}
	FileCookConsumerContext context = { out_path };
	const bool succeeded = aeron_gltf_cook_data(data, in_path, NULL, NULL, write_cooked_glb, &context, opts);
	cgltf_free(data);
	return succeeded;
}
