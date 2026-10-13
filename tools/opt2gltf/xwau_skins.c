#include "xwau_skins.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include "util.h"

static const char* const kExtensions[] = { ".bmp", ".png", ".jpg" };
#define EXTENSION_COUNT (sizeof kExtensions / sizeof kExtensions[0])

static bool has_extension(const char* name) {
	const size_t len = strlen(name);
	for (size_t e = 0; e < EXTENSION_COUNT; ++e)
		if (len >= 4 && strcasecmp(name + len - 4, kExtensions[e]) == 0)
			return true;
	return false;
}

/* Whether folder `dir` has a file at its top level. */
static bool has_files(const char* dir) {
	size_t count   = 0;
	char** entries = ListDir(dir, false, &count);
	bool   found   = false;
	for (size_t i = 0; !found && i < count; ++i) {
		char path[2048];
		found = PathFormat(path, sizeof path, "%s/%s", dir, entries[i]) && IsFile(path);
	}
	FreeList(entries, count);
	return found;
}

/* ---- Zip archives ------------------------------------------------------------- */

static uint32_t le16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

static uint32_t le32(const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }

/* The data of the first entry of the zip `zip` whose file name (whatever its
 * folder) is `name` (case-insensitive), stored or deflated; NULL when it has
 * none. */
static uint8_t* zip_entry(const uint8_t* zip, size_t size, const char* name, size_t* out_size,
						  const char* label, const char* path) {
	/* The end of central directory record, up to 64 KiB of comment after it. */
	size_t end = size >= 22 ? size - 22 : 0;
	while (size >= 22 && le32(zip + end) != 0x06054b50) {
		if (end == 0 || size - end > 22 + 65535)
			return NULL;
		--end;
	}
	if (size < 22)
		return NULL;
	const size_t entries = le16(zip + end + 10);
	size_t       at      = le32(zip + end + 16);
	for (size_t i = 0; i < entries && at + 46 <= size && le32(zip + at) == 0x02014b50; ++i) {
		const uint32_t method = le16(zip + at + 10), packed = le32(zip + at + 20),
					   unpacked = le32(zip + at + 24);
		const size_t   name_len = le16(zip + at + 28),
					   next     = at + 46 + name_len + le16(zip + at + 30) + le16(zip + at + 32);
		const size_t   local    = le32(zip + at + 42);
		if (at + 46 + name_len > size)
			return NULL;
		/* The file name, after its last '/' or '\'. */
		const char* entry = (const char*)zip + at + 46;
		size_t      start = 0;
		for (size_t c = 0; c < name_len; ++c)
			if (entry[c] == '/' || entry[c] == '\\')
				start = c + 1;
		at = next;
		if (name_len - start != strlen(name) || strncasecmp(entry + start, name, name_len - start) != 0)
			continue;
		if (local + 30 > size || le32(zip + local) != 0x04034b50)
			break;
		const size_t data = local + 30 + le16(zip + local + 26) + le16(zip + local + 28);
		if (data + packed > size)
			break;
		uint8_t* out = (uint8_t*)malloc(unpacked ? unpacked : 1);
		if (!out)
			return NULL;
		if (method == 0 && packed == unpacked) {
			memcpy(out, zip + data, unpacked);
		} else if (method != 8 ||
				   stbi_zlib_decode_noheader_buffer((char*)out, (int)unpacked, (const char*)zip + data,
													(int)packed) != (int)unpacked) {
			Report(label, "%s: cannot read %s", path, name);
			free(out);
			return NULL;
		}
		*out_size = unpacked;
		return out;
	}
	return NULL;
}

/* ---- Skins -------------------------------------------------------------------- */

static bool skin_set_has(const XwauSkinSet* set, const char* name) {
	for (size_t i = 0; i < set->count; ++i)
		if (strcmp(set->names[i], name) == 0)
			return true;
	return false;
}

static bool skin_set_add(XwauSkinSet* set, const char* name) {
	if (skin_set_has(set, name))
		return true;
	char*  copy = StrDup(name);
	char** slot = copy ? ARRAY_PUSH(set->names, set->count) : NULL;
	if (!slot) {
		free(copy);
		return false;
	}
	LowerInPlace(copy);
	*slot = copy;
	return true;
}

bool XwauSkins_Names(const char* game, const char* model, XwauSkinSet* out) {
	memset(out, 0, sizeof *out);
	char rel[512], dir[1024];
	if (!PathFormat(rel, sizeof rel, "FlightModels/Skins/%s", model))
		return false;
	if (!Xwau_FindDir(game, rel, dir, sizeof dir))
		return true;
	out->exists    = true;
	bool   ok      = true;
	size_t count   = 0;
	char** entries = ListDir(dir, false, &count);
	for (size_t i = 0; ok && i < count; ++i) {
		char path[2048];
		if (!(ok = PathFormat(path, sizeof path, "%s/%s", dir, entries[i])))
			break;
		const size_t len = strlen(entries[i]);
		if (IsDir(path)) {
			if (has_files(path))
				ok = skin_set_add(out, entries[i]);
		} else if (IsFile(path)) {
			ok = skin_set_add(out, "");
			if (ok && len > 4 && strcasecmp(entries[i] + len - 4, ".zip") == 0) {
				char base[1024];
				snprintf(base, sizeof base, "%.*s", (int)(len - 4), entries[i]);
				ok = skin_set_add(out, base);
			}
		}
	}
	FreeList(entries, count);
	return ok;
}

void XwauSkins_FreeNames(XwauSkinSet* set) {
	FreeList(set->names, set->count);
	memset(set, 0, sizeof *set);
}

/* A list token as CombineTextures reads it: the skin (lower case) and its
 * opacity. */
static void parse_token(const char* token, char* name, size_t cap, int* opacity) {
	const char* dash = strchr(token, '-');
	snprintf(name, cap, "%.*s", dash ? (int)(dash - token) : (int)strlen(token), token);
	for (char* c = name; *c; ++c)
		*c = Xwau_Lower(*c);
	*opacity = 100;
	if (!dash)
		return;
	/* The text up to the next '-', trimmed: [+-]?[0-9]+ within the int range. */
	const char* next = strchr(dash + 1, '-');
	char        text[64];
	snprintf(text, sizeof text, "%.*s", next ? (int)(next - dash - 1) : (int)strlen(dash + 1), dash + 1);
	Xwau_Trim(text, XWAU_RULES_CS);
	const char* digits = text + (text[0] == '+' || text[0] == '-');
	if (!*digits || strspn(digits, "0123456789") != strlen(digits))
		return;
	long long value;
	if (!Xwau_Stoi(text, &value))
		return;
	*opacity = value < 0 ? 0 : value > 100 ? 100 : (int)value;
}

/* Appends the canonical list of `tokens` to `key`: the existing skins,
 * "-<opacity>" when not 100, joined with ','. */
static void canonical(const XwauLines* tokens, const XwauSkinSet* skins, char* key, size_t cap) {
	size_t layers = 0;
	for (size_t i = 0; i < tokens->count; ++i) {
		char name[256];
		int  opacity;
		parse_token(tokens->items[i], name, sizeof name, &opacity);
		if (!skin_set_has(skins, name))
			continue;
		const size_t len = strlen(key);
		if (opacity == 100)
			snprintf(key + len, cap - len, "%s%s", layers ? "," : "", name);
		else
			snprintf(key + len, cap - len, "%s%s-%d", layers ? "," : "", name, opacity);
		++layers;
	}
}

bool XwauSkins_Markings(const XwauLines* lines, const char* model, int states, const XwauSkinSet* skins,
						XwauMarkings* out, size_t* count) {
	*count = 0;
	if (!skins->exists)
		return true;
	char base_value[4096];
	XwauLines_Value(lines, model, XWAU_RULES_CS, base_value, sizeof base_value);
	/* The <model>_fgc_<i> values. */
	char (*fgc)[4096] = (char (*)[4096])calloc(XWAU_SKINS_MAX_MARKINGS, sizeof *fgc);
	XwauLines base;
	if (!fgc || !Xwau_Tokenize(base_value, XWAU_RULES_CS, &base)) {
		free(fgc);
		return false;
	}
	int fgc_count = 0, default_count = 0;
	for (int i = 0; i < XWAU_SKINS_MAX_MARKINGS; ++i) {
		char key[512], skin[32];
		snprintf(key, sizeof key, "%s_fgc_%d", model, i);
		XwauLines_Value(lines, key, XWAU_RULES_CS, fgc[i], sizeof fgc[i]);
		if (fgc[i][0])
			fgc_count = i + 1;
		snprintf(skin, sizeof skin, "default_%d", i);
		if (skin_set_has(skins, skin))
			default_count = i + 1;
	}
	const bool has_default = skin_set_has(skins, "default") || default_count;
	if (!(has_default || base.count || fgc_count)) {
		XwauLines_Free(&base);
		free(fgc);
		return true;
	}
	int markings = fgc_count > states ? fgc_count : states;
	if (default_count > markings)
		markings = default_count;
	const bool has_base = has_default || base_value[0];
	bool       ok       = true;
	for (int i = 0; ok && i < markings; ++i) {
		XwauMarkings* r = &out[(*count)++];
		r->state        = i < states ? i : 0;
		r->list[0]      = 0;
		if (fgc[i][0] || has_base) {
			XwauLines own, tokens = { 0 };
			ok = Xwau_Tokenize(fgc[i], XWAU_RULES_CS, &own);
			for (size_t t = 0; ok && t < base.count + own.count; ++t) {
				const char* token = t < base.count ? base.items[t] : own.items[t - base.count];
				ok                = XwauLines_Add(&tokens, token, strlen(token));
			}
			if (ok && !tokens.count) {
				char fallback[32], lower[32];
				snprintf(lower, sizeof lower, "default_%d", i);
				snprintf(fallback, sizeof fallback, skin_set_has(skins, lower) ? "Default_%d" : "Default", i);
				ok = XwauLines_Add(&tokens, fallback, strlen(fallback));
			}
			if (ok)
				canonical(&tokens, skins, r->list, sizeof r->list);
			XwauLines_Free(&own);
			XwauLines_Free(&tokens);
		}
		r->skinned = r->list[0] != 0;
	}
	XwauLines_Free(&base);
	free(fgc);
	return ok;
}

/* Index of `name` in the skin list, added when new; -1 when out of memory. */
static int skin_index(XwauSkins* s, const char* name) {
	for (size_t i = 0; i < s->skins.skin_count; ++i)
		if (strcmp(s->skin_names[i], name) == 0)
			return (int)i;
	char*  copy = StrDup(name);
	char** slot = copy ? ARRAY_PUSH(s->skin_names, s->skins.skin_count) : NULL;
	if (!slot) {
		free(copy);
		return -1;
	}
	*slot = copy;
	return (int)s->skins.skin_count - 1;
}

static bool add_variant(XwauSkins* s, const char* name, int state, const char* key, const char* label) {
	for (size_t i = 0; i < s->skins.variant_count; ++i)
		if (strcmp(s->variant_names[i], name) == 0)
			return true;
	/* The variants and their names, side by side. */
	const size_t v    = s->skins.variant_count;
	char*        copy = StrDup(name);
	if (!copy || !ArrayGrow((void**)&s->variants, v, sizeof *s->variants) ||
		!ArrayGrow((void**)&s->variant_names, v, sizeof *s->variant_names)) {
		free(copy);
		return false;
	}
	s->variant_names[v]  = copy;
	s->variants[v].state = (uint32_t)state;
	s->skins.variant_count++;
	/* The layers, stored by index: pointers are set once all are known. */
	char list[1024];
	snprintf(list, sizeof list, "%s", key);
	for (char *token = list, *next; token; token = next) {
		next = strchr(token, ',');
		if (next)
			*next++ = 0;
		char* dash    = strchr(token, '-');
		int   opacity = 100;
		if (dash) {
			*dash   = 0;
			opacity = atoi(dash + 1);
		}
		if (!token[0]) {
			Report(label, "variant %s names the empty skin; layer left out", name);
			continue;
		}
		const int index = skin_index(s, token);
		size_t    layer = 0;
		for (size_t i = 0; i < v; ++i)
			layer += s->variants[i].layer_count;
		layer += s->variants[v].layer_count;
		if (index < 0 || !ArrayGrow((void**)&s->layers, layer, sizeof *s->layers))
			return false;
		s->layers[layer] = (XwauSkinLayer) { (uint32_t)index, (uint32_t)opacity };
		s->variants[v].layer_count++;
	}
	return true;
}

/* The skin image file `file` (lower case): from the skin's folders, else
 * its zip. */
static uint8_t* skin_file(const char* game, const char* model, const char* skin, const char* file,
						  size_t* size, const char* label) {
	char rel[1024], dir[1024];
	if (!PathFormat(rel, sizeof rel, "FlightModels/Skins/%s/%s", model, skin))
		return NULL;
	if (Xwau_FindDir(game, rel, dir, sizeof dir) && has_files(dir)) {
		size_t count   = 0;
		char** entries = ListDir(dir, false, &count);
		for (size_t i = 0; i < count; ++i) {
			char entry[2048];
			if (strcasecmp(entries[i], file) == 0 &&
				PathFormat(entry, sizeof entry, "%s/%s", dir, entries[i]) && IsFile(entry)) {
				FreeList(entries, count);
				/* An empty file is no image. */
				uint8_t* data = (uint8_t*)ReadWholeFile(entry, size);
				if (data && !*size) {
					free(data);
					data = NULL;
				}
				return data;
			}
		}
		FreeList(entries, count);
		return NULL;
	}
	if (!PathFormat(rel, sizeof rel, "FlightModels/Skins/%s", model) ||
		!Xwau_FindDir(game, rel, dir, sizeof dir))
		return NULL;
	size_t count     = 0;
	char** entries   = ListDir(dir, false, &count);
	char   zip[2048] = "";
	for (size_t i = 0; i < count; ++i) {
		char path[2048], lower[1024];
		snprintf(lower, sizeof lower, "%s", entries[i]);
		LowerInPlace(lower);
		if (strlen(lower) == strlen(skin) + 4 && strncmp(lower, skin, strlen(skin)) == 0 &&
			strcmp(lower + strlen(skin), ".zip") == 0 &&
			PathFormat(path, sizeof path, "%s/%s", dir, entries[i]) && IsFile(path))
			snprintf(zip, sizeof zip, "%s", path);
	}
	FreeList(entries, count);
	if (!zip[0])
		return NULL;
	size_t   zip_size = 0;
	uint8_t* data     = (uint8_t*)ReadWholeFile(zip, &zip_size);
	uint8_t* entry    = data ? zip_entry(data, zip_size, file, size, label, zip) : NULL;
	free(data);
	return entry;
}

/* The skin's image for texture `t`: NULL rgba when it has none, or when its
 * size differs (reported). */
static bool load_image(const char* game, const char* model, const char* skin, const opt_texture_t* t,
					   const char* label, size_t* skipped, OptGltfImageView* out) {
	char name[256];
	snprintf(name, sizeof name, "%s", t->name);
	LowerInPlace(name);
	memset(out, 0, sizeof *out);
	if (!name[0])
		return true;
	for (size_t e = 0; e < EXTENSION_COUNT; ++e) {
		char file[300];
		snprintf(file, sizeof file, "%s%s", name, kExtensions[e]);
		size_t   size = 0;
		uint8_t* data = skin_file(game, model, skin, file, &size, label);
		if (!data)
			continue;
		int      w = 0, h = 0, channels = 0;
		uint8_t* rgba = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
		free(data);
		if (!rgba) {
			Report(label, "skin %s image %s: %s", skin, file, stbi_failure_reason());
			return false;
		}
		if (w != t->width || h != t->height) {
			Report(label, "skin %s image %s is %dx%d, texture %dx%d; skipped", skin, file, w, h,
				   (int)t->width, (int)t->height);
			stbi_image_free(rgba);
			++*skipped;
			return true;
		}
		/* OPT row order: the hook flips the loaded image (FlipPixels). */
		uint8_t*     flipped = (uint8_t*)malloc((size_t)w * h * 4);
		const size_t stride  = (size_t)w * 4;
		if (!flipped) {
			stbi_image_free(rgba);
			return false;
		}
		for (int y = 0; y < h; ++y)
			memcpy(flipped + (size_t)y * stride, rgba + (size_t)(h - 1 - y) * stride, stride);
		stbi_image_free(rgba);
		*out = (OptGltfImageView) { flipped, (uint32_t)w, (uint32_t)h };
		return true;
	}
	return true;
}

int XwauSkins_StateCount(const opt_file_t* opt) {
	int32_t states = 1;
	for (int32_t mi = 0; mi < opt->mesh_count; ++mi)
		for (int32_t g = 0; opt->meshes[mi].lod_count > 0 && g < opt->meshes[mi].lods[0].group_count; ++g)
			if (opt->meshes[mi].lods[0].groups[g].state_count > states)
				states = opt->meshes[mi].lods[0].groups[g].state_count;
	return (int)states;
}

bool XwauSkins_Resolve(const char* game, const XwauMissions* missions, const char* model,
					   const opt_file_t* opt, const char* label, XwauSkins* out) {
	memset(out, 0, sizeof *out);
	XwauSkinSet skins;
	if (!XwauSkins_Names(game, model, &skins))
		return false;
	const int     states = XwauSkins_StateCount(opt);
	XwauMarkings* lists  = (XwauMarkings*)malloc(XWAU_SKINS_MAX_MARKINGS * sizeof *lists);
	bool          ok     = lists != NULL;
	for (size_t c = 0; ok && skins.exists && c < XwauMissions_SkinContextCount(missions); ++c) {
		size_t count = 0;
		ok = XwauSkins_Markings(XwauMissions_SkinContext(missions, c), model, states, &skins, lists, &count);
		for (size_t i = 0; ok && i < count; ++i) {
			char name[1100];
			snprintf(name, sizeof name, "%d|%s", lists[i].state, lists[i].list);
			if (lists[i].skinned)
				ok = add_variant(out, name, lists[i].state, lists[i].list, label);
		}
		if (ok && c == 0 && count) {
			out->markings = (char**)calloc(count, sizeof *out->markings);
			ok            = out->markings != NULL;
			for (size_t i = 0; ok && i < count; ++i) {
				char name[1100];
				snprintf(name, sizeof name, "%d|%s", lists[i].state, lists[i].list);
				ok = !lists[i].skinned || (out->markings[i] = StrDup(name)) != NULL;
			}
			out->skins.markings_count = ok ? count : 0;
		}
	}
	free(lists);
	XwauSkins_FreeNames(&skins);
	if (ok && !out->skins.variant_count) {
		XwauSkins_Free(out);
		return true;
	}
	/* The layers' pointers, now that every variant's are known. */
	size_t next = 0;
	for (size_t v = 0; ok && v < out->skins.variant_count; ++v) {
		out->variants[v].name   = out->variant_names[v];
		out->variants[v].layers = &out->layers[next];
		next += out->variants[v].layer_count;
	}
	out->skins.variants         = out->variants;
	out->skins.default_markings = (const char* const*)out->markings;

	const size_t texture_count = (size_t)opt->texture_count;
	size_t       found = 0, skipped = 0;
	if (ok) {
		const size_t count = out->skins.skin_count * texture_count;
		out->images        = (OptGltfImageView*)calloc(count ? count : 1, sizeof *out->images);
		out->image_count   = out->images ? count : 0;
		ok                 = out->images != NULL;
	}
	for (size_t s = 0; ok && s < out->skins.skin_count; ++s) {
		for (size_t t = 0; ok && t < texture_count; ++t) {
			OptGltfImageView* view = &out->images[s * texture_count + t];
			ok = load_image(game, model, out->skin_names[s], &opt->textures[t], label, &skipped, view);
			found += view->rgba != NULL;
		}
	}
	if (!ok) {
		XwauSkins_Free(out);
		return false;
	}
	out->skins.images = out->images;
	Report(label, "%zu skin variants, %zu skins, %zu skin images, %zu skipped", out->skins.variant_count,
		   out->skins.skin_count, found, skipped);
	return true;
}

void XwauSkins_Free(XwauSkins* s) {
	for (size_t i = 0; i < s->image_count; ++i)
		free((void*)s->images[i].rgba);
	free(s->images);
	for (size_t i = 0; s->skin_names && i < s->skins.skin_count; ++i)
		free(s->skin_names[i]);
	free(s->skin_names);
	for (size_t i = 0; s->variant_names && i < s->skins.variant_count; ++i)
		free(s->variant_names[i]);
	free(s->variant_names);
	free(s->variants);
	free(s->layers);
	for (size_t i = 0; s->markings && i < s->skins.markings_count; ++i)
		free(s->markings[i]);
	free(s->markings);
	memset(s, 0, sizeof *s);
}
