#include "xwau_anim.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opt_json.h"
#include "util.h"
#include "xwau_dat.h"
#include "xwau_effect_bake.h"

/* Frames of an XWAU `_rand` sequence: frame 0, then random picks. */
#define XWAU_ANIM_RANDOM_FRAMES 64

/* ---- Player states, in XWAU priority order ------------------------------ */

typedef struct EventState {
	const char* event; /* GameEvent name; NULL marks the hangar slot */
	const char* state;
} EventState;

/* GetCurrentATCIndex (Materials.h:711-842): warning lights, cannons, broken
 * instruments, hull, locks, the hangar rule, target selected, none. */
static const EventState kEventStates[] = {
	{ "EVT_WLIGHT_LEFT", "threat.laser" },
	{ "EVT_WLIGHT_MID_LEFT", "threat.turret" },
	{ "EVT_WLIGHT_MID_RIGHT", "threat.beam" },
	{ "EVT_WLIGHT_RIGHT_YELLOW", "threat.missile_locking" },
	{ "EVT_WLIGHT_RIGHT_RED", "threat.missile_locked" },
	{ "EVT_CANNON_1_READY", "cannon.1" },
	{ "EVT_CANNON_2_READY", "cannon.2" },
	{ "EVT_CANNON_3_READY", "cannon.3" },
	{ "EVT_CANNON_4_READY", "cannon.4" },
	{ "EVT_CANNON_5_READY", "cannon.5" },
	{ "EVT_CANNON_6_READY", "cannon.6" },
	{ "EVT_CANNON_7_READY", "cannon.7" },
	{ "EVT_CANNON_8_READY", "cannon.8" },
	{ "EVT_BROKEN_CMD", "broken.cmd" },
	{ "EVT_BROKEN_LASER_ION", "broken.laser_ion" },
	{ "EVT_BROKEN_BEAM_WEAPON", "broken.beam_weapon" },
	{ "EVT_BROKEN_SHIELDS", "broken.shields" },
	{ "EVT_BROKEN_THROTTLE", "broken.throttle" },
	{ "EVT_BROKEN_SENSORS", "broken.sensors" },
	{ "EVT_BROKEN_LASER_RECHARGE", "broken.laser_recharge" },
	{ "EVT_BROKEN_ENGINE_POWER", "broken.engine_power" },
	{ "EVT_BROKEN_SHIELD_RECHARGE", "broken.shield_recharge" },
	{ "EVT_BROKEN_BEAM_RECHARGE", "broken.beam_recharge" },
	{ "EVT_DAMAGE_25", "hull.25" },
	{ "EVT_DAMAGE_50", "hull.50" },
	{ "EVT_DAMAGE_75", "hull.75" },
	{ "EVT_LASER_LOCKED", "laser_lock" },
	{ "EVT_WARHEAD_LOCKING", "warhead.locking" },
	{ "EVT_WARHEAD_LOCKED", "warhead.locked" },
	{ NULL, "hangar" },
	{ "EVT_TARGET_SEL", "target" },
	{ "", "always" },
};
#define EVENT_STATE_COUNT (sizeof kEventStates / sizeof kEventStates[0])
#define HANGAR_SLOT 29

static const char* const kDamageEvents[3] = { "IEVT_DAMAGE_75", "IEVT_DAMAGE_50", "IEVT_DAMAGE_25" };
static const char* const kEnergyEvents[3] = { "IEVT_SHIELDS_DOWN", "IEVT_TRACTOR_BEAM", "IEVT_JAMMING_BEAM" };
static const char* const kEnergyNames[3]  = { "shields", "tractor", "jamming" };

/* ---- Context ---------------------------------------------------------- */

typedef struct OpenDat {
	char     rel[256]; /* lower case, slashes */
	char     path[1024];
	XwauDat* dat; /* NULL when unreadable */
} OpenDat;

typedef struct Owned {
	void** items;
	size_t count;
} Owned;

struct XwauAnim {
	char             model[128];
	char             label[256];
	XwauAnimFindFile find;
	void*            find_context;
	OpenDat**        dats; /* individually allocated: entries keep pointers */
	size_t           dat_count;
	Owned            owned;
	bool             any;
	/* Report counters. */
	size_t flipbooks, damage_materials, energy_materials, anim_bases, always_illuminations, missing_frames;
	size_t skipped_no_illum, skipped_missing_dat, skipped_unsupported, unknown_events, mixed_damage;
	size_t skipped_on_screens;
};

static bool own(XwauAnim* a, void* p) {
	if (!p)
		return false;
	void** grown = (void**)realloc(a->owned.items, (a->owned.count + 1) * sizeof *grown);
	if (!grown) {
		free(p);
		return false;
	}
	a->owned.items                   = grown;
	a->owned.items[a->owned.count++] = p;
	return true;
}

XwauAnim* XwauAnim_Begin(const char* model, XwauAnimFindFile find, void* find_context, const char* label) {
	XwauAnim* a = (XwauAnim*)calloc(1, sizeof *a);
	if (!a)
		return NULL;
	snprintf(a->model, sizeof a->model, "%s", model);
	snprintf(a->label, sizeof a->label, "%s", label);
	a->find         = find;
	a->find_context = find_context;
	return a;
}

void XwauAnim_End(XwauAnim* a) {
	if (!a)
		return;
	for (size_t i = 0; i < a->owned.count; ++i)
		free(a->owned.items[i]);
	free(a->owned.items);
	for (size_t i = 0; i < a->dat_count; ++i) {
		XwauDat_Close(a->dats[i]->dat);
		free(a->dats[i]);
	}
	free(a->dats);
	free(a);
}

bool XwauAnim_Any(const XwauAnim* a) { return a->any; }

/* "Effects\GenericDamage.dat" -> rel "effects/genericdamage.dat", library
 * "genericdamage". */
static void dat_names(const char* written, char* rel, size_t rel_cap, char* library, size_t lib_cap) {
	size_t n = 0;
	for (const char* p = written; *p && n + 1 < rel_cap; ++p)
		rel[n++] = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
	rel[n]           = 0;
	const char* base = strrchr(rel, '/');
	base             = base ? base + 1 : rel;
	snprintf(library, lib_cap, "%s", base);
	char* dot = strrchr(library, '.');
	if (dot)
		*dot = 0;
}

static const OpenDat* open_dat(XwauAnim* a, const char* written) {
	char rel[256], library[64];
	dat_names(written, rel, sizeof rel, library, sizeof library);
	for (size_t i = 0; i < a->dat_count; ++i)
		if (strcmp(a->dats[i]->rel, rel) == 0)
			return a->dats[i]->dat ? a->dats[i] : NULL;
	OpenDat** grown = (OpenDat**)realloc(a->dats, (a->dat_count + 1) * sizeof *grown);
	if (!grown)
		return NULL;
	a->dats    = grown;
	OpenDat* d = (OpenDat*)calloc(1, sizeof *d);
	if (!d)
		return NULL;
	grown[a->dat_count++] = d;
	snprintf(d->rel, sizeof d->rel, "%s", rel);
	if (a->find(a->find_context, written, d->path, sizeof d->path)) {
		char err[512];
		d->dat = XwauDat_Open(d->path, err, sizeof err);
		if (!d->dat)
			Report(a->label, "%s", err);
	}
	return d->dat ? d : NULL;
}

/* ---- JSON ------------------------------------------------------------- */

/* `value` as the shortest decimal that reads back as the same float. */
static cJSON* shortest_float(float value) {
	char text[24];
	for (int digits = 1; digits <= 9; ++digits) {
		snprintf(text, sizeof text, "%.*g", digits, (double)value);
		if (strtof(text, NULL) == value)
			break;
	}
	return cJSON_CreateRaw(text);
}

/* ---- Sequences -------------------------------------------------------- */

static uint32_t fnv1a(uint32_t h, const char* s) {
	for (; *s; ++s)
		h = (h ^ (uint8_t)*s) * 16777619u;
	return h;
}

/* xorshift32 */
static uint32_t next_random(uint32_t* state) {
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *state = x;
}

typedef struct Frames {
	XwauSeqFrame* items;
	size_t        count;
} Frames;

/* Whether image (group, image) is in the DAT. */
static bool dat_has_image(const OpenDat* d, int group, int image) {
	const int*   ids   = NULL;
	const size_t count = XwauDat_ListGroup(d->dat, group, &ids);
	for (size_t i = 0; i < count; ++i)
		if (ids[i] == image)
			return true;
	return false;
}

/* The frames a sequence plays: its listed frames, or its DAT group at
 * 1/fps (one frame when fps <= 0, which never advances). Frames whose image
 * the DAT lacks (content errors) are left out and counted. */
static bool sequence_frames(XwauAnim* a, const XwauSequence* q, const OpenDat* d, Frames* out) {
	memset(out, 0, sizeof *out);
	if (!q->group_frames) {
		out->items = (XwauSeqFrame*)malloc(q->frame_count * sizeof *out->items);
		if (!out->items)
			return false;
		for (size_t i = 0; i < q->frame_count; ++i) {
			if (dat_has_image(d, q->frames[i].group, q->frames[i].image))
				out->items[out->count++] = q->frames[i];
			else
				a->missing_frames++;
		}
		return own(a, out->items);
	}
	const int* ids   = NULL;
	size_t     count = 0;
	int        single;
	if (q->image >= 0) {
		if (!dat_has_image(d, q->group, q->image)) {
			a->missing_frames++;
			return true;
		}
		single = q->image;
		ids    = &single;
		count  = 1;
	} else {
		count = XwauDat_ListGroup(d->dat, q->group, &ids);
	}
	if (count == 0)
		return true;
	if (q->fps <= 0.0f)
		count = 1;
	out->items = (XwauSeqFrame*)malloc(count * sizeof *out->items);
	if (!out->items)
		return false;
	for (size_t i = 0; i < count; ++i)
		out->items[i] = (XwauSeqFrame) { q->group, ids[i], q->fps > 0.0f ? 1.0f / q->fps : 1.0f };
	out->count = count;
	return own(a, out->items);
}

/* An `_rand` sequence as a loop: frame 0, then random picks, each keeping
 * its duration (AnimatedTexControl::Animate). */
static bool randomize(XwauAnim* a, const char* texture, const XwauSequence* q, Frames* f) {
	if (q->mode != XWAU_SEQ_RANDOM || f->count < 2)
		return true;
	XwauSeqFrame* picks = (XwauSeqFrame*)malloc(XWAU_ANIM_RANDOM_FRAMES * sizeof *picks);
	if (!picks || !own(a, picks))
		return false;
	uint32_t seed = fnv1a(fnv1a(fnv1a(fnv1a(2166136261u, a->model), texture), q->key), q->event);
	if (!seed)
		seed = 1;
	picks[0] = f->items[0];
	for (size_t i = 1; i < XWAU_ANIM_RANDOM_FRAMES; ++i)
		picks[i] = f->items[next_random(&seed) % f->count];
	f->items = picks;
	f->count = XWAU_ANIM_RANDOM_FRAMES;
	return true;
}

/* Whether a damage stage's image shows nothing: white (a neutral multiply),
 * or missing from its DAT (a content error, left out of the library). */
static bool image_is_white(const OpenDat* d, int group, int image) {
	XwauDatImage im;
	char         err[256];
	if (!XwauDat_LoadImageFrom(d->dat, group, image, &im, err, sizeof err))
		return true;
	bool         white = true;
	const size_t n     = (size_t)im.width * im.height;
	for (size_t i = 0; i < n && white; ++i)
		white = im.rgba[i * 4] == 255 && im.rgba[i * 4 + 1] == 255 && im.rgba[i * 4 + 2] == 255;
	XwauDat_FreeImage(&im);
	return white;
}

static int state_slot(const char* event) {
	for (size_t i = 0; i < EVENT_STATE_COUNT; ++i)
		if (kEventStates[i].event && strcmp(kEventStates[i].event, event) == 0)
			return (int)i;
	return -1;
}

/* A flipbook entry ready to print. */
typedef struct Flipbook {
	const XwauSequence* seq;
	const OpenDat*      dat;
	Frames              frames;
} Flipbook;

/* Adds a flipbook's entry to `list`; false when out of memory. */
static bool flipbook_json(cJSON* list, const char* state, int layer, const Flipbook* fb, float unlit) {
	const XwauSequence* q = fb->seq;
	char                library[64], rel[256];
	dat_names(fb->dat->rel, rel, sizeof rel, library, sizeof library);
	cJSON* object = OptJson_AddObject(list, NULL);
	bool   ok = OptJson_Add(object, "state", cJSON_CreateString(state)) &&
				OptJson_Add(object, "layer", cJSON_CreateString(layer ? "emissive" : "baseColor")) &&
				OptJson_Add(object, "mode", cJSON_CreateString(q->mode == XWAU_SEQ_ONCE ? "once" : "loop")) &&
				OptJson_Add(object, "library", cJSON_CreateString(library));
	cJSON* frames         = ok ? OptJson_AddArray(object, "frames") : NULL;
	const XwauFxKind kind = layer ? XWAU_FX_ILLUMINATION : XWAU_FX_COLOUR;
	ok                    = frames != NULL;
	for (size_t i = 0; ok && i < fb->frames.count; ++i) {
		const XwauSeqFrame* f = &fb->frames.items[i];
		char                key[96];
		XwauFx_Key(key, sizeof key, f->group, f->image, kind, q->tint, q->alpha_mode);
		cJSON* frame = OptJson_AddArray(frames, NULL);
		ok           = OptJson_Add(frame, NULL, cJSON_CreateString(key)) &&
					   OptJson_Add(frame, NULL, OptJson_Float(f->seconds));
	}
	/* A colour frame replaces the texture, so it is drawn as unlit as the
	   texture's bake (XWAU's animation shader drops Shadeless: a deviation
	   that keeps unlit screens unlit while they animate). */
	if (ok && layer == XWAU_SEQ_COLOUR && unlit > 0.0f)
		ok = OptJson_Add(object, "unlit", OptJson_Float(unlit));
	return ok;
}

static const XwauSequence* find_instance(const XwauMatSequences* list, const char* event, size_t nth) {
	for (size_t i = 0; i < list->count; ++i) {
		if (list->items[i].instance && strcmp(list->items[i].event, event) == 0) {
			if (nth == 0)
				return &list->items[i];
			nth--;
		}
	}
	return NULL;
}

static size_t count_instance(const XwauMatSequences* list, const char* event) {
	size_t n = 0;
	for (size_t i = 0; i < list->count; ++i)
		n += list->items[i].instance && strcmp(list->items[i].event, event) == 0;
	return n;
}

/* The list an instance event comes from: the texture's own lines, else
 * [Default]'s (ATCListContainsEventType). */
static const XwauMatSequences* instance_source(const XwauMatSequences* own_list,
											   const XwauMatSequences* inherited, const char* event) {
	if (count_instance(own_list, event))
		return own_list;
	return count_instance(inherited, event) ? inherited : NULL;
}

/* Adds the material's `damage` to `object`; false when out of memory. */
static bool damage_json(XwauAnim* a, cJSON* object, const XwauMatSequences* own_list,
						const XwauMatSequences* inherited) {
	const XwauMatSequences* src[3];
	size_t                  sizes[3], alternatives = 0;
	for (int k = 0; k < 3; ++k) {
		src[k]   = instance_source(own_list, inherited, kDamageEvents[k]);
		sizes[k] = src[k] ? count_instance(src[k], kDamageEvents[k]) : 0;
		if (sizes[k] > alternatives)
			alternatives = sizes[k];
	}
	if (!alternatives)
		return true;
	const XwauSequence* first = NULL;
	for (int k = 0; k < 3 && !first; ++k)
		if (src[k])
			first = find_instance(src[k], kDamageEvents[k], 0);
	if (first->overlay != XWAU_SEQ_MULT) {
		a->skipped_unsupported++;
		return true;
	}
	const OpenDat* d = open_dat(a, first->dat);
	if (!d) {
		a->skipped_missing_dat++;
		return true;
	}
	if ((sizes[0] && sizes[0] != alternatives) || (sizes[1] && sizes[1] != alternatives) ||
		(sizes[2] && sizes[2] != alternatives) || !sizes[0] || !sizes[1] || !sizes[2])
		a->mixed_damage++;
	char library[64], rel[256];
	dat_names(d->rel, rel, sizeof rel, library, sizeof library);
	bool has_own_instance = false;
	for (size_t i = 0; i < own_list->count; ++i)
		has_own_instance |= own_list->items[i].instance;
	const int choice    = src[0] ? src[0]->section : (src[1] ? src[1]->section : src[2]->section);
	const int placement = has_own_instance ? own_list->section : 0;
	cJSON*    damage    = OptJson_AddObject(object, "damage");
	bool      ok        = OptJson_Add(damage, "library", cJSON_CreateString(library)) &&
						  OptJson_Add(damage, "choice", cJSON_CreateNumber(choice)) &&
						  OptJson_Add(damage, "placement", cJSON_CreateNumber(placement));
	cJSON*    list      = ok ? OptJson_AddArray(damage, "alternatives") : NULL;
	ok                  = list != NULL;
	for (size_t n = 0; ok && n < alternatives; ++n) {
		const XwauSequence* line[3];
		bool                any = false;
		for (int k = 0; k < 3; ++k) {
			line[k] = sizes[k] ? find_instance(src[k], kDamageEvents[k], n % sizes[k]) : NULL;
			if (line[k] &&
				(line[k]->overlay != XWAU_SEQ_MULT || line[k]->group_frames || !line[k]->frame_count ||
				 !open_dat(a, line[k]->dat) || open_dat(a, line[k]->dat) != d))
				line[k] = NULL;
		}
		cJSON* alternative = OptJson_AddObject(list, NULL);
		cJSON* stages      = OptJson_AddArray(alternative, "stages");
		ok                 = stages != NULL;
		for (int k = 0; ok && k < 3; ++k) {
			const XwauSequence* q = line[k];
			if (q && !image_is_white(d, q->frames[0].group, q->frames[0].image)) {
				char key[96];
				XwauFx_Key(key, sizeof key, q->frames[0].group, q->frames[0].image, XWAU_FX_MULTIPLY, q->tint,
						   q->alpha_mode);
				ok  = OptJson_Add(stages, NULL, cJSON_CreateString(key));
				any = true;
			} else {
				ok = OptJson_Add(stages, NULL, cJSON_CreateNull());
			}
		}
		/* Placement: the alternative's first stage line (XWAU keeps it per
		 * line; shipped alternatives repeat it across stages). */
		const XwauSequence* m = line[0] ? line[0] : (line[1] ? line[1] : line[2]);
		if (ok && any && m) {
			if (m->rand_scale)
				ok = OptJson_Add(alternative, "scale", OptJson_Floats(m->scale, 2));
			if (ok && m->loc == XWAU_SEQ_LOC_INSIDE) {
				ok = OptJson_Add(alternative, "offset", cJSON_CreateString("inside"));
			} else if (ok && m->loc == XWAU_SEQ_LOC_RANGE) {
				/* XWAU moves the square by (x1 - x0)·r - x0 per axis: the
				 * range min = -x0, max = x1 - 2·x0. */
				cJSON* offset = OptJson_AddObject(alternative, "offset");
				cJSON* min    = OptJson_AddArray(offset, "min");
				cJSON* max    = OptJson_AddArray(offset, "max");
				ok = OptJson_Add(min, NULL, shortest_float(-m->loc_range[0])) &&
					 OptJson_Add(min, NULL, shortest_float(-m->loc_range[1])) &&
					 OptJson_Add(max, NULL, shortest_float(m->loc_range[2] - 2.0f * m->loc_range[0])) &&
					 OptJson_Add(max, NULL, shortest_float(m->loc_range[3] - 2.0f * m->loc_range[1]));
			}
		}
	}
	if (!ok)
		return false;
	a->damage_materials++;
	return true;
}

bool XwauAnim_Texture(XwauAnim* a, const XwauMatFile* mat, const opt_texture_t* tex, float unlit,
					  XwauAnimScreen screen, XwauGltfAnim* out) {
	memset(out, 0, sizeof *out);
	if (!mat)
		return true;
	XwauMatSequences own_list, inherited;
	XwauMat_FindSequences(mat, tex->name, &own_list, &inherited);
	const bool has_illum = XwauGltf_TextureHasIllum(tex);

	/* Global events: per layer, the last line of each event
	 * (AssignTextureEvent overwrites). */
	const XwauSequence* chosen[2][EVENT_STATE_COUNT];
	memset(chosen, 0, sizeof chosen);
	bool energy[3] = { false, false, false };
	for (size_t i = 0; i < own_list.count; ++i) {
		const XwauSequence* q = &own_list.items[i];
		a->unknown_events += q->event_unknown;
		if (q->instance)
			continue;
		/* Frames sample the material's own UVs: a sequence that scales,
		   offsets or clamps them (none in XWAU's data) is not converted. */
		if (q->overlay != XWAU_SEQ_REPLACE || q->aspect != 1.0f || q->offset[0] != 0.0f ||
			q->offset[1] != 0.0f || q->clamp) {
			a->skipped_unsupported++;
			continue;
		}
		const int slot = state_slot(q->event);
		if (slot < 0) {
			a->skipped_unsupported++;
			continue;
		}
		chosen[q->layer][slot] = q;
	}
	/* Energy: the texture's own lines, else [Default]'s. */
	for (int k = 0; k < 3; ++k) {
		const XwauMatSequences* src = instance_source(&own_list, &inherited, kEnergyEvents[k]);
		if (!src)
			continue;
		const XwauSequence* q = find_instance(src, kEnergyEvents[k], 0);
		if (q->overlay == XWAU_SEQ_SCREEN)
			energy[k] = true;
		else
			a->skipped_unsupported++;
	}
	/* A Dynamic Cockpit screen: XWAU draws it with its screen shader
	   (PixelShaderDC.hlsl), not the animation shader. A colour frame takes
	   the cover's place; without a cover only cockpit damage shows, unlit,
	   in place of the display; a hologram's shader shows no frame. The
	   screen shader reads no illumination frame, hull damage or energy
	   effect. */
	if (screen != XWAU_ANIM_SCREEN_NONE) {
		for (size_t s = 0; s < EVENT_STATE_COUNT; ++s) {
			const bool keep =
				screen == XWAU_ANIM_SCREEN_COVER ||
				(screen == XWAU_ANIM_SCREEN_NO_COVER && strncmp(kEventStates[s].state, "broken.", 7) == 0);
			a->skipped_on_screens +=
				(chosen[XWAU_SEQ_ILLUMINATION][s] != NULL) + (!keep && chosen[XWAU_SEQ_COLOUR][s] != NULL);
			chosen[XWAU_SEQ_ILLUMINATION][s] = NULL;
			if (!keep)
				chosen[XWAU_SEQ_COLOUR][s] = NULL;
		}
		for (int k = 0; k < 3; ++k) {
			a->skipped_on_screens += energy[k];
			energy[k] = false;
		}
		if (screen == XWAU_ANIM_SCREEN_NO_COVER)
			unlit = 1.0f;
	}

	cJSON*          object       = cJSON_CreateObject();
	bool            ok           = object != NULL;
	bool            any_illum    = false;
	const Flipbook* always_illum = NULL;
	Flipbook        books[2][EVENT_STATE_COUNT];
	memset(books, 0, sizeof books);
	for (int layer = 0; layer < 2; ++layer) {
		for (size_t s = 0; s < EVENT_STATE_COUNT; ++s) {
			const XwauSequence* q = chosen[layer][s];
			if (!q)
				continue;
			if (layer == XWAU_SEQ_ILLUMINATION && !has_illum) {
				a->skipped_no_illum++;
				continue;
			}
			Flipbook* fb = &books[layer][s];
			fb->seq      = q;
			fb->dat      = open_dat(a, q->dat);
			if (!fb->dat) {
				a->skipped_missing_dat++;
				fb->seq = NULL;
				continue;
			}
			if (!sequence_frames(a, q, fb->dat, &fb->frames) || !randomize(a, tex->name, q, &fb->frames)) {
				cJSON_Delete(object);
				return false;
			}
			if (!fb->frames.count) {
				a->skipped_missing_dat++;
				fb->seq = NULL;
			}
		}
	}
	bool any_book = false;
	for (int layer = 0; layer < 2; ++layer)
		for (size_t s = 0; s < EVENT_STATE_COUNT; ++s)
			any_book |= books[layer][s].seq != NULL;
	cJSON* flipbooks = ok && any_book ? OptJson_AddArray(object, "flipbooks") : NULL;
	ok &= !any_book || flipbooks;
	if (ok && any_book) {
		for (int layer = 0; ok && layer < 2; ++layer) {
			const bool target = books[layer][state_slot("EVT_TARGET_SEL")].seq ||
								books[layer][state_slot("EVT_LASER_LOCKED")].seq ||
								books[layer][state_slot("EVT_WARHEAD_LOCKING")].seq ||
								books[layer][state_slot("EVT_WARHEAD_LOCKED")].seq;
			for (size_t s = 0; ok && s < EVENT_STATE_COUNT; ++s) {
				if (s == HANGAR_SLOT && target) {
					/* The hangar shows the material as it is. */
					cJSON* hangar = OptJson_AddObject(flipbooks, NULL);
					ok = OptJson_Add(hangar, "state", cJSON_CreateString("hangar")) &&
						 OptJson_Add(hangar, "layer", cJSON_CreateString(layer ? "emissive" : "baseColor")) &&
						 OptJson_Add(hangar, "mode", cJSON_CreateString("loop")) &&
						 OptJson_Add(hangar, "frames", cJSON_CreateArray());
					continue;
				}
				const Flipbook* fb = &books[layer][s];
				if (!fb->seq)
					continue;
				ok = flipbook_json(flipbooks, kEventStates[s].state, layer, fb, unlit);
				a->flipbooks++;
				if (layer == XWAU_SEQ_ILLUMINATION) {
					any_illum = true;
					if (s == EVENT_STATE_COUNT - 1)
						always_illum = fb;
				}
			}
		}
	}
	if (ok && screen == XWAU_ANIM_SCREEN_NONE)
		ok = damage_json(a, object, &own_list, &inherited);
	if (ok && (energy[0] || energy[1] || energy[2])) {
		cJSON* list = OptJson_AddArray(object, "energy");
		ok          = list != NULL;
		for (int k = 0; ok && k < 3; ++k)
			if (energy[k])
				ok = OptJson_Add(list, NULL, cJSON_CreateString(kEnergyNames[k]));
		a->energy_materials++;
	}
	if (!ok || !object->child) {
		cJSON_Delete(object);
		return ok;
	}
	char* json = OptJson_Print(object);
	if (!own(a, json))
		return false;
	out->json      = json;
	out->anim_base = any_illum;
	a->anim_bases += any_illum;
	a->any = true;
	if (always_illum && always_illum->frames.count) {
		out->always_illumination = true;
		a->always_illuminations++;
	}
	return true;
}

void XwauAnim_Report(const XwauAnim* a) {
	if (!a->any && !a->skipped_no_illum && !a->skipped_missing_dat && !a->skipped_unsupported &&
		!a->unknown_events && !a->missing_frames && !a->skipped_on_screens)
		return;
	Report(a->label,
		   "animations: %zu flipbook(s), %zu damage and %zu energy material(s), %zu with "
		   "illumination frames, %zu always lit",
		   a->flipbooks, a->damage_materials, a->energy_materials, a->anim_bases, a->always_illuminations);
	if (a->skipped_no_illum)
		Report(a->label, "%zu illumination sequence(s) on textures without illumination skipped",
			   a->skipped_no_illum);
	if (a->skipped_missing_dat)
		Report(a->label, "%zu sequence(s) with a missing DAT or image skipped", a->skipped_missing_dat);
	if (a->missing_frames)
		Report(a->label, "%zu frame(s) whose image their DAT lacks left out", a->missing_frames);
	if (a->skipped_on_screens)
		Report(a->label,
			   "%zu sequence(s) and energy effect(s) on Dynamic Cockpit screens left out "
			   "(XWAU's screen shader shows none)",
			   a->skipped_on_screens);
	if (a->skipped_unsupported)
		Report(a->label, "%zu sequence(s) with an unsupported event, layer or UV mapping skipped",
			   a->skipped_unsupported);
	if (a->unknown_events)
		Report(a->label, "%zu sequence(s) with an unknown event (played as EVT_NONE)", a->unknown_events);
	if (a->mixed_damage)
		Report(a->label, "%zu material(s) with uneven damage stage lists", a->mixed_damage);
}
