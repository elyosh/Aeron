/*
 * aeron_gltf_cook — cooking orchestration.
 *
 * Read source .gltf via cgltf, decode every referenced PNG via imgbake's
 * png_read, build the per-channel atlases (KTX2 images embedded in the GLB
 * BIN), repoint material texture bindings to the atlas textures, which name
 * their image and page through AERON_flight_model, with
 * KHR_texture_transform sub-rects, write a single .glb via cgltf_write.
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
	out->zstd_level         = KTX2_ZSTD_LEVEL_DEFAULT;
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

/* The texture of one (image, page, LOD limit): its AERON_flight_model
 * object names them (tools/extensions/AERON_flight_model, "Textures and
 * images"); it has no source of its own. Appended to `textures` when new;
 * the caller sized the array for every rect. */
static cgltf_texture* atlas_texture(cgltf_texture* textures, size_t* count, size_t image, cgltf_sampler* sampler,
									const ChannelRect* rect) {
	char object[96];
	if (rect->max_lod < CHANNEL_ATLAS_UNCLAMPED_LOD)
		snprintf(object, sizeof object, "{\"source\":%zu,\"layer\":%d,\"maxLod\":%d}", image, rect->page,
				 rect->max_lod);
	else
		snprintf(object, sizeof object, "{\"source\":%zu,\"layer\":%d}", image, rect->page);
	for (size_t i = 0; i < *count; i++)
		if (strcmp(textures[i].extensions[0].data, object) == 0)
			return &textures[i];
	cgltf_texture* texture = &textures[*count];
	memset(texture, 0, sizeof *texture);
	texture->extensions = (cgltf_extension*)calloc(1, sizeof *texture->extensions);
	if (!texture->extensions)
		return NULL;
	texture->extensions_count   = 1;
	texture->extensions[0].name = cook_strdup("AERON_flight_model");
	texture->extensions[0].data = cook_strdup(object);
	texture->sampler            = sampler;
	(*count)++;
	return texture->extensions[0].name && texture->extensions[0].data ? texture : NULL;
}

static void free_atlas_textures(cgltf_texture* textures, size_t count) {
	for (size_t i = 0; textures && i < count; i++) {
		if (textures[i].extensions) {
			free(textures[i].extensions[0].name);
			free(textures[i].extensions[0].data);
			free(textures[i].extensions);
		}
	}
	free(textures);
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

/* ---------- variant groups ----------------------------------------
 *
 * Images only some variants use are packed by channel and variant set into
 * groups, each its own KTX2 block, so a runtime loads only the groups of
 * the variants it shows. An image is core when a primitive's own material
 * uses it, or it is in a channel no own material uses; otherwise it belongs
 * to the variants whose mapped materials use it. A group's pages have its
 * channel's core atlas size, and its own image after the core images: a
 * runtime finds its variants through the materials that reference it. */

typedef struct CookGroup {
	int          channel;  /* AeronGltfCookChannel */
	char*        variants; /* its variant indices, printed as a JSON array */
	ChannelAtlas atlas;
	uint8_t*     ktx2;
	size_t       ktx2_size;
} CookGroup;

/* The rect of material `mi`'s channel `c` binding, its page one of its
 * image's (the core's or its group's), and that image's index; false when
 * not bound. */
static bool binding_rect(const ChannelAtlas* atlases, const CookGroup* groups, int* const* group_of,
						 int* const* rect_of, const int* core_image, size_t first_group_image, int c, size_t mi,
						 ChannelRect* out, size_t* image) {
	if (rect_of[c][mi] < 0)
		return false;
	if (group_of[c][mi] >= 0) {
		*out   = groups[group_of[c][mi]].atlas.rects[rect_of[c][mi]];
		*image = first_group_image + (size_t)group_of[c][mi];
		return true;
	}
	*out   = atlases[c].rects[rect_of[c][mi]];
	*image = (size_t)core_image[c];
	return true;
}

/* Marks the images material `m` binds (its channels and transmission):
 * `core` set, or variant `v`'s bit in `uses`. */
static void mark_material_images(const cgltf_data* data, cgltf_material* m, bool* core, uint8_t* uses,
								 size_t variants, size_t v, bool is_core) {
	const cgltf_image* images[AERON_GLTF_COOK_CHANNEL_COUNT + 1] = { 0 };
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
		const cgltf_texture_view* tv = material_channel_view(m, c);
		images[c]                    = tv && tv->texture ? tv->texture->image : NULL;
	}
	if (m->has_transmission && m->transmission.transmission_texture.texture)
		images[AERON_GLTF_COOK_CHANNEL_COUNT] = m->transmission.transmission_texture.texture->image;
	for (size_t k = 0; k < sizeof images / sizeof *images; k++) {
		if (!images[k])
			continue;
		const size_t i = (size_t)(images[k] - data->images);
		if (is_core)
			core[i] = true;
		else
			uses[i * variants + v] = 1;
	}
}

/* The variants each image belongs to (variant groups above), printed as a
 * JSON array, NULL for a core image; NULL table on OOM. */
static char** derive_image_variants(const cgltf_data* data) {
	char**       out      = (char**)calloc(data->images_count ? data->images_count : 1, sizeof *out);
	const size_t variants = data->variants_count;
	if (!out || !variants || !data->images_count)
		return out;
	bool*    core = (bool*)calloc(data->images_count, sizeof *core);
	uint8_t* uses = (uint8_t*)calloc(data->images_count * variants, 1);
	bool     ok   = core && uses;
	for (size_t mi = 0; ok && mi < data->meshes_count; mi++) {
		for (size_t pi = 0; pi < data->meshes[mi].primitives_count; pi++) {
			const cgltf_primitive* p = &data->meshes[mi].primitives[pi];
			if (p->material)
				mark_material_images(data, p->material, core, uses, variants, 0, true);
			for (size_t k = 0; k < p->mappings_count; k++)
				mark_material_images(data, p->mappings[k].material, core, uses, variants,
									 p->mappings[k].variant, false);
		}
	}
	/* A channel no primitive's own material uses has no core atlas for
	 * groups to follow: its images are core. */
	bool own_channel[AERON_GLTF_COOK_CHANNEL_COUNT] = { false };
	for (size_t mi = 0; ok && mi < data->meshes_count; mi++)
		for (size_t pi = 0; pi < data->meshes[mi].primitives_count; pi++)
			for (int c = 0; data->meshes[mi].primitives[pi].material && c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
				const cgltf_texture_view* tv = material_channel_view(data->meshes[mi].primitives[pi].material, c);
				own_channel[c] |= tv && tv->texture && tv->texture->image;
			}
	for (size_t mi = 0; ok && mi < data->meshes_count; mi++)
		for (size_t pi = 0; pi < data->meshes[mi].primitives_count; pi++)
			for (size_t k = 0; k < data->meshes[mi].primitives[pi].mappings_count; k++)
				for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
					const cgltf_texture_view* tv =
						material_channel_view(data->meshes[mi].primitives[pi].mappings[k].material, c);
					if (!own_channel[c] && tv && tv->texture && tv->texture->image)
						core[tv->texture->image - data->images] = true;
				}
	for (size_t i = 0; ok && i < data->images_count; i++) {
		if (core[i])
			continue;
		cJSON* list = NULL;
		for (size_t v = 0; v < variants; v++) {
			if (!uses[i * variants + v])
				continue;
			list = list ? list : cJSON_CreateArray();
			cJSON_AddItemToArray(list, cJSON_CreateNumber((double)v));
		}
		if (list) {
			out[i] = cJSON_PrintUnformatted(list);
			ok     = out[i] != NULL;
			cJSON_Delete(list);
		}
	}
	free(core);
	free(uses);
	if (!ok) {
		for (size_t i = 0; i < data->images_count; i++)
			free(out[i]);
		free(out);
		return NULL;
	}
	return out;
}

/* The atlas of channel `c` that `variants_of` goes to: the group of its
 * variants (`image_variants`, created when new), else the core atlas. Sets
 * *group_out (-1: core); NULL on OOM. */
static ChannelAtlas* target_atlas(const cgltf_data* data, char* const* image_variants, ChannelAtlas* atlases,
								  CookGroup** groups, int* group_count, int c, const cgltf_image* variants_of,
								  int* group_out) {
	*group_out           = -1;
	const char* derived  = image_variants[variants_of - data->images];
	char*       variants = derived ? cook_strdup(derived) : NULL;
	if (!variants)
		return &atlases[c];
	int g = 0;
	while (g < *group_count && ((*groups)[g].channel != c || strcmp((*groups)[g].variants, variants) != 0))
		g++;
	if (g == *group_count) {
		CookGroup* grown = (CookGroup*)realloc(*groups, (size_t)(*group_count + 1) * sizeof *grown);
		if (!grown) {
			free(variants);
			return NULL;
		}
		*groups = grown;
		memset(&grown[g], 0, sizeof grown[g]);
		channel_atlas_init(&grown[g].atlas, c);
		grown[g].channel  = c;
		grown[g].variants = variants;
		(*group_count)++;
	} else {
		free(variants);
	}
	*group_out = g;
	return &(*groups)[g].atlas;
}

/* The pixels of `image`, from the provider or the PNG cache. */
static bool image_view(const char* source_label, AeronGltfCookImageProvider image_provider,
					   void* image_context, ImageCache* cache, cgltf_image* image,
					   AeronGltfCookImageView* view) {
	memset(view, 0, sizeof *view);
	if (image_provider)
		return image_provider(image_context, image, view);
	const CachedImage* img = load_or_cache_image(cache, source_label, image->uri);
	if (!img)
		return false;
	view->rgba   = img->rgba;
	view->width  = img->w;
	view->height = img->h;
	return true;
}

/* Places `image` on channel `c`: in the group of its variants (created
 * when new), else on the core atlas; an image already placed shares its
 * rect. Sets *group_out (-1: core) and *rect_out. */
static bool place_image(cgltf_data* data, char* const* image_variants, const char* source_label,
						AeronGltfCookImageProvider image_provider, void* image_context, ImageCache* cache,
						const AeronGltfCookOptions* opts, ChannelAtlas* atlases, CookGroup** groups,
						int* group_count, int c, cgltf_image* image, int* group_out, int* rect_out) {
	ChannelAtlas* atlas = target_atlas(data, image_variants, atlases, groups, group_count, c, image, group_out);
	if (!atlas)
		return false;
	*rect_out = find_channel_rect(atlas, image);
	if (*rect_out >= 0)
		return true;
	AeronGltfCookImageView view;
	if (!image_view(source_label, image_provider, image_context, cache, image, &view))
		return false;
	*rect_out =
		channel_atlas_add_rect(atlas, image, view.width, view.height, view.rgba, opts->max_texture_size);
	return *rect_out >= 0;
}

/* ---------- transmission ------------------------------------------
 *
 * KHR_materials_transmission's texture goes in the base colour's alpha,
 * which the runtime reads as the transmission of a transmissive material
 * (aeron/scene/gltf_mesh.h): the material's base image with A the
 * transmission texture's R, or 255 without one (the factor alone applies).
 * A merged image is keyed by a placeholder cgltf_image whose uri names both
 * sources, so materials sharing them share its rect. */

typedef struct MergedImage {
	cgltf_image key;
	uint8_t*    rgba;
} MergedImage;

typedef struct MergedImages {
	MergedImage** items;
	int           count;
} MergedImages;

static void merged_images_free(MergedImages* merged) {
	for (int i = 0; i < merged->count; i++) {
		free(merged->items[i]->key.uri);
		free(merged->items[i]->rgba);
		free(merged->items[i]);
	}
	free(merged->items);
	memset(merged, 0, sizeof *merged);
}

static bool same_texture_mapping(const cgltf_texture_view* a, const cgltf_texture_view* b) {
	if (a->texcoord != b->texcoord || a->has_transform != b->has_transform)
		return false;
	if (!a->has_transform)
		return true;
	const cgltf_texture_transform* x = &a->transform;
	const cgltf_texture_transform* y = &b->transform;
	return x->offset[0] == y->offset[0] && x->offset[1] == y->offset[1] && x->scale[0] == y->scale[0] &&
		   x->scale[1] == y->scale[1] && x->rotation == y->rotation && x->has_texcoord == y->has_texcoord &&
		   (!x->has_texcoord || x->texcoord == y->texcoord);
}

/* Places transmissive material `m`'s base image on the base channel, with
 * the transmission texture in its alpha. Sets *handled when it did (the
 * caller then skips the material's own base image). */
static bool place_transmission(cgltf_data* data, char* const* image_variants, const char* source_label,
							   AeronGltfCookImageProvider image_provider, void* image_context,
							   ImageCache* cache, const AeronGltfCookOptions* opts, ChannelAtlas* atlases,
							   CookGroup** groups, int* group_count, MergedImages* merged, cgltf_material* m,
							   int* group_out, int* rect_out, bool* handled) {
	const int           c     = AERON_GLTF_COOK_CHANNEL_BASE_COLOR;
	cgltf_texture_view* base  = material_channel_view(m, c);
	cgltf_texture_view* trans = &m->transmission.transmission_texture;
	const char*         name  = m->name ? m->name : "<unnamed>";
	*handled                  = false;
	cgltf_image* base_image   = base && base->texture && base->texture->image && base->texture->image->uri
									? base->texture->image
									: NULL;
	cgltf_image* trans_image =
		trans->texture && trans->texture->image && trans->texture->image->uri ? trans->texture->image : NULL;
	if (m->alpha_mode != cgltf_alpha_mode_opaque)
		fprintf(stderr, "[aeron_gltf_cook] material '%s': transmissive, its alpha mode is ignored\n", name);
	if (trans_image && !base) {
		fprintf(stderr,
				"[aeron_gltf_cook] material '%s': transmission texture without pbrMetallicRoughness "
				"ignored (the factor applies)\n",
				name);
		trans_image = NULL;
	} else if (trans_image && base_image && !same_texture_mapping(base, trans)) {
		fprintf(stderr,
				"[aeron_gltf_cook] material '%s': transmission texture mapped unlike the base colour "
				"ignored (the factor applies)\n",
				name);
		trans_image = NULL;
	}
	if (!base_image && !trans_image)
		return true;
	*handled = true;

	size_t key_size =
		strlen(base_image ? base_image->uri : "") + strlen(trans_image ? trans_image->uri : "") + 32;
	char* key = (char*)malloc(key_size);
	if (!key)
		return false;
	snprintf(key, key_size, "%s#transmission=%s", base_image ? base_image->uri : "",
			 trans_image ? trans_image->uri : "");
	ChannelAtlas* atlas =
		target_atlas(data, image_variants, atlases, groups, group_count, c, base_image ? base_image : trans_image,
					 group_out);
	cgltf_image probe = { 0 };
	probe.uri         = key;
	*rect_out         = atlas ? find_channel_rect(atlas, &probe) : -1;
	if (!atlas || *rect_out >= 0) {
		free(key);
		return atlas != NULL;
	}

	AeronGltfCookImageView base_view = { 0 }, trans_view = { 0 };
	if ((base_image &&
		 !image_view(source_label, image_provider, image_context, cache, base_image, &base_view)) ||
		(trans_image &&
		 !image_view(source_label, image_provider, image_context, cache, trans_image, &trans_view))) {
		free(key);
		return false;
	}
	const int     w     = base_image ? base_view.width : trans_view.width;
	const int     h     = base_image ? base_view.height : trans_view.height;
	MergedImage*  image = (MergedImage*)calloc(1, sizeof *image);
	MergedImage** grown =
		image ? (MergedImage**)realloc(merged->items, (size_t)(merged->count + 1) * sizeof *grown) : NULL;
	uint8_t* rgba = grown ? (uint8_t*)malloc((size_t)w * (size_t)h * 4) : NULL;
	if (!rgba) {
		free(key);
		free(image);
		if (grown)
			merged->items = grown;
		return false;
	}
	merged->items                  = grown;
	merged->items[merged->count++] = image;
	image->key.uri                 = key;
	image->rgba                    = rgba;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint8_t* px = &rgba[((size_t)y * (size_t)w + (size_t)x) * 4];
			if (base_image)
				memcpy(px, &base_view.rgba[((size_t)y * (size_t)w + (size_t)x) * 4], 3);
			else
				px[0] = px[1] = px[2] = 255;
			px[3] = 255;
			if (trans_image) {
				/* Nearest texel at the same normalized position. */
				const size_t tx = (size_t)x * (size_t)trans_view.width / (size_t)w;
				const size_t ty = (size_t)y * (size_t)trans_view.height / (size_t)h;
				px[3]           = trans_view.rgba[(ty * (size_t)trans_view.width + tx) * 4];
			}
		}
	}
	*rect_out = channel_atlas_add_rect(atlas, &image->key, w, h, rgba, opts->max_texture_size);
	return *rect_out >= 0;
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
	if (opts.zstd_supercompress &&
		!ktx2_set_zstd_level(opts.zstd_level ? opts.zstd_level : KTX2_ZSTD_LEVEL_DEFAULT)) {
		fprintf(stderr, "[aeron_gltf_cook] invalid zstd level %d\n", opts.zstd_level);
		return false;
	}

	if (data->buffers_count != 1) {
		fprintf(stderr,
				"[aeron_gltf_cook] expected 1 source buffer, got %zu — opt2gltf "
				"should produce single-buffer .gltf files\n",
				data->buffers_count);
		return false;
	}

	ImageCache cache;
	image_cache_init(&cache);
	MergedImages merged = { 0 };

	ChannelAtlas atlases[AERON_GLTF_COOK_CHANNEL_COUNT];
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++)
		channel_atlas_init(&atlases[c], c);

	/* Rect index of each (channel, material) binding, or -1. */
	int*          rect_of[AERON_GLTF_COOK_CHANNEL_COUNT]    = { 0 };
	uint8_t*      ktx2_bufs[AERON_GLTF_COOK_CHANNEL_COUNT]  = { 0 };
	size_t        ktx2_sizes[AERON_GLTF_COOK_CHANNEL_COUNT] = { 0 };
	cgltf_buffer* grown_src_buf                             = NULL;
	cgltf_size    source_base_size                          = 0;
	/* Variant groups, and each (channel, material) binding's group or -1. */
	CookGroup* groups                                  = NULL;
	int        group_count                             = 0;
	int*       group_of[AERON_GLTF_COOK_CHANNEL_COUNT] = { 0 };

	char** image_variants = derive_image_variants(data);
	bool   ok             = validate_material_textures(data) && image_variants;
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT && ok; c++) {
		group_of[c] = (int*)malloc((data->materials_count ? data->materials_count : 1) * sizeof *group_of[c]);
		ok          = group_of[c] != NULL;
		for (size_t mi = 0; mi < data->materials_count && ok; mi++)
			group_of[c][mi] = -1;
	}
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
		cgltf_material* m           = &data->materials[mi];
		bool            merged_base = false;
		if (m->has_transmission && m->transmission.transmission_factor > 0.0f)
			ok = place_transmission(data, image_variants, source_label, image_provider, image_context, &cache,
									&opts, atlases,
									&groups, &group_count, &merged, m,
									&group_of[AERON_GLTF_COOK_CHANNEL_BASE_COLOR][mi],
									&rect_of[AERON_GLTF_COOK_CHANNEL_BASE_COLOR][mi], &merged_base);
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT && ok; c++) {
			cgltf_texture_view* tv = material_channel_view(m, c);
			if (!tv || !tv->texture || !tv->texture->image || !tv->texture->image->uri)
				continue;
			if (c == AERON_GLTF_COOK_CHANNEL_BASE_COLOR && merged_base)
				continue;
			/* An image some variants use goes to their group. */
			ok = place_image(data, image_variants, source_label, image_provider, image_context, &cache, &opts,
							 atlases, &groups, &group_count, c, tv->texture->image, &group_of[c][mi],
							 &rect_of[c][mi]);
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
		int channel_groups = 0;
		for (int g = 0; g < group_count; g++)
			channel_groups += groups[g].channel == c;
		if (ca->rect_count == 0 && channel_groups) {
			fprintf(stderr, "[aeron_gltf_cook] %s: variant groups without a core atlas\n", kChannels[c].name);
			ok = false;
			break;
		}
		if (ca->rect_count == 0) {
			/* No material binds this channel: it has no image. */
			continue;
		}
		/* Groups: pages of the core's size, and one mip count for all. */
		if (channel_groups) {
			/* Group pages take the core's page size. A grouped image the
			 * core's pages can't hold (an emissive or metallic-roughness
			 * image of a texture whose own is a constant, beside a small
			 * core) grows the core's pages to its footprint, or to square
			 * power-of-two pages when it needs a dedicated layer. */
			int largest = 0, widest = 0, tallest = 0;
			for (int g = 0; g < group_count; g++) {
				for (int r = 0; groups[g].channel == c && r < groups[g].atlas.rect_count; r++) {
					const ChannelRect* rect = &groups[g].atlas.rects[r];
					const int          gut  = opts.mip_min_subrect;
					const int          fw   = (rect->src_w + 3 * gut - 1) / gut * gut;
					const int          fh   = (rect->src_h + 3 * gut - 1) / gut * gut;
					largest                 = rect->src_w > largest ? rect->src_w : largest;
					largest                 = rect->src_h > largest ? rect->src_h : largest;
					widest                  = fw > widest ? fw : widest;
					tallest                 = fh > tallest ? fh : tallest;
				}
			}
			if (largest > ca->page / 2) {
				int page = ca->page;
				while (page < largest)
					page *= 2;
				ca->fixed_width = ca->fixed_height = page;
				ok                                 = channel_atlas_pack(ca, page, opts.mip_min_subrect);
			} else if (widest > ca->width || tallest > ca->height) {
				ca->fixed_width  = widest > ca->width ? widest : ca->width;
				ca->fixed_height = tallest > ca->height ? tallest : ca->height;
				ok               = channel_atlas_pack(ca, ca->page, opts.mip_min_subrect);
			}
			if (!ok)
				break;
			int pages = 0;
			for (int g = 0; g < group_count && ok; g++) {
				if (groups[g].channel != c)
					continue;
				groups[g].atlas.fixed_width  = ca->width;
				groups[g].atlas.fixed_height = ca->height;
				ok = channel_atlas_pack(&groups[g].atlas, ca->page, opts.mip_min_subrect);
				pages += groups[g].atlas.page_count;
				if (ok && groups[g].atlas.mip_count > ca->mip_count)
					ca->mip_count = groups[g].atlas.mip_count;
			}
			double used = 0.0;
			for (int g = 0; g < group_count && ok; g++) {
				if (groups[g].channel != c)
					continue;
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
			fprintf(stderr,
					"[aeron_gltf_cook] %s: %d core page(s) of %d×%d, %d variant group(s) on %d page(s), "
					"%.0f%% of the group pages used\n",
					kChannels[c].name, ca->page_count, ca->width, ca->height, channel_groups, pages,
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
		if (ktx2_sizes[c])
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

	/* The images: one per channel with an atlas, then one per variant group,
	 * each a KTX2 in a new buffer view after the source's. */
	int    core_image[AERON_GLTF_COOK_CHANNEL_COUNT];
	size_t core_images = 0;
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++)
		core_image[c] = ktx2_sizes[c] ? (int)core_images++ : -1;
	const size_t       image_count = core_images + (size_t)group_count;
	size_t             old_bv      = data->buffer_views_count;
	const size_t       new_bv_n    = old_bv + image_count;
	cgltf_buffer_view* new_bvs     = (cgltf_buffer_view*)calloc(new_bv_n ? new_bv_n : 1, sizeof *new_bvs);
	cgltf_image*       new_imgs    = (cgltf_image*)calloc(image_count ? image_count : 1, sizeof *new_imgs);
	cgltf_sampler*     new_smp     = (cgltf_sampler*)calloc(1, sizeof *new_smp);
	size_t             texture_capacity = 1;
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++)
		texture_capacity += (size_t)atlases[c].rect_count;
	for (int g = 0; g < group_count; g++)
		texture_capacity += (size_t)groups[g].atlas.rect_count;
	size_t         new_tex_count = 0;
	cgltf_texture* new_texs      = (cgltf_texture*)calloc(texture_capacity, sizeof *new_texs);
	/* Preserve material bindings so the memory API can restore the caller's
	 * graph after the consumer returns. */
	const size_t        saved_view_count = data->materials_count * AERON_GLTF_COOK_CHANNEL_COUNT;
	cgltf_texture_view* saved_views =
		(cgltf_texture_view*)calloc(saved_view_count ? saved_view_count : 1, sizeof *saved_views);
	/* Transmission textures, which the cooked graph carries in the base
	 * colour's alpha. */
	cgltf_texture_view* saved_transmission = (cgltf_texture_view*)calloc(
		data->materials_count ? data->materials_count : 1, sizeof *saved_transmission);
	/* The cooked file requires AERON_flight_model: its textures are found
	 * only through it. */
	char*  required_name      = cook_strdup("AERON_flight_model");
	char** cooked_required    = (char**)calloc(data->extensions_required_count + 1, sizeof *cooked_required);
	char** cooked_used        = (char**)calloc(data->extensions_used_count + 1, sizeof *cooked_used);
	bool   arrays_ok          = new_bvs && new_imgs && new_smp && new_texs && saved_views && saved_transmission &&
					   required_name && cooked_required && cooked_used;
	for (size_t i = 0; arrays_ok && i < old_bv; i++)
		new_bvs[i] = data->buffer_views[i];
	for (int c = 0; arrays_ok && c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
		if (core_image[c] < 0)
			continue;
		cgltf_buffer_view* bv = &new_bvs[old_bv + (size_t)core_image[c]];
		bv->buffer            = src_buf;
		bv->offset            = ktx2_offsets[c];
		bv->size              = ktx2_sizes[c];
		bv->type              = cgltf_buffer_view_type_invalid;
	}
	for (int g = 0; arrays_ok && g < group_count; g++) {
		cgltf_buffer_view* bv = &new_bvs[old_bv + core_images + (size_t)g];
		bv->buffer            = src_buf;
		bv->offset            = group_offsets[g];
		bv->size              = groups[g].ktx2_size;
		bv->type              = cgltf_buffer_view_type_invalid;
	}
	free(group_offsets);
	for (size_t i = 0; arrays_ok && i < image_count; i++) {
		new_imgs[i].buffer_view = &new_bvs[old_bv + i];
		new_imgs[i].mime_type   = cook_strdup("image/ktx2");
		arrays_ok               = new_imgs[i].mime_type != NULL;
	}

	/* New sampler: linear+linear with mipmaps, repeat-wrap. REPEAT is
	 * exact inside a sub-rect because the runtime wraps UVs into the rect
	 * and every footprint is filled by REPEAT. */
	if (arrays_ok) {
		new_smp[0].name       = cook_strdup("atlas_sampler");
		new_smp[0].mag_filter = cgltf_filter_type_linear;
		new_smp[0].min_filter = cgltf_filter_type_linear_mipmap_linear;
		new_smp[0].wrap_s     = cgltf_wrap_mode_repeat;
		new_smp[0].wrap_t     = cgltf_wrap_mode_repeat;
	}

	/* New textures: one per (image, page, LOD limit) that a binding uses. */
	for (size_t mi = 0; mi < data->materials_count && arrays_ok; mi++) {
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT && arrays_ok; c++) {
			ChannelRect rect;
			size_t      image;
			if (binding_rect(atlases, groups, group_of, rect_of, core_image, core_images, c, mi, &rect, &image))
				arrays_ok = atlas_texture(new_texs, &new_tex_count, image, &new_smp[0], &rect) != NULL;
		}
	}
	if (!arrays_ok) {
		free(new_bvs);
		for (size_t i = 0; new_imgs && i < image_count; i++)
			free(new_imgs[i].mime_type);
		free(new_imgs);
		if (new_smp)
			free(new_smp[0].name);
		free(new_smp);
		free_atlas_textures(new_texs, new_tex_count);
		free(saved_views);
		free(saved_transmission);
		free(required_name);
		free(cooked_required);
		free(cooked_used);
		ok = false;
		goto cleanup;
	}

	for (size_t mi = 0; mi < data->materials_count; mi++) {
		for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
			cgltf_texture_view* tv = material_channel_view(&data->materials[mi], c);
			if (tv)
				saved_views[mi * AERON_GLTF_COOK_CHANNEL_COUNT + (size_t)c] = *tv;
		}
		cgltf_texture_view* transmission = &data->materials[mi].transmission.transmission_texture;
		saved_transmission[mi]           = *transmission;
		memset(transmission, 0, sizeof *transmission);
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
			ChannelRect rect;
			size_t      image;
			if (!binding_rect(atlases, groups, group_of, rect_of, core_image, core_images, c, mi, &rect, &image)) {
				/* This material doesn't author this channel — clear
				 * the binding entirely so cgltf_write emits no
				 * texture reference. The FS falls back to the
				 * per-material factor. */
				tv->texture       = NULL;
				tv->has_transform = 0;
				memset(&tv->transform, 0, sizeof tv->transform);
				continue;
			}
			const ChannelAtlas* ca = group_of[c][mi] >= 0 ? &groups[group_of[c][mi]].atlas : &atlases[c];
			float               off[2], scl[2];
			channel_atlas_rect_uv_transform(ca, &rect, off, scl);
			tv->texture                = atlas_texture(new_texs, &new_tex_count, image, &new_smp[0], &rect);
			tv->texcoord               = 0;
			tv->has_transform          = 1;
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
	cgltf_buffer_view* orig_bvs             = data->buffer_views;
	cgltf_image*       orig_imgs            = data->images;
	cgltf_texture*     orig_texs            = data->textures;
	cgltf_sampler*     orig_smps            = data->samplers;
	size_t             orig_bvs_count       = data->buffer_views_count;
	size_t             orig_imgs_count      = data->images_count;
	size_t             orig_texs_count      = data->textures_count;
	size_t             orig_smps_count      = data->samplers_count;
	char**             orig_required        = data->extensions_required;
	size_t             orig_required_count  = data->extensions_required_count;
	char**             orig_used            = data->extensions_used;
	size_t             orig_used_count      = data->extensions_used_count;

	data->buffer_views       = new_bvs;
	data->buffer_views_count = new_bv_n;
	data->images             = new_imgs;
	data->images_count       = image_count;
	data->textures           = new_texs;
	data->textures_count     = new_tex_count;
	data->samplers           = new_smp;
	data->samplers_count     = 1;
	size_t required_count    = 0;
	for (size_t i = 0; i < orig_required_count; i++)
		if (strcmp(orig_required[i], required_name) != 0)
			cooked_required[required_count++] = orig_required[i];
	cooked_required[required_count++] = required_name;
	size_t used_count                 = 0;
	for (size_t i = 0; i < orig_used_count; i++)
		if (strcmp(orig_used[i], required_name) != 0)
			cooked_used[used_count++] = orig_used[i];
	cooked_used[used_count++]       = required_name;
	data->extensions_required       = cooked_required;
	data->extensions_required_count = required_count;
	data->extensions_used           = cooked_used;
	data->extensions_used_count     = used_count;

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
	const void* orig_bin      = data->bin;
	cgltf_size  orig_bin_size = data->bin_size;
	char*       orig_buf0_uri = src_buf->uri;
	data->bin                 = src_buf->data;
	data->bin_size            = src_buf->size;
	src_buf->uri              = NULL;
	ok                        = consumer(consumer_context, data);

	data->bin      = orig_bin;
	data->bin_size = orig_bin_size;
	src_buf->uri   = orig_buf0_uri;
	src_buf->size  = base_size;

	/* Restore the original arrays so cgltf_free can release them
	 * without confusing our owned arrays. */
	data->buffer_views              = orig_bvs;
	data->buffer_views_count        = orig_bvs_count;
	data->images                    = orig_imgs;
	data->images_count              = orig_imgs_count;
	data->textures                  = orig_texs;
	data->textures_count            = orig_texs_count;
	data->samplers                  = orig_smps;
	data->samplers_count            = orig_smps_count;
	data->extensions_required       = orig_required;
	data->extensions_required_count = orig_required_count;
	data->extensions_used           = orig_used;
	data->extensions_used_count     = orig_used_count;

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
		data->materials[mi].transmission.transmission_texture = saved_transmission[mi];
	}

	/* Free our owned arrays. */
	free(saved_views);
	free(saved_transmission);
	free(new_bvs);
	for (size_t i = 0; i < image_count; i++)
		free(new_imgs[i].mime_type);
	free(new_imgs);
	free_atlas_textures(new_texs, new_tex_count);
	free(new_smp[0].name);
	free(new_smp);
	free(required_name);
	free(cooked_required);
	free(cooked_used);

cleanup:
	if (grown_src_buf)
		grown_src_buf->size = source_base_size;
	for (int g = 0; g < group_count; g++) {
		free(groups[g].variants);
		free(groups[g].ktx2);
		channel_atlas_free(&groups[g].atlas);
	}
	free(groups);
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++)
		free(group_of[c]);
	for (int c = 0; c < AERON_GLTF_COOK_CHANNEL_COUNT; c++) {
		free(ktx2_bufs[c]);
		free(rect_of[c]);
		channel_atlas_free(&atlases[c]);
	}
	image_cache_free(&cache);
	merged_images_free(&merged);
	for (size_t i = 0; image_variants && i < data->images_count; i++)
		free(image_variants[i]);
	free(image_variants);
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
