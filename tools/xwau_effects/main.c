/*
 * xwau_effects — builds the effect image libraries and the game effect data
 * of converted XWAU models (docs: xwau_animated_materials.md, "Asset Format").
 *
 *   xwau_effects [--xwau-root DIR]... --out DIR [SIDECAR | DIR]...
 *
 * Reads the `.effects.json` sidecars opt2gltf writes next to each converted
 * model (files, or directories searched two levels deep), and the DAT-named
 * material files (Materials/dat-<G>-<I>.mat) of the roots. Writes into --out:
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
#include "channel_atlas.h"
#include "ktx2_writer.h"
#include "xwau_dat.h"
#include "xwau_effect_bake.h"
#include "xwau_mat.h"

#define MAX_ROOTS 16
/* Pages of 2048 + 2048/16 texels: with 16-texel gutters, two 1024² or four
 * 512² images fit a row (a 2048² page holds one 1024² image). Every image,
 * up to IMAGE_MAX, is a shared rect: none needs a dedicated layer. */
#define PAGE_SIZE 2176
#define PACK_SIZE 4096
#define IMAGE_MAX 2048
#define GUTTER 16
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
	/* Output */
	int layer_rect, emissive_rect;
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
	r->layer_rect = r->emissive_rect = -1;
	return true;
}

static bool kind_from_name(const char* name, XwauFxKind* out) {
	for (int k = XWAU_FX_ILLUMINATION; k <= XWAU_FX_MARK; ++k)
		if (strcmp(name, XwauFx_KindName((XwauFxKind)k)) == 0) {
			*out = (XwauFxKind)k;
			return true;
		}
	return false;
}

/* ---- Sidecars ----------------------------------------------------------- */

static char* read_text(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* text = n >= 0 ? (char*)malloc((size_t)n + 1) : NULL;
	if (text && fread(text, 1, (size_t)n, f) == (size_t)n)
		text[n] = 0;
	else {
		free(text);
		text = NULL;
	}
	fclose(f);
	return text;
}

static size_t g_sidecars;

static bool read_sidecar(const char* path) {
	char*  text = read_text(path);
	cJSON* root = text ? cJSON_Parse(text) : NULL;
	free(text);
	const cJSON* images = cJSON_GetObjectItemCaseSensitive(root, "images");
	if (!cJSON_IsArray(images)) {
		fprintf(stderr, "xwau_effects: %s: not a sidecar\n", path);
		cJSON_Delete(root);
		return false;
	}
	bool         ok = true;
	const cJSON* item;
	cJSON_ArrayForEach(item, images) {
		const cJSON* dat   = cJSON_GetObjectItemCaseSensitive(item, "dat");
		const cJSON* group = cJSON_GetObjectItemCaseSensitive(item, "group");
		const cJSON* image = cJSON_GetObjectItemCaseSensitive(item, "image");
		const cJSON* kind  = cJSON_GetObjectItemCaseSensitive(item, "kind");
		const cJSON* tint  = cJSON_GetObjectItemCaseSensitive(item, "tint");
		const cJSON* alpha = cJSON_GetObjectItemCaseSensitive(item, "alphaMode");
		XwauFxKind   k;
		if (!cJSON_IsString(dat) || !cJSON_IsNumber(group) || !cJSON_IsNumber(image) ||
			!cJSON_IsString(kind) || !kind_from_name(kind->valuestring, &k) ||
			cJSON_GetArraySize(tint) != 3) {
			fprintf(stderr, "xwau_effects: %s: malformed image entry\n", path);
			ok = false;
			continue;
		}
		float t[3];
		for (int c = 0; c < 3; ++c)
			t[c] = (float)cJSON_GetArrayItem(tint, c)->valuedouble;
		ok &= request(dat->valuestring, group->valueint, image->valueint, k, t,
					  cJSON_IsNumber(alpha) ? alpha->valueint : 0, false);
	}
	cJSON_Delete(root);
	g_sidecars++;
	return ok;
}

static bool ends_with(const char* s, const char* suffix) {
	const size_t ls = strlen(s), lf = strlen(suffix);
	return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

/* A sidecar file, or a directory searched `depth` levels deep. */
static bool read_sidecars(const char* path, int depth) {
	struct stat st;
	if (stat(path, &st) != 0) {
		fprintf(stderr, "xwau_effects: %s: not found\n", path);
		return false;
	}
	if (S_ISREG(st.st_mode))
		return read_sidecar(path);
	if (!S_ISDIR(st.st_mode) || depth < 0)
		return true;
	DIR* d = opendir(path);
	if (!d)
		return false;
	bool           ok = true;
	struct dirent* de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		char child[1024];
		snprintf(child, sizeof child, "%s/%s", path, de->d_name);
		if (stat(child, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			ok &= read_sidecars(child, depth - 1);
		else if (ends_with(de->d_name, ".effects.json"))
			ok &= read_sidecar(child);
	}
	closedir(d);
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

static void yaml_rect(FILE* f, const char* name, const ChannelAtlas* ca, int index) {
	const ChannelRect* r = &ca->rects[index];
	const int          w = r->dedicated ? ca->width : r->src_w, h = r->dedicated ? ca->height : r->src_h;
	fprintf(f, "%s: { page: %d, rect: [%d, %d, %d, %d], lod: %d }", name, r->page, r->x, r->y, w, h,
			r->max_lod);
}

static bool encode(const ChannelAtlas* ca, const char* path) {
	uint8_t* buf  = NULL;
	size_t   size = 0;
	bool     ok   = write_ktx2_bc7_with_generated_mips_to_buffer_limited_alpha(
						ca->width, ca->height, ca->page_count, ca->rgba, KTX2_BC7_QUALITY_MED, KTX2_TF_SRGB, true,
						ca->mip_count, KTX2_ALPHA_PREMULTIPLIED, &buf, &size) &&
					write_file(path, buf, size);
	free(buf);
	return ok;
}

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
	Decoded*     decoded       = (Decoded*)calloc(l->count, sizeof *decoded);
	Baked*       baked         = (Baked*)calloc(l->count, sizeof *baked);
	size_t       decoded_count = 0;
	bool         ok            = decoded && baked;
	ChannelAtlas layer, emissive;
	channel_atlas_init(&layer, AERON_GLTF_COOK_CHANNEL_BASE_COLOR);
	channel_atlas_init(&emissive, AERON_GLTF_COOK_CHANNEL_EMISSIVE);
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
		const int max = r->kind == XWAU_FX_ENERGY ? ENERGY_MAX : IMAGE_MAX;
		if (b->layer)
			r->layer_rect = channel_atlas_add_rect(&layer, r, b->w, b->h, b->layer, max);
		if (b->emissive)
			r->emissive_rect = channel_atlas_add_rect(&emissive, r, b->w, b->h, b->emissive, max);
		ok = (!b->layer || r->layer_rect >= 0) && (!b->emissive || r->emissive_rect >= 0);
	}
	XwauDat_Close(dat);
	ChannelAtlas* atlases[2] = { &layer, &emissive };
	const char*   suffix[2]  = { "layer", "emissive" };
	for (int a = 0; ok && a < 2; ++a) {
		if (!atlases[a]->rect_count)
			continue;
		atlases[a]->fixed_width = atlases[a]->fixed_height = PAGE_SIZE;
		ok = channel_atlas_pack(atlases[a], PACK_SIZE, GUTTER) && channel_atlas_materialize(atlases[a]);
		snprintf(path, sizeof path, "%s/%s.%s.ktx2", out_dir, l->name, suffix[a]);
		ok = ok && encode(atlases[a], path);
		if (!ok)
			fprintf(stderr, "xwau_effects: %s: cannot pack or write the %s pages\n", l->name, suffix[a]);
	}
	if (ok) {
		snprintf(path, sizeof path, "%s/%s.yaml", out_dir, l->name);
		FILE* f = fopen(path, "wb");
		ok      = f != NULL;
		if (f) {
			fprintf(f,
					"version: 1\nsource: %s\npage_size: %d\nlayer_pages: %d\nemissive_pages: %d\nimages:\n",
					l->dat, PAGE_SIZE, layer.rect_count ? layer.page_count : 0,
					emissive.rect_count ? emissive.page_count : 0);
			for (size_t i = 0; i < l->count; ++i) {
				const Request* r = &l->requests[i];
				if (r->layer_rect < 0 && r->emissive_rect < 0)
					continue;
				fprintf(f, "  \"%s\": { ", r->key);
				if (r->layer_rect >= 0)
					yaml_rect(f, "layer", &layer, r->layer_rect);
				if (r->emissive_rect >= 0) {
					fprintf(f, "%s", r->layer_rect >= 0 ? ", " : "");
					yaml_rect(f, "emissive", &emissive, r->emissive_rect);
				}
				if (r->crop)
					fprintf(f, ", crop: [%g, %g, %g, %g]", (double)kBlastMarkCrop[0],
							(double)kBlastMarkCrop[1], (double)kBlastMarkCrop[2], (double)kBlastMarkCrop[3]);
				fprintf(f, " }\n");
			}
			ok = fclose(f) == 0;
		}
	}
	if (ok)
		fprintf(stderr, "xwau_effects: %s: %zu image(s), %d layer and %d emissive page(s)\n", l->name,
				l->count, layer.rect_count ? layer.page_count : 0,
				emissive.rect_count ? emissive.page_count : 0);
	channel_atlas_free(&layer);
	channel_atlas_free(&emissive);
	for (size_t i = 0; baked && i < l->count; ++i) {
		free(baked[i].layer);
		free(baked[i].emissive);
	}
	for (size_t k = 0; decoded && k < decoded_count; ++k)
		XwauDat_FreeImage(&decoded[k].im);
	free(baked);
	free(decoded);
	return ok;
}

static int cmp_library(const void* a, const void* b) {
	return strcmp(((const Library*)a)->name, ((const Library*)b)->name);
}

static int usage(void) {
	fprintf(stderr, "usage: xwau_effects [--xwau-root DIR]... --out DIR [SIDECAR | DIR]...\n");
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
		ok &= read_sidecars(argv[i], 2);
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
	fprintf(stderr, "xwau_effects: %zu sidecar(s), %zu librar%s\n", g_sidecars, g_library_count,
			g_library_count == 1 ? "y" : "ies");
	qsort(g_libraries, g_library_count, sizeof *g_libraries, cmp_library);
	for (size_t i = 0; i < g_library_count; ++i)
		ok &= build_library(&g_libraries[i], out);
	for (size_t i = 0; i < g_library_count; ++i)
		free(g_libraries[i].requests);
	free(g_libraries);
	return ok ? 0 : 1;
}
