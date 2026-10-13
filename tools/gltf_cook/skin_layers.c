/*
 * Skin layers: expansion of `skins` and `skinVariants` into composite
 * materials and KHR_materials_variants variants (skin_layers.h).
 */

#include "skin_layers.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cgltf.h"
#include "png_read.h"

#define TAG "[aeron_gltf_cook] skins"
#define COMPOSITE_SCHEME "aeron-composite:"

enum { CH_BASE, CH_EMISSIVE, CH_MR, CH_COUNT };

static const char* const kChannelName[CH_COUNT] = { "base", "emissive", "mr" };

/* ---- Inputs ---------------------------------------------------------- */

/* A skin on one material: the image of each channel (-1: none). */
typedef struct MaterialSkin {
	char* name;
	int   image[CH_COUNT];
	float emissive_strength; /* < 0: not given */
} MaterialSkin;

typedef struct SkinLayer {
	char* skin;
	int   opacity;
} SkinLayer;

typedef struct SkinVariant {
	char*      name;
	int        base; /* variant index of the base, -1 for none */
	SkinLayer* layers;
	size_t     layer_count;
} SkinVariant;

/* ---- Composites ------------------------------------------------------ */

typedef struct LayerSource {
	int   image;    /* the layer's image for the channel */
	int   coverage; /* its base color image: alpha is the coverage */
	int   opacity;
	float scale; /* emissive: its strength over the composite's */
} LayerSource;

typedef struct CompositeImage {
	int          channel;
	int          own_image; /* -1: the uniform own_value */
	uint8_t      own_value[4];
	float        own_scale;
	LayerSource* layers;
	size_t       layer_count;
	char*        label;
	uint8_t*     rgba;
	int          w, h;
	bool         done, failed;
} CompositeImage;

typedef struct LoadedImage {
	int      image;
	uint8_t* rgba;
	int      w, h;
} LoadedImage;

typedef struct CompositeMaterial {
	int   material; /* the source material */
	char* key;      /* its layers, as skin:opacity,... */
	int   index;    /* the composite material */
} CompositeMaterial;

struct SkinLayers {
	char*  gltf_path;
	bool   verbose;
	char** uris; /* the file's image URIs, by image index */
	size_t uri_count;

	CompositeImage* composites;
	size_t          composite_count;
	LoadedImage**   loaded; /* each allocated alone, so that pointers to them stay valid */
	size_t          loaded_count;
	double          seconds;
	size_t          material_count; /* composite materials */
	size_t          variant_count;
};

static char* dup_string(const char* s) {
	const size_t n    = strlen(s) + 1;
	char*        copy = (char*)malloc(n);
	if (copy)
		memcpy(copy, s, n);
	return copy;
}

static bool grow(void** items, size_t count, size_t* capacity, size_t size) {
	if (count < *capacity)
		return true;
	const size_t cap   = *capacity ? *capacity * 2 : 8;
	void*        grown = realloc(*items, cap * size);
	if (!grown)
		return false;
	*items    = grown;
	*capacity = cap;
	return true;
}

static const cJSON* get(const cJSON* object, const char* key) {
	return object ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
}

static cJSON* get_or_add_object(cJSON* parent, const char* key) {
	cJSON* item = cJSON_GetObjectItemCaseSensitive(parent, key);
	if (!item)
		item = cJSON_AddObjectToObject(parent, key);
	return item;
}

static cJSON* get_or_add_array(cJSON* parent, const char* key) {
	cJSON* item = cJSON_GetObjectItemCaseSensitive(parent, key);
	if (!item)
		item = cJSON_AddArrayToObject(parent, key);
	return item;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
static bool fail(const char* format, ...) {
	va_list args;
	va_start(args, format);
	fprintf(stderr, TAG ": ");
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
	va_end(args);
	return false;
}

static bool valid_skin_name(const char* name) {
	const size_t n = strlen(name);
	if (n < 1 || n > 63)
		return false;
	for (size_t i = 0; i < n; ++i) {
		const char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return false;
	}
	return true;
}

/* The image of the texture reference `info`, or -1 when absent; -2 when
 * invalid. */
static int texture_image(const cJSON* document, const cJSON* info) {
	if (!info)
		return -1;
	const cJSON* index    = get(info, "index");
	const cJSON* textures = get(document, "textures");
	if (!cJSON_IsNumber(index) || index->valueint < 0 || index->valueint >= cJSON_GetArraySize(textures))
		return -2;
	const cJSON* source = get(cJSON_GetArrayItem(textures, index->valueint), "source");
	if (!cJSON_IsNumber(source) || source->valueint < 0 ||
		source->valueint >= cJSON_GetArraySize(get(document, "images")))
		return -2;
	return source->valueint;
}

static cJSON* flight_model(cJSON* object) { return (cJSON*)get(get(object, "extensions"), "AERON_flight_model"); }

/* The model root: the default scene's first node. */
static cJSON* model_root(cJSON* document) {
	const cJSON* scene_index = get(document, "scene");
	const int    s           = cJSON_IsNumber(scene_index) ? scene_index->valueint : 0;
	const cJSON* nodes       = get(cJSON_GetArrayItem(get(document, "scenes"), s), "nodes");
	const cJSON* first       = cJSON_GetArrayItem(nodes, 0);
	return cJSON_IsNumber(first) ? cJSON_GetArrayItem(get(document, "nodes"), first->valueint) : NULL;
}

bool SkinLayers_Present(const cJSON* document) {
	const cJSON* material;
	cJSON_ArrayForEach(material, get(document, "materials")) {
		if (get(flight_model((cJSON*)material), "skins"))
			return true;
	}
	return get(flight_model(model_root((cJSON*)document)), "skinVariants") != NULL;
}

/* ---- Reading --------------------------------------------------------- */

typedef struct Inputs {
	MaterialSkin** skins; /* per material */
	size_t*        skin_count;
	size_t         material_count;
	SkinVariant*   variants;
	size_t         variant_count;
	char**         khr_names;
	size_t         khr_count;
} Inputs;

static void inputs_free(Inputs* in) {
	for (size_t m = 0; in->skins && m < in->material_count; ++m) {
		for (size_t s = 0; s < in->skin_count[m]; ++s)
			free(in->skins[m][s].name);
		free(in->skins[m]);
	}
	free(in->skins);
	free(in->skin_count);
	for (size_t v = 0; v < in->variant_count; ++v) {
		for (size_t l = 0; l < in->variants[v].layer_count; ++l)
			free(in->variants[v].layers[l].skin);
		free(in->variants[v].layers);
		free(in->variants[v].name);
	}
	free(in->variants);
	free(in->khr_names);
}

static const MaterialSkin* material_skin(const Inputs* in, size_t m, const char* name) {
	for (size_t s = 0; s < in->skin_count[m]; ++s)
		if (strcmp(in->skins[m][s].name, name) == 0)
			return &in->skins[m][s];
	return NULL;
}

static const char* material_name(const cJSON* document, size_t m) {
	const cJSON* name = get(cJSON_GetArrayItem(get(document, "materials"), (int)m), "name");
	return cJSON_IsString(name) ? name->valuestring : "<unnamed>";
}

static bool read_material_skins(const cJSON* document, Inputs* in) {
	const cJSON* materials = get(document, "materials");
	in->material_count     = (size_t)cJSON_GetArraySize(materials);
	in->skins              = (MaterialSkin**)calloc(in->material_count + 1, sizeof *in->skins);
	in->skin_count         = (size_t*)calloc(in->material_count + 1, sizeof *in->skin_count);
	if (!in->skins || !in->skin_count)
		return fail("out of memory");
	for (size_t m = 0; m < in->material_count; ++m) {
		const cJSON* skins = get(flight_model(cJSON_GetArrayItem(materials, (int)m)), "skins");
		if (!skins)
			continue;
		if (!cJSON_IsObject(skins))
			return fail("material '%s': skins is not an object", material_name(document, m));
		const size_t count = (size_t)cJSON_GetArraySize(skins);
		in->skins[m]       = (MaterialSkin*)calloc(count + 1, sizeof *in->skins[m]);
		if (!in->skins[m])
			return fail("out of memory");
		const cJSON* skin;
		cJSON_ArrayForEach(skin, skins) {
			MaterialSkin* out = &in->skins[m][in->skin_count[m]++];
			out->name         = dup_string(skin->string);
			if (!out->name)
				return fail("out of memory");
			if (!valid_skin_name(skin->string))
				return fail("material '%s': invalid skin name '%s'", material_name(document, m), skin->string);
			out->image[CH_BASE]     = texture_image(document, get(skin, "baseColorTexture"));
			out->image[CH_EMISSIVE] = texture_image(document, get(skin, "emissiveTexture"));
			out->image[CH_MR]       = texture_image(document, get(skin, "metallicRoughnessTexture"));
			if (out->image[CH_BASE] < 0 || out->image[CH_EMISSIVE] == -2 || out->image[CH_MR] == -2)
				return fail("material '%s', skin '%s': missing or invalid texture", material_name(document, m),
							skin->string);
			const cJSON* strength  = get(skin, "emissiveStrength");
			out->emissive_strength = cJSON_IsNumber(strength) ? (float)strength->valuedouble : -1.0f;
			if (strength && (!cJSON_IsNumber(strength) || strength->valuedouble < 0.0))
				return fail("material '%s', skin '%s': emissiveStrength is a number >= 0",
							material_name(document, m), skin->string);
		}
	}
	return true;
}

static int variant_index(const Inputs* in, const char* name) {
	for (size_t v = 0; v < in->khr_count; ++v)
		if (strcmp(in->khr_names[v], name) == 0)
			return (int)v;
	return -1;
}

static bool read_variants(cJSON* document, Inputs* in) {
	const cJSON* khr = get(get(get(document, "extensions"), "KHR_materials_variants"), "variants");
	in->khr_count    = (size_t)cJSON_GetArraySize(khr);
	in->khr_names    = (char**)calloc(in->khr_count + 1, sizeof *in->khr_names);
	if (!in->khr_names)
		return fail("out of memory");
	for (size_t v = 0; v < in->khr_count; ++v) {
		const cJSON* name = get(cJSON_GetArrayItem(khr, (int)v), "name");
		in->khr_names[v]  = cJSON_IsString(name) ? name->valuestring : "";
	}
	const cJSON* list = get(flight_model(model_root(document)), "skinVariants");
	if (!list)
		return true;
	if (!cJSON_IsArray(list))
		return fail("skinVariants is not a list");
	in->variant_count = (size_t)cJSON_GetArraySize(list);
	in->variants      = (SkinVariant*)calloc(in->variant_count + 1, sizeof *in->variants);
	if (!in->variants)
		return fail("out of memory");
	for (size_t v = 0; v < in->variant_count; ++v) {
		const cJSON* item   = cJSON_GetArrayItem(list, (int)v);
		const cJSON* name   = get(item, "name");
		const cJSON* base   = get(item, "base");
		const cJSON* layers = get(item, "layers");
		SkinVariant* out    = &in->variants[v];
		if (!cJSON_IsString(name) || !name->valuestring[0])
			return fail("a skin variant has no name");
		out->name = dup_string(name->valuestring);
		if (!out->name)
			return fail("out of memory");
		if (variant_index(in, out->name) >= 0)
			return fail("skin variant '%s' has the name of a variant of the file", out->name);
		for (size_t w = 0; w < v; ++w)
			if (strcmp(in->variants[w].name, out->name) == 0)
				return fail("skin variant '%s' is repeated", out->name);
		out->base = -1;
		if (base) {
			if (!cJSON_IsString(base) || (out->base = variant_index(in, base->valuestring)) < 0)
				return fail("skin variant '%s': base '%s' is not a variant of the file", out->name,
							cJSON_IsString(base) ? base->valuestring : "?");
		}
		if (!cJSON_IsArray(layers) || cJSON_GetArraySize(layers) == 0)
			return fail("skin variant '%s' has no layers", out->name);
		out->layer_count = (size_t)cJSON_GetArraySize(layers);
		out->layers      = (SkinLayer*)calloc(out->layer_count, sizeof *out->layers);
		if (!out->layers)
			return fail("out of memory");
		for (size_t l = 0; l < out->layer_count; ++l) {
			const cJSON* layer   = cJSON_GetArrayItem(layers, (int)l);
			const cJSON* skin    = get(layer, "skin");
			const cJSON* opacity = get(layer, "opacity");
			if (!cJSON_IsString(skin))
				return fail("skin variant '%s': a layer has no skin", out->name);
			out->layers[l].skin    = dup_string(skin->valuestring);
			out->layers[l].opacity = opacity ? opacity->valueint : 100;
			if (!out->layers[l].skin)
				return fail("out of memory");
			if (opacity && (!cJSON_IsNumber(opacity) || opacity->valueint < 0 || opacity->valueint > 100))
				return fail("skin variant '%s': opacity of '%s' is 0 to 100", out->name, skin->valuestring);
			bool found = false;
			for (size_t m = 0; !found && m < in->material_count; ++m)
				found = material_skin(in, m, skin->valuestring) != NULL;
			if (!found)
				return fail("skin variant '%s': no material has skin '%s'", out->name, skin->valuestring);
		}
	}
	/* Skins no variant uses. */
	for (size_t m = 0; m < in->material_count; ++m)
		for (size_t s = 0; s < in->skin_count[m]; ++s) {
			bool used = false;
			for (size_t v = 0; !used && v < in->variant_count; ++v)
				for (size_t l = 0; !used && l < in->variants[v].layer_count; ++l)
					used = strcmp(in->variants[v].layers[l].skin, in->skins[m][s].name) == 0;
			if (!used)
				fprintf(stderr, TAG ": warning: material '%s': no skin variant uses skin '%s'\n",
						material_name(document, m), in->skins[m][s].name);
		}
	return true;
}

/* ---- Colour ---------------------------------------------------------- */

static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

static float linear_to_srgb(float c) {
	c = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static uint8_t unorm8(float x) {
	x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
	return (uint8_t)(x * 255.0f + 0.5f);
}

static float number_or(const cJSON* item, float fallback) {
	return cJSON_IsNumber(item) ? (float)item->valuedouble : fallback;
}

/* ---- Expansion ------------------------------------------------------- */

typedef struct Expansion {
	cJSON*             document;
	Inputs*            in;
	SkinLayers*        out;
	CompositeMaterial* materials;
	size_t             material_count, material_capacity;
	size_t             composite_capacity;
	bool               uses_strength;
} Expansion;

/* Adds an image with a composite URI, and a texture of it. Returns the
 * texture index, or -1. */
static int add_composite_texture(Expansion* x, size_t composite, const char* name, const cJSON* sampler) {
	cJSON* images   = get_or_add_array(x->document, "images");
	cJSON* textures = get_or_add_array(x->document, "textures");
	cJSON* image    = cJSON_CreateObject();
	cJSON* texture  = cJSON_CreateObject();
	char   uri[64];
	snprintf(uri, sizeof uri, COMPOSITE_SCHEME "%zu", composite);
	if (!images || !textures || !image || !texture || !cJSON_AddStringToObject(image, "uri", uri) ||
		!cJSON_AddStringToObject(image, "name", name) ||
		!cJSON_AddNumberToObject(texture, "source", cJSON_GetArraySize(images))) {
		cJSON_Delete(image);
		cJSON_Delete(texture);
		return -1;
	}
	if (cJSON_IsNumber(sampler))
		cJSON_AddNumberToObject(texture, "sampler", sampler->valueint);
	cJSON_AddItemToArray(images, image);
	cJSON_AddItemToArray(textures, texture);
	return cJSON_GetArraySize(textures) - 1;
}

/* The texture reference of the composite: the material's own reference of
 * the channel with the new index, or else the base color reference's
 * texture coordinates. */
static cJSON* composite_reference(const cJSON* own, const cJSON* base, int texture) {
	cJSON* info = NULL;
	if (own) {
		info = cJSON_Duplicate(own, true);
		cJSON_DeleteItemFromObjectCaseSensitive(info, "index");
		cJSON_DeleteItemFromObjectCaseSensitive(info, "scale");
		cJSON_DeleteItemFromObjectCaseSensitive(info, "strength");
	} else {
		info = cJSON_CreateObject();
		const cJSON* tex_coord = get(base, "texCoord");
		const cJSON* transform = get(get(base, "extensions"), "KHR_texture_transform");
		if (info && tex_coord)
			cJSON_AddItemToObject(info, "texCoord", cJSON_Duplicate(tex_coord, true));
		if (info && transform)
			cJSON_AddItemToObject(get_or_add_object(info, "extensions"), "KHR_texture_transform",
								  cJSON_Duplicate(transform, true));
	}
	if (info)
		cJSON_AddNumberToObject(info, "index", texture);
	return info;
}

static const cJSON* reference_sampler(const cJSON* document, const cJSON* info) {
	const cJSON* index = get(info, "index");
	return cJSON_IsNumber(index) ? get(cJSON_GetArrayItem(get(document, "textures"), index->valueint), "sampler")
								 : NULL;
}

/* Builds the composite image of channel `ch` of `mat` (a copy of material
 * m) for `chain`, and points the copy at it. */
static bool composite_channel(Expansion* x, cJSON* mat, size_t m, const MaterialSkin* const* skins,
							  const SkinLayer* chain, size_t count, int ch, const char* name) {
	size_t with = 0;
	for (size_t l = 0; l < count; ++l)
		with += skins[l]->image[ch] >= 0;
	if (!with)
		return true;

	cJSON*       pbr       = get_or_add_object(mat, "pbrMetallicRoughness");
	const cJSON* base_info = get(pbr, "baseColorTexture");
	cJSON*       own_info  = (cJSON*)(ch == CH_BASE		 ? base_info
									  : ch == CH_EMISSIVE ? get(mat, "emissiveTexture")
														  : get(pbr, "metallicRoughnessTexture"));
	const int    own_image = texture_image(x->document, own_info);
	if (own_image == -2)
		return fail("material '%s': invalid %s texture", material_name(x->document, m), kChannelName[ch]);

	if (!grow((void**)&x->out->composites, x->out->composite_count, &x->composite_capacity,
			  sizeof *x->out->composites))
		return fail("out of memory");
	const size_t    index = x->out->composite_count;
	CompositeImage* c     = &x->out->composites[index];
	memset(c, 0, sizeof *c);
	c->channel   = ch;
	c->own_image = own_image;
	c->own_scale = 1.0f;
	c->layers    = (LayerSource*)calloc(with, sizeof *c->layers);
	c->label     = (char*)malloc(strlen(name) + 16);
	if (!c->layers || !c->label)
		return fail("out of memory");
	snprintf(c->label, strlen(name) + 16, "%s %s", name, kChannelName[ch]);
	x->out->composite_count++;

	/* The own value of a channel without its own image, encoded as the
	 * channel's images are; the composite then uses factor 1. */
	c->own_value[3] = 255;
	if (ch == CH_BASE) {
		const cJSON* factor = get(pbr, "baseColorFactor");
		for (int k = 0; k < 3; ++k)
			c->own_value[k] = unorm8(linear_to_srgb(number_or(cJSON_GetArrayItem(factor, k), 1.0f)));
	} else if (ch == CH_EMISSIVE) {
		const cJSON* factor = get(mat, "emissiveFactor");
		for (int k = 0; k < 3; ++k)
			c->own_value[k] = unorm8(linear_to_srgb(number_or(cJSON_GetArrayItem(factor, k), 0.0f)));
	} else {
		c->own_value[0] = 255;
		c->own_value[1] = unorm8(number_or(get(pbr, "roughnessFactor"), 1.0f));
		c->own_value[2] = unorm8(number_or(get(pbr, "metallicFactor"), 1.0f));
	}

	/* Emissive strength: the material's, else the first emissive layer's,
	 * else 1. Sources with another strength are scaled to it. */
	float  strength     = 1.0f;
	cJSON* strength_ext = (cJSON*)get(get(mat, "extensions"), "KHR_materials_emissive_strength");
	const bool own_has  = strength_ext != NULL;
	const float own_strength = own_has ? number_or(get(strength_ext, "emissiveStrength"), 1.0f) : 1.0f;
	if (ch == CH_EMISSIVE) {
		strength = own_strength;
		if (!own_has)
			for (size_t l = 0; l < count; ++l)
				if (skins[l]->image[CH_EMISSIVE] >= 0) {
					strength = skins[l]->emissive_strength >= 0.0f ? skins[l]->emissive_strength : 1.0f;
					break;
				}
		c->own_scale = strength > 0.0f ? own_strength / strength : 1.0f;
	}
	for (size_t l = 0; l < count; ++l) {
		if (skins[l]->image[ch] < 0)
			continue;
		LayerSource* s = &c->layers[c->layer_count++];
		s->image       = skins[l]->image[ch];
		s->coverage    = skins[l]->image[CH_BASE];
		s->opacity     = chain[l].opacity;
		s->scale       = 1.0f;
		if (ch == CH_EMISSIVE && strength > 0.0f) {
			const float layer = skins[l]->emissive_strength >= 0.0f ? skins[l]->emissive_strength : own_strength;
			s->scale          = layer / strength;
		}
	}

	char texture_name[512];
	snprintf(texture_name, sizeof texture_name, "%s_%s", name, kChannelName[ch]);
	const int texture = add_composite_texture(x, index, texture_name,
											  reference_sampler(x->document, own_info ? own_info : base_info));
	cJSON*    info    = texture >= 0 ? composite_reference(own_info, base_info, texture) : NULL;
	if (!info)
		return fail("out of memory");
	if (ch == CH_BASE) {
		cJSON_ReplaceItemInObjectCaseSensitive(pbr, "baseColorTexture", info);
		if (!base_info)
			cJSON_AddItemToObject(pbr, "baseColorTexture", info);
		if (own_image < 0) {
			cJSON*      factor = (cJSON*)get(pbr, "baseColorFactor");
			const float alpha  = number_or(cJSON_GetArrayItem(factor, 3), 1.0f);
			cJSON_DeleteItemFromObjectCaseSensitive(pbr, "baseColorFactor");
			const double rgba[4] = { 1.0, 1.0, 1.0, alpha };
			cJSON_AddItemToObject(pbr, "baseColorFactor", cJSON_CreateDoubleArray(rgba, 4));
		}
	} else if (ch == CH_EMISSIVE) {
		if (own_info)
			cJSON_ReplaceItemInObjectCaseSensitive(mat, "emissiveTexture", info);
		else
			cJSON_AddItemToObject(mat, "emissiveTexture", info);
		if (own_image < 0) {
			cJSON_DeleteItemFromObjectCaseSensitive(mat, "emissiveFactor");
			const double one[3] = { 1.0, 1.0, 1.0 };
			cJSON_AddItemToObject(mat, "emissiveFactor", cJSON_CreateDoubleArray(one, 3));
		}
		if (strength != 1.0f || own_has) {
			cJSON* ext = get_or_add_object(get_or_add_object(mat, "extensions"), "KHR_materials_emissive_strength");
			cJSON_DeleteItemFromObjectCaseSensitive(ext, "emissiveStrength");
			cJSON_AddNumberToObject(ext, "emissiveStrength", strength);
			x->uses_strength = true;
		}
	} else {
		if (own_info)
			cJSON_ReplaceItemInObjectCaseSensitive(pbr, "metallicRoughnessTexture", info);
		else
			cJSON_AddItemToObject(pbr, "metallicRoughnessTexture", info);
		if (own_image < 0) {
			cJSON_DeleteItemFromObjectCaseSensitive(pbr, "metallicFactor");
			cJSON_DeleteItemFromObjectCaseSensitive(pbr, "roughnessFactor");
		}
	}
	return true;
}

/* The composite material of material m with `chain` (the variant's layers
 * that m has), created when new. Returns its index, or -1. */
static int composite_material(Expansion* x, size_t m, const SkinLayer* chain, size_t count) {
	size_t key_size = 32;
	for (size_t l = 0; l < count; ++l)
		key_size += strlen(chain[l].skin) + 8;
	char* key = (char*)malloc(key_size);
	if (!key)
		return -1;
	key[0] = 0;
	for (size_t l = 0; l < count; ++l) {
		const size_t used = strlen(key);
		snprintf(key + used, key_size - used, "%s%s:%d", l ? "," : "", chain[l].skin, chain[l].opacity);
	}
	for (size_t i = 0; i < x->material_count; ++i)
		if (x->materials[i].material == (int)m && strcmp(x->materials[i].key, key) == 0) {
			free(key);
			return x->materials[i].index;
		}

	cJSON*       materials = (cJSON*)get(x->document, "materials");
	cJSON*       mat       = cJSON_Duplicate(cJSON_GetArrayItem(materials, (int)m), true);
	const size_t number    = x->material_count;
	char         name[384];
	snprintf(name, sizeof name, "%s_skin%03zu", material_name(x->document, m), number);
	const MaterialSkin** skins = (const MaterialSkin**)calloc(count, sizeof *skins);
	bool                 ok    = mat && skins &&
			  grow((void**)&x->materials, x->material_count, &x->material_capacity, sizeof *x->materials);
	for (size_t l = 0; ok && l < count; ++l)
		skins[l] = material_skin(x->in, m, chain[l].skin);
	if (ok) {
		cJSON_DeleteItemFromObjectCaseSensitive(mat, "name");
		cJSON_AddStringToObject(mat, "name", name);
		/* The composite has no skins of its own. */
		cJSON* extensions = (cJSON*)get(mat, "extensions");
		cJSON* own        = flight_model(mat);
		cJSON_DeleteItemFromObjectCaseSensitive(own, "skins");
		if (own && !own->child)
			cJSON_DeleteItemFromObjectCaseSensitive(extensions, "AERON_flight_model");
		if (extensions && !extensions->child)
			cJSON_DeleteItemFromObjectCaseSensitive(mat, "extensions");
	}
	for (int ch = 0; ok && ch < CH_COUNT; ++ch)
		ok = composite_channel(x, mat, m, skins, chain, count, ch, name);
	free(skins);
	if (!ok) {
		cJSON_Delete(mat);
		free(key);
		return -1;
	}
	cJSON_AddItemToArray(materials, mat);
	CompositeMaterial* entry = &x->materials[x->material_count++];
	entry->material          = (int)m;
	entry->key               = key;
	entry->index             = cJSON_GetArraySize(materials) - 1;
	return entry->index;
}

/* Adds variant `variant` to the primitive's mapping to `material`. */
static bool map_variant(cJSON* primitive, int material, int variant) {
	cJSON* mappings = get_or_add_array(
		get_or_add_object(get_or_add_object(primitive, "extensions"), "KHR_materials_variants"), "mappings");
	if (!mappings)
		return false;
	cJSON* mapping;
	cJSON_ArrayForEach(mapping, mappings) {
		const cJSON* to = get(mapping, "material");
		if (cJSON_IsNumber(to) && to->valueint == material)
			return cJSON_AddItemToArray(get_or_add_array(mapping, "variants"), cJSON_CreateNumber(variant));
	}
	mapping = cJSON_CreateObject();
	if (!mapping || !cJSON_AddNumberToObject(mapping, "material", material)) {
		cJSON_Delete(mapping);
		return false;
	}
	cJSON* list = cJSON_AddArrayToObject(mapping, "variants");
	return list && cJSON_AddItemToArray(list, cJSON_CreateNumber(variant)) &&
		   cJSON_AddItemToArray(mappings, mapping);
}

/* The material the primitive maps variant `variant` to, or -1. */
static int mapped_material(const cJSON* primitive, int variant) {
	const cJSON* mapping;
	cJSON_ArrayForEach(mapping, get(get(get(primitive, "extensions"), "KHR_materials_variants"), "mappings")) {
		const cJSON* index;
		cJSON_ArrayForEach(index, get(mapping, "variants")) {
			const cJSON* to = get(mapping, "material");
			if (cJSON_IsNumber(index) && index->valueint == variant && cJSON_IsNumber(to))
				return to->valueint;
		}
	}
	return -1;
}

static bool expand(Expansion* x) {
	Inputs*      in        = x->in;
	cJSON*       document  = x->document;
	const size_t khr_count = in->khr_count;

	/* The variants: the file's, then the skin variants. */
	cJSON* variants = NULL;
	if (in->variant_count)
		variants = get_or_add_array(
			get_or_add_object(get_or_add_object(document, "extensions"), "KHR_materials_variants"), "variants");
	for (size_t v = 0; variants && v < in->variant_count; ++v) {
		cJSON* item = cJSON_CreateObject();
		if (!item || !cJSON_AddStringToObject(item, "name", in->variants[v].name)) {
			cJSON_Delete(item);
			return fail("out of memory");
		}
		cJSON_AddItemToArray(variants, item);
	}
	cJSON* used  = get_or_add_array(document, "extensionsUsed");
	bool   found = false;
	if (!used)
		return fail("out of memory");
	const cJSON* name;
	cJSON_ArrayForEach(name, used) { found |= cJSON_IsString(name) && strcmp(name->valuestring, "KHR_materials_variants") == 0; }
	if (!found && in->variant_count)
		cJSON_AddItemToArray(used, cJSON_CreateString("KHR_materials_variants"));

	/* The primitives. */
	SkinLayer*   chain = NULL;
	size_t       most  = 1;
	for (size_t v = 0; v < in->variant_count; ++v)
		most = in->variants[v].layer_count > most ? in->variants[v].layer_count : most;
	chain = (SkinLayer*)calloc(most, sizeof *chain);
	if (!chain)
		return fail("out of memory");
	const cJSON* mesh;
	cJSON_ArrayForEach(mesh, get(document, "meshes")) {
		cJSON* primitive;
		cJSON_ArrayForEach(primitive, get(mesh, "primitives")) {
			const cJSON* material = get(primitive, "material");
			if (!cJSON_IsNumber(material) || material->valueint < 0 || (size_t)material->valueint >= in->material_count)
				continue;
			const size_t m = (size_t)material->valueint;
			for (size_t v = 0; v < in->variant_count; ++v) {
				const SkinVariant* sv    = &in->variants[v];
				size_t             count = 0;
				for (size_t l = 0; l < sv->layer_count; ++l)
					if (material_skin(in, m, sv->layers[l].skin))
						chain[count++] = sv->layers[l];
				const int variant = (int)(khr_count + v);
				int       to      = -1;
				if (count) {
					if ((to = composite_material(x, m, chain, count)) < 0) {
						free(chain);
						return false;
					}
				} else if (sv->base >= 0) {
					to = mapped_material(primitive, sv->base);
				}
				if (to >= 0 && !map_variant(primitive, to, variant)) {
					free(chain);
					return fail("out of memory");
				}
			}
		}
	}
	free(chain);

	/* The skins and skin variants are now variants. */
	cJSON* material;
	cJSON_ArrayForEach(material, get(document, "materials")) {
		cJSON* own = flight_model(material);
		cJSON_DeleteItemFromObjectCaseSensitive(own, "skins");
		if (own && !own->child) {
			cJSON* ext = (cJSON*)get(material, "extensions");
			cJSON_DeleteItemFromObjectCaseSensitive(ext, "AERON_flight_model");
			if (ext && !ext->child)
				cJSON_DeleteItemFromObjectCaseSensitive(material, "extensions");
		}
	}
	cJSON_DeleteItemFromObjectCaseSensitive(flight_model(model_root(document)), "skinVariants");
	if (x->uses_strength) {
		found = false;
		cJSON_ArrayForEach(name, used) {
			found |= cJSON_IsString(name) && strcmp(name->valuestring, "KHR_materials_emissive_strength") == 0;
		}
		if (!found)
			cJSON_AddItemToArray(used, cJSON_CreateString("KHR_materials_emissive_strength"));
	}
	x->out->material_count = x->material_count;
	x->out->variant_count  = in->variant_count;
	return true;
}

SkinLayers* SkinLayers_Expand(cJSON* document, const char* gltf_path, bool verbose) {
	SkinLayers* out = (SkinLayers*)calloc(1, sizeof *out);
	Inputs      in  = { 0 };
	if (!out || !(out->gltf_path = dup_string(gltf_path))) {
		free(out);
		fail("out of memory");
		return NULL;
	}
	out->verbose = verbose;
	/* The URIs of the file's images, before composites are added. */
	const cJSON* images = get(document, "images");
	out->uri_count      = (size_t)cJSON_GetArraySize(images);
	out->uris           = (char**)calloc(out->uri_count + 1, sizeof *out->uris);
	bool ok             = out->uris != NULL;
	for (size_t i = 0; ok && i < out->uri_count; ++i) {
		const cJSON* uri = get(cJSON_GetArrayItem(images, (int)i), "uri");
		if (cJSON_IsString(uri))
			ok = (out->uris[i] = dup_string(uri->valuestring)) != NULL;
	}
	Expansion x = { .document = document, .in = &in, .out = out };
	ok          = ok && read_material_skins(document, &in) && read_variants(document, &in) && expand(&x);
	for (size_t i = 0; i < x.material_count; ++i)
		free(x.materials[i].key);
	free(x.materials);
	inputs_free(&in);
	if (!ok) {
		SkinLayers_Free(out);
		return NULL;
	}
	return out;
}

/* ---- Pixels ---------------------------------------------------------- */

static void resolve_relative(char* out, size_t cap, const char* gltf_path, const char* rel) {
	const char* slash = strrchr(gltf_path, '/');
	if (!slash || rel[0] == '/') {
		snprintf(out, cap, "%s", rel);
		return;
	}
	snprintf(out, cap, "%.*s%s", (int)(slash - gltf_path) + 1, gltf_path, rel);
}

static const LoadedImage* load_image(SkinLayers* s, int image) {
	for (size_t i = 0; i < s->loaded_count; ++i)
		if (s->loaded[i]->image == image)
			return s->loaded[i];
	if (image < 0 || (size_t)image >= s->uri_count || !s->uris[image]) {
		fprintf(stderr, TAG ": image %d has no file\n", image);
		return NULL;
	}
	char path[2048];
	resolve_relative(path, sizeof path, s->gltf_path, s->uris[image]);
	LoadedImage img = { .image = image };
	char        err[256];
	if (!read_png_rgba(path, &img.rgba, &img.w, &img.h, err, sizeof err)) {
		fprintf(stderr, TAG ": PNG load failed: %s (%s)\n", path, err);
		return NULL;
	}
	LoadedImage** grown = (LoadedImage**)realloc(s->loaded, (s->loaded_count + 1) * sizeof *grown);
	LoadedImage*  item  = (LoadedImage*)malloc(sizeof *item);
	if (grown)
		s->loaded = grown;
	if (!grown || !item) {
		free(item);
		free(img.rgba);
		return NULL;
	}
	*item                         = img;
	s->loaded[s->loaded_count++] = item;
	return item;
}

/* Scales an sRGB-encoded value by `scale` in linear light. */
static uint8_t scale_srgb(uint8_t v, float scale) {
	if (scale == 1.0f)
		return v;
	return unorm8(linear_to_srgb(srgb_to_linear(v / 255.0f) * scale));
}

static bool composite(SkinLayers* s, CompositeImage* c) {
	if (c->done)
		return true;
	if (c->failed)
		return false;
	c->failed           = true;
	const clock_t start = clock();
	const LoadedImage* own = c->own_image >= 0 ? load_image(s, c->own_image) : NULL;
	if (c->own_image >= 0 && !own)
		return false;
	int w = own ? own->w : 0, h = own ? own->h : 0;
	for (size_t l = 0; l < c->layer_count; ++l) {
		const LoadedImage* layer = load_image(s, c->layers[l].image);
		const LoadedImage* cover = load_image(s, c->layers[l].coverage);
		if (!layer || !cover)
			return false;
		if (!w) {
			w = layer->w;
			h = layer->h;
		}
		if (layer->w != w || layer->h != h || cover->w != w || cover->h != h) {
			fprintf(stderr, TAG ": %s: a layer is %dx%d, not %dx%d\n", c->label, layer->w, layer->h, w, h);
			return false;
		}
	}
	const size_t n = (size_t)w * (size_t)h;
	c->rgba        = (uint8_t*)malloc(n * 4 + 1);
	if (!c->rgba)
		return false;
	for (size_t i = 0; i < n; ++i) {
		uint8_t* d = &c->rgba[i * 4];
		if (own) {
			memcpy(d, &own->rgba[i * 4], 4);
			if (c->channel != CH_BASE)
				d[3] = 255;
		} else {
			memcpy(d, c->own_value, 4);
		}
		if (c->channel == CH_EMISSIVE && c->own_scale != 1.0f)
			for (int k = 0; k < 3; ++k)
				d[k] = scale_srgb(d[k], c->own_scale);
	}
	for (size_t l = 0; l < c->layer_count; ++l) {
		const LayerSource* src   = &c->layers[l];
		const uint8_t*     layer = load_image(s, src->image)->rgba;
		const uint8_t*     cover = load_image(s, src->coverage)->rgba;
		for (size_t i = 0; i < n; ++i) {
			const int a = ((int)cover[i * 4 + 3] * src->opacity + 50) / 100;
			if (!a)
				continue;
			uint8_t* d = &c->rgba[i * 4];
			for (int k = 0; k < 3; ++k) {
				const int v = c->channel == CH_EMISSIVE ? scale_srgb(layer[i * 4 + k], src->scale) : layer[i * 4 + k];
				d[k]        = (uint8_t)(d[k] * (255 - a) / 255 + v * a / 255);
			}
		}
	}
	c->w       = w;
	c->h       = h;
	c->done    = true;
	c->failed  = false;
	s->seconds += (double)(clock() - start) / CLOCKS_PER_SEC;
	return true;
}

static long composite_index(const char* uri) {
	if (!uri || strncmp(uri, COMPOSITE_SCHEME, strlen(COMPOSITE_SCHEME)) != 0)
		return -1;
	char*      end   = NULL;
	const long index = strtol(uri + strlen(COMPOSITE_SCHEME), &end, 10);
	return end && !*end ? index : -1;
}

bool SkinLayers_ImageProvider(void* context, const struct cgltf_image* image, AeronGltfCookImageView* out) {
	SkinLayers* s = (SkinLayers*)context;
	memset(out, 0, sizeof *out);
	const long c = composite_index(image->uri);
	if (c >= 0) {
		if ((size_t)c >= s->composite_count || !composite(s, &s->composites[c]))
			return false;
		out->rgba   = s->composites[c].rgba;
		out->width  = s->composites[c].w;
		out->height = s->composites[c].h;
		return true;
	}
	for (size_t i = 0; image->uri && i < s->uri_count; ++i)
		if (s->uris[i] && strcmp(s->uris[i], image->uri) == 0) {
			const LoadedImage* img = load_image(s, (int)i);
			if (!img)
				return false;
			out->rgba   = img->rgba;
			out->width  = img->w;
			out->height = img->h;
			return true;
		}
	fprintf(stderr, TAG ": image '%s' is not a file of the model\n", image->uri ? image->uri : "");
	return false;
}

void SkinLayers_Report(const SkinLayers* s) {
	if (s && s->verbose)
		fprintf(stderr, TAG ": %zu skin variant(s), %zu composite material(s), %zu composite image(s), "
						"composited in %.2f s\n",
				s->variant_count, s->material_count, s->composite_count, s->seconds);
}

void SkinLayers_Free(SkinLayers* s) {
	if (!s)
		return;
	for (size_t c = 0; c < s->composite_count; ++c) {
		free(s->composites[c].layers);
		free(s->composites[c].label);
		free(s->composites[c].rgba);
	}
	free(s->composites);
	for (size_t i = 0; i < s->loaded_count; ++i) {
		free(s->loaded[i]->rgba);
		free(s->loaded[i]);
	}
	free(s->loaded);
	for (size_t i = 0; i < s->uri_count; ++i)
		free(s->uris[i]);
	free(s->uris);
	free(s->gltf_path);
	free(s);
}
