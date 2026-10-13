#include "xwau_gltf_skins.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"

/* Checks that the skin layers are in range, and that the variant names are
 * set and unique. */
static bool skin_validate(const XwauSkinVariants* skins, const char* basename) {
	for (size_t v = 0; v < skins->variant_count; ++v) {
		const XwauSkinVariant* variant = &skins->variants[v];
		bool                   ok      = variant->name && variant->name[0];
		for (size_t w = 0; ok && w < v; ++w)
			ok = strcmp(skins->variants[w].name, variant->name) != 0;
		for (size_t l = 0; ok && l < variant->layer_count; ++l)
			ok = variant->layers[l].skin < skins->skin_count && variant->layers[l].opacity <= 100;
		if (!ok) {
			Report(basename, "invalid or duplicate skin variant %zu '%s'", v,
				   variant->name ? variant->name : "");
			return false;
		}
	}
	return true;
}

/* ---- Variants ----------------------------------------------------------- */

struct XwauGltfSkins {
	OptGltfVariant*          variants; /* the plain variants: no skin of theirs has an image */
	OptGltfVariants          out;
	char**                   strings; /* image names, files and skin names */
	size_t                   string_count;
	OptGltfSkinLayer*        layers; /* grouped by texture */
	size_t                   layer_count;
	size_t*                  first_layer; /* per texture */
	size_t*                  texture_layers;
	uint8_t**                pixels; /* the layer images, owned */
	size_t                   pixel_count;
	size_t                   emissive_layers, metal_rough_layers, shared_images;
	const char**             skin_names; /* valid names, per skin */
	OptGltfSkinVariant*      skin_variants;
	OptGltfSkinVariantLayer* skin_layers;
	int32_t*                 plain_material_of;
};

/* Prints `format` into a new string that `s` keeps. Returns NULL when out of
 * memory. */
static const char* keep(XwauGltfSkins* s, const char* format, ...) {
	char    text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof text, format, args);
	va_end(args);
	char*  copy = StrDup(text);
	char** slot = copy ? ARRAY_PUSH(s->strings, s->string_count) : NULL;
	if (!slot) {
		free(copy);
		return NULL;
	}
	return *slot = copy;
}

/* ---- Layers ------------------------------------------------------------ */

/* A skin name of the format (1 to 63 of a-z, 0-9, '.', '_', '-'), unique
 * among `names`. */
static const char* layer_skin_name(XwauGltfSkins* s, const char* name, const char* const* names, size_t count) {
	char base[64];
	size_t n = 0;
	for (const char* c = name; *c && n < 56; ++c) {
		const char lower = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c;
		base[n++] = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '.' ||
							lower == '-'
						? lower
						: '_';
	}
	if (!n)
		base[n++] = '_';
	base[n] = 0;
	for (int k = 1;; ++k) {
		char candidate[72];
		if (k == 1)
			snprintf(candidate, sizeof candidate, "%s", base);
		else
			snprintf(candidate, sizeof candidate, "%s_%d", base, k);
		bool taken = false;
		for (size_t i = 0; !taken && i < count; ++i)
			taken = names[i] && strcmp(names[i], candidate) == 0;
		if (!taken)
			return keep(s, "%s", candidate);
	}
}

/* Keeps `rgba`, or returns an equal image kept before (and frees `rgba`). */
static uint8_t* keep_pixels(XwauGltfSkins* s, uint8_t* rgba, size_t bytes, size_t* sizes) {
	for (size_t i = 0; i < s->pixel_count; ++i)
		if (sizes[i] == bytes && memcmp(s->pixels[i], rgba, bytes) == 0) {
			free(rgba);
			s->shared_images++;
			return s->pixels[i];
		}
	uint8_t** slot = ARRAY_PUSH(s->pixels, s->pixel_count);
	if (!slot) {
		free(rgba);
		return NULL;
	}
	sizes[s->pixel_count - 1] = bytes;
	return *slot = rgba;
}

static void mr_texel(const XwauBaked* b, size_t i, uint8_t out[4]) {
	if (b->metal_rough) {
		memcpy(out, &b->metal_rough[i * 4], 4);
		return;
	}
	out[0] = 255;
	out[1] = (uint8_t)(fminf(fmaxf(b->roughness, 0.0f), 1.0f) * 255.0f + 0.5f);
	out[2] = (uint8_t)(fminf(fmaxf(b->metallic, 0.0f), 1.0f) * 255.0f + 0.5f);
	out[3] = 255;
}

/* The images of one skin on one texture, before the texture decides which
 * channels its layers carry. */
typedef struct LayerCandidate {
	size_t   skin;
	uint8_t* base;     /* baked base color, the skin's alpha as coverage */
	uint8_t* emissive; /* baked emissive, NULL when neither it nor the texture emits */
	uint8_t* mr;       /* baked metallic-roughness */
	bool     emissive_differs, mr_differs; /* from the texture's own, under the coverage */
} LayerCandidate;

static void candidate_free(LayerCandidate* c) {
	free(c->base);
	free(c->emissive);
	free(c->mr);
	memset(c, 0, sizeof *c);
}

/* Bakes skin `skin` (image `img`) on texture `ti`: the skin's colors with the
 * texture's alpha, illumination and material. */
static bool layer_bake(const opt_file_t* opt, int base_shade, const XwauGltfMaterial* xwau,
					   const XwauGltfAnim* anims, const XwauBaked* own, int32_t ti, const OptGltfImageView* img,
					   LayerCandidate* out) {
	const opt_texture_t* t   = &opt->textures[ti];
	const uint32_t       w   = (uint32_t)t->width, h = (uint32_t)t->height;
	const size_t         n   = (size_t)w * h;
	uint8_t*             raw = OptGltf_DecodeTexture(t, base_shade);
	if (!raw)
		return false;
	for (size_t i = 0; i < n; ++i)
		memcpy(&raw[i * 4], &img->rgba[i * 4], 3);
	const XwauGltfAnim* a      = anims ? &anims[ti] : NULL;
	uint8_t*            colour = a && a->anim_base ? XwauGltf_RgbaDup(raw, n) : NULL;
	XwauBaked           lay;
	if ((a && a->anim_base && !colour) || !XwauGltf_BakeRgba(t, &xwau[ti], raw, &lay)) {
		free(colour);
		return false;
	}
	XwauGltf_AnimBase(&colour, w, h, a, &lay);
	out->base     = (uint8_t*)malloc(n * 4);
	out->mr       = (uint8_t*)malloc(n * 4);
	out->emissive = lay.emissive || own->emissive ? (uint8_t*)malloc(n * 4) : NULL;
	bool ok       = out->base && out->mr && (out->emissive || (!lay.emissive && !own->emissive));
	for (size_t i = 0; ok && i < n; ++i) {
		memcpy(&out->base[i * 4], &lay.base[i * 4], 3);
		out->base[i * 4 + 3] = img->rgba[i * 4 + 3];
		mr_texel(&lay, i, &out->mr[i * 4]);
		if (out->emissive) {
			for (int k = 0; k < 3; ++k)
				out->emissive[i * 4 + k] = lay.emissive ? lay.emissive[i * 4 + k] : 0;
			out->emissive[i * 4 + 3] = 255;
		}
		if (!img->rgba[i * 4 + 3])
			continue;
		for (int k = 0; k < 3; ++k)
			out->emissive_differs |= (lay.emissive ? lay.emissive[i * 4 + k] : 0) !=
									 (own->emissive ? own->emissive[i * 4 + k] : 0);
		uint8_t om[4];
		mr_texel(own, i, om);
		out->mr_differs |= out->mr[i * 4 + 1] != om[1] || out->mr[i * 4 + 2] != om[2];
	}
	XwauGltf_BakedFree(&lay);
	if (!ok)
		candidate_free(out);
	return ok;
}

/* Turns the candidates of texture `ti` into its layers. If one skin changes
 * the emissive (or metallic-roughness) under its coverage, every skin of the
 * texture gets an emissive (or metallic-roughness) layer: a later layer must
 * replace what an earlier one changed, as the composite of the baked colors
 * would. */
static bool layers_finish(XwauGltfSkins* s, const char* basename, int32_t ti, uint32_t w, uint32_t h,
						  LayerCandidate* candidates, size_t count, size_t* sizes) {
	const size_t n            = (size_t)w * h;
	bool         any_emissive = false, any_mr = false;
	for (size_t c = 0; c < count; ++c) {
		any_emissive |= candidates[c].emissive_differs;
		any_mr |= candidates[c].mr_differs;
	}
	bool ok = true;
	for (size_t c = 0; ok && c < count; ++c) {
		LayerCandidate*   in   = &candidates[c];
		OptGltfSkinLayer* out  = &s->layers[s->layer_count];
		const char*       skin = s->skin_names[in->skin];
		memset(out, 0, sizeof *out);
		out->skin      = skin;
		out->base.view = (OptGltfImageView) { keep_pixels(s, in->base, n * 4, sizes), w, h };
		in->base       = NULL;
		out->base.name = keep(s, "skin_%s_Tex%02d", skin, (int)ti);
		out->base.file = keep(s, "skins/%s/%s_Tex%02d.png", skin, basename, (int)ti);
		ok             = out->base.view.rgba && out->base.name && out->base.file;
		if (ok && any_emissive) {
			if (!in->emissive)
				in->emissive = (uint8_t*)calloc(n * 4, 1); /* emits nothing */
			for (size_t i = 0; in->emissive && i < n; ++i)
				in->emissive[i * 4 + 3] = 255;
			out->emissive.view     = (OptGltfImageView) { keep_pixels(s, in->emissive, n * 4, sizes), w, h };
			in->emissive           = NULL;
			out->emissive.name     = keep(s, "skin_%s_Tex%02d_emissive", skin, (int)ti);
			out->emissive.file     = keep(s, "skins/%s/%s_Tex%02d_emissive.png", skin, basename, (int)ti);
			out->emissive_strength = XWAU_GLTF_ILLUM_GAIN;
			ok                     = out->emissive.view.rgba && out->emissive.name && out->emissive.file;
			s->emissive_layers++;
		}
		if (ok && any_mr) {
			out->metallic_roughness.view = (OptGltfImageView) { keep_pixels(s, in->mr, n * 4, sizes), w, h };
			in->mr                       = NULL;
			out->metallic_roughness.name = keep(s, "skin_%s_Tex%02d_mr", skin, (int)ti);
			out->metallic_roughness.file = keep(s, "skins/%s/%s_Tex%02d_mr.png", skin, basename, (int)ti);
			ok = out->metallic_roughness.view.rgba && out->metallic_roughness.name && out->metallic_roughness.file;
			s->metal_rough_layers++;
		}
		if (ok)
			s->layer_count++;
	}
	return ok;
}

/* Builds the skin layers of each skinnable texture and the variants: a
 * variant with layers becomes a skin variant; one whose skins have no image
 * stays a plain variant, which shows its state. */
static bool layers_build(XwauGltfSkins* s, const opt_file_t* opt, const char* basename,
						 const XwauSkinVariants* skins, const char* const* skin_names, const OptGltfScreen* screens,
						 int base_shade, const XwauGltfMaterial* xwau, const XwauGltfAnim* anims,
						 const XwauBaked* baked) {
	const size_t tc = (size_t)opt->texture_count;
	s->first_layer  = (size_t*)calloc(tc + 1, sizeof *s->first_layer);
	s->texture_layers = (size_t*)calloc(tc + 1, sizeof *s->texture_layers);
	s->skin_names   = (const char**)calloc(skins->skin_count + 1, sizeof *s->skin_names);
	s->layers       = (OptGltfSkinLayer*)calloc(skins->skin_count * tc + 1, sizeof *s->layers);
	size_t* sizes   = (size_t*)calloc(skins->skin_count * tc * 3 + 1, sizeof *sizes);
	bool*   has     = (bool*)calloc(skins->skin_count + 1, sizeof *has);
	bool    ok      = s->first_layer && s->texture_layers && s->skin_names && s->layers && sizes && has;
	for (size_t k = 0; ok && k < skins->skin_count; ++k)
		ok = (s->skin_names[k] = layer_skin_name(s, skin_names[k], s->skin_names, k)) != NULL;
	/* Skinnable textures: state 0 of LOD 0 face groups, not screens. */
	bool* skinnable = ok ? (bool*)calloc(tc + 1, sizeof *skinnable) : NULL;
	ok              = ok && skinnable;
	for (int32_t mi = 0; ok && mi < opt->mesh_count; ++mi) {
		const opt_lod_t* lod = &opt->meshes[mi].lods[0];
		for (int32_t g = 0; g < lod->group_count; ++g) {
			const int32_t t = lod->groups[g].texture_index;
			if (lod->groups[g].face_count > 0 && t >= 0 && t < opt->texture_count && baked[t].base &&
				!(screens && screens[t].present))
				skinnable[t] = true;
		}
	}
	LayerCandidate* candidates =
		ok ? (LayerCandidate*)calloc(skins->skin_count + 1, sizeof *candidates) : NULL;
	ok = ok && candidates;
	for (size_t t = 0; ok && t < tc; ++t) {
		s->first_layer[t] = s->layer_count;
		size_t count      = 0;
		for (size_t k = 0; ok && skinnable[t] && k < skins->skin_count; ++k) {
			const OptGltfImageView* img = &skins->images[k * tc + t];
			if (!img->rgba)
				continue;
			candidates[count].skin = k;
			ok = layer_bake(opt, base_shade, xwau, anims, &baked[t], (int32_t)t, img, &candidates[count]);
			if (ok) {
				count++;
				has[k] = true;
			}
		}
		const opt_texture_t* tex = &opt->textures[t];
		ok = ok && layers_finish(s, basename, (int32_t)t, (uint32_t)tex->width, (uint32_t)tex->height, candidates,
								 count, sizes);
		s->texture_layers[t] = s->layer_count - s->first_layer[t];
		for (size_t c = 0; c < count; ++c)
			candidate_free(&candidates[c]);
	}
	free(candidates);
	free(skinnable);
	free(sizes);

	/* The variants. */
	size_t total_layers = 0;
	for (size_t v = 0; v < skins->variant_count; ++v)
		total_layers += skins->variants[v].layer_count;
	s->skin_variants     = (OptGltfSkinVariant*)calloc(skins->variant_count + 1, sizeof *s->skin_variants);
	s->skin_layers       = (OptGltfSkinVariantLayer*)calloc(total_layers + 1, sizeof *s->skin_layers);
	s->variants          = (OptGltfVariant*)calloc(skins->variant_count + 1, sizeof *s->variants);
	s->plain_material_of = (int32_t*)malloc((skins->variant_count * tc + 1) * sizeof *s->plain_material_of);
	ok = ok && s->skin_variants && s->skin_layers && s->variants && s->plain_material_of;
	size_t skin_variant_count = 0, plain_count = 0, used_layers = 0;
	for (size_t v = 0; ok && v < skins->variant_count; ++v) {
		const XwauSkinVariant* in    = &skins->variants[v];
		OptGltfSkinVariantLayer* first = &s->skin_layers[used_layers];
		size_t                   count = 0;
		for (size_t l = 0; l < in->layer_count; ++l)
			if (has[in->layers[l].skin])
				first[count++] = (OptGltfSkinVariantLayer) { s->skin_names[in->layers[l].skin], in->layers[l].opacity };
		used_layers += count;
		if (count)
			s->skin_variants[skin_variant_count++] = (OptGltfSkinVariant) { in->name, in->state, first, count };
		else
			s->variants[plain_count++] = (OptGltfVariant) { in->name, in->state };
	}
	for (size_t i = 0; ok && i < plain_count * tc; ++i)
		s->plain_material_of[i] = -1;
	free(has);
	if (!ok)
		return false;
	s->out = (OptGltfVariants) {
		.variants           = s->variants,
		.count              = plain_count,
		.material_of        = s->plain_material_of,
		.skin_variants      = s->skin_variants,
		.skin_variant_count = skin_variant_count,
		.default_markings   = skins->default_markings,
		.markings_count     = skins->markings_count,
	};
	return true;
}

const OptGltfSkinLayer* XwauGltfSkins_Layers(const XwauGltfSkins* s, int32_t texture, size_t* count) {
	*count = s ? s->texture_layers[texture] : 0;
	return *count ? &s->layers[s->first_layer[texture]] : NULL;
}

XwauGltfSkins* XwauGltfSkins_Build(const opt_file_t* opt, const char* basename, const XwauSkinVariants* skins,
								   const char* const* skin_names, const OptGltfScreen* screens, int base_shade,
								   const XwauGltfMaterial* materials, const XwauGltfAnim* anims,
								   const XwauBaked* baked) {
	if (!skin_validate(skins, basename))
		return NULL;
	XwauGltfSkins* s = (XwauGltfSkins*)calloc(1, sizeof *s);
	if (!s || !layers_build(s, opt, basename, skins, skin_names, screens, base_shade, materials, anims, baked)) {
		Report(NULL, "out of memory (skins)");
		XwauGltfSkins_Free(s);
		return NULL;
	}
	return s;
}

const OptGltfVariants* XwauGltfSkins_Variants(const XwauGltfSkins* s) { return &s->out; }

void XwauGltfSkins_Report(const XwauGltfSkins* s, const char* basename) {
	Report(basename,
		   "%zu skin variants (%zu plain), %zu skin layers (%zu emissive, %zu metallic-roughness), "
		   "%zu shared images, %zu markings",
		   s->out.skin_variant_count + s->out.count, s->out.count, s->layer_count, s->emissive_layers,
		   s->metal_rough_layers, s->shared_images, s->out.markings_count);
}

void XwauGltfSkins_Free(XwauGltfSkins* s) {
	if (!s)
		return;
	free(s->variants);
	free(s->layers);
	free(s->first_layer);
	free(s->texture_layers);
	for (size_t i = 0; i < s->pixel_count; ++i)
		free(s->pixels[i]);
	free(s->pixels);
	free(s->skin_names);
	free(s->skin_variants);
	free(s->skin_layers);
	free(s->plain_material_of);
	for (size_t i = 0; i < s->string_count; ++i)
		free(s->strings[i]);
	free(s->strings);
	free(s);
}
