/*
 * xwau_effects — imports the effect images and the game effect data of
 * converted XWAU models into an asset source tree (docs: asset_sources.md).
 *
 *   xwau_effects [--xwau-root DIR]... --sources TREE [MODEL.anim.yaml | TREE]...
 *
 * Reads the library images the models use (the flipbooks and damage of
 * model animation files, given or found as models/<key>/<key>.anim.yaml in
 * a source tree) and the DAT-named material files
 * (Materials/dat-<G>-<I>.mat) of the roots. A library is the images of
 * Effects/<library>.dat; a library with no such DAT in the roots is not an
 * XWAU one (authored content) and is left alone.
 * Roots are searched in order, the first holding a file wins (as opt2gltf).
 *
 * Writes TREE/effects/<library>/<library>.yaml (an effect library file,
 * tools/gltf_cook/animated_materials.md), each image's inputs as PNGs under
 * frames/ (XWAU's rules applied: an illumination frame as its emissive and
 * its base colour with coverage, the tint and alpha mode baked in), with its
 * size cap and crop; and TREE/effects/effects.yaml (the energy effects and the
 * flipbooks of DAT images the game draws directly, the blast marks of group
 * 3050). The libraries are then built from the tree like any other.
 */

#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "png_write.h"
#include "xwau_dat.h"
#include "xwau_effect_bake.h"
#include "xwau_mat.h"
#include "yaml_json.h"

#define MAX_ROOTS 16
/* Energy frames are capped at ENERGY_MAX (`maxSize`); other images keep the
 * library's default cap. */
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

/* The library images of one material's flipbooks and damage. */
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

/* The library images a model animation file's materials use. */
static bool read_anim(const char* path) {
	char   err[512];
	cJSON* root = YamlJson_Load(path, err, sizeof err);
	if (!root) {
		fprintf(stderr, "xwau_effects: %s\n", err);
		return false;
	}
	bool         ok = true;
	const cJSON* material;
	cJSON_ArrayForEach(material, cJSON_GetObjectItemCaseSensitive(root, "materials")) {
		ok &= material_keys(path, material);
	}
	cJSON_Delete(root);
	g_models++;
	return ok;
}

static int cmp_string(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* The sorted entries of directory `path` that `keep` accepts. */
static char** list_dir(const char* path, bool (*keep)(const char* dir, const char* name), size_t* count) {
	*count  = 0;
	DIR* d  = opendir(path);
	if (!d)
		return NULL;
	char**         names = NULL;
	struct dirent* de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' || !keep(path, de->d_name))
			continue;
		char** grown = (char**)realloc(names, (*count + 1) * sizeof *grown);
		if (!grown || !(grown[*count] = strdup(de->d_name))) {
			names = grown ? grown : names;
			break;
		}
		names = grown;
		(*count)++;
	}
	closedir(d);
	qsort(names, *count, sizeof *names, cmp_string);
	return names;
}

static bool is_model_dir(const char* dir, const char* name) {
	char        path[1024];
	struct stat st;
	snprintf(path, sizeof path, "%s/%s/%s.anim.yaml", dir, name, name);
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* A model animation file, or a source tree's models/<key>/<key>.anim.yaml
 * in name order. */
static bool read_models(const char* path) {
	struct stat st;
	if (stat(path, &st) != 0) {
		fprintf(stderr, "xwau_effects: %s: not found\n", path);
		return false;
	}
	if (S_ISREG(st.st_mode))
		return read_anim(path);
	char models[1024];
	snprintf(models, sizeof models, "%s/models", path);
	if (stat(models, &st) == 0 && S_ISDIR(st.st_mode)) {
		size_t count = 0;
		char** keys  = list_dir(models, is_model_dir, &count);
		bool   ok    = true;
		for (size_t i = 0; i < count; ++i) {
			char anim[2048];
			snprintf(anim, sizeof anim, "%s/%s/%s.anim.yaml", models, keys[i], keys[i]);
			ok &= read_anim(anim);
			free(keys[i]);
		}
		free(keys);
		return ok;
	}
	fprintf(stderr, "xwau_effects: %s: not a model animation file or a source tree\n", path);
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

static bool write_file(const char* path, const void* data, size_t size) {
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	const bool ok = fwrite(data, 1, size, f) == size;
	return fclose(f) == 0 && ok;
}

/* ---- Sources ------------------------------------------------------------ */

static bool make_dir(const char* path) {
	struct stat st;
	return mkdir(path, 0755) == 0 || (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* The library builder's transfer functions (8-bit sRGB in and out), to
 * check that the written inputs bake back into the library's images. */
static float lin8(uint8_t v) {
	const float c = v / 255.0f;
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static uint8_t srgb8(float c) {
	c             = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
	const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)(s * 255.0f + 0.5f);
}

static uint8_t unorm8f(float c) { return (uint8_t)((c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c)) * 255.0f + 0.5f); }

static int max_diff(int d, int a, int b) { return abs(a - b) > d ? abs(a - b) : d; }

/* Writes `rgba` as <dir>/<file>; false after a message. */
static bool write_frame(const char* dir, const char* file, int w, int h, const uint8_t* rgba) {
	char png[1200];
	snprintf(png, sizeof png, "%s/%s", dir, file);
	if (write_png_rgba(png, w, h, rgba))
		return true;
	fprintf(stderr, "xwau_effects: cannot write %s\n", png);
	return false;
}

/* Writes library `l` as sources under `effects_dir`: <name>/<name>.yaml
 * and its images under <name>/frames/, each key's inputs in the library
 * file's kinds (the XWAU rules applied: illumination as an emissive image and a
 * base colour image with its coverage, tint and alpha mode baked in).
 * Reports the largest difference, per channel, between what the library
 * builder bakes from them and the XWAU bake (8-bit rounding of the base colour). */
static bool write_library_sources(Library* l, const char* effects_dir) {
	char path[1024], err[512], dir[1024];
	if (!find_file(l->dat, path, sizeof path)) {
		fprintf(stderr, "xwau_effects: %s: %s not found\n", l->name, l->dat);
		return false;
	}
	qsort(l->requests, l->count, sizeof *l->requests, cmp_request);
	snprintf(dir, sizeof dir, "%s/%s", effects_dir, l->name);
	char frames[1100];
	snprintf(frames, sizeof frames, "%s/frames", dir);
	if (!make_dir(dir) || !make_dir(frames)) {
		fprintf(stderr, "xwau_effects: cannot create %s\n", frames);
		return false;
	}
	XwauDat* dat = XwauDat_Open(path, err, sizeof err);
	if (!dat) {
		fprintf(stderr, "xwau_effects: %s\n", err);
		return false;
	}
	Text y = { 0 };
	text_add(&y, "# Effect library %s, imported from XWAU's %s.\n", l->name, l->dat);
	text_add(&y, "version: 1\nlibrary: %s\nimages:\n", l->name);
	bool   ok     = true;
	size_t images = 0;
	int    diff   = 0;
	for (size_t i = 0; ok && i < l->count; ++i) {
		const Request* r = &l->requests[i];
		XwauDatImage   im;
		if (!XwauDat_LoadImageFrom(dat, r->group, r->image, &im, err, sizeof err)) {
			fprintf(stderr, "xwau_effects: %s\n", err);
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
		/* The file names: the key, ':' made '_'. */
		char base[128], file[160], base_color[160];
		snprintf(base, sizeof base, "frames/%s", r->key);
		for (char* c = base; *c; ++c)
			if (*c == ':')
				*c = '_';
		bool any_base_color = false;
		if (ok && r->kind == XWAU_FX_ILLUMINATION) {
			/* Base colour: coverage c = 1 - k, colour layer_rgb / c. */
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
			snprintf(file, sizeof file, "%s_emissive.png", base);
			snprintf(base_color, sizeof base_color, "%s_base_color.png", base);
			ok = write_frame(dir, file, w, h, emissive) && (!any_base_color || write_frame(dir, base_color, w, h, input));
			text_add(&y, "  \"%s\": { kind: emissive, emissive: %s", r->key, file);
			if (any_base_color)
				text_add(&y, ", baseColor: %s", base_color);
		} else if (ok && (r->kind == XWAU_FX_COLOUR || r->kind == XWAU_FX_MULTIPLY)) {
			snprintf(file, sizeof file, "%s.png", base);
			ok = write_frame(dir, file, w, h, layer);
			text_add(&y, "  \"%s\": { kind: %s, %s: %s", r->key,
					 r->kind == XWAU_FX_MULTIPLY ? "damage" : "baseColor",
					 r->kind == XWAU_FX_MULTIPLY ? "factor" : "baseColor", file);
		} else if (ok) {
			/* Mark, energy: the tinted frame with its alpha, which the library
			 * builder premultiplies. */
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
			snprintf(file, sizeof file, "%s.png", base);
			ok = write_frame(dir, file, w, h, input);
			text_add(&y, "  \"%s\": { kind: %s, %s: %s", r->key, XwauFx_KindName(r->kind),
					 r->kind == XWAU_FX_MARK ? "baseColor" : "emissive", file);
		}
		if (ok) {
			if (r->kind == XWAU_FX_ENERGY)
				text_add(&y, ", maxSize: %d", ENERGY_MAX);
			if (r->crop)
				text_add(&y, ", crop: [%.9g, %.9g, %.9g, %.9g]", kBlastMarkCrop[0], kBlastMarkCrop[1],
						 kBlastMarkCrop[2], kBlastMarkCrop[3]);
			text_add(&y, " }\n");
			images++;
		}
		free(layer);
		free(emissive);
		free(input);
		XwauDat_FreeImage(&im);
	}
	XwauDat_Close(dat);
	snprintf(path, sizeof path, "%s/%s.yaml", dir, l->name);
	ok = ok && y.s && write_file(path, y.s, y.len);
	free(y.s);
	if (ok)
		fprintf(stderr, "xwau_effects: %s: %zu image(s) as sources (largest rounding: %d/255)\n", l->name, images,
				diff);
	return ok;
}

static int cmp_library(const void* a, const void* b) {
	return strcmp(((const Library*)a)->name, ((const Library*)b)->name);
}

static int usage(void) {
	fprintf(stderr, "usage: xwau_effects [--xwau-root DIR]... --sources TREE [MODEL.anim.yaml | TREE]...\n");
	return 2;
}

int main(int argc, char** argv) {
	const char* sources     = NULL;
	int         first_input = argc;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--xwau-root") == 0 && i + 1 < argc) {
			if (g_root_count == MAX_ROOTS)
				return usage();
			g_roots[g_root_count++] = argv[++i];
		} else if (strcmp(argv[i], "--sources") == 0 && i + 1 < argc) {
			sources = argv[++i];
		} else if (argv[i][0] == '-') {
			return usage();
		} else {
			first_input = i;
			break;
		}
	}
	if (!sources || !g_root_count)
		return usage();
	char out[1024];
	snprintf(out, sizeof out, "%s/effects", sources);
	if (!make_dir(out)) {
		fprintf(stderr, "xwau_effects: cannot create %s\n", out);
		return 1;
	}
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
		ok &= write_library_sources(&g_libraries[i], out);
	for (size_t i = 0; i < g_library_count; ++i)
		free(g_libraries[i].requests);
	free(g_libraries);
	return ok ? 0 : 1;
}
