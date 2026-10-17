/*
 * Writes the effect library images of converted XWAU models into a project
 * (xwau_library.h).
 */

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "xwau_library.h"

#include "cJSON.h"
#include "effect_files.h"
#include "png_write.h"
#include "util.h"
#include "xwau_dat.h"
#include "xwau_effect_bake.h"
#include "xwau_mat.h"
#include "yaml_json.h"

/* Energy frames are capped at ENERGY_MAX (`maxSize`). Other images keep the
 * default cap of the library. */
#define ENERGY_MAX 512
#define RANDOM_FRAMES 64

/* XWAU samples only the central 35% of every blast-mark image. */
#define BLAST_MARK_GROUP 3050
static const float kBlastMarkCrop[4] = { 0.3f, 0.3f, 0.65f, 0.65f };

static const char* g_game;

/* ---- Paths -------------------------------------------------------------- */

static int find_file(const char* rel, char* out, size_t cap) { return Xwau_FindFile(g_game, rel, out, cap); }

/* ---- Library requests ------------------------------------------------------ */

typedef struct Request {
	char       key[96];
	int        group, image;
	XwauFxKind kind;
	float      tint[3];
	int        alpha_mode;
	bool       crop;
} Request;

typedef struct Library {
	char     name[64];
	char     dat[256]; /* lower-case relative path */
	Request* requests;
	size_t   count;
} Library;

static Library* g_libraries;
static size_t   g_library_count;

static Library* library_for(const char* dat_written) {
	char rel[256], name[64];
	if (!Xwau_DatNames(dat_written, rel, sizeof rel, name, sizeof name))
		return NULL;
	for (size_t i = 0; i < g_library_count; ++i)
		if (strcmp(g_libraries[i].name, name) == 0) {
			if (strcmp(g_libraries[i].dat, rel) != 0)
				Report(NULL, "library %s comes from both %s and %s, using the first one", name,
					   g_libraries[i].dat, rel);
			return &g_libraries[i];
		}
	Library* l = ARRAY_PUSH(g_libraries, g_library_count);
	if (!l)
		return NULL;
	snprintf(l->name, sizeof l->name, "%s", name);
	snprintf(l->dat, sizeof l->dat, "%s", rel);
	return l;
}

static bool request(const char* dat_written, int group, int image, XwauFxKind kind, const float tint[3],
					int alpha_mode, bool crop) {
	Library* l = library_for(dat_written);
	if (!l)
		return false;
	char key[96];
	XwauFx_Key(key, sizeof key, group, image, kind, tint, alpha_mode);
	for (size_t i = 0; i < l->count; ++i)
		if (strcmp(l->requests[i].key, key) == 0) {
			l->requests[i].crop |= crop;
			return true;
		}
	Request* r = ARRAY_PUSH(l->requests, l->count);
	if (!r)
		return false;
	snprintf(r->key, sizeof r->key, "%s", key);
	r->group      = group;
	r->image      = image;
	r->kind       = kind;
	r->alpha_mode = alpha_mode == 1 ? 1 : 0;
	r->crop       = crop;
	memcpy(r->tint, tint, sizeof r->tint);
	return true;
}

/* ---- Models --------------------------------------------------------------- */

static size_t g_models;
static char** g_skipped; /* libraries without an XWAU DAT */
static size_t g_skipped_count;

/* Requests the image `key` from the library `library`, which is the DAT
 * Effects/<library>.dat. If the game has no such DAT, the library is marked as
 * skipped. */
static bool model_key(const char* path, const char* library, const char* key) {
	for (size_t i = 0; i < g_skipped_count; ++i)
		if (strcmp(g_skipped[i], library) == 0)
			return true;
	char dat[256], found[1024];
	if (!PathFormat(dat, sizeof dat, "effects/%s.dat", library))
		return false;
	bool known = false;
	for (size_t i = 0; i < g_library_count && !known; ++i)
		known = strcmp(g_libraries[i].dat, dat) == 0;
	if (!known && !find_file(dat, found, sizeof found)) {
		char*  copy = StrDup(library);
		char** slot = copy ? ARRAY_PUSH(g_skipped, g_skipped_count) : NULL;
		if (!slot) {
			free(copy);
			return false;
		}
		*slot = copy;
		return true;
	}
	int        group, image, alpha_mode;
	XwauFxKind kind;
	float      tint[3];
	if (!XwauFx_ParseKey(key, &group, &image, &kind, tint, &alpha_mode)) {
		Report(path, "library %s: \"%s\" is not an XWAU image key", library, key);
		return false;
	}
	return request(dat, group, image, kind, tint, alpha_mode, false);
}

/* Requests the library images used by the flipbooks and damage of one
 * material. */
static bool material_keys(const char* path, const cJSON* material) {
	bool         ok = true;
	const cJSON* book;
	cJSON_ArrayForEach(book, cJSON_GetObjectItemCaseSensitive(material, "flipbooks")) {
		const cJSON* library = cJSON_GetObjectItemCaseSensitive(book, "library");
		const cJSON* frame;
		cJSON_ArrayForEach(frame, cJSON_GetObjectItemCaseSensitive(book, "frames")) {
			const cJSON* key = cJSON_GetArrayItem(frame, 0);
			if (cJSON_IsString(library) && cJSON_IsString(key))
				ok &= model_key(path, library->valuestring, key->valuestring);
		}
	}
	const cJSON* damage  = cJSON_GetObjectItemCaseSensitive(material, "damage");
	const cJSON* library = cJSON_GetObjectItemCaseSensitive(damage, "library");
	const cJSON* alternative;
	cJSON_ArrayForEach(alternative, cJSON_GetObjectItemCaseSensitive(damage, "alternatives")) {
		const cJSON* stage;
		cJSON_ArrayForEach(stage, cJSON_GetObjectItemCaseSensitive(alternative, "stages")) {
			if (cJSON_IsString(library) && cJSON_IsString(stage))
				ok &= model_key(path, library->valuestring, stage->valuestring);
		}
	}
	return ok;
}

/* Requests the library images used by the materials of a model .gltf. */
static bool read_model(const char* path) {
	char   err[512];
	cJSON* root = YamlJson_Load(path, err, sizeof err);
	if (!root) {
		Report(NULL, "%s", err);
		return false;
	}
	bool         ok = true;
	const cJSON* material;
	cJSON_ArrayForEach(material, cJSON_GetObjectItemCaseSensitive(root, "materials")) {
		const cJSON* extensions = cJSON_GetObjectItemCaseSensitive(material, "extensions");
		ok &= material_keys(path, cJSON_GetObjectItemCaseSensitive(extensions, "AERON_flight_model"));
	}
	cJSON_Delete(root);
	g_models++;
	return ok;
}

/* Reads a model .gltf, or every models/<key>/<key>.gltf of a project in name
 * order. */
static bool read_models(const char* path) {
	if (IsFile(path))
		return read_model(path);
	if (!IsDir(path)) {
		Report(path, "not found");
		return false;
	}
	char models[1024];
	if (!PathFormat(models, sizeof models, "%s/models", path))
		return false;
	if (IsDir(models)) {
		size_t count = 0;
		char** keys  = ListDir(models, true, &count);
		bool   ok    = true;
		for (size_t i = 0; i < count; ++i) {
			char model[2048];
			if (keys[i][0] == '.')
				continue;
			if (!PathFormat(model, sizeof model, "%s/%s/%s.gltf", models, keys[i], keys[i]))
				ok = false;
			else if (IsFile(model))
				ok &= read_model(model);
		}
		FreeList(keys, count);
		return ok;
	}
	Report(path, "not a model .gltf or a project");
	return false;
}

/* ---- effects.yaml ------------------------------------------------------- */

typedef struct Text {
	char*  s;
	size_t len, cap;
} Text;

static void text_add(Text* t, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	const int n = vsnprintf(NULL, 0, fmt, args);
	va_end(args);
	if (n < 0 || (!t->s && t->cap))
		return;
	if (t->len + (size_t)n + 1 > t->cap) {
		size_t cap = t->cap ? t->cap * 2 : 4096;
		while (cap < t->len + (size_t)n + 1)
			cap *= 2;
		char* grown = (char*)realloc(t->s, cap);
		if (!grown) {
			free(t->s);
			t->s   = NULL;
			t->cap = 1;
			return;
		}
		t->s   = grown;
		t->cap = cap;
	}
	va_start(args, fmt);
	vsnprintf(t->s + t->len, t->cap - t->len, fmt, args);
	va_end(args);
	t->len += (size_t)n;
}

/* The energy effects use groups 2, 1 and 3 of GenericDamage.dat, like the
 * IEVT_SHIELDS_DOWN, IEVT_TRACTOR_BEAM and IEVT_JAMMING_BEAM lines that XWAU
 * ships. */
typedef struct Energy {
	const char* name;
	int         group;
	float       fps;
	bool        once;
} Energy;

static const Energy kEnergy[3] = {
	{ "shields", 2, 40.0f, true },
	{ "tractor", 1, 20.0f, false },
	{ "jamming", 3, 20.0f, false },
};
#define ENERGY_DAT "Effects\\GenericDamage.dat"
#define ENERGY_STRENGTH 1.0f
#define ENERGY_TILE 0.3f

static const float kWhite[3] = { 1.0f, 1.0f, 1.0f };

static bool energy_yaml(Text* y) {
	char rel[1024];
	if (!find_file(ENERGY_DAT, rel, sizeof rel)) {
		Report(NULL, "%s not found, so there are no energy effects", ENERGY_DAT);
		return true;
	}
	char     err[512];
	XwauDat* dat = XwauDat_Open(rel, err, sizeof err);
	if (!dat) {
		Report(NULL, "%s", err);
		return false;
	}
	char lrel[256], library[64];
	if (!Xwau_DatNames(ENERGY_DAT, lrel, sizeof lrel, library, sizeof library))
		return false;
	text_add(y, "energy:\n");
	bool ok = true;
	for (int e = 0; e < 3; ++e) {
		const int* ids   = NULL;
		size_t     count = XwauDat_ListGroup(dat, kEnergy[e].group, &ids);
		text_add(y, "  %s: { library: %s, mode: %s, strength: %g, tile: %g, frames: [", kEnergy[e].name,
				 library, kEnergy[e].once ? "once" : "loop", (double)ENERGY_STRENGTH, (double)ENERGY_TILE);
		for (size_t i = 0; i < count; ++i) {
			char key[96];
			XwauFx_Key(key, sizeof key, kEnergy[e].group, ids[i], XWAU_FX_ENERGY, kWhite, 0);
			text_add(y, "%s[\"%s\", %.9g]", i ? ", " : "", key, (double)(1.0f / kEnergy[e].fps));
			ok &= request(ENERGY_DAT, kEnergy[e].group, ids[i], XWAU_FX_ENERGY, kWhite, 0, false);
		}
		text_add(y, "] }\n");
	}
	XwauDat_Close(dat);
	return ok;
}

static int cmp_int(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

static int cmp_dat_image(const void* a, const void* b) {
	const int* x = (const int*)a;
	const int* y = (const int*)b;
	return x[0] != y[0] ? x[0] - y[0] : x[1] - y[1];
}

/* Requests in key order, so packing does not depend on input order. */
static int cmp_request(const void* a, const void* b) {
	return strcmp(((const Request*)a)->key, ((const Request*)b)->key);
}

/* xorshift32 */
static uint32_t next_random(uint32_t* state) {
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *state = x;
}

static uint32_t fnv1a(const char* s) {
	uint32_t h = 2166136261u;
	for (; *s; ++s)
		h = (h ^ (uint8_t)*s) * 16777619u;
	return h ? h : 1;
}

typedef struct DatImage {
	int  group, image;
	char mat[1024]; /* resolved material file */
} DatImage;

/* Writes the flipbook of a DAT image from the sequences of its [Default]
 * section. It uses the last EVT_NONE colour sequence, like XWAU's
 * TextureATCIndices[TEXTURE][EVT_NONE]. */
static bool image_flipbook(Text* y, const char* name, const XwauMatFile* mat, int group, int image) {
	XwauMatSequences own, inherited;
	XwauMat_FindSequences(mat, "", &own, &inherited);
	const XwauSequence* q = NULL;
	for (size_t i = 0; i < own.count; ++i)
		if (!own.items[i].instance && !own.items[i].event[0] && own.items[i].layer == XWAU_SEQ_COLOUR)
			q = &own.items[i];
	if (!q)
		return false;
	char path[1024], err[512];
	if (!find_file(q->dat, path, sizeof path)) {
		Report(name, "%s not found", q->dat);
		return false;
	}
	XwauDat* dat = XwauDat_Open(path, err, sizeof err);
	if (!dat) {
		Report(NULL, "%s", err);
		return false;
	}
	/* The frames are either listed, or the images of a DAT group at 1/fps each. */
	XwauSeqFrame* frames = NULL;
	size_t        count  = 0;
	if (q->group_frames) {
		const int* ids = NULL;
		int        single;
		size_t     n = 0;
		if (q->image >= 0) {
			single = q->image;
			ids    = &single;
			n      = 1;
		} else {
			n = XwauDat_ListGroup(dat, q->group, &ids);
		}
		if (q->fps <= 0.0f && n)
			n = 1;
		frames = (XwauSeqFrame*)malloc((n ? n : 1) * sizeof *frames);
		for (size_t i = 0; frames && i < n; ++i)
			frames[i] =
				(XwauSeqFrame) { q->group, ids[i], q->fps > 0.0f ? 1.0f / q->fps : 1.0f, q->intensity };
		count = frames ? n : 0;
	} else {
		frames = (XwauSeqFrame*)malloc((q->frame_count ? q->frame_count : 1) * sizeof *frames);
		if (frames)
			memcpy(frames, q->frames, q->frame_count * sizeof *frames);
		count = frames ? q->frame_count : 0;
	}
	XwauDat_Close(dat);
	if (!count) {
		free(frames);
		return false;
	}
	/* A _rand sequence becomes frame 0 followed by random picks. Each pick keeps
	 * its duration. */
	if (q->mode == XWAU_SEQ_RANDOM && count > 1) {
		XwauSeqFrame* picks = (XwauSeqFrame*)malloc(RANDOM_FRAMES * sizeof *picks);
		if (!picks) {
			free(frames);
			return false;
		}
		uint32_t seed = fnv1a(name);
		picks[0]      = frames[0];
		for (size_t i = 1; i < RANDOM_FRAMES; ++i)
			picks[i] = frames[next_random(&seed) % count];
		free(frames);
		frames = picks;
		count  = RANDOM_FRAMES;
	}
	const bool       mark = group == BLAST_MARK_GROUP;
	const XwauFxKind kind = mark ? XWAU_FX_MARK : XWAU_FX_COLOUR;
	char             rel[256], library[64];
	if (!Xwau_DatNames(q->dat, rel, sizeof rel, library, sizeof library))
		return false;
	text_add(y, "  \"%d-%d\": { library: %s, kind: %s, mode: %s, frames: [", group, image, library,
			 XwauFx_KindName(kind), q->mode == XWAU_SEQ_ONCE ? "once" : "loop");
	bool ok = true;
	for (size_t i = 0; i < count; ++i) {
		char key[96];
		XwauFx_Key(key, sizeof key, frames[i].group, frames[i].image, kind, q->tint, q->alpha_mode);
		text_add(y, "%s[\"%s\", %.9g]", i ? ", " : "", key, (double)frames[i].seconds);
		ok &= request(q->dat, frames[i].group, frames[i].image, kind, q->tint, q->alpha_mode, mark);
	}
	text_add(y, "] }\n");
	free(frames);
	return ok;
}

/* Writes the flipbooks of the material files named after DAT images
 * ("dat-<G>-<I>.mat"). */
static bool images_yaml(Text* y) {
	DatImage* found      = NULL;
	size_t    count      = 0;
	char      mdir[1024] = "";
	if (Xwau_FindDir(g_game, "Materials", mdir, sizeof mdir)) {
		size_t entry_count = 0;
		char** entries     = ListDir(mdir, false, &entry_count);
		for (size_t e = 0; e < entry_count; ++e) {
			int  g = 0, i = 0;
			char tail[16];
			if (strncasecmp(entries[e], "dat-", 4) != 0 ||
				sscanf(entries[e] + 4, "%d-%d.%15s", &g, &i, tail) != 3 || strcasecmp(tail, "mat") != 0)
				continue;
			DatImage* image = ARRAY_PUSH(found, count);
			if (!image || !PathFormat(image->mat, sizeof image->mat, "%s/%s", mdir, entries[e])) {
				FreeList(entries, entry_count);
				free(found);
				return false;
			}
			image->group = g;
			image->image = i;
		}
		FreeList(entries, entry_count);
	}
	qsort(found, count, sizeof *found, cmp_dat_image);
	XwauMatParams defaults;
	XwauMat_CodeDefaults(&defaults);
	text_add(y, "images:\n");
	bool ok          = true;
	int  marks       = 0;
	int* mark_images = (int*)malloc((count ? count : 1) * sizeof *mark_images);
	for (size_t k = 0; k < count; ++k) {
		XwauMatFile* mat = XwauMat_Load(found[k].mat, &defaults);
		if (!mat)
			continue;
		char name[64];
		snprintf(name, sizeof name, "dat-%d-%d", found[k].group, found[k].image);
		const bool has = image_flipbook(y, name, mat, found[k].group, found[k].image);
		XwauMat_Free(mat);
		if (has && found[k].group == BLAST_MARK_GROUP && mark_images)
			mark_images[marks++] = found[k].image;
	}
	/* A blast-mark image without a sequence gets a static flipbook of its own
	 * image. */
	if (marks) {
		char        path[1024], err[512];
		const char* dat_written = "ResData\\BlastMarks.dat";
		if (find_file(dat_written, path, sizeof path)) {
			XwauDat* dat = XwauDat_Open(path, err, sizeof err);
			if (dat) {
				const int* ids = NULL;
				size_t     n   = XwauDat_ListGroup(dat, BLAST_MARK_GROUP, &ids);
				int*       all = (int*)malloc((n ? n : 1) * sizeof *all);
				if (all) {
					memcpy(all, ids, n * sizeof *all);
					qsort(all, n, sizeof *all, cmp_int);
					for (size_t i = 0; i < n; ++i) {
						bool animated = false;
						for (int m = 0; m < marks && !animated; ++m)
							animated = mark_images[m] == all[i];
						if (animated)
							continue;
						char key[96];
						XwauFx_Key(key, sizeof key, BLAST_MARK_GROUP, all[i], XWAU_FX_MARK, kWhite, 0);
						text_add(y,
								 "  \"%d-%d\": { library: blastmarks, kind: mark, mode: loop, frames: "
								 "[[\"%s\", 1]] }\n",
								 BLAST_MARK_GROUP, all[i], key);
						ok &= request(dat_written, BLAST_MARK_GROUP, all[i], XWAU_FX_MARK, kWhite, 0, true);
					}
					free(all);
				}
				XwauDat_Close(dat);
			} else {
				Report(NULL, "%s", err);
			}
		}
	}
	free(mark_images);
	free(found);
	return ok;
}

/* ---- Sources ------------------------------------------------------------ */

/* The sRGB transfer functions of the library builder, on 8-bit values. They
 * check that the written inputs bake back into the same library images. */
static float lin8(uint8_t v) {
	const float c = v / 255.0f;
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static uint8_t srgb8(float c) {
	c             = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
	const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)(s * 255.0f + 0.5f);
}

static uint8_t unorm8f(float c) {
	return (uint8_t)((c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c)) * 255.0f + 0.5f);
}

static int max_diff(int d, int a, int b) { return abs(a - b) > d ? abs(a - b) : d; }

/* Writes `rgba` to <dir>/<file>. Returns false after reporting an error. */
static bool write_frame(const char* dir, const char* file, int w, int h, const uint8_t* rgba) {
	char png[1200];
	if (!PathFormat(png, sizeof png, "%s/%s", dir, file))
		return false;
	if (write_png_rgba(png, w, h, rgba))
		return true;
	Report(NULL, "cannot write %s", png);
	return false;
}

/* Writes the sources of library `l` under `effects_dir`. They are the file
 * <name>/<name>.yaml and its images under <name>/frames/. The inputs of each
 * key use the kinds of the library file, with XWAU's rules applied: an
 * illumination frame becomes an emissive image and a base colour image with
 * coverage, and the tint and alpha mode are baked in. Reports the largest
 * per-channel difference between what the library builder bakes from these
 * inputs and the XWAU bake. The difference comes from the 8-bit rounding of
 * the base colour. */
static bool write_library_sources(Library* l, const char* effects_dir, bool merge) {
	char path[1024], err[512], dir[1024];
	if (!find_file(l->dat, path, sizeof path)) {
		Report(l->name, "%s not found", l->dat);
		return false;
	}
	qsort(l->requests, l->count, sizeof *l->requests, cmp_request);
	char frames[1100];
	if (!PathFormat(dir, sizeof dir, "%s/%s", effects_dir, l->name) ||
		!PathFormat(frames, sizeof frames, "%s/frames", dir))
		return false;
	if (!MakeDirs(dir) || !MakeDirs(frames)) {
		Report(NULL, "cannot create %s", frames);
		return false;
	}
	XwauDat* dat = XwauDat_Open(path, err, sizeof err);
	if (!dat) {
		Report(NULL, "%s", err);
		return false;
	}
	/* The library file is written in canonical form (effect_files.h). When
	 * merging, the existing file in the project keeps its images, and only the
	 * missing ones are added. */
	char file[1100];
	if (!PathFormat(file, sizeof file, "%s/%s.yaml", dir, l->name)) {
		XwauDat_Close(dat);
		return false;
	}
	const bool existing = merge && (IsFile(file) || IsDir(file));
	cJSON*     doc      = existing ? YamlJson_Load(file, err, sizeof err) : cJSON_CreateObject();
	cJSON*     entries  = existing ? cJSON_GetObjectItemCaseSensitive(doc, "images") : NULL;
	if (existing && !doc)
		Report(NULL, "%s", err);
	bool ok = doc && (existing ? cJSON_IsObject(entries)
							   : cJSON_AddNumberToObject(doc, "version", 1) &&
									 cJSON_AddStringToObject(doc, "library", l->name) &&
									 (entries = cJSON_AddObjectToObject(doc, "images")));
	if (existing && doc && !ok)
		Report(file, "no images");
	size_t images = 0;
	int    diff   = 0;
	for (size_t i = 0; ok && i < l->count; ++i) {
		const Request* r = &l->requests[i];
		if (existing && cJSON_GetObjectItemCaseSensitive(entries, r->key))
			continue;
		XwauDatImage im;
		if (!XwauDat_LoadImageFrom(dat, r->group, r->image, &im, err, sizeof err)) {
			Report(NULL, "%s", err);
			continue;
		}
		const int    w = (int)im.width, h = (int)im.height;
		const size_t n        = (size_t)w * h;
		uint8_t*     layer    = XwauFx_HasLayer(r->kind) ? (uint8_t*)malloc(n * 4) : NULL;
		uint8_t*     emissive = XwauFx_HasEmissive(r->kind) ? (uint8_t*)malloc(n * 4) : NULL;
		uint8_t*     input    = (uint8_t*)malloc(n * 4);
		ok = input && (layer || !XwauFx_HasLayer(r->kind)) && (emissive || !XwauFx_HasEmissive(r->kind));
		if (ok)
			XwauFx_Bake(r->kind, im.rgba, n, r->tint, r->alpha_mode, layer, emissive);
		/* A file name is the key with ':' replaced by '_'. */
		char   base[128] = "", file[160], base_color[160];
		cJSON* entry     = NULL;
		ok               = ok && PathFormat(base, sizeof base, "frames/%s", r->key);
		for (char* c = base; *c; ++c)
			if (*c == ':')
				*c = '_';
		bool any_base_color = false;
		if (ok && r->kind == XWAU_FX_ILLUMINATION) {
			/* Base colour: the coverage is c = 1 - k and the colour is layer_rgb / c. */
			for (size_t t = 0; t < n; ++t) {
				const uint8_t* lt = &layer[t * 4];
				uint8_t*       pt = &input[t * 4];
				const float    c  = (255 - lt[3]) / 255.0f;
				pt[3]             = (uint8_t)(255 - lt[3]);
				for (int k = 0; k < 3; ++k)
					pt[k] = c > 0.0f ? srgb8(lin8(lt[k]) / c) : 0;
				any_base_color |= pt[3] != 0;
				for (int k = 0; k < 3; ++k)
					diff = max_diff(diff, lt[k], pt[3] ? srgb8(lin8(pt[k]) * c) : 0);
				diff = max_diff(diff, lt[3], (uint8_t)((1.0f - c) * 255.0f + 0.5f));
			}
			ok    = PathFormat(file, sizeof file, "%s_emissive.png", base) &&
					PathFormat(base_color, sizeof base_color, "%s_base_color.png", base) &&
					write_frame(dir, file, w, h, emissive) &&
					(!any_base_color || write_frame(dir, base_color, w, h, input));
			entry = cJSON_AddObjectToObject(entries, r->key);
			cJSON_AddStringToObject(entry, "kind", "emissive");
			cJSON_AddStringToObject(entry, "emissive", file);
			if (any_base_color)
				cJSON_AddStringToObject(entry, "baseColor", base_color);
		} else if (ok && (r->kind == XWAU_FX_COLOUR || r->kind == XWAU_FX_MULTIPLY)) {
			ok    = PathFormat(file, sizeof file, "%s.png", base) && write_frame(dir, file, w, h, layer);
			entry = cJSON_AddObjectToObject(entries, r->key);
			cJSON_AddStringToObject(entry, "kind", r->kind == XWAU_FX_MULTIPLY ? "damage" : "baseColor");
			cJSON_AddStringToObject(entry, r->kind == XWAU_FX_MULTIPLY ? "factor" : "baseColor", file);
		} else if (ok) {
			/* Mark and energy: writes the tinted frame with its alpha. The library
			 * builder premultiplies it. */
			const uint8_t* baked = r->kind == XWAU_FX_MARK ? layer : emissive;
			for (size_t t = 0; t < n; ++t) {
				const uint8_t* src = &im.rgba[t * 4];
				uint8_t*       pt  = &input[t * 4];
				for (int k = 0; k < 3; ++k) {
					pt[k] = unorm8f(src[k] / 255.0f * r->tint[k]);
					diff  = max_diff(diff, baked[t * 4 + k], srgb8(lin8(pt[k]) * (src[3] / 255.0f)));
				}
				pt[3] = src[3];
			}
			ok    = PathFormat(file, sizeof file, "%s.png", base) && write_frame(dir, file, w, h, input);
			entry = cJSON_AddObjectToObject(entries, r->key);
			cJSON_AddStringToObject(entry, "kind", XwauFx_KindName(r->kind));
			cJSON_AddStringToObject(entry, r->kind == XWAU_FX_MARK ? "baseColor" : "emissive", file);
		}
		if (ok) {
			if (r->kind == XWAU_FX_ENERGY)
				cJSON_AddNumberToObject(entry, "maxSize", ENERGY_MAX);
			if (r->crop)
				cJSON_AddItemToObject(entry, "crop", cJSON_CreateFloatArray(kBlastMarkCrop, 4));
			ok = entry != NULL;
			images++;
		}
		free(layer);
		free(emissive);
		free(input);
		XwauDat_FreeImage(&im);
	}
	XwauDat_Close(dat);
	char header[300];
	snprintf(header, sizeof header, "# Effect library %s, imported from XWAU's %s.\n", l->name, l->dat);
	if (existing && !images) {
		cJSON_Delete(doc);
		return ok;
	}
	ok = ok && EffectFiles_Write("opt2gltf", doc, existing ? NULL : header, file);
	cJSON_Delete(doc);
	if (ok)
		Report(NULL, "library %s: %zu image(s) %s (largest rounding error: %d/255)", l->name, images,
			   existing ? "added" : "written", diff);
	return ok;
}

static int cmp_library(const void* a, const void* b) {
	return strcmp(((const Library*)a)->name, ((const Library*)b)->name);
}

/* Clears what the previous run read. */
static void reset(const char* game) {
	for (size_t i = 0; i < g_library_count; ++i)
		free(g_libraries[i].requests);
	free(g_libraries);
	for (size_t i = 0; i < g_skipped_count; ++i)
		free(g_skipped[i]);
	free(g_skipped);
	g_libraries     = NULL;
	g_library_count = 0;
	g_skipped       = NULL;
	g_skipped_count = 0;
	g_models        = 0;
	g_game          = game;
}

/* Writes the libraries read into <project>/effects. */
static bool write_libraries(const char* effects, bool merge) {
	for (size_t i = 0; i < g_skipped_count; ++i)
		Report(NULL, "library %s: no Effects/%s.dat, not an XWAU library; left alone", g_skipped[i],
			   g_skipped[i]);
	qsort(g_libraries, g_library_count, sizeof *g_libraries, cmp_library);
	bool ok = true;
	for (size_t i = 0; i < g_library_count; ++i)
		ok &= write_library_sources(&g_libraries[i], effects, merge);
	return ok;
}

bool XwauLibrary_Rebuild(const char* game, const char* project, const char* const* inputs, size_t count) {
	reset(game);
	char out[1024];
	if (!PathFormat(out, sizeof out, "%s/effects", project))
		return false;
	if (!MakeDirs(project) || !MakeDirs(out)) {
		Report(NULL, "cannot create %s", out);
		return false;
	}
	bool ok = true;
	for (size_t i = 0; i < count; ++i)
		ok &= read_models(inputs[i]);
	Text y = { 0 };
	text_add(&y, "version: 1\n");
	ok &= energy_yaml(&y) && images_yaml(&y);
	if (!y.s) {
		Report(NULL, "out of memory");
		reset(NULL);
		return false;
	}
	char path[1024];
	if (!PathFormat(path, sizeof path, "%s/effects.yaml", out) || !WriteWholeFile(path, y.s, y.len)) {
		Report(NULL, "cannot write %s", path);
		ok = false;
	}
	free(y.s);
	Report(NULL, "%zu model(s), %zu librar%s", g_models, g_library_count, g_library_count == 1 ? "y" : "ies");
	ok &= write_libraries(out, false);
	reset(NULL);
	return ok;
}

bool XwauLibrary_AddModel(const char* game, const char* project, const char* gltf) {
	reset(game);
	char out[1024];
	bool ok = PathFormat(out, sizeof out, "%s/effects", project) && read_model(gltf);
	if (ok && g_library_count && !MakeDirs(out)) {
		Report(NULL, "cannot create %s", out);
		ok = false;
	}
	ok = ok && write_libraries(out, true);
	reset(NULL);
	return ok;
}
