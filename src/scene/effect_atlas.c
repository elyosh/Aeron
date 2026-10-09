/*
 * AeronEffectAtlas — resident effect image libraries. See
 * aeron/scene/effect_atlas.h.
 */

#include "aeron/scene/effect_atlas.h"

#include "aeron/config_file.h"
#include "aeron/log.h"
#include "aeron/scene/image_cache.h"
#include "aeron/scene/ktx2_reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EFFECT_NAME_MAX 64
#define EFFECT_KTX2_MAX_SIZE (512u * 1024u * 1024u)

typedef struct EffectEntry {
	char*            key;
	AeronEffectImage image; /* layer[] holds the library page */
} EffectEntry;

typedef struct EffectLibrary {
	char         name[EFFECT_NAME_MAX];
	uint32_t     refs; /* 0: free slot */
	uint32_t     page_count[2];
	uint32_t*    pages[2]; /* atlas layer of each page */
	EffectEntry* entries;  /* sorted by key */
	uint32_t     entry_count;
} EffectLibrary;

/* One of the two array textures. */
typedef struct EffectTexture {
	AeronTexture*      texture;
	AeronTextureFormat format;
	int                levels;
	uint32_t           capacity; /* layers */
	uint8_t*           used;     /* per layer */
} EffectTexture;

struct AeronEffectAtlas {
	AeronVfs*      vfs;
	AeronVfsRoot   root;
	char           dir[256];
	uint32_t       page_size;
	EffectTexture  tex[2];
	EffectLibrary* libraries;
	uint32_t       library_count;
	AeronTexture** retired;
	uint32_t       retired_count, retired_capacity;
};

static const char* kSuffix[2] = { "layer", "emissive" };

AeronEffectAtlas* AeronEffectAtlas_Create(AeronVfs* vfs, AeronVfsRoot root, const char* dir) {
	if (!vfs || !dir || strlen(dir) >= sizeof(((AeronEffectAtlas*)0)->dir))
		return NULL;
	AeronEffectAtlas* atlas = (AeronEffectAtlas*)calloc(1, sizeof *atlas);
	if (atlas) {
		atlas->vfs  = vfs;
		atlas->root = root;
		snprintf(atlas->dir, sizeof atlas->dir, "%s", dir);
	}
	return atlas;
}

static void library_free(EffectLibrary* l) {
	for (uint32_t i = 0; i < l->entry_count; ++i)
		free(l->entries[i].key);
	free(l->entries);
	free(l->pages[0]);
	free(l->pages[1]);
	memset(l, 0, sizeof *l);
}

void AeronEffectAtlas_Commit(AeronEffectAtlas* atlas) {
	if (!atlas)
		return;
	for (uint32_t i = 0; i < atlas->retired_count; ++i)
		Aeron_DestroyTexture(atlas->retired[i]);
	atlas->retired_count = 0;
}

void AeronEffectAtlas_Destroy(AeronEffectAtlas* atlas) {
	if (!atlas)
		return;
	AeronEffectAtlas_Commit(atlas);
	free(atlas->retired);
	for (uint32_t i = 0; i < atlas->library_count; ++i)
		library_free(&atlas->libraries[i]);
	free(atlas->libraries);
	for (int t = 0; t < 2; ++t) {
		if (atlas->tex[t].texture)
			Aeron_DestroyTexture(atlas->tex[t].texture);
		free(atlas->tex[t].used);
	}
	free(atlas);
}

static int entry_compare(const void* a, const void* b) {
	return strcmp(((const EffectEntry*)a)->key, ((const EffectEntry*)b)->key);
}

/* `{ page, rect: [x, y, w, h], lod }` of one texture of an index entry. */
static int read_place(const AeronConfigNode* node, uint32_t page_count, uint32_t page_size,
					  AeronEffectImage* img, int t) {
	if (!node)
		return 1;
	const AeronConfigNode* rect = AeronConfigNode_MapGet(node, "rect");
	const int64_t          page = AeronConfigNode_Int(AeronConfigNode_MapGet(node, "page"), -1);
	const int64_t          lod  = AeronConfigNode_Int(AeronConfigNode_MapGet(node, "lod"), -1);
	if (page < 0 || (uint64_t)page >= page_count || lod < 0 || lod > 15 ||
		AeronConfigNode_SequenceCount(rect) != 4)
		return 0;
	int64_t r[4];
	for (int k = 0; k < 4; ++k)
		r[k] = AeronConfigNode_Int(AeronConfigNode_SequenceGet(rect, (size_t)k), -1);
	if (r[0] < 0 || r[1] < 0 || r[2] <= 0 || r[3] <= 0 || r[0] + r[2] > page_size || r[1] + r[3] > page_size)
		return 0;
	img->present[t] = true;
	img->layer[t]   = (uint32_t)page;
	img->lod[t]     = (uint32_t)lod;
	for (int k = 0; k < 4; ++k)
		img->rect[t][k] = (uint32_t)r[k];
	return 1;
}

static int read_index(const AeronConfigFile* index, EffectLibrary* l, uint32_t page_size, const char* label) {
	const AeronConfigNode* images = AeronConfigFile_GetNode(index, "images");
	const size_t           count  = AeronConfigNode_MapCount(images);
	l->entries                    = (EffectEntry*)calloc(count ? count : 1, sizeof *l->entries);
	if (!l->entries)
		return 0;
	for (size_t i = 0; i < count; ++i) {
		const AeronConfigNode* node = AeronConfigNode_MapValueAt(images, i);
		const AeronConfigNode* crop = AeronConfigNode_MapGet(node, "crop");
		EffectEntry*           e    = &l->entries[l->entry_count];
		e->image.crop[2] = e->image.crop[3] = 1.0f;
		if (!read_place(AeronConfigNode_MapGet(node, "layer"), l->page_count[0], page_size, &e->image, 0) ||
			!read_place(AeronConfigNode_MapGet(node, "emissive"), l->page_count[1], page_size, &e->image,
						1) ||
			(crop && AeronConfigNode_SequenceCount(crop) != 4)) {
			Aeron_LogError("aeron.scene", "%s: malformed image '%s'", label,
						   AeronConfigNode_MapKeyAt(images, i));
			return 0;
		}
		for (int k = 0; crop && k < 4; ++k)
			e->image.crop[k] =
				(float)AeronConfigNode_Float(AeronConfigNode_SequenceGet(crop, (size_t)k), 0.0);
		e->key = strdup(AeronConfigNode_MapKeyAt(images, i));
		if (!e->key)
			return 0;
		l->entry_count++;
	}
	qsort(l->entries, l->entry_count, sizeof *l->entries, entry_compare);
	return 1;
}

/* Grows texture `t` to `capacity` layers, copying the used ones. */
static int texture_grow(AeronEffectAtlas* atlas, AeronCommandBuffer* cmd, int t, uint32_t capacity) {
	EffectTexture* et   = &atlas->tex[t];
	uint8_t*       used = (uint8_t*)realloc(et->used, capacity);
	if (!used)
		return 0;
	memset(used + et->capacity, 0, capacity - et->capacity);
	et->used = used;
	char name[32];
	snprintf(name, sizeof name, "effect_atlas.%s", kSuffix[t]);
	AeronTexture* texture = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width       = (int)atlas->page_size,
		.height      = (int)atlas->page_size,
		.mip_count   = et->levels,
		.format      = et->format,
		.usage       = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.layer_count = (int)capacity,
		.debug_name  = name,
	});
	if (!texture)
		return 0;
	for (uint32_t layer = 0; et->texture && layer < et->capacity; ++layer) {
		if (!et->used[layer])
			continue;
		for (int level = 0; level < et->levels; ++level) {
			const uint32_t size = atlas->page_size >> level ? atlas->page_size >> level : 1u;
			if (!Aeron_CopyTextureCmd(cmd, &(AeronTextureCopyDesc) { .source                = et->texture,
																	 .source_mip_level      = (uint32_t)level,
																	 .source_layer          = layer,
																	 .destination           = texture,
																	 .destination_mip_level = (uint32_t)level,
																	 .destination_layer     = layer,
																	 .width                 = size,
																	 .height                = size })) {
				Aeron_DestroyTexture(texture);
				return 0;
			}
		}
	}
	if (et->texture) {
		if (atlas->retired_count == atlas->retired_capacity) {
			const uint32_t cap = atlas->retired_capacity ? atlas->retired_capacity * 2u : 4u;
			AeronTexture** r   = (AeronTexture**)realloc(atlas->retired, cap * sizeof *r);
			if (!r) {
				Aeron_DestroyTexture(texture);
				return 0;
			}
			atlas->retired          = r;
			atlas->retired_capacity = cap;
		}
		atlas->retired[atlas->retired_count++] = et->texture;
	}
	et->texture  = texture;
	et->capacity = capacity;
	return 1;
}

/* Uploads `ktx`'s pages into free layers of texture `t`, recording them in
 * `pages`. */
static int load_pages(AeronEffectAtlas* atlas, AeronCommandBuffer* cmd, int t, const Ktx2* ktx,
					  uint32_t* pages, uint32_t page_count, const char* label) {
	EffectTexture* et = &atlas->tex[t];
	if (ktx2_width(ktx) != (int)atlas->page_size || ktx2_height(ktx) != (int)atlas->page_size ||
		ktx2_face_count(ktx) != 1 ||
		(uint32_t)(ktx2_layer_count(ktx) > 0 ? ktx2_layer_count(ktx) : 1) != page_count) {
		Aeron_LogError("aeron.scene", "%s: %s pages do not match the index", label, kSuffix[t]);
		return 0;
	}
	if (!et->levels) {
		et->format = ktx2_format(ktx);
		et->levels = ktx2_level_count(ktx);
	} else if (ktx2_format(ktx) != et->format || ktx2_level_count(ktx) != et->levels) {
		Aeron_LogError("aeron.scene", "%s: %s pages differ in format or mips from the loaded libraries",
					   label, kSuffix[t]);
		return 0;
	}
	uint32_t free_layers = 0;
	for (uint32_t layer = 0; layer < et->capacity; ++layer)
		free_layers += !et->used[layer];
	if (free_layers < page_count && !texture_grow(atlas, cmd, t, et->capacity + page_count - free_layers)) {
		Aeron_LogError("aeron.scene", "%s: cannot grow the %s atlas", label, kSuffix[t]);
		return 0;
	}
	for (uint32_t p = 0, layer = 0; p < page_count; ++layer)
		if (!et->used[layer])
			pages[p++] = layer;
	if (!Aeron_ImageUploadKtx2IntoLayers(cmd, ktx, et->texture, pages)) {
		Aeron_LogError("aeron.scene", "%s: %s page upload failed", label, kSuffix[t]);
		return 0;
	}
	for (uint32_t p = 0; p < page_count; ++p)
		et->used[pages[p]] = 1;
	return 1;
}

static void library_release_layers(AeronEffectAtlas* atlas, EffectLibrary* l) {
	for (int t = 0; t < 2; ++t)
		for (uint32_t p = 0; l->pages[t] && p < l->page_count[t]; ++p)
			atlas->tex[t].used[l->pages[t][p]] = 0;
}

static int library_load(AeronEffectAtlas* atlas, AeronCommandBuffer* cmd, EffectLibrary* l) {
	AeronVfs* const    vfs  = atlas->vfs;
	const AeronVfsRoot root = atlas->root;
	char               path[512], label[512];
	snprintf(label, sizeof label, "%s/%s", atlas->dir, l->name);
	snprintf(path, sizeof path, "%s.yaml", label);
	AeronConfigFile* index = NULL;
	AeronConfigError error = { 0 };
	if (!AeronConfigFile_LoadYamlEx(vfs, root, path, &index, &error)) {
		Aeron_LogError("aeron.scene", "effect library %s: %s", path, error.message);
		return 0;
	}
	const int64_t version       = AeronConfigFile_GetInt(index, "version", 0);
	const int64_t page_size     = AeronConfigFile_GetInt(index, "page_size", 0);
	const int64_t page_count[2] = { AeronConfigFile_GetInt(index, "layer_pages", 0),
									AeronConfigFile_GetInt(index, "emissive_pages", 0) };
	int           ok            = version == 1 && page_size > 0 && page_size <= 16384 && page_count[0] >= 0 &&
								  page_count[1] >= 0 && page_count[0] < 4096 && page_count[1] < 4096 &&
								  (!atlas->page_size || (uint32_t)page_size == atlas->page_size);
	if (!ok)
		Aeron_LogError("aeron.scene", "%s: unsupported version, page size or page count", label);
	if (ok && !atlas->page_size)
		atlas->page_size = (uint32_t)page_size;
	for (int t = 0; ok && t < 2; ++t) {
		l->page_count[t] = (uint32_t)page_count[t];
		if (!l->page_count[t])
			continue;
		l->pages[t]    = (uint32_t*)calloc(l->page_count[t], sizeof *l->pages[t]);
		uint8_t* bytes = NULL;
		size_t   size  = 0;
		snprintf(path, sizeof path, "%s.%s.ktx2", label, kSuffix[t]);
		ok = l->pages[t] && AeronVfs_ReadAll(vfs, root, path, EFFECT_KTX2_MAX_SIZE, &bytes, &size);
		if (!ok) {
			Aeron_LogError("aeron.scene", "effect library: cannot read %s", path);
			free(l->pages[t]);
			l->pages[t] = NULL;
			break;
		}
		Ktx2* ktx = ktx2_open_mem(bytes, size, path);
		ok        = ktx && load_pages(atlas, cmd, t, ktx, l->pages[t], l->page_count[t], label);
		ktx2_close(ktx);
		free(bytes);
		if (!ok) {
			free(l->pages[t]);
			l->pages[t] = NULL;
		}
	}
	ok = ok && read_index(index, l, atlas->page_size, label);
	AeronConfigFile_Destroy(index);
	if (!ok) {
		library_release_layers(atlas, l);
		library_free(l);
		return 0;
	}
	Aeron_LogInfo("aeron.scene", "effect library %s: %u images, %u layer and %u emissive pages", label,
				  l->entry_count, l->page_count[0], l->page_count[1]);
	return 1;
}

int AeronEffectAtlas_Acquire(AeronEffectAtlas* atlas, AeronCommandBuffer* cmd, const char* name,
							 uint32_t* out_library) {
	if (!atlas || !cmd || !name || !out_library || strlen(name) >= EFFECT_NAME_MAX)
		return 0;
	uint32_t slot = atlas->library_count;
	for (uint32_t i = 0; i < atlas->library_count; ++i) {
		EffectLibrary* l = &atlas->libraries[i];
		if (l->refs && strcmp(l->name, name) == 0) {
			l->refs++;
			*out_library = i;
			return 1;
		}
		if (!l->refs && slot == atlas->library_count)
			slot = i;
	}
	if (slot == atlas->library_count) {
		EffectLibrary* grown =
			(EffectLibrary*)realloc(atlas->libraries, (atlas->library_count + 1u) * sizeof *atlas->libraries);
		if (!grown)
			return 0;
		atlas->libraries = grown;
		memset(&atlas->libraries[slot], 0, sizeof *atlas->libraries);
		atlas->library_count++;
	}
	EffectLibrary* l = &atlas->libraries[slot];
	snprintf(l->name, sizeof l->name, "%s", name);
	if (!library_load(atlas, cmd, l))
		return 0;
	l->refs      = 1;
	*out_library = slot;
	return 1;
}

void AeronEffectAtlas_Release(AeronEffectAtlas* atlas, uint32_t library) {
	if (!atlas || library >= atlas->library_count || !atlas->libraries[library].refs)
		return;
	EffectLibrary* l = &atlas->libraries[library];
	if (--l->refs)
		return;
	library_release_layers(atlas, l);
	library_free(l);
}

int AeronEffectAtlas_Find(const AeronEffectAtlas* atlas, uint32_t library, const char* key,
						  AeronEffectImage* out) {
	if (!atlas || library >= atlas->library_count || !atlas->libraries[library].refs || !key)
		return 0;
	const EffectLibrary* l     = &atlas->libraries[library];
	const EffectEntry    probe = { .key = (char*)key };
	const EffectEntry*   e =
		(const EffectEntry*)bsearch(&probe, l->entries, l->entry_count, sizeof *l->entries, entry_compare);
	if (!e)
		return 0;
	*out = e->image;
	for (int t = 0; t < 2; ++t)
		if (out->present[t])
			out->layer[t] = l->pages[t][e->image.layer[t]];
	return 1;
}

AeronTexture* AeronEffectAtlas_Texture(const AeronEffectAtlas* atlas, int which) {
	return atlas && (which == AERON_EFFECT_LAYER || which == AERON_EFFECT_EMISSIVE)
			   ? atlas->tex[which].texture
			   : NULL;
}

uint32_t AeronEffectAtlas_PageSize(const AeronEffectAtlas* atlas) { return atlas ? atlas->page_size : 0u; }

/* ---- effects.yaml ------------------------------------------------------ */

typedef struct EffectDataFrame {
	AeronEffectImage image;
	bool             found;
	double           end; /* seconds from the flipbook start */
} EffectDataFrame;

typedef struct EffectDataImage {
	char*            key;
	bool             once;
	float            strength, tile; /* energy effects */
	EffectDataFrame* frames;
	uint32_t         frame_count;
} EffectDataImage;

static const char* const kEnergyNames[AERON_EFFECT_ENERGY_COUNT] = { "shields", "tractor", "jamming" };

struct AeronEffectData {
	AeronEffectAtlas* atlas;
	EffectDataImage*  images; /* sorted by key */
	uint32_t          image_count;
	EffectDataImage   energy[AERON_EFFECT_ENERGY_COUNT]; /* frame_count 0: absent */
	uint32_t*         libraries;
	uint32_t          library_count;
};

static int data_image_compare(const void* a, const void* b) {
	return strcmp(((const EffectDataImage*)a)->key, ((const EffectDataImage*)b)->key);
}

/* The data's index of library `name`, acquiring it once; UINT32_MAX when it
 * cannot be loaded. */
static uint32_t data_library(AeronEffectData* data, AeronCommandBuffer* cmd, const char* name,
							 uint32_t capacity) {
	for (uint32_t i = 0; i < data->library_count; ++i)
		if (strcmp(data->atlas->libraries[data->libraries[i]].name, name) == 0)
			return data->libraries[i];
	uint32_t library;
	if (data->library_count == capacity || !AeronEffectAtlas_Acquire(data->atlas, cmd, name, &library))
		return UINT32_MAX;
	data->libraries[data->library_count++] = library;
	return library;
}

/* A flipbook entry: an image's (`which` AERON_EFFECT_LAYER, kind "mark") or
 * an energy effect's (AERON_EFFECT_EMISSIVE, with strength and tile). */
static int data_read_flipbook(AeronEffectData* data, AeronCommandBuffer* cmd, const char* key,
							  const AeronConfigNode* node, uint32_t library_capacity, int which,
							  EffectDataImage* out) {
	const char*            library = AeronConfigNode_String(AeronConfigNode_MapGet(node, "library"), NULL);
	const char*            kind    = AeronConfigNode_String(AeronConfigNode_MapGet(node, "kind"), NULL);
	const char*            mode    = AeronConfigNode_String(AeronConfigNode_MapGet(node, "mode"), NULL);
	const AeronConfigNode* frames  = AeronConfigNode_MapGet(node, "frames");
	const size_t           count   = AeronConfigNode_SequenceCount(frames);
	if (!library || (which == AERON_EFFECT_LAYER && (!kind || strcmp(kind, "mark") != 0)) || !mode ||
		(strcmp(mode, "loop") != 0 && strcmp(mode, "once") != 0) || count == 0 || count > 0xFFFF)
		return 0;
	if (which == AERON_EFFECT_EMISSIVE) {
		out->strength = (float)AeronConfigNode_Float(AeronConfigNode_MapGet(node, "strength"), -1.0);
		out->tile     = (float)AeronConfigNode_Float(AeronConfigNode_MapGet(node, "tile"), -1.0);
		if (!(out->strength >= 0.0f) || !(out->tile > 0.0f))
			return 0;
	}
	out->key    = strdup(key);
	out->once   = strcmp(mode, "once") == 0;
	out->frames = (EffectDataFrame*)calloc(count, sizeof *out->frames);
	if (!out->key || !out->frames)
		return 0;
	const uint32_t lib = data_library(data, cmd, library, library_capacity);
	double         end = 0.0;
	for (size_t f = 0; f < count; ++f) {
		const AeronConfigNode* frame   = AeronConfigNode_SequenceGet(frames, f);
		const char*            image   = AeronConfigNode_String(AeronConfigNode_SequenceGet(frame, 0), NULL);
		const double           seconds = AeronConfigNode_Float(AeronConfigNode_SequenceGet(frame, 1), -1.0);
		if (AeronConfigNode_SequenceCount(frame) != 2 || !image || !(seconds >= 0.0))
			return 0;
		EffectDataFrame* d = &out->frames[f];
		end += seconds;
		d->end   = end;
		d->found = lib != UINT32_MAX && AeronEffectAtlas_Find(data->atlas, lib, image, &d->image) &&
				   d->image.present[which];
	}
	out->frame_count = (uint32_t)count;
	return 1;
}

AeronEffectData* AeronEffectData_Load(AeronEffectAtlas* atlas, AeronCommandBuffer* cmd) {
	if (!atlas || !cmd)
		return NULL;
	char path[512];
	snprintf(path, sizeof path, "%s/effects.yaml", atlas->dir);
	AeronConfigFile* yaml  = NULL;
	AeronConfigError error = { 0 };
	if (!AeronConfigFile_LoadYamlEx(atlas->vfs, atlas->root, path, &yaml, &error)) {
		if (error.code != AERON_CONFIG_ERROR_NOT_FOUND)
			Aeron_LogError("aeron.scene", "%s: %s", path, error.message);
		return NULL;
	}
	const AeronConfigNode* images = AeronConfigFile_GetNode(yaml, "images");
	const AeronConfigNode* energy    = AeronConfigFile_GetNode(yaml, "energy");
	const uint32_t         count  = (uint32_t)AeronConfigNode_MapCount(images);
	const uint32_t         libraries = count + AERON_EFFECT_ENERGY_COUNT;
	AeronEffectData*       data   = (AeronEffectData*)calloc(1, sizeof *data);
	int                    ok     = data && AeronConfigFile_GetInt(yaml, "version", 0) == 1;
	if (ok) {
		data->atlas     = atlas;
		data->images    = (EffectDataImage*)calloc(count ? count : 1, sizeof *data->images);
		data->libraries = (uint32_t*)calloc(libraries, sizeof *data->libraries);
		ok              = data->images && data->libraries;
	}
	for (uint32_t i = 0; ok && i < count; ++i) {
		ok = data_read_flipbook(data, cmd, AeronConfigNode_MapKeyAt(images, i),
								AeronConfigNode_MapValueAt(images, i), libraries, AERON_EFFECT_LAYER,
								&data->images[i]);
		data->image_count = i + 1u;
		if (!ok)
			Aeron_LogError("aeron.scene", "%s: malformed image '%s'", path,
						   AeronConfigNode_MapKeyAt(images, i));
	}
	for (int e = 0; ok && e < AERON_EFFECT_ENERGY_COUNT; ++e) {
		const AeronConfigNode* node = AeronConfigNode_MapGet(energy, kEnergyNames[e]);
		if (!node)
			continue;
		ok = data_read_flipbook(data, cmd, kEnergyNames[e], node, libraries, AERON_EFFECT_EMISSIVE,
								&data->energy[e]);
		if (!ok)
			Aeron_LogError("aeron.scene", "%s: malformed energy effect '%s'", path, kEnergyNames[e]);
	}
	AeronConfigFile_Destroy(yaml);
	if (!ok) {
		AeronEffectData_Destroy(data);
		return NULL;
	}
	qsort(data->images, data->image_count, sizeof *data->images, data_image_compare);
	Aeron_LogInfo("aeron.scene", "%s: %u image flipbooks from %u libraries", path, data->image_count,
				  data->library_count);
	return data;
}

void AeronEffectData_Destroy(AeronEffectData* data) {
	if (!data)
		return;
	for (uint32_t i = 0; i < data->library_count; ++i)
		AeronEffectAtlas_Release(data->atlas, data->libraries[i]);
	for (uint32_t i = 0; i < data->image_count; ++i) {
		free(data->images[i].key);
		free(data->images[i].frames);
	}
	for (int e = 0; e < AERON_EFFECT_ENERGY_COUNT; ++e) {
		free(data->energy[e].key);
		free(data->energy[e].frames);
	}
	free(data->images);
	free(data->libraries);
	free(data);
}

int AeronEffectData_FindImage(const AeronEffectData* data, const char* key) {
	if (!data || !key)
		return -1;
	const EffectDataImage  probe = { .key = (char*)key };
	const EffectDataImage* found = (const EffectDataImage*)bsearch(&probe, data->images, data->image_count,
																   sizeof *data->images, data_image_compare);
	return found ? (int)(found - data->images) : -1;
}

double AeronEffectData_ImageDuration(const AeronEffectData* data, int index) {
	if (!data || index < 0 || (uint32_t)index >= data->image_count)
		return 0.0;
	const EffectDataImage* image = &data->images[index];
	return image->frames[image->frame_count - 1].end;
}

int AeronEffectData_ImageFrame(const AeronEffectData* data, int index, double t, AeronEffectImage* out) {
	if (!data || index < 0 || (uint32_t)index >= data->image_count || !out)
		return 0;
	const EffectDataImage* image = &data->images[index];
	const double           total = image->frames[image->frame_count - 1].end;
	double                 local = 0.0;
	if (total > 0.0)
		local = image->once ? (t < 0.0 ? 0.0 : (t < total ? t : total)) : t - total * floor(t / total);
	uint32_t f = 0;
	while (f + 1 < image->frame_count && !(local < image->frames[f].end))
		f++;
	if (!image->frames[f].found)
		return 0;
	*out = image->frames[f].image;
	return 1;
}

int AeronEffectData_Energy(const AeronEffectData* data, int effect, AeronEffectEnergy* out) {
	if (!data || effect < 0 || effect >= AERON_EFFECT_ENERGY_COUNT || !out ||
		!data->energy[effect].frame_count)
		return 0;
	const EffectDataImage* e = &data->energy[effect];
	out->once                = e->once;
	out->strength            = e->strength;
	out->tile                = e->tile;
	out->frame_count         = e->frame_count;
	return 1;
}

int AeronEffectData_EnergyFrame(const AeronEffectData* data, int effect, uint32_t frame,
								AeronEffectImage* out, double* end) {
	if (!data || effect < 0 || effect >= AERON_EFFECT_ENERGY_COUNT ||
		frame >= data->energy[effect].frame_count)
		return 0;
	const EffectDataFrame* f = &data->energy[effect].frames[frame];
	if (end)
		*end = f->end;
	if (!f->found)
		return 0;
	if (out)
		*out = f->image;
	return 1;
}
