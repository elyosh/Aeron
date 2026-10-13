#include "xwau_gltf_skins.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"

/* One composite: texture `texture` with the layer chain `chain`, shared by
 * `variants`. The chain holds the layers of a variant that have an image
 * for the texture. */
typedef struct SkinComposite {
	int32_t        texture;
	XwauSkinLayer* chain;
	size_t         chain_count;
	XwauBaked      baked;
	/* Set when the baked emissive, metallic-roughness or transmission is
	 * equal to that of the texture. The composite material then uses the
	 * image of the texture, and the baked copy is freed. */
	bool      same_emissive;
	bool      same_metal_rough;
	bool      same_transmission;
	uint32_t* variants; /* skin variant indices, ascending */
	size_t    variant_count;
	uint8_t*  colour; /* colours before the bake, for XwauGltf_AnimBase */
} SkinComposite;

typedef struct SkinState {
	SkinComposite* composites;
	size_t         count;
	/* [skin variant * texture_count + texture]: composite index or -1. */
	int32_t* composite_of;
} SkinState;

static void skin_state_free(SkinState* state) {
	for (size_t c = 0; c < state->count; ++c) {
		free(state->composites[c].chain);
		free(state->composites[c].variants);
		free(state->composites[c].colour);
		XwauGltf_BakedFree(&state->composites[c].baked);
	}
	free(state->composites);
	free(state->composite_of);
	memset(state, 0, sizeof *state);
}

/* Same as CombineTextures in the hook: blends with straight alpha, in
 * integer arithmetic, onto the color channels. The destination alpha is
 * kept. */
static void skin_blend(uint8_t* dst, const OptGltfImageView* src, uint32_t opacity) {
	const size_t n = (size_t)src->width * src->height;
	for (size_t i = 0; i < n; ++i) {
		const int a = ((int)src->rgba[i * 4 + 3] * (int)opacity + 50) / 100;
		for (int k = 0; k < 3; ++k)
			dst[i * 4 + k] = (uint8_t)(dst[i * 4 + k] * (255 - a) / 255 + src->rgba[i * 4 + k] * a / 255);
	}
}

static bool skin_chain_equal(const SkinComposite* c, int32_t texture, const XwauSkinLayer* chain,
							 size_t count) {
	if (c->texture != texture || c->chain_count != count)
		return false;
	for (size_t i = 0; i < count; ++i)
		if (c->chain[i].skin != chain[i].skin || c->chain[i].opacity != chain[i].opacity)
			return false;
	return true;
}

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

/* Finds and bakes the composites. `baked` holds the bake of each texture.
 * Textures without pixels and screen surfaces get no skins. */
static bool skin_state_build(const opt_file_t* opt, const XwauSkinVariants* skins,
							 const OptGltfScreen* screens, int base_shade, const XwauGltfMaterial* xwau,
							 const XwauGltfAnim* anim, const XwauBaked* baked, SkinState* state) {
	memset(state, 0, sizeof *state);
	const size_t texture_count = (size_t)opt->texture_count;
	bool*        skinnable     = (bool*)calloc(texture_count ? texture_count : 1, sizeof *skinnable);
	state->composite_of =
		(int32_t*)malloc((skins->variant_count * texture_count ? skins->variant_count * texture_count : 1) *
						 sizeof *state->composite_of);
	size_t max_layers = 1;
	for (size_t v = 0; v < skins->variant_count; ++v)
		if (skins->variants[v].layer_count > max_layers)
			max_layers = skins->variants[v].layer_count;
	XwauSkinLayer* chain = (XwauSkinLayer*)malloc(max_layers * sizeof *chain);
	if (!skinnable || !state->composite_of || !chain)
		goto fail;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi) {
		const opt_lod_t* lod = &opt->meshes[mi].lods[0];
		for (int32_t g = 0; g < lod->group_count; ++g) {
			const int32_t t = lod->groups[g].texture_index;
			if (lod->groups[g].face_count > 0 && t >= 0 && t < opt->texture_count && baked[t].base &&
				!(screens && screens[t].present))
				skinnable[t] = true;
		}
	}
	for (size_t v = 0; v < skins->variant_count; ++v) {
		const XwauSkinVariant* variant = &skins->variants[v];
		for (size_t t = 0; t < texture_count; ++t) {
			int32_t* slot = &state->composite_of[v * texture_count + t];
			*slot         = -1;
			if (!skinnable[t])
				continue;
			size_t count = 0;
			for (size_t l = 0; l < variant->layer_count; ++l)
				if (skins->images[variant->layers[l].skin * texture_count + t].rgba)
					chain[count++] = variant->layers[l];
			if (count == 0)
				continue;
			size_t c = 0;
			while (c < state->count && !skin_chain_equal(&state->composites[c], (int32_t)t, chain, count))
				++c;
			if (c == state->count) {
				SkinComposite* added = ARRAY_PUSH(state->composites, state->count);
				if (!added)
					goto fail;
				added->texture     = (int32_t)t;
				added->chain       = (XwauSkinLayer*)malloc(count * sizeof *added->chain);
				added->chain_count = count;
				added->variants    = (uint32_t*)malloc(skins->variant_count * sizeof *added->variants);
				if (!added->chain || !added->variants)
					goto fail;
				memcpy(added->chain, chain, count * sizeof *chain);
			}
			state->composites[c].variants[state->composites[c].variant_count++] = (uint32_t)v;
			*slot                                                               = (int32_t)c;
		}
	}
	for (size_t c = 0; c < state->count; ++c) {
		SkinComposite*       composite = &state->composites[c];
		const opt_texture_t* t         = &opt->textures[composite->texture];
		const XwauBaked*     own       = &baked[composite->texture];
		uint8_t*             rgba      = OptGltf_DecodeTexture(t, base_shade);
		if (!rgba)
			goto fail;
		for (size_t l = 0; l < composite->chain_count; ++l)
			skin_blend(rgba,
					   &skins->images[composite->chain[l].skin * texture_count + (size_t)composite->texture],
					   composite->chain[l].opacity);
		const XwauGltfAnim* a = anim ? &anim[composite->texture] : NULL;
		if (a && a->anim_base) {
			composite->colour = XwauGltf_RgbaDup(rgba, (size_t)t->width * (size_t)t->height);
			if (!composite->colour) {
				free(rgba);
				goto fail;
			}
		}
		if (!XwauGltf_BakeRgba(t, &xwau[composite->texture], rgba, &composite->baked))
			goto fail;
		XwauGltf_AnimBase(&composite->colour, (uint32_t)t->width, (uint32_t)t->height, a, &composite->baked);
		const size_t bytes  = (size_t)t->width * (size_t)t->height * 4u;
		XwauBaked*   result = &composite->baked;
		composite->same_emissive =
			(!result->emissive && !own->emissive) ||
			(result->emissive && own->emissive && memcmp(result->emissive, own->emissive, bytes) == 0);
		composite->same_metal_rough =
			(!result->metal_rough && !own->metal_rough && result->metallic == own->metallic &&
			 result->roughness == own->roughness) ||
			(result->metal_rough && own->metal_rough &&
			 memcmp(result->metal_rough, own->metal_rough, bytes) == 0);
		if (composite->same_emissive) {
			free(result->emissive);
			result->emissive = NULL;
		}
		if (composite->same_metal_rough) {
			free(result->metal_rough);
			result->metal_rough = NULL;
		}
		composite->same_transmission = (!result->transmission && !own->transmission) ||
									   (result->transmission && own->transmission &&
										memcmp(result->transmission, own->transmission, bytes) == 0);
		if (composite->same_transmission) {
			free(result->transmission);
			result->transmission = NULL;
		}
	}
	free(skinnable);
	free(chain);
	return true;

fail:
	free(skinnable);
	free(chain);
	skin_state_free(state);
	return false;
}

/* ---- Variants ----------------------------------------------------------- */

struct XwauGltfSkins {
	SkinState               state;
	OptGltfVariant*         variants;
	OptGltfVariantMaterial* materials;
	OptGltfVariants         out;
	char**                  strings; /* image names, files and suffixes */
	size_t                  string_count;
	size_t                  own_emissive;
	size_t                  own_metal_rough;
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

/* Sets image `kind` of composite `c` ("" for its base image). */
static bool composite_image(XwauGltfSkins* s, const char* basename, size_t c, int32_t texture,
							const char* kind, uint8_t* rgba, uint32_t w, uint32_t h, OptGltfImage* out) {
	out->view = (OptGltfImageView) { rgba, w, h };
	out->name = keep(s, "Tex%02d_skin%03zu%s", (int)texture, c, kind);
	out->file = keep(s, "%s_Tex%02d_skin%03zu%s.png", basename, (int)texture, c, kind);
	return out->name && out->file;
}

/* Builds the material of composite `c`. It is a copy of the material of
 * its texture, with the base color of the composite. Where the emissive,
 * metallic-roughness or transmission differ, it uses its own. */
static bool composite_material(XwauGltfSkins* s, const char* basename, size_t c, const OptGltfMaterial* own,
							   const XwauGltfAnim* anims, OptGltfVariantMaterial* out) {
	SkinComposite*   composite = &s->state.composites[c];
	XwauBaked*       baked     = &composite->baked;
	const int32_t    ti        = composite->texture;
	const uint32_t   w = own->base.view.width, h = own->base.view.height;
	OptGltfMaterial* m = &out->material;
	out->texture       = ti;
	out->suffix        = keep(s, "_skin%03zu", c);
	bool ok            = out->suffix && composite_image(s, basename, c, ti, "", baked->base, w, h, &m->base);
	m->base_color[0] = m->base_color[1] = m->base_color[2] = 1.0f;
	if (composite->same_emissive) {
		m->emissive = own->emissive;
		memcpy(m->emissive_factor, own->emissive_factor, sizeof m->emissive_factor);
		m->emissive_strength = own->emissive_strength;
	} else if (baked->emissive) {
		ok &= composite_image(s, basename, c, ti, "_emissive", baked->emissive, w, h, &m->emissive);
		m->emissive_factor[0] = m->emissive_factor[1] = m->emissive_factor[2] = 1.0f;
		m->emissive_strength                                                  = XWAU_GLTF_ILLUM_GAIN;
		s->own_emissive++;
	}
	if (composite->same_metal_rough) {
		m->metallic_roughness = own->metallic_roughness;
		m->metallic           = own->metallic;
		m->roughness          = own->roughness;
	} else if (baked->metal_rough) {
		ok &= composite_image(s, basename, c, ti, "_mr", baked->metal_rough, w, h, &m->metallic_roughness);
		m->metallic = m->roughness = 1.0f;
		s->own_metal_rough++;
	} else {
		m->metallic  = baked->metallic;
		m->roughness = baked->roughness;
	}
	if (composite->same_transmission)
		m->transmission = own->transmission;
	else if (baked->transmission)
		ok &=
			composite_image(s, basename, c, ti, "_transmission", baked->transmission, w, h, &m->transmission);
	m->normal         = own->normal;
	m->specular       = true;
	m->specular_color = baked->specular_color;
	m->json           = anims ? anims[ti].json : NULL;
	return ok;
}

XwauGltfSkins* XwauGltfSkins_Build(const opt_file_t* opt, const char* basename, const XwauSkinVariants* skins,
								   const OptGltfScreen* screens, int base_shade,
								   const XwauGltfMaterial* materials, const XwauGltfAnim* anims,
								   const XwauBaked* baked, const OptGltfMaterial* textures) {
	if (!skin_validate(skins, basename))
		return NULL;
	XwauGltfSkins* s = (XwauGltfSkins*)calloc(1, sizeof *s);
	if (!s || !skin_state_build(opt, skins, screens, base_shade, materials, anims, baked, &s->state)) {
		Report(NULL, "out of memory (skins)");
		free(s);
		return NULL;
	}
	s->variants =
		(OptGltfVariant*)calloc(skins->variant_count ? skins->variant_count : 1, sizeof *s->variants);
	s->materials = (OptGltfVariantMaterial*)calloc(s->state.count ? s->state.count : 1, sizeof *s->materials);
	bool ok      = s->variants && s->materials;
	for (size_t v = 0; ok && v < skins->variant_count; ++v)
		s->variants[v] = (OptGltfVariant) { skins->variants[v].name, skins->variants[v].state };
	for (size_t c = 0; ok && c < s->state.count; ++c)
		ok = composite_material(s, basename, c, &textures[s->state.composites[c].texture], anims,
								&s->materials[c]);
	if (!ok) {
		Report(NULL, "out of memory (skins)");
		XwauGltfSkins_Free(s);
		return NULL;
	}
	s->out = (OptGltfVariants) {
		.variants         = s->variants,
		.count            = skins->variant_count,
		.materials        = s->materials,
		.material_count   = s->state.count,
		.material_of      = s->state.composite_of,
		.default_markings = skins->default_markings,
		.markings_count   = skins->markings_count,
	};
	return s;
}

const OptGltfVariants* XwauGltfSkins_Variants(const XwauGltfSkins* s) { return &s->out; }

void XwauGltfSkins_Report(const XwauGltfSkins* s, const char* basename) {
	Report(basename,
		   "%zu skin variants, %zu composites (%zu own emissive, %zu own metallic-roughness), "
		   "%zu markings",
		   s->out.count, s->state.count, s->own_emissive, s->own_metal_rough, s->out.markings_count);
}

void XwauGltfSkins_Free(XwauGltfSkins* s) {
	if (!s)
		return;
	skin_state_free(&s->state);
	free(s->variants);
	free(s->materials);
	for (size_t i = 0; i < s->string_count; ++i)
		free(s->strings[i]);
	free(s->strings);
	free(s);
}
