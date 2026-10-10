/*
 * xwau_effects — builds the effect image libraries and the game effect data
 * of converted XWAU models (docs: xwau_animated_materials.md, "Asset Format").
 *
 *   xwau_effects [--xwau-root DIR]... --out DIR [MODEL.glb | DIR]...
 *
 * Reads the library images the models use (the keys of their aeronFlipbooks
 * frames and aeronDamage stages, from the .glb files given or found in the
 * directories given) and the DAT-named material files
 * (Materials/dat-<G>-<I>.mat) of the roots. A library is the images of
 * Effects/<library>.dat; a library with no such DAT in the roots is not an
 * XWAU one (authored content) and is left alone. Writes into --out:
 *   - effects.yaml: the energy effects and the flipbooks of DAT images the
 *     game draws directly (the blast marks of group 3050);
 *   - per library (one per source DAT): <name>.layer.ktx2, <name>.emissive.ktx2
 *     (when it has emission) and the <name>.yaml index.
 * Roots are searched in order, the first holding a file wins (as opt2gltf).
 */

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "effect_pack.h"
#include "xwau_dat.h"
#include "xwau_effect_bake.h"
#include "xwau_mat.h"

#define MAX_ROOTS 16
/* Images are capped at EFFECT_PACK_IMAGE_MAX (only Factory.dat's 2678×1536
 * frames are larger), energy frames at ENERGY_MAX. */
#define ENERGY_MAX 512
#define RANDOM_FRAMES 64

/* Blast marks: XWAU samples the central 35% of every blast-mark image. */
#define BLAST_MARK_GROUP 3050
static const float kBlastMarkCrop[4] = { 0.3f, 0.3f, 0.65f, 0.65f };

static const char* g_roots[MAX_ROOTS];
static int         g_root_count;

/* ---- Paths -------------------------------------------------------------- */

/* Resolves a game-relative path under the first root that has it, matching
 * each component case-insensitively. */
static int find_file(const char* rel, char* out, size_t cap) {
	for (int r = 0; r < g_root_count; ++r) {
		char path[1024], parts[512];
		snprintf(path, sizeof path, "%s", g_roots[r]);
		snprintf(parts, sizeof parts, "%s", rel);
		for (char* c = parts; *c; ++c)
			if (*c == '\\')
				*c = '/';
		int   found = 1;
		char* save  = NULL;
		for (char* part = strtok_r(parts, "/", &save); part && found; part = strtok_r(NULL, "/", &save)) {
			DIR* d = opendir(path);
			found  = 0;
			if (!d)
				break;
			struct dirent* de;
			while ((de = readdir(d))) {
				if (strcasecmp(de->d_name, part) == 0) {
					size_t len = strlen(path);
					snprintf(path + len, sizeof path - len, "/%s", de->d_name);
					found = 1;
					break;
				}
			}
			closedir(d);
		}
		struct stat st;
		if (found && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
			snprintf(out, cap, "%s", path);
			return 1;
		}
	}
	return 0;
}

/* "Effects\GenericDamage.dat" -> "effects/genericdamage.dat", "genericdamage". */
static void dat_names(const char* written, char* rel, size_t rel_cap, char* library, size_t lib_cap) {
	size_t n = 0;
	for (const char* p = written; *p && n + 1 < rel_cap; ++p)
		rel[n++] = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
	rel[n]           = 0;
	const char* base = strrchr(rel, '/');
	snprintf(library, lib_cap, "%s", base ? base + 1 : rel);
	char* dot = strrchr(library, '.');
	if (dot)
		*dot = 0;
}

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
	dat_names(dat_written, rel, sizeof rel, name, sizeof name);
	for (size_t i = 0; i < g_library_count; ++i)
		if (strcmp(g_libraries[i].name, name) == 0) {
			if (strcmp(g_libraries[i].dat, rel) != 0)
				fprintf(stderr, "xwau_effects: library %s comes from %s and %s; using the first\n", name,
						g_libraries[i].dat, rel);
			return &g_libraries[i];
		}
	Library* grown = (Library*)realloc(g_libraries, (g_library_count + 1) * sizeof *grown);
	if (!grown)
		return NULL;
	g_libraries = grown;
	Library* l  = &grown[g_library_count++];
	memset(l, 0, sizeof *l);
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
	Request* grown = (Request*)realloc(l->requests, (l->count + 1) * sizeof *grown);
	if (!grown)
		return false;
	l->requests = grown;
	Request* r  = &grown[l->count++];
	memset(r, 0, sizeof *r);
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

/* Requests the image `key` of library `library`, the DAT Effects/<library>.dat;
 * notes the library as skipped when the roots have no such DAT. */
static bool model_key(const char* path, const char* library, const char* key) {
	for (size_t i = 0; i < g_skipped_count; ++i)
		if (strcmp(g_skipped[i], library) == 0)
			return true;
	char dat[256], found[1024];
	snprintf(dat, sizeof dat, "effects/%s.dat", library);
	bool known = false;
	for (size_t i = 0; i < g_library_count && !known; ++i)
		known = strcmp(g_libraries[i].dat, dat) == 0;
	if (!known && !find_file(dat, found, sizeof found)) {
		char** grown = (char**)realloc(g_skipped, (g_skipped_count + 1) * sizeof *grown);
		if (!grown || !(grown[g_skipped_count] = strdup(library))) {
			g_skipped = grown ? grown : g_skipped;
			return false;
		}
		g_skipped = grown;
		g_skipped_count++;
		return true;
	}
	int        group, image, alpha_mode;
	XwauFxKind kind;
	float      tint[3];
	if (!XwauFx_ParseKey(key, &group, &image, &kind, tint, &alpha_mode)) {
		fprintf(stderr, "xwau_effects: %s: library %s: \"%s\" is not an XWAU image key\n", path, library,
				key);
		return false;
	}
	return request(dat, group, image, kind, tint, alpha_mode, false);
}

/* The JSON chunk of a .glb, parsed; NULL when it is not one. */
static cJSON* read_glb_json(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t head[20];
	cJSON*  root = NULL;
	if (fread(head, 1, sizeof head, f) == sizeof head && memcmp(head, "glTF", 4) == 0 &&
		memcmp(head + 16, "JSON", 4) == 0) {
		const uint32_t n    = (uint32_t)head[12] | (uint32_t)head[13] << 8 | (uint32_t)head[14] << 16 |
							  (uint32_t)head[15] << 24;
		char*          text = (char*)malloc((size_t)n + 1);
		if (text && fread(text, 1, n, f) == n) {
			text[n] = 0;
			root    = cJSON_ParseWithLength(text, n);
		}
		free(text);
	}
	fclose(f);
	return root;
}

/* The library images a model's materials use. */
static bool read_model(const char* path) {
	cJSON* root = read_glb_json(path);
	if (!root) {
		fprintf(stderr, "xwau_effects: %s: not a glTF binary\n", path);
		return false;
	}
	bool         ok = true;
	const cJSON* material;
	cJSON_ArrayForEach(material, cJSON_GetObjectItemCaseSensitive(root, "materials")) {
		const cJSON* extras = cJSON_GetObjectItemCaseSensitive(material, "extras");
		const cJSON* book;
		cJSON_ArrayForEach(book, cJSON_GetObjectItemCaseSensitive(extras, "aeronFlipbooks")) {
			const cJSON* library = cJSON_GetObjectItemCaseSensitive(book, "library");
			const cJSON* frame;
			cJSON_ArrayForEach(frame, cJSON_GetObjectItemCaseSensitive(book, "frames")) {
				const cJSON* key = cJSON_GetArrayItem(frame, 0);
				if (cJSON_IsString(library) && cJSON_IsString(key))
					ok &= model_key(path, library->valuestring, key->valuestring);
			}
		}
		const cJSON* damage  = cJSON_GetObjectItemCaseSensitive(extras, "aeronDamage");
		const cJSON* library = cJSON_GetObjectItemCaseSensitive(damage, "library");
		const cJSON* alternative;
		cJSON_ArrayForEach(alternative, cJSON_GetObjectItemCaseSensitive(damage, "alternatives")) {
			const cJSON* stage;
			cJSON_ArrayForEach(stage, cJSON_GetObjectItemCaseSensitive(alternative, "stages")) {
				if (cJSON_IsString(library) && cJSON_IsString(stage))
					ok &= model_key(path, library->valuestring, stage->valuestring);
			}
		}
	}
	cJSON_Delete(root);
	g_models++;
	return ok;
}

static bool ends_with(const char* s, const char* suffix) {
	const size_t ls = strlen(s), lf = strlen(suffix);
	return ls >= lf && strcasecmp(s + ls - lf, suffix) == 0;
}

static int cmp_string(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* A .glb file, or the .glb files of a directory, in name order. */
static bool read_models(const char* path) {
	struct stat st;
	if (stat(path, &st) != 0) {
		fprintf(stderr, "xwau_effects: %s: not found\n", path);
		return false;
	}
	if (S_ISREG(st.st_mode))
		return read_model(path);
	DIR* d = opendir(path);
	if (!d)
		return false;
	char**         names = NULL;
	size_t         count = 0;
	bool           ok    = true;
	struct dirent* de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' || !ends_with(de->d_name, ".glb"))
			continue;
		char** grown = (char**)realloc(names, (count + 1) * sizeof *grown);
		if (!grown || !(grown[count] = strdup(de->d_name))) {
			names = grown ? grown : names;
			ok    = false;
			break;
		}
		names = grown;
		count++;
	}
	closedir(d);
	qsort(names, count, sizeof *names, cmp_string);
	for (size_t i = 0; i < count; ++i) {
		char child[1024];
		snprintf(child, sizeof child, "%s/%s", path, names[i]);
		ok &= read_model(child);
		free(names[i]);
	}
	free(names);
	return ok;
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

/* The energy effects: GenericDamage.dat groups 2, 1 and 3 (the shipped
 * IEVT_SHIELDS_DOWN / IEVT_TRACTOR_BEAM / IEVT_JAMMING_BEAM lines). */
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
		fprintf(stderr, "xwau_effects: %s not found: no energy effects\n", ENERGY_DAT);
		return true;
	}
	char     err[512];
	XwauDat* dat = XwauDat_Open(rel, err, sizeof err);
	if (!dat) {
		fprintf(stderr, "xwau_effects: %s\n", err);
		return false;
	}
	char lrel[256], library[64];
	dat_names(ENERGY_DAT, lrel, sizeof lrel, library, sizeof library);
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

/* The flipbook of a DAT image from its [Default] sequences: the last
 * EVT_NONE colour sequence (XWAU's TextureATCIndices[TEXTURE][EVT_NONE]). */
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
		fprintf(stderr, "xwau_effects: %s: %s not found\n", name, q->dat);
		return false;
	}
	XwauDat* dat = XwauDat_Open(path, err, sizeof err);
	if (!dat) {
		fprintf(stderr, "xwau_effects: %s\n", err);
		return false;
	}
	/* Frames: listed, or the DAT group at 1/fps. */
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
			frames[i] = (XwauSeqFrame) { q->group, ids[i], q->fps > 0.0f ? 1.0f / q->fps : 1.0f };
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
	/* _rand: frame 0, then random picks, each keeping its duration. */
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
	dat_names(q->dat, rel, sizeof rel, library, sizeof library);
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

/* DAT-named materials of the roots: "dat-<G>-<I>.mat", first root wins. */
static bool images_yaml(Text* y) {
	DatImage* found = NULL;
	size_t    count = 0;
	for (int r = 0; r < g_root_count; ++r) {
		DIR* root = opendir(g_roots[r]);
		if (!root)
			continue;
		struct dirent* de;
		char           mdir[1024] = "";
		while ((de = readdir(root)))
			if (strcasecmp(de->d_name, "Materials") == 0)
				snprintf(mdir, sizeof mdir, "%s/%s", g_roots[r], de->d_name);
		closedir(root);
		DIR* d = mdir[0] ? opendir(mdir) : NULL;
		if (!d)
			continue;
		while ((de = readdir(d))) {
			int  g = 0, i = 0;
			char tail[16];
			if (strncasecmp(de->d_name, "dat-", 4) != 0 ||
				sscanf(de->d_name + 4, "%d-%d.%15s", &g, &i, tail) != 3 || strcasecmp(tail, "mat") != 0)
				continue;
			bool dup = false;
			for (size_t k = 0; k < count && !dup; ++k)
				dup = found[k].group == g && found[k].image == i;
			if (dup)
				continue;
			DatImage* grown = (DatImage*)realloc(found, (count + 1) * sizeof *grown);
			if (!grown) {
				closedir(d);
				free(found);
				return false;
			}
			found = grown;
			memset(&found[count], 0, sizeof found[count]);
			found[count].group = g;
			found[count].image = i;
			snprintf(found[count].mat, sizeof found[count].mat, "%s/%s", mdir, de->d_name);
			count++;
		}
		closedir(d);
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
	/* Blast-mark images without a sequence: their own image, static. */
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
				fprintf(stderr, "xwau_effects: %s\n", err);
			}
		}
	}
	free(mark_images);
	free(found);
	return ok;
}

/* ---- Library build ------------------------------------------------------ */

typedef struct Decoded {
	int          group, image;
	XwauDatImage im;
	bool         ok;
} Decoded;

typedef struct Baked {
	uint8_t* layer;
	uint8_t* emissive;
	int      w, h;
} Baked;

static bool write_file(const char* path, const void* data, size_t size) {
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	const bool ok = fwrite(data, 1, size, f) == size;
	return fclose(f) == 0 && ok;
}

/* Decodes and bakes a library's images, and writes it with effect_pack. */
static bool build_library(Library* l, const char* out_dir) {
	char path[1024], err[512];
	if (!find_file(l->dat, path, sizeof path)) {
		fprintf(stderr, "xwau_effects: %s: %s not found\n", l->name, l->dat);
		return false;
	}
	XwauDat* dat = XwauDat_Open(path, err, sizeof err);
	if (!dat) {
		fprintf(stderr, "xwau_effects: %s\n", err);
		return false;
	}
	qsort(l->requests, l->count, sizeof *l->requests, cmp_request);
	Decoded*         decoded       = (Decoded*)calloc(l->count, sizeof *decoded);
	Baked*           baked         = (Baked*)calloc(l->count, sizeof *baked);
	EffectPackImage* images        = (EffectPackImage*)calloc(l->count ? l->count : 1, sizeof *images);
	size_t           decoded_count = 0, image_count = 0;
	bool             ok = decoded && baked && images;
	for (size_t i = 0; ok && i < l->count; ++i) {
		Request* r = &l->requests[i];
		Decoded* d = NULL;
		for (size_t k = 0; k < decoded_count && !d; ++k)
			if (decoded[k].group == r->group && decoded[k].image == r->image)
				d = &decoded[k];
		if (!d) {
			d        = &decoded[decoded_count++];
			d->group = r->group;
			d->image = r->image;
			d->ok    = XwauDat_LoadImageFrom(dat, r->group, r->image, &d->im, err, sizeof err);
			if (!d->ok)
				fprintf(stderr, "xwau_effects: %s\n", err);
		}
		if (!d->ok)
			continue;
		const size_t n = (size_t)d->im.width * d->im.height;
		Baked*       b = &baked[i];
		b->w           = (int)d->im.width;
		b->h           = (int)d->im.height;
		b->layer       = XwauFx_HasLayer(r->kind) ? (uint8_t*)malloc(n * 4) : NULL;
		b->emissive    = XwauFx_HasEmissive(r->kind) ? (uint8_t*)malloc(n * 4) : NULL;
		if ((XwauFx_HasLayer(r->kind) && !b->layer) || (XwauFx_HasEmissive(r->kind) && !b->emissive)) {
			ok = false;
			break;
		}
		XwauFx_Bake(r->kind, d->im.rgba, n, r->tint, r->alpha_mode, b->layer, b->emissive);
		images[image_count++] = (EffectPackImage) {
			.key      = r->key,
			.width    = b->w,
			.height   = b->h,
			.layer    = b->layer,
			.emissive = b->emissive,
			.max_size = r->kind == XWAU_FX_ENERGY ? ENERGY_MAX : EFFECT_PACK_IMAGE_MAX,
			.crop     = r->crop ? kBlastMarkCrop : NULL,
		};
	}
	XwauDat_Close(dat);
	EffectPackResult result = { 0 };
	ok = ok && EffectPack_Write("xwau_effects", out_dir, l->name, l->dat, images, image_count, &result);
	if (ok)
		fprintf(stderr, "xwau_effects: %s: %zu image(s), %d layer and %d emissive page(s)\n", l->name,
				l->count, result.layer_pages, result.emissive_pages);
	for (size_t i = 0; baked && i < l->count; ++i) {
		free(baked[i].layer);
		free(baked[i].emissive);
	}
	for (size_t k = 0; decoded && k < decoded_count; ++k)
		XwauDat_FreeImage(&decoded[k].im);
	free(images);
	free(baked);
	free(decoded);
	return ok;
}

static int cmp_library(const void* a, const void* b) {
	return strcmp(((const Library*)a)->name, ((const Library*)b)->name);
}

static int usage(void) {
	fprintf(stderr, "usage: xwau_effects [--xwau-root DIR]... --out DIR [MODEL.glb | DIR]...\n");
	return 2;
}

int main(int argc, char** argv) {
	const char* out         = NULL;
	int         first_input = argc;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--xwau-root") == 0 && i + 1 < argc) {
			if (g_root_count == MAX_ROOTS)
				return usage();
			g_roots[g_root_count++] = argv[++i];
		} else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
			out = argv[++i];
		} else if (argv[i][0] == '-') {
			return usage();
		} else {
			first_input = i;
			break;
		}
	}
	if (!out || !g_root_count)
		return usage();
	bool ok = true;
	for (int i = first_input; i < argc; ++i)
		ok &= read_models(argv[i]);
	Text y = { 0 };
	text_add(&y, "version: 1\n");
	ok &= energy_yaml(&y) && images_yaml(&y);
	if (!y.s) {
		fprintf(stderr, "xwau_effects: out of memory\n");
		return 1;
	}
	char path[1024];
	snprintf(path, sizeof path, "%s/effects.yaml", out);
	if (!write_file(path, y.s, y.len)) {
		fprintf(stderr, "xwau_effects: cannot write %s\n", path);
		ok = false;
	}
	free(y.s);
	fprintf(stderr, "xwau_effects: %zu model(s), %zu librar%s\n", g_models, g_library_count,
			g_library_count == 1 ? "y" : "ies");
	for (size_t i = 0; i < g_skipped_count; ++i) {
		fprintf(stderr, "xwau_effects: library %s: no Effects/%s.dat, not an XWAU library; left alone\n",
				g_skipped[i], g_skipped[i]);
		free(g_skipped[i]);
	}
	free(g_skipped);
	qsort(g_libraries, g_library_count, sizeof *g_libraries, cmp_library);
	for (size_t i = 0; i < g_library_count; ++i)
		ok &= build_library(&g_libraries[i], out);
	for (size_t i = 0; i < g_library_count; ++i)
		free(g_libraries[i].requests);
	free(g_libraries);
	return ok ? 0 : 1;
}
