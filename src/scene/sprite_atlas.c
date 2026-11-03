/*
 * Sprite atlas files: the loaders and the writer.
 *
 * Aeron_SpriteAtlasLoad uses libyaml's document API. The files are a few KB,
 * so building the tree is cheap.
 */

#include "aeron/scene/sprite_atlas.h"

#include "aeron/config_file.h"
#include "aeron/log.h"
#include "aeron/numeric.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <yaml.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int config_int(const AeronConfigNode* map, const char* key, int fallback) {
	const AeronConfigNode* value = AeronConfigNode_MapGet(map, key);
	if (AeronConfigNode_Type(value) != AERON_CONFIG_INT)
		return fallback;
	int64_t parsed = AeronConfigNode_Int(value, fallback);
	return parsed < INT_MIN || parsed > INT_MAX ? fallback : (int)parsed;
}

/* Reads the frame's `light: [r, g, b]` into `out`, or -1 in each channel
 * when the frame has no such key. Returns whether it has one. */
static bool config_light(const AeronConfigNode* frame, float* out) {
	const AeronConfigNode* light = AeronConfigNode_MapGet(frame, "light");
	out[0] = out[1] = out[2] = -1.0f;
	if (AeronConfigNode_Type(light) != AERON_CONFIG_SEQUENCE || AeronConfigNode_SequenceCount(light) != 3)
		return false;
	for (size_t i = 0; i < 3; ++i) {
		const AeronConfigNode*    value = AeronConfigNode_SequenceGet(light, i);
		const AeronConfigNodeType type  = AeronConfigNode_Type(value);
		if (type != AERON_CONFIG_INT && type != AERON_CONFIG_FLOAT) {
			out[0] = out[1] = out[2] = -1.0f;
			return false;
		}
		out[i] = (float)AeronConfigNode_Float(value, -1.0);
	}
	return true;
}

/* Allocates the per-frame arrays of `count` frames. */
static bool allocate_frame_arrays(AeronSpriteAtlas* out, size_t count) {
	out->frames = calloc(count, sizeof *out->frames);
	out->ids    = calloc(count, sizeof *out->ids);
	out->pages  = calloc(count, sizeof *out->pages);
	out->size   = calloc(count * 2, sizeof *out->size);
	out->offset = calloc(count * 2, sizeof *out->offset);
	out->light  = calloc(count * 3, sizeof *out->light);
	if (!out->frames || !out->ids || !out->pages || !out->size || !out->offset || !out->light)
		return false;
	for (size_t i = 0; i < count; ++i) {
		out->ids[i]       = -1;
		out->light[i * 3] = out->light[i * 3 + 1] = out->light[i * 3 + 2] = -1.0f;
	}
	return true;
}

/* Frees the optional arrays that no frame uses, and sets the counts. */
static void free_unused_frame_arrays(AeronSpriteAtlas* out, int frame_count) {
	bool have_ids = false, have_size = false, have_offset = false, have_light = false;
	int  max_page = 0;
	for (int i = 0; i < frame_count; ++i) {
		have_ids |= out->ids[i] >= 0;
		have_size |= out->size[i * 2] != 0.0f || out->size[i * 2 + 1] != 0.0f;
		have_offset |= out->offset[i * 2] != 0.0f || out->offset[i * 2 + 1] != 0.0f;
		have_light |= out->light[i * 3] >= 0.0f;
		if (out->pages[i] > max_page)
			max_page = out->pages[i];
	}
	if (!have_ids) {
		free(out->ids);
		out->ids = NULL;
	}
	if (max_page == 0) {
		free(out->pages);
		out->pages = NULL;
	}
	if (!have_size) {
		free(out->size);
		out->size = NULL;
	}
	if (!have_offset) {
		free(out->offset);
		out->offset = NULL;
	}
	if (!have_light) {
		free(out->light);
		out->light = NULL;
	}
	out->frame_count = frame_count;
	out->page_count  = max_page + 1;
}

/* Reads `units`: design (the default) or game. */
static bool parse_units_value(const char* value, bool* game_units) {
	*game_units = value && strcmp(value, "game") == 0;
	return !value || *game_units || strcmp(value, "design") == 0;
}

/* Reads a `[a, b]` sequence of numbers into `out`. */
static bool read_number_pair(const AeronConfigNode* frame, const char* key, float* out) {
	const AeronConfigNode* pair = AeronConfigNode_MapGet(frame, key);
	if (!pair)
		return true;
	if (AeronConfigNode_Type(pair) != AERON_CONFIG_SEQUENCE || AeronConfigNode_SequenceCount(pair) != 2)
		return false;
	for (size_t i = 0; i < 2; ++i) {
		const AeronConfigNode*    value = AeronConfigNode_SequenceGet(pair, i);
		const AeronConfigNodeType type  = AeronConfigNode_Type(value);
		if (type != AERON_CONFIG_INT && type != AERON_CONFIG_FLOAT)
			return false;
		out[i] = (float)AeronConfigNode_Float(value, 0.0);
	}
	return true;
}

bool Aeron_SpriteAtlasLoadVfs(AeronSpriteAtlas* out, AeronVfs* vfs, AeronVfsRoot root,
							  const char* yaml_path) {
	if (!out || !vfs || !yaml_path || !yaml_path[0])
		return false;
	memset(out, 0, sizeof *out);
	AeronConfigFile* document = NULL;
	AeronConfigError error    = { 0 };
	if (!AeronConfigFile_LoadYamlEx(vfs, root, yaml_path, &document, &error)) {
		Aeron_LogError("aeron.scene", "[atlas] %s:%d: %s", yaml_path, error.line, error.message);
		return false;
	}
	const AeronConfigNode* document_root = AeronConfigFile_Root(document);
	const AeronConfigNode* atlas         = AeronConfigNode_MapGet(document_root, "atlas");
	const AeronConfigNode* frames        = AeronConfigNode_MapGet(document_root, "frames");
	const size_t           frame_count   = AeronConfigNode_SequenceCount(frames);
	out->atlas_w                         = config_int(atlas, "w", 0);
	out->atlas_h                         = config_int(atlas, "h", 0);
	if (AeronConfigNode_Type(document_root) != AERON_CONFIG_MAP ||
		AeronConfigNode_Type(atlas) != AERON_CONFIG_MAP ||
		AeronConfigNode_Type(frames) != AERON_CONFIG_SEQUENCE || frame_count == 0 || frame_count > INT_MAX ||
		out->atlas_w <= 0 || out->atlas_h <= 0 ||
		!parse_units_value(AeronConfigNode_String(AeronConfigNode_MapGet(document_root, "units"), NULL),
						   &out->game_units)) {
		Aeron_LogError("aeron.scene", "[atlas] %s: invalid atlas document", yaml_path);
		AeronConfigFile_Destroy(document);
		return false;
	}
	if (!allocate_frame_arrays(out, frame_count)) {
		AeronConfigFile_Destroy(document);
		Aeron_SpriteAtlasFree(out);
		return false;
	}
	for (size_t index = 0; index < frame_count; ++index) {
		const AeronConfigNode* frame = AeronConfigNode_SequenceGet(frames, index);
		int                    x = config_int(frame, "x", -1), y = config_int(frame, "y", -1);
		int                    w = config_int(frame, "w", -1), h = config_int(frame, "h", -1);
		int                    page = config_int(frame, "page", 0);
		if (AeronConfigNode_Type(frame) != AERON_CONFIG_MAP || x < 0 || y < 0 || w <= 0 || h <= 0 ||
			page < 0 || page > INT16_MAX || x > out->atlas_w - w || y > out->atlas_h - h ||
			!read_number_pair(frame, "size", &out->size[index * 2]) ||
			!read_number_pair(frame, "offset", &out->offset[index * 2])) {
			Aeron_LogError("aeron.scene", "[atlas] %s: invalid frame %zu", yaml_path, index);
			AeronConfigFile_Destroy(document);
			Aeron_SpriteAtlasFree(out);
			return false;
		}
		out->frames[index] = (AeronSpriteRect) { x, y, w, h };
		out->ids[index]    = config_int(frame, "id", -1);
		out->pages[index]  = (int16_t)page;
		config_light(frame, &out->light[index * 3]);
	}
	free_unused_frame_arrays(out, (int)frame_count);
	AeronConfigFile_Destroy(document);
	return true;
}

/* Pull a scalar node's string content; NULL when not a scalar. */
static const char* node_scalar(const yaml_document_t* doc, int node_id) {
	yaml_node_t* n = yaml_document_get_node((yaml_document_t*)doc, node_id);
	if (!n || n->type != YAML_SCALAR_NODE)
		return NULL;
	return (const char*)n->data.scalar.value;
}

static int node_int(const yaml_document_t* doc, int node_id) {
	const char* s = node_scalar(doc, node_id);
	return s ? atoi(s) : 0;
}

static float node_float(const yaml_document_t* doc, int node_id) {
	const char* s = node_scalar(doc, node_id);
	double      value;
	return s && Aeron_ParseAsciiDouble(s, strlen(s), &value) ? (float)value : 0.0f;
}

/* Reads `w` and `h` of the `atlas:` map. The other keys are ignored. */
static void parse_atlas_mapping(const yaml_document_t* doc, const yaml_node_t* m, AeronSpriteAtlas* out) {
	for (yaml_node_pair_t* kv = m->data.mapping.pairs.start; kv < m->data.mapping.pairs.top; kv++) {
		const char* k = node_scalar(doc, kv->key);
		if (!k)
			continue;
		if (strcmp(k, "w") == 0)
			out->atlas_w = node_int(doc, kv->value);
		else if (strcmp(k, "h") == 0)
			out->atlas_h = node_int(doc, kv->value);
	}
}

/* Reads a sequence of `count` numbers into `out`, when the node is one. */
static void read_number_sequence(const yaml_document_t* doc, int node_id, int count, float* out) {
	const yaml_node_t* list = yaml_document_get_node((yaml_document_t*)doc, node_id);
	if (list && list->type == YAML_SEQUENCE_NODE &&
		list->data.sequence.items.top - list->data.sequence.items.start == count) {
		for (int i = 0; i < count; i++)
			out[i] = node_float(doc, list->data.sequence.items.start[i]);
	}
}

/* Reads one frame map into frame `index` of `out`. */
static void parse_frame_mapping(const yaml_document_t* doc, const yaml_node_t* m, AeronSpriteAtlas* out,
								int index) {
	AeronSpriteRect* rect = &out->frames[index];
	for (yaml_node_pair_t* kv = m->data.mapping.pairs.start; kv < m->data.mapping.pairs.top; kv++) {
		const char* k = node_scalar(doc, kv->key);
		if (!k)
			continue;
		if (strcmp(k, "light") == 0)
			read_number_sequence(doc, kv->value, 3, &out->light[index * 3]);
		else if (strcmp(k, "size") == 0)
			read_number_sequence(doc, kv->value, 2, &out->size[index * 2]);
		else if (strcmp(k, "offset") == 0)
			read_number_sequence(doc, kv->value, 2, &out->offset[index * 2]);
		else if (strcmp(k, "x") == 0)
			rect->x = node_float(doc, kv->value);
		else if (strcmp(k, "y") == 0)
			rect->y = node_float(doc, kv->value);
		else if (strcmp(k, "w") == 0)
			rect->w = node_float(doc, kv->value);
		else if (strcmp(k, "h") == 0)
			rect->h = node_float(doc, kv->value);
		else if (strcmp(k, "id") == 0)
			out->ids[index] = (int32_t)node_int(doc, kv->value);
		else if (strcmp(k, "page") == 0)
			out->pages[index] = (int16_t)node_int(doc, kv->value);
	}
}

bool Aeron_SpriteAtlasLoad(AeronSpriteAtlas* out, const char* yaml_path) {
	if (!out || !yaml_path)
		return false;
	memset(out, 0, sizeof *out);

	FILE* fp = fopen(yaml_path, "rb");
	if (!fp) {
		Aeron_LogError("aeron.scene", "[atlas] cannot open %s", yaml_path);
		return false;
	}

	yaml_parser_t   parser;
	yaml_document_t doc;
	if (!yaml_parser_initialize(&parser)) {
		fclose(fp);
		return false;
	}
	yaml_parser_set_input_file(&parser, fp);

	if (!yaml_parser_load(&parser, &doc)) {
		Aeron_LogError("aeron.scene", "[atlas] %s: parse error at line %lu col %lu: %s", yaml_path,
					   (unsigned long)parser.problem_mark.line + 1,
					   (unsigned long)parser.problem_mark.column + 1,
					   parser.problem ? parser.problem : "(no detail)");
		yaml_parser_delete(&parser);
		fclose(fp);
		return false;
	}

	yaml_node_t* root = yaml_document_get_root_node(&doc);
	if (!root || root->type != YAML_MAPPING_NODE) {
		Aeron_LogError("aeron.scene", "[atlas] %s: top-level node is not a mapping", yaml_path);
		goto fail;
	}

	/* The order of the keys in the file doesn't matter. */
	for (yaml_node_pair_t* kv = root->data.mapping.pairs.start; kv < root->data.mapping.pairs.top; kv++) {
		const char*  k = node_scalar(&doc, kv->key);
		yaml_node_t* v = yaml_document_get_node(&doc, kv->value);
		if (!k || !v)
			continue;
		if (strcmp(k, "units") == 0) {
			if (!parse_units_value(node_scalar(&doc, kv->value), &out->game_units) ||
				v->type != YAML_SCALAR_NODE) {
				Aeron_LogError("aeron.scene", "[atlas] %s: units must be design or game", yaml_path);
				goto fail;
			}
		} else if (strcmp(k, "atlas") == 0 && v->type == YAML_MAPPING_NODE) {
			parse_atlas_mapping(&doc, v, out);
		} else if (strcmp(k, "frames") == 0 && v->type == YAML_SEQUENCE_NODE) {
			int n = (int)(v->data.sequence.items.top - v->data.sequence.items.start);
			if (n <= 0)
				continue;
			if (!allocate_frame_arrays(out, (size_t)n)) {
				Aeron_LogError("aeron.scene", "[atlas] %s: out of memory (%d frames)", yaml_path, n);
				goto fail;
			}
			int idx = 0;
			for (yaml_node_item_t* it = v->data.sequence.items.start;
				 it < v->data.sequence.items.top && idx < n; it++, idx++) {
				yaml_node_t* fr = yaml_document_get_node(&doc, *it);
				if (fr && fr->type == YAML_MAPPING_NODE)
					parse_frame_mapping(&doc, fr, out, idx);
			}
			free_unused_frame_arrays(out, n);
		}
	}

	yaml_document_delete(&doc);
	yaml_parser_delete(&parser);
	fclose(fp);

	if (out->frame_count == 0) {
		Aeron_LogError("aeron.scene", "[atlas] %s: no `frames:` sequence found", yaml_path);
		Aeron_SpriteAtlasFree(out);
		return false;
	}
	return true;

fail:
	yaml_document_delete(&doc);
	yaml_parser_delete(&parser);
	fclose(fp);
	Aeron_SpriteAtlasFree(out);
	return false;
}

void Aeron_SpriteAtlasFree(AeronSpriteAtlas* a) {
	if (!a)
		return;
	free(a->frames);
	free(a->ids);
	free(a->pages);
	free(a->size);
	free(a->offset);
	free(a->light);
	memset(a, 0, sizeof *a);
}

int Aeron_SpriteAtlasFindById(const AeronSpriteAtlas* a, int32_t id) {
	if (!a || !a->ids || id < 0)
		return -1;
	for (int i = 0; i < a->frame_count; i++) {
		if (a->ids[i] == id)
			return i;
	}
	return -1;
}

/* Rounds a rect value to a whole pixel, with negative values clamped to
 * 0. The files hold whole pixels. */
static int round_pos(float f) {
	if (f <= 0.0f)
		return 0;
	return (int)(f + 0.5f);
}

bool Aeron_SpriteAtlasSave(const AeronSpriteAtlas* a, const char* yaml_path, char* err, size_t errsz) {
	if (!a || !yaml_path || !yaml_path[0]) {
		if (err && errsz)
			snprintf(err, errsz, "invalid args");
		return false;
	}

	/* tmp path = "<yaml_path>.tmp". Atomic rename keeps the source-of-
	 * truth intact when fprintf fails partway through. */
	char tmp_path[1100];
	int  n = snprintf(tmp_path, sizeof tmp_path, "%s.tmp", yaml_path);
	if (n < 0 || n >= (int)sizeof tmp_path) {
		if (err && errsz)
			snprintf(err, errsz, "path too long");
		return false;
	}

	FILE* f = fopen(tmp_path, "w");
	if (!f) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", tmp_path, strerror(errno));
		return false;
	}

	if ((a->game_units && fprintf(f, "units: game\n") < 0) ||
		fprintf(f, "atlas: {w: %d, h: %d}\nframes:\n", a->atlas_w, a->atlas_h) < 0)
		goto io_err;
	for (int i = 0; i < a->frame_count; i++) {
		const AeronSpriteRect* r = &a->frames[i];
		char                   head[48], tail[160];
		int                    hn = 0, tn = 0;
		head[0] = tail[0] = '\0';
		if (a->ids)
			hn += snprintf(head + hn, sizeof head - (size_t)hn, "id: %d, ", (int)a->ids[i]);
		if (a->pages)
			snprintf(head + hn, sizeof head - (size_t)hn, "page: %d, ", (int)a->pages[i]);
		if (a->size)
			tn += snprintf(tail + tn, sizeof tail - (size_t)tn, ", size: [%.9g, %.9g]",
						   (double)a->size[i * 2], (double)a->size[i * 2 + 1]);
		if (a->offset)
			tn += snprintf(tail + tn, sizeof tail - (size_t)tn, ", offset: [%.9g, %.9g]",
						   (double)a->offset[i * 2], (double)a->offset[i * 2 + 1]);
		if (a->light && a->light[i * 3] >= 0.0f)
			snprintf(tail + tn, sizeof tail - (size_t)tn, ", light: [%.9g, %.9g, %.9g]",
					 (double)a->light[i * 3], (double)a->light[i * 3 + 1], (double)a->light[i * 3 + 2]);
		if (fprintf(f, "  - {%sx: %d, y: %d, w: %d, h: %d%s}\n", head, round_pos(r->x), round_pos(r->y),
					round_pos(r->w), round_pos(r->h), tail) < 0)
			goto io_err;
	}

	if (fflush(f) != 0)
		goto io_err;
	if (fclose(f) != 0) {
		f = NULL;
		if (err && errsz)
			snprintf(err, errsz, "close %s: %s", tmp_path, strerror(errno));
		SDL_RemovePath(tmp_path);
		return false;
	}
	f = NULL;

	if (!SDL_RenamePath(tmp_path, yaml_path)) {
		if (err && errsz)
			snprintf(err, errsz, "rename %s -> %s: %s", tmp_path, yaml_path, SDL_GetError());
		SDL_RemovePath(tmp_path);
		return false;
	}
	return true;

io_err:
	if (err && errsz)
		snprintf(err, errsz, "write %s: %s", tmp_path, strerror(errno));
	if (f)
		fclose(f);
	SDL_RemovePath(tmp_path);
	return false;
}
