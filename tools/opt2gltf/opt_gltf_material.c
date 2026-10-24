/* Builds the glTF materials. Each texture gets either a stock material
 * (decoded colors, self-lit texels, alpha class or alpha override,
 * fullbright) or a given material. This file also handles screen surfaces,
 * the variant materials, and the AERON_flight_model object of each
 * material. */

#include <math.h>

#include "opt_gltf_internal.h"
#include "opt_json.h"

/* RGB565 word → 8-bit triple. */
static inline void rgb565_to_rgb8(uint16_t w, uint8_t* r, uint8_t* g, uint8_t* b) {
	uint8_t r5 = (w >> 11) & 0x1F;
	uint8_t g6 = (w >> 5) & 0x3F;
	uint8_t b5 = w & 0x1F;
	*r         = (uint8_t)((r5 << 3) | (r5 >> 2));
	*g         = (uint8_t)((g6 << 2) | (g6 >> 4));
	*b         = (uint8_t)((b5 << 3) | (b5 >> 2));
}

/* Decodes the base mip of an OPT texture into RGBA8, using palette shade
 * row `shade` (the base-color row of the file, see opt_palette_base_shade).
 * 32bpp textures ignore the shade and keep their authored color. Their
 * alpha comes from t->alpha, which is set only when the texture has the
 * alpha flag. Without it, they are opaque. The caller owns the returned
 * buffer. Returns NULL on bad inputs. */
uint8_t* OptGltf_DecodeTexture(const opt_texture_t* t, int shade) {
	if (!t || !t->pixels || t->width <= 0 || t->height <= 0)
		return NULL;
	const size_t base_bytes = (size_t)t->width * (size_t)t->height;
	uint8_t*     out        = (uint8_t*)malloc(base_bytes * 4);
	if (!out)
		return NULL;
	if (t->bytes_per_texel == 4) {
		for (size_t i = 0; i < base_bytes; ++i) {
			out[i * 4 + 0] = t->pixels[i * 4 + 2];
			out[i * 4 + 1] = t->pixels[i * 4 + 1];
			out[i * 4 + 2] = t->pixels[i * 4 + 0];
			out[i * 4 + 3] = t->alpha ? t->alpha[i] : 255;
		}
		return out;
	}
	const uint8_t* src = t->pixels;
	const uint8_t* pal = t->palette + ((size_t)shade * OPT_PALETTE_COLORS * OPT_PALETTE_BPP);
	for (size_t i = 0; i < base_bytes; ++i) {
		uint8_t  idx = src[i];
		uint16_t w   = (uint16_t)pal[idx * 2] | (uint16_t)((uint16_t)pal[idx * 2 + 1] << 8);
		uint8_t  r, g, b;
		rgb565_to_rgb8(w, &r, &g, &b);
		out[i * 4 + 0] = r;
		out[i * 4 + 1] = g;
		out[i * 4 + 2] = b;
		/* Alpha comes from an XWA TextureAlpha node (base mip). Without one,
		 * the texel is opaque. */
		out[i * 4 + 3] = t->alpha ? t->alpha[i] : 255;
	}
	return out;
}

/* Decodes the base mip into an emissive RGBA8 map. Self-illuminated texels
 * get their glow color and alpha 255. All other texels are transparent
 * black. The alpha is the classic lightmap coverage mask. gltf_cook
 * premultiplies it before it generates the mips, so that the runtime can
 * reconstruct the original gamma-space filtering and SRCALPHA pass in the
 * opaque PBR shader. Returns NULL if the texture has no emissive layer, so
 * that no emissive PNG or material is written for it. The caller owns the
 * returned buffer.
 *
 * For XWA files (version 5), this matches the engine EXACTLY
 * (ModelTexture_FilterHardwarePalette + CreateD3DfromTexture). Palette
 * entries are classified by opt_palette_classic_lit, and the glow color
 * comes from palette row 10. There is no lightmap for a texture whose name
 * starts with "_", or when all or none of the 256 entries are lit. Older
 * files keep the flat-ramp heuristic with `tol`, and sample the base
 * shade. */
static uint8_t* decode_texture_emissive_rgba8(const opt_file_t* opt, const opt_texture_t* t, int shade,
											  int tol) {
	if (!t || !t->pixels || t->width <= 0 || t->height <= 0)
		return NULL;
	/* The classifiers read the palette. A 32bpp texture has no palette, and
	 * its illumination is a separate plane (t->illum). */
	if (t->bytes_per_texel == 4)
		return NULL;
	const int xwa = opt && opt->version >= 5;
	if (xwa && t->name[0] == '_') {
		return NULL; /* engine rule: textures named "_*" never glow */
	}

	int lit[OPT_PALETTE_COLORS];
	int lit_count = 0;
	for (int i = 0; i < OPT_PALETTE_COLORS; ++i) {
		lit[i] = xwa ? opt_palette_classic_lit(t->palette, i)
					 : opt_palette_index_emissive(opt, t->palette, i, tol);
		lit_count += lit[i];
	}
	/* Engine rule (clearedCount at 0x44A600): a lightmap is built only
	 * when SOME but not ALL palette entries are lit. */
	if (xwa && (lit_count == 0 || lit_count == OPT_PALETTE_COLORS)) {
		return NULL;
	}

	const int      glow_shade = xwa ? OPT_XWA_GLOW_SHADE : shade;
	const size_t   base_bytes = (size_t)t->width * (size_t)t->height;
	const uint8_t* src        = t->pixels;
	const uint8_t* pal        = t->palette + ((size_t)glow_shade * OPT_PALETTE_COLORS * OPT_PALETTE_BPP);
	uint8_t*       out        = NULL;
	for (size_t i = 0; i < base_bytes; ++i) {
		uint8_t idx = src[i];
		if (!lit[idx]) {
			if (out) {
				out[i * 4 + 0] = 0;
				out[i * 4 + 1] = 0;
				out[i * 4 + 2] = 0;
				out[i * 4 + 3] = 0;
			}
			continue;
		}
		if (!out) {
			out = (uint8_t*)calloc(base_bytes, 4);
			if (!out)
				return NULL;
		}
		uint16_t w = (uint16_t)pal[idx * 2] | (uint16_t)((uint16_t)pal[idx * 2 + 1] << 8);
		uint8_t  r, g, b;
		rgb565_to_rgb8(w, &r, &g, &b);
		out[i * 4 + 0] = r;
		out[i * 4 + 1] = g;
		out[i * 4 + 2] = b;
		out[i * 4 + 3] = 255;
	}
	return out; /* NULL when no texel qualified */
}

/* ===== Switch-state alpha classes =================================== */

/* The renderer splits the indices of a model once into OPAQUE, MASK and
 * BLEND ranges, so all material variants of a primitive must have the same
 * alpha class. The state textures of a switched face group can have
 * different classes, for example an opaque texture and a texture with
 * translucent texels. Every state then uses a copy of its material that is
 * promoted to the most transparent class among them. An opaque texture
 * draws the same under MASK (alpha 1 passes any cutoff) or BLEND. There is
 * one copy per base material and class. A transmissive material draws in
 * the BLEND range (aeron/scene/gltf_mesh.h). */

int opt_gltf_alpha_class_rank(const cgltf_material* m) {
	if (m->alpha_mode == cgltf_alpha_mode_blend ||
		(m->has_transmission && m->transmission.transmission_factor > 0.0f))
		return 2;
	return m->alpha_mode == cgltf_alpha_mode_mask ? 1 : 0;
}

cgltf_material* opt_gltf_promoted_material(GltfBuild* build, cgltf_material* base, int rank,
										   size_t base_count, cgltf_material** cache) {
	const size_t index = (size_t)(base - build->materials);
	if (index >= base_count || opt_gltf_alpha_class_rank(base) >= rank)
		return base;
	cgltf_material** slot = &cache[index * 2 + (size_t)(rank - 1)];
	if (*slot)
		return *slot;
	cgltf_material* copy = &build->materials[build->materials_count++];
	*copy                = *base;
	copy->name           = opt_gltf_keep_string(
		build, xprintf_dup("%s_%s", base->name ? base->name : "Mat", rank == 2 ? "Blend" : "Mask"));
	if (rank == 2) {
		copy->alpha_mode = cgltf_alpha_mode_blend;
	} else {
		copy->alpha_mode   = cgltf_alpha_mode_mask;
		copy->alpha_cutoff = 0.5f;
	}
	if (build->material_anim) {
		build->material_anim[copy - build->materials]   = build->material_anim[index];
		build->material_screen[copy - build->materials] = build->material_screen[index];
		build->material_given[copy - build->materials]  = build->material_given[index];
	}
	*slot = copy;
	return copy;
}

/* ===== Conversion ==================================================== */

typedef struct AlphaHistogram {
	uint64_t zero_count;
	uint64_t full_count;
	uint64_t intermediate_count;
} AlphaHistogram;

static OptGltfAlphaMode classify_alpha(const uint8_t* alpha, size_t sample_count, AlphaHistogram* histogram) {
	AlphaHistogram result = { 0, 0, 0 };
	if (!alpha || sample_count == 0) {
		if (histogram)
			*histogram = result;
		return OPT_GLTF_ALPHA_OPAQUE;
	}

	for (size_t i = 0; i < sample_count; ++i) {
		result.zero_count += alpha[i] == 0u;
		result.full_count += alpha[i] == 255u;
	}
	result.intermediate_count = (uint64_t)sample_count - result.zero_count - result.full_count;
	if (histogram)
		*histogram = result;
	if (result.full_count == sample_count)
		return OPT_GLTF_ALPHA_OPAQUE;

	const uint64_t endpoint_count = result.zero_count + result.full_count;
	if (result.zero_count != 0 && result.full_count != 0 &&
		result.intermediate_count <= endpoint_count / 9u) {
		return OPT_GLTF_ALPHA_MASK;
	}
	return OPT_GLTF_ALPHA_BLEND;
}

OptGltfAlphaMode OptGltf_ClassifyAlpha(const uint8_t* alpha, size_t sample_count) {
	return classify_alpha(alpha, sample_count, NULL);
}

static int alpha_name_equal(const char* a, const char* b) {
	if (!a || !b)
		return 0;
	while (*a && *b) {
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if (ca >= 'a' && ca <= 'z')
			ca = (unsigned char)(ca - ('a' - 'A'));
		if (cb >= 'a' && cb <= 'z')
			cb = (unsigned char)(cb - ('a' - 'A'));
		if (ca != cb)
			return 0;
	}
	return *a == *b;
}

static const OptGltfAlphaOverride* find_alpha_override(const OptGltfBuildOptions* options,
													   const opt_texture_t* texture, int32_t texture_index) {
	if (!options || !options->material.alpha_overrides || options->material.alpha_override_count == 0)
		return NULL;
	char generated_name[32];
	snprintf(generated_name, sizeof generated_name, "Tex%02d", (int)texture_index);
	for (size_t i = 0; i < options->material.alpha_override_count; ++i) {
		const OptGltfAlphaOverride* override = &options->material.alpha_overrides[i];
		if (alpha_name_equal(override->texture_name, texture->name) ||
			alpha_name_equal(override->texture_name, generated_name)) {
			return override;
		}
	}
	return NULL;
}

static size_t alpha_override_match_count(const OptGltfBuildOptions* options, const opt_texture_t* texture,
										 int32_t texture_index) {
	if (!options || !options->material.alpha_overrides)
		return 0;
	char generated_name[32];
	snprintf(generated_name, sizeof generated_name, "Tex%02d", (int)texture_index);
	size_t count = 0;
	for (size_t i = 0; i < options->material.alpha_override_count; ++i) {
		const char* name = options->material.alpha_overrides[i].texture_name;
		count += alpha_name_equal(name, texture->name) || alpha_name_equal(name, generated_name);
	}
	return count;
}

static const char* alpha_mode_name(OptGltfAlphaMode mode) {
	switch (mode) {
		case OPT_GLTF_ALPHA_OPAQUE:
			return "opaque";
		case OPT_GLTF_ALPHA_MASK:
			return "mask";
		case OPT_GLTF_ALPHA_BLEND:
			return "blend";
		default:
			return "invalid";
	}
}

bool opt_gltf_validate_alpha_overrides(const OptGltfBuildOptions* options, opt_error_t* error) {
	if (!options || options->material.alpha_override_count == 0)
		return true;
	if (!options->material.alpha_overrides) {
		if (error)
			snprintf(error->msg, sizeof error->msg, "alpha override count is set but there are no records");
		return false;
	}
	for (size_t i = 0; i < options->material.alpha_override_count; ++i) {
		const OptGltfAlphaOverride* item = &options->material.alpha_overrides[i];
		if (!item->texture_name || !item->texture_name[0] || item->alpha_mode < OPT_GLTF_ALPHA_OPAQUE ||
			item->alpha_mode > OPT_GLTF_ALPHA_BLEND || !isfinite(item->alpha_cutoff) ||
			item->alpha_cutoff < 0.0f || item->alpha_cutoff > 1.0f) {
			if (error)
				snprintf(error->msg, sizeof error->msg, "invalid alpha override %zu", i);
			return false;
		}
		for (size_t previous = 0; previous < i; ++previous) {
			if (alpha_name_equal(item->texture_name,
								 options->material.alpha_overrides[previous].texture_name)) {
				if (error)
					snprintf(error->msg, sizeof error->msg, "duplicate alpha override '%s'",
							 item->texture_name);
				return false;
			}
		}
	}
	return true;
}

/* ===== Screen surfaces ============================================== */

/* Returns the `screen` JSON for the AERON_flight_model object of the
 * material, owned by the caller, or NULL. */
static char* screen_json(const OptGltfScreen* s) {
	cJSON* object = cJSON_CreateObject();
	bool   ok     = OptJson_Add(object, "cover", cJSON_CreateBool(s->cover_base.rgba && !s->hologram)) &&
					OptJson_Add(object, "transparent", cJSON_CreateBool(s->transparent)) &&
					OptJson_Add(object, "alwaysVisible", cJSON_CreateBool(s->always_visible)) &&
					OptJson_Add(object, "hologram", cJSON_CreateBool(s->hologram)) &&
					OptJson_Add(object, "noisy", cJSON_CreateBool(s->noisy)) &&
					OptJson_Add(object, "hide", cJSON_CreateNumber(s->hide));
	cJSON* rects  = ok ? OptJson_AddArray(object, "rects") : NULL;
	ok            = rects != NULL;
	for (uint32_t i = 0; ok && i < s->rect_count; ++i) {
		const OptGltfScreenRect* r    = &s->rects[i];
		cJSON*                   rect = OptJson_AddObject(rects, NULL);
		ok                            = OptJson_Add(rect, "source", cJSON_CreateNumber(r->source)) &&
										OptJson_Add(rect, "dst", OptJson_Floats(r->dst, 4)) &&
										OptJson_Add(rect, "bg", OptJson_Floats(r->bg, 3)) &&
										OptJson_Add(rect, "highlight", OptJson_Floats(r->highlight, 3)) &&
										OptJson_Add(rect, "lock", OptJson_Floats(r->lock, 3)) &&
										OptJson_Add(rect, "gain", OptJson_Float(r->gain)) &&
										OptJson_Add(rect, "layers", cJSON_CreateNumber(r->layers));
		if (ok && (r->layers & OPT_GLTF_SCREEN_LAYER_BAR))
			ok = OptJson_Add(rect, "bar", OptJson_Floats(r->bar, 3));
	}
	if (!ok) {
		cJSON_Delete(object);
		return NULL;
	}
	return OptJson_Print(object);
}

/* Rewrites the material of a screen texture (opt_gltf.h, "Screen surfaces").
 * `cover_base` and `cover_emissive` are the textures of the cover, or NULL. */
static bool screen_apply(GltfBuild* build, cgltf_material* mat, const OptGltfScreen* s,
						 cgltf_texture* cover_base, cgltf_texture* cover_emissive) {
	char* screen = screen_json(s);
	if (!screen)
		return false;
	build->material_screen[mat - build->materials] = opt_gltf_keep_string(build, screen);

	mat->normal_texture.texture                                    = NULL;
	mat->pbr_metallic_roughness.metallic_roughness_texture.texture = NULL;
	mat->pbr_metallic_roughness.metallic_factor                    = 0.0f;
	mat->pbr_metallic_roughness.roughness_factor                   = 1.0f;
	const bool cover                                               = cover_base && !s->hologram;
	for (int channel = 0; channel < 3; ++channel) {
		mat->pbr_metallic_roughness.base_color_factor[channel] = cover ? 1.0f : 0.0f;
		mat->emissive_factor[channel]                          = cover && cover_emissive ? 1.0f : 0.0f;
	}
	mat->pbr_metallic_roughness.base_color_factor[3]       = 1.0f;
	mat->pbr_metallic_roughness.base_color_texture.texture = cover ? cover_base : NULL;
	mat->emissive_texture.texture                          = cover ? cover_emissive : NULL;
	mat->has_emissive_strength                             = cover && cover_emissive;
	mat->emissive_strength.emissive_strength = cover && cover_emissive ? s->cover_emissive_strength : 1.0f;
	mat->alpha_mode   = s->transparent || s->hologram ? cgltf_alpha_mode_blend : cgltf_alpha_mode_opaque;
	mat->alpha_cutoff = 0.5f;
	return true;
}

/* The energy effects. energy_bits returns the effects listed in an
 * AERON_flight_model entry, as bits of this array. */
static const char* const kAnimEnergies[] = { "shields", "tractor", "jamming" };

static unsigned energy_bits(const cJSON* entry) {
	unsigned     bits = 0;
	const cJSON* name;
	cJSON_ArrayForEach(name, cJSON_GetObjectItemCaseSensitive(entry, "energy")) {
		for (unsigned e = 0; e < 3; ++e)
			if (cJSON_IsString(name) && strcmp(name->valuestring, kAnimEnergies[e]) == 0)
				bits |= 1u << e;
	}
	return bits;
}

/* Adds an `energy` list with the effects in `bits` to `object`. Returns
 * false when out of memory. */
bool opt_gltf_add_energy(cJSON* object, unsigned bits) {
	cJSON* list = OptJson_AddArray(object, "energy");
	bool   ok   = list != NULL;
	for (unsigned e = 0; ok && e < 3; ++e)
		if (bits & 1u << e)
			ok = OptJson_Add(list, NULL, cJSON_CreateString(kAnimEnergies[e]));
	return ok;
}

/* Adds the material's skin layers to its AERON_flight_model object `entry`
 * as `skins`, with the indices of their textures. */
static bool skins_json(const OptGltfDocument* document, cJSON* entry, const OptGltfMaterial* given) {
	cJSON* skins = OptJson_AddObject(entry, "skins");
	bool   ok    = skins != NULL;
	for (size_t l = 0; ok && l < given->skin_count; ++l) {
		const OptGltfSkinLayer* layer   = &given->skins[l];
		static const char* const keys[] = { "baseColorTexture", "emissiveTexture", "metallicRoughnessTexture" };
		const OptGltfImage*      skin[3] = { &layer->base, &layer->emissive, &layer->metallic_roughness };
		cJSON*                   object  = OptJson_AddObject(skins, layer->skin);
		ok                               = object != NULL;
		for (int k = 0; ok && k < 3; ++k) {
			if (!skin[k]->view.rgba)
				continue;
			const int texture = opt_gltf_texture_of_pixels(document, skin[k]->view.rgba);
			cJSON*    info    = texture >= 0 ? OptJson_AddObject(object, keys[k]) : NULL;
			ok                = info && OptJson_Add(info, "index", cJSON_CreateNumber(texture));
		}
		if (ok && layer->emissive_strength > 0.0f)
			ok = OptJson_Add(object, "emissiveStrength", cJSON_CreateNumber(layer->emissive_strength));
	}
	return ok;
}

/* Builds the AERON_flight_model object of each material, with its animation
 * and its screen. The set of energy effects that most materials have
 * becomes the `energy` of the model root (*common_energy, bits of
 * kAnimEnergies). On a tie, the smaller bit value wins. Counting every
 * material is correct because unused materials were already removed (see
 * opt_gltf_prune_unused). Only the materials with a different set keep their
 * own `energy`, which is `[]` when they have no effect. This includes the
 * materials without an animation object, such as the untextured one. */
bool opt_gltf_material_extensions(OptGltfDocument* document, unsigned* common_energy) {
	GltfBuild*   gb       = &document->gb;
	const size_t count    = gb->materials_count;
	cJSON**      entries  = (cJSON**)calloc(count ? count : 1, sizeof *entries);
	unsigned     tally[8] = { 0 };
	bool         ok       = entries != NULL;
	for (size_t m = 0; ok && m < count; ++m) {
		const char* anim = gb->material_anim ? gb->material_anim[m] : NULL;
		if (anim) {
			entries[m] = cJSON_Parse(anim);
			ok         = entries[m] != NULL;
		}
		tally[energy_bits(entries[m])]++;
	}
	unsigned common = 0;
	for (unsigned bits = 1; bits < 8; ++bits)
		if (tally[bits] > tally[common])
			common = bits;
	for (size_t m = 0; ok && m < count; ++m) {
		cJSON* entry = entries[m] ? entries[m] : cJSON_CreateObject();
		entries[m]   = entry;
		ok           = entry != NULL;
		if (ok && common) {
			const unsigned bits = energy_bits(entry);
			cJSON_DeleteItemFromObjectCaseSensitive(entry, "energy");
			if (bits != common)
				ok = opt_gltf_add_energy(entry, bits);
		}
		const char* screen = gb->material_screen ? gb->material_screen[m] : NULL;
		if (ok && screen)
			ok = OptJson_Add(entry, "screen", cJSON_Parse(screen));
		const OptGltfMaterial* given = gb->material_given ? gb->material_given[m] : NULL;
		if (ok && given && given->skin_count)
			ok = skins_json(document, entry, given);
		if (ok && entry->child) {
			char*            printed   = cJSON_PrintUnformatted(entry);
			cgltf_extension* extension = (cgltf_extension*)calloc(1, sizeof *extension);
			ok                         = printed && extension;
			if (ok) {
				extension->name                   = (char*)"AERON_flight_model";
				extension->data                   = opt_gltf_keep_string(gb, printed);
				gb->materials[m].extensions       = extension;
				gb->materials[m].extensions_count = 1;
			} else {
				free(printed);
				free(extension);
			}
		}
	}
	*common_energy = common;
	for (size_t m = 0; entries && m < count; ++m)
		cJSON_Delete(entries[m]);
	free(entries);
	return ok;
}

/* ---- Texture preparation ------------------------------------------------ */

/* With `emissive`, each texture with self-illuminated texels (flat palette
 * ramp) gets a second, emissive image. With fullbright, each texture with
 * alpha gets one too (fullbright_emissive). Like all images, these are
 * counted before the arrays are allocated, because cgltf points into the
 * image and texture arrays and they are never reallocated. */
static bool find_stock_emissive(Build* b, size_t* images) {
	const opt_file_t* opt = b->opt;
	b->tex_emissive = (int*)calloc(opt->texture_count ? opt->texture_count : 1, sizeof *b->tex_emissive);
	if (!b->tex_emissive)
		return opt_gltf_build_fail(b, "out of memory");
	for (int32_t i = 0; b->emissive && i < opt->texture_count; ++i) {
		uint8_t* em =
			decode_texture_emissive_rgba8(opt, &opt->textures[i], b->base_shade, OPT_EMISSIVE_SHADE_TOL);
		if (em) {
			b->tex_emissive[i] = 1;
			++*images;
			free(em);
		}
	}
	for (int32_t i = 0; b->fullbright && i < opt->texture_count; ++i) {
		if (opt->textures[i].alpha)
			++*images;
	}
	return true;
}

/* Lists the images of a given material in the order they are written:
 * base color, emissive, metallic-roughness, transmission, normal. */
static void material_images(const OptGltfMaterial* m, const OptGltfImage* out[5]) {
	out[0] = &m->base;
	out[1] = &m->emissive;
	out[2] = &m->metallic_roughness;
	out[3] = &m->transmission;
	out[4] = &m->normal;
}

/* Adds `pixels` to `seen` (`*count` entries) if it is not there yet.
 * Returns true if it was added. */
static bool see_pixels(const uint8_t** seen, size_t* count, const uint8_t* pixels) {
	for (size_t i = 0; i < *count; ++i)
		if (seen[i] == pixels)
			return false;
	seen[(*count)++] = pixels;
	return true;
}

/* Counts the images of the given materials, except the base images of the
 * textures, which are already counted. Each distinct image is counted once.
 * The texture materials come first, then the variant materials. */
static bool count_given_images(Build* b, size_t* images) {
	const opt_file_t* opt      = b->opt;
	const size_t      variants = b->variants ? b->variants->material_count : 0;
	const uint8_t**   seen =
		(const uint8_t**)malloc(((size_t)opt->texture_count + variants) * 5 * sizeof *seen + 1);
	size_t count = 0;
	if (!seen)
		return opt_gltf_build_fail(b, "out of memory (given materials)");
	size_t skin_images = 0;
	for (int32_t i = 0; i < opt->texture_count; ++i)
		skin_images += b->materials[i].skin_count * 3;
	const uint8_t** grown = (const uint8_t**)realloc(
		seen, (((size_t)opt->texture_count + variants) * 5 + skin_images) * sizeof *seen + 1);
	if (!grown) {
		free(seen);
		return opt_gltf_build_fail(b, "out of memory (given materials)");
	}
	seen = grown;
	for (int32_t i = 0; i < opt->texture_count; ++i) {
		const OptGltfImage* list[5];
		material_images(&b->materials[i], list);
		for (int k = 1; b->materials[i].base.view.rgba && k < 5; ++k)
			*images += list[k]->view.rgba && see_pixels(seen, &count, list[k]->view.rgba);
		for (size_t l = 0; b->materials[i].base.view.rgba && l < b->materials[i].skin_count; ++l) {
			const OptGltfSkinLayer* layer   = &b->materials[i].skins[l];
			const OptGltfImage*     skin[3] = { &layer->base, &layer->emissive, &layer->metallic_roughness };
			for (int k = 0; k < 3; ++k)
				*images += skin[k]->view.rgba && see_pixels(seen, &count, skin[k]->view.rgba);
		}
	}
	for (size_t m = 0; m < variants; ++m) {
		const OptGltfImage* list[5];
		material_images(&b->variants->materials[m].material, list);
		for (int k = 0; k < 5; ++k)
			*images += list[k]->view.rgba && see_pixels(seen, &count, list[k]->view.rgba);
	}
	free(seen);
	return true;
}

/* Counts the cover images of screen surfaces, once for each cover. */
static bool find_screen_covers(Build* b, size_t* images) {
	const opt_file_t*    opt     = b->opt;
	const OptGltfScreen* screens = b->screens;
	b->screen_cover_image        = (int32_t*)malloc((opt->texture_count ? (size_t)opt->texture_count : 1) *
													sizeof *b->screen_cover_image);
	if (!b->screen_cover_image)
		return opt_gltf_build_fail(b, "out of memory (screens)");
	for (int32_t i = 0; i < opt->texture_count; ++i) {
		b->screen_cover_image[i] = -1;
		const OptGltfScreen* s   = &screens[i];
		if (!s->present || s->hologram || !s->cover_base.rgba)
			continue;
		bool shared = false;
		for (int32_t j = 0; j < i && !shared; ++j)
			shared = screens[j].present && !screens[j].hologram &&
					 screens[j].cover_base.rgba == s->cover_base.rgba;
		if (!shared)
			*images += s->cover_emissive.rgba ? 2u : 1u;
	}
	return true;
}

bool opt_gltf_prepare_textures(Build* b) {
	const opt_file_t* opt    = b->opt;
	GltfBuild*        gb     = b->gb;
	size_t            images = (size_t)opt->texture_count;
	b->base_shade            = opt_palette_base_shade(opt);
	if (!find_stock_emissive(b, &images) || (b->materials && !count_given_images(b, &images)) ||
		(b->screens && !find_screen_covers(b, &images)))
		return false;

	gb->images   = (cgltf_image*)calloc(images ? images : 1, sizeof *gb->images);
	gb->textures = (cgltf_texture*)calloc(images ? images : 1, sizeof *gb->textures);
	/* Room for the base materials (one per texture, the untextured one, then
	 * the variant materials), followed by up to two alpha-promoted copies of
	 * each (see opt_gltf_promoted_material). Primitives point into this
	 * array, so it is never reallocated. */
	const size_t materials =
		((size_t)opt->texture_count + 1 + (b->variants ? b->variants->material_count : 0)) * 3;
	gb->materials       = (cgltf_material*)calloc(materials, sizeof *gb->materials);
	gb->material_anim   = (const char**)calloc(materials, sizeof *gb->material_anim);
	gb->material_screen = (const char**)calloc(materials, sizeof *gb->material_screen);
	gb->material_given  = (const OptGltfMaterial**)calloc(materials, sizeof *gb->material_given);
	b->document->image_pixels =
		(OptGltfOwnedImage*)calloc(images ? images : 1, sizeof *b->document->image_pixels);
	b->document->image_pixels_count = images;
	b->image_count                  = images;
	if (!gb->images || !gb->textures || !gb->materials || !gb->material_anim || !gb->material_screen ||
		!gb->material_given ||
		!b->document->image_pixels)
		return opt_gltf_build_fail(b, "out of memory (textures)");
	b->image_slot = (size_t)opt->texture_count;
	return true;
}

/* ---- Texture materials -------------------------------------------------- */

/* Sets the alpha mode of the material from an alpha override, or else by
 * classifying the alpha of the texture. A given material uses the alpha of
 * its base image when it sets alpha_from_base, and is opaque otherwise. */
static bool material_alpha(Build* b, int32_t i, cgltf_material* mat, const uint8_t* rgba) {
	const opt_texture_t* t               = &b->opt->textures[i];
	AlphaHistogram       alpha_histogram = { 0, 0, 0 };
	uint8_t*             given_alpha     = NULL;
	if (b->materials && rgba && b->materials[i].alpha_from_base) {
		const size_t n = (size_t)t->width * (size_t)t->height;
		given_alpha    = (uint8_t*)malloc(n);
		if (!given_alpha)
			return opt_gltf_build_fail(b, "out of memory (alpha)");
		for (size_t k = 0; k < n; ++k)
			given_alpha[k] = rgba[k * 4 + 3];
	}
	OptGltfAlphaMode alpha_mode = classify_alpha(b->materials ? given_alpha : t->alpha,
												 (size_t)t->width * (size_t)t->height, &alpha_histogram);
	free(given_alpha);
	float                       alpha_cutoff   = 0.5f;
	const OptGltfAlphaOverride* alpha_override = find_alpha_override(b->options, t, i);
	if (alpha_override_match_count(b->options, t, i) > 1)
		return opt_gltf_build_fail(b, "several alpha overrides match texture '%s'", t->name);
	if (alpha_override) {
		if (alpha_override->alpha_mode < OPT_GLTF_ALPHA_OPAQUE ||
			alpha_override->alpha_mode > OPT_GLTF_ALPHA_BLEND || !isfinite(alpha_override->alpha_cutoff) ||
			alpha_override->alpha_cutoff < 0.0f || alpha_override->alpha_cutoff > 1.0f ||
			(alpha_override->alpha_mode != OPT_GLTF_ALPHA_OPAQUE && !t->alpha))
			return opt_gltf_build_fail(b, "invalid alpha override for texture '%s'", t->name);
		alpha_mode   = alpha_override->alpha_mode;
		alpha_cutoff = alpha_override->alpha_cutoff;
	}
	if (t->alpha)
		opt_gltf_log(b->options, false,
					 "[opt_alpha] %s: mode=%s source=%s zero=%llu full=%llu intermediate=%llu cutoff=%.3g",
					 t->name[0] ? t->name : b->gb->images[i].name, alpha_mode_name(alpha_mode),
					 alpha_override ? "override" : "heuristic",
					 (unsigned long long)alpha_histogram.zero_count,
					 (unsigned long long)alpha_histogram.full_count,
					 (unsigned long long)alpha_histogram.intermediate_count, (double)alpha_cutoff);
	if (alpha_mode == OPT_GLTF_ALPHA_MASK) {
		mat->alpha_mode   = cgltf_alpha_mode_mask;
		mat->alpha_cutoff = alpha_cutoff;
	} else if (alpha_mode == OPT_GLTF_ALPHA_BLEND) {
		mat->alpha_mode = cgltf_alpha_mode_blend;
	}
	return true;
}

/* Self-illuminated texels (such as lit windows) get a second texture with
 * their glow colour on black. It is used as emissiveTexture, with an
 * emissiveFactor of 1. The classic renderer drew these texels over the lit
 * surface as glow · c + lit · (1 - c), where c is their coverage, which is
 * 1 on a lit texel. So the base colour is set to black under them, and the
 * standard lit + emission result looks the same. */
static void stock_emissive(Build* b, int32_t i, cgltf_material* mat) {
	const opt_texture_t* t  = &b->opt->textures[i];
	uint8_t*             em = decode_texture_emissive_rgba8(b->opt, t, b->base_shade, OPT_EMISSIVE_SHADE_TOL);
	uint8_t*             base_rgba = b->document->image_pixels[i].rgba;
	if (em && base_rgba) {
		for (size_t k = 0; k < (size_t)t->width * (size_t)t->height; ++k)
			if (em[k * 4 + 3])
				base_rgba[k * 4 + 0] = base_rgba[k * 4 + 1] = base_rgba[k * 4 + 2] = 0;
	}
	if (!em)
		return;
	mat->emissive_texture.texture = opt_gltf_set_image(
		b->document, b->image_slot++, em, (uint32_t)t->width, (uint32_t)t->height,
		xprintf_dup("Tex%02d_emissive", (int)i), xprintf_dup("%s_Tex%02d_emissive.png", b->basename, (int)i));
	set_color(mat->emissive_factor, 1.0f, 1.0f, 1.0f);
}

static float srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static uint8_t linear_to_srgb8(float c) {
	c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
	c = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)(c * 255.0f + 0.5f);
}

/* The emission of a fullbright texture with alpha: its colour multiplied by
 * its alpha, in linear values, so that the blend applies the alpha twice. A
 * projectile's emission is strong, and its faint edges would stay bright
 * otherwise. Returns false when out of memory. */
static bool fullbright_emissive(Build* b, int32_t i, cgltf_material* mat, const uint8_t* rgba) {
	const opt_texture_t* t  = &b->opt->textures[i];
	const size_t         n  = (size_t)t->width * (size_t)t->height;
	uint8_t*             em = (uint8_t*)malloc(n * 4);
	if (!em)
		return opt_gltf_build_fail(b, "out of memory (emissive)");
	for (size_t k = 0; k < n; ++k) {
		const float alpha = rgba[k * 4 + 3] / 255.0f;
		for (int c = 0; c < 3; ++c)
			em[k * 4 + c] = linear_to_srgb8(srgb_to_linear(rgba[k * 4 + c] / 255.0f) * alpha);
		em[k * 4 + 3] = 255;
	}
	mat->emissive_texture.texture = opt_gltf_set_image(
		b->document, b->image_slot++, em, (uint32_t)t->width, (uint32_t)t->height,
		xprintf_dup("Tex%02d_emissive", (int)i), xprintf_dup("%s_Tex%02d_emissive.png", b->basename, (int)i));
	return true;
}

/* Returns the texture for an image of a given material. The image is
 * written to the next slot, unless an image with the same pixels was
 * already written. */
static cgltf_texture* given_image(Build* b, const OptGltfImage* image) {
	for (size_t slot = 0; slot < b->image_slot; ++slot)
		if (b->document->image_pixels[slot].rgba == image->view.rgba)
			return &b->gb->textures[slot];
	if (b->image_slot == b->image_count)
		abort(); /* count_given_images missed it */
	cgltf_texture* texture =
		opt_gltf_set_image(b->document, b->image_slot, (uint8_t*)image->view.rgba, image->view.width,
						   image->view.height, xstrdup(image->name), xstrdup(image->file));
	b->document->image_pixels[b->image_slot++].borrowed = true;
	return texture;
}

/* Applies the inputs of a given material other than its base color image.
 * Images are written in the order of material_images. */
static void given_material(Build* b, cgltf_material* mat, const OptGltfMaterial* in) {
	if (in->name)
		mat->name = opt_gltf_keep_string(b->gb, xstrdup(in->name));
	set_color(mat->pbr_metallic_roughness.base_color_factor, in->base_color[0], in->base_color[1],
			  in->base_color[2]);
	if (in->emissive.view.rgba)
		mat->emissive_texture.texture = given_image(b, &in->emissive);
	set_color(mat->emissive_factor, in->emissive_factor[0], in->emissive_factor[1], in->emissive_factor[2]);
	if (in->emissive_strength > 0.0f) {
		mat->has_emissive_strength               = 1;
		mat->emissive_strength.emissive_strength = in->emissive_strength;
	}
	if (in->metallic_roughness.view.rgba)
		mat->pbr_metallic_roughness.metallic_roughness_texture.texture =
			given_image(b, &in->metallic_roughness);
	mat->pbr_metallic_roughness.metallic_factor  = in->metallic;
	mat->pbr_metallic_roughness.roughness_factor = in->roughness;
	if (in->transmission.view.rgba) {
		mat->has_transmission                          = 1;
		mat->transmission.transmission_factor          = 1.0f;
		mat->transmission.transmission_texture.texture = given_image(b, &in->transmission);
	}
	if (in->normal.view.rgba) {
		mat->normal_texture.texture = given_image(b, &in->normal);
		mat->normal_texture.scale   = 1.0f;
	}
	if (in->specular) {
		mat->has_specular             = 1;
		mat->specular.specular_factor = 1.0f;
		set_color(mat->specular.specular_color_factor, in->specular_color, in->specular_color,
				  in->specular_color);
	}
	b->gb->material_anim[mat - b->gb->materials] = in->json;
	/* The skin layers' images; opt_gltf_material_extensions writes `skins`. */
	for (size_t l = 0; l < in->skin_count; ++l) {
		const OptGltfSkinLayer* layer   = &in->skins[l];
		const OptGltfImage*     skin[3] = { &layer->base, &layer->emissive, &layer->metallic_roughness };
		for (int k = 0; k < 3; ++k)
			if (skin[k]->view.rgba)
				given_image(b, skin[k]);
	}
	if (in->skin_count)
		b->gb->material_given[mat - b->gb->materials] = in;
}

/* Replaces the textures of a screen surface's material (opt_gltf.h) with
 * the images of its cover. The images of each cover are written once. */
static bool screen_material(Build* b, int32_t i, cgltf_material* mat) {
	const OptGltfScreen* s       = &b->screens[i];
	cgltf_texture*       base    = NULL;
	cgltf_texture*       glow    = NULL;
	int32_t              sharing = -1;
	for (int32_t j = 0; j < i && sharing < 0 && s->cover_base.rgba && !s->hologram; ++j)
		if (b->screen_cover_image[j] >= 0 && b->screens[j].cover_base.rgba == s->cover_base.rgba)
			sharing = j;
	if (sharing >= 0) {
		b->screen_cover_image[i] = b->screen_cover_image[sharing];
	} else if (s->cover_base.rgba && !s->hologram) {
		const OptGltfImageView* views[2] = { &s->cover_base, &s->cover_emissive };
		const char*             kinds[2] = { "base", "emissive" };
		const char*             cover    = s->cover_name ? s->cover_name : "cover";
		b->screen_cover_image[i]         = (int32_t)b->image_slot;
		for (int k = 0; k < 2; ++k) {
			if (!views[k]->rgba)
				continue;
			const size_t bytes = (size_t)views[k]->width * views[k]->height * 4u;
			uint8_t*     copy  = (uint8_t*)malloc(bytes);
			if (!copy)
				return opt_gltf_build_fail(b, "out of memory (screen cover)");
			memcpy(copy, views[k]->rgba, bytes);
			opt_gltf_set_image(b->document, b->image_slot++, copy, views[k]->width, views[k]->height,
							   xprintf_dup("Cover_%s_%s", cover, kinds[k]),
							   xprintf_dup("%s_cover_%s_%s.png", b->basename, cover, kinds[k]));
		}
	}
	if (b->screen_cover_image[i] >= 0) {
		base = &b->gb->textures[b->screen_cover_image[i]];
		glow = s->cover_emissive.rgba ? &b->gb->textures[b->screen_cover_image[i] + 1] : NULL;
	}
	if (!screen_apply(b->gb, mat, s, base, glow))
		return opt_gltf_build_fail(b, "out of memory (screen)");
	return true;
}

/* Writes the image (slot i) and the material (material i) of texture `i`. */
static bool texture_material(Build* b, int32_t i) {
	const opt_texture_t*   t     = &b->opt->textures[i];
	GltfBuild*             gb    = b->gb;
	const OptGltfMaterial* given = b->materials ? &b->materials[i] : NULL;
	uint8_t* rgba = given ? (uint8_t*)given->base.view.rgba : OptGltf_DecodeTexture(t, b->base_shade);
	if (rgba) {
		opt_gltf_set_image(b->document, (size_t)i, rgba, (uint32_t)t->width, (uint32_t)t->height,
						   xprintf_dup("Tex%02d", (int)i),
						   xprintf_dup("%s_Tex%02d.png", b->basename, (int)i));
		b->document->image_pixels[i].borrowed = given != NULL;
	} else {
		gb->images[i].name      = opt_gltf_keep_string(gb, xprintf_dup("Tex%02d (missing pixels)", (int)i));
		gb->textures[i].image   = &gb->images[i];
		gb->textures[i].sampler = &gb->sampler;
		gb->textures[i].name    = gb->images[i].name;
	}

	/* PBR material with only baseColorTexture. By default, metallic is 0
	 * and roughness is 1. */
	cgltf_material* mat             = &gb->materials[i];
	mat->name                       = opt_gltf_keep_string(gb, xprintf_dup("Mat_Tex%02d", (int)i));
	mat->has_pbr_metallic_roughness = 1;
	set_color(mat->pbr_metallic_roughness.base_color_factor, 1.0f, 1.0f, 1.0f);
	mat->pbr_metallic_roughness.base_color_factor[3]       = 1.0f;
	mat->pbr_metallic_roughness.metallic_factor            = 0.0f;
	mat->pbr_metallic_roughness.roughness_factor           = 1.0f;
	mat->pbr_metallic_roughness.base_color_texture.texture = &gb->textures[i];
	if (!material_alpha(b, i, mat, rgba))
		return false;

	/* Fullbright: the texture lights itself and the scene adds no light,
	 * like projectiles in the classic renderer. Alpha still comes from the
	 * base texture. */
	if (b->fullbright) {
		set_color(mat->pbr_metallic_roughness.base_color_factor, 0.0f, 0.0f, 0.0f);
		mat->has_specular             = 1;
		mat->specular.specular_factor = 0.0f;
		mat->emissive_texture.texture = &gb->textures[i];
		if (t->alpha && rgba && !fullbright_emissive(b, i, mat, rgba))
			return false;
		set_color(mat->emissive_factor, 1.0f, 1.0f, 1.0f);
		mat->has_emissive_strength               = 1;
		mat->emissive_strength.emissive_strength = b->fullbright_strength;
	}
	if (b->tex_emissive[i])
		stock_emissive(b, i, mat);
	if (given && rgba)
		given_material(b, mat, given);
	return !(b->screens && b->screens[i].present) || screen_material(b, i, mat);
}

static void report_unused_alpha_overrides(const Build* b) {
	const OptGltfBuildOptions* o = b->options;
	for (size_t override_index = 0;
		 o->material.alpha_overrides && override_index < o->material.alpha_override_count; ++override_index) {
		bool matched = false;
		for (int32_t texture_index = 0; !matched && texture_index < b->opt->texture_count; ++texture_index) {
			char generated_name[32];
			snprintf(generated_name, sizeof generated_name, "Tex%02d", (int)texture_index);
			matched =
				alpha_name_equal(o->material.alpha_overrides[override_index].texture_name,
								 b->opt->textures[texture_index].name) ||
				alpha_name_equal(o->material.alpha_overrides[override_index].texture_name, generated_name);
		}
		if (!matched)
			opt_gltf_log(o, true, "unused alpha override '%s'",
						 o->material.alpha_overrides[override_index].texture_name);
	}
}

/* The material of untextured face groups (texture_index -1). */
static void untextured_material(Build* b) {
	cgltf_material* untex             = &b->gb->materials[b->opt->texture_count];
	untex->name                       = opt_gltf_keep_string(b->gb, xstrdup("Mat_Untextured"));
	untex->has_pbr_metallic_roughness = 1;
	set_color(untex->pbr_metallic_roughness.base_color_factor, 0.6f, 0.6f, 0.6f);
	untex->pbr_metallic_roughness.base_color_factor[3] = 1.0f;
	untex->pbr_metallic_roughness.metallic_factor      = 0.0f;
	untex->pbr_metallic_roughness.roughness_factor     = 1.0f;
	if (b->fullbright) {
		for (int channel = 0; channel < 3; ++channel) {
			untex->emissive_factor[channel] = untex->pbr_metallic_roughness.base_color_factor[channel];
			untex->pbr_metallic_roughness.base_color_factor[channel] = 0.0f;
		}
		untex->has_emissive_strength               = 1;
		untex->emissive_strength.emissive_strength = b->fullbright_strength;
		untex->has_specular                        = 1;
		untex->specular.specular_factor            = 0.0f;
	}
	if (b->materials) {
		/* Use the given factors. Images are ignored. */
		const OptGltfMaterial* um = b->options->material.untextured;
		set_color(untex->pbr_metallic_roughness.base_color_factor, um->base_color[0], um->base_color[1],
				  um->base_color[2]);
		set_color(untex->emissive_factor, um->emissive_factor[0], um->emissive_factor[1],
				  um->emissive_factor[2]);
		if (um->emissive_strength > 0.0f) {
			untex->has_emissive_strength               = 1;
			untex->emissive_strength.emissive_strength = um->emissive_strength;
		}
		untex->pbr_metallic_roughness.metallic_factor  = um->metallic;
		untex->pbr_metallic_roughness.roughness_factor = um->roughness;
		if (um->specular) {
			untex->has_specular             = 1;
			untex->specular.specular_factor = 1.0f;
			set_color(untex->specular.specular_color_factor, um->specular_color, um->specular_color,
					  um->specular_color);
		}
	}
	b->gb->materials_count = (size_t)b->opt->texture_count + 1;
	b->untextured          = (size_t)b->opt->texture_count;
}

/* Builds variant material `m`, at index untextured + 1 + m. Its name is the
 * name of its texture's material plus the variant suffix, and it copies the
 * alpha mode of that material. All its other inputs are its own. */
static void variant_material(Build* b, size_t m) {
	GltfBuild*                    gb  = b->gb;
	const OptGltfVariantMaterial* in  = &b->variants->materials[m];
	const cgltf_material*         of  = &gb->materials[in->texture];
	cgltf_material*               mat = &gb->materials[gb->materials_count++];
	mat->name                         = opt_gltf_keep_string(gb, xprintf_dup("%s%s", of->name, in->suffix));
	mat->has_pbr_metallic_roughness   = 1;
	mat->pbr_metallic_roughness.base_color_factor[3]       = 1.0f;
	mat->pbr_metallic_roughness.base_color_texture.texture = given_image(b, &in->material.base);
	mat->alpha_mode                                        = of->alpha_mode;
	mat->alpha_cutoff                                      = of->alpha_cutoff;
	OptGltfMaterial own                                    = in->material;
	own.name                                               = NULL;
	given_material(b, mat, &own);
}

bool opt_gltf_build_materials(Build* b) {
	for (int32_t i = 0; i < b->opt->texture_count; ++i)
		if (!texture_material(b, i))
			return false;
	report_unused_alpha_overrides(b);
	untextured_material(b);
	for (size_t m = 0; b->variants && m < b->variants->material_count; ++m)
		variant_material(b, m);
	b->gb->images_count   = b->image_slot;
	b->gb->textures_count = b->image_slot;
	return true;
}
