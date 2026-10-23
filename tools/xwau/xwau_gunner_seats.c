#include "xwau_gunner_seats.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "opt.h"
#include "util.h"

/* The stock gunner cockpit, which object type 317 loads. */
#define STOCK_COCKPIT "CorellianTransportGunner"
#define STOCK_COCKPIT_TYPE 317

/* The offsets of the seat fields in a craft record, for the first seat. The
   second seat's field follows each one. */
enum {
	RECORD_SEAT_Y           = 574,
	RECORD_SEAT_Z           = 578,
	RECORD_SEAT_X           = 582,
	RECORD_SEAT_MOUNT_B     = 586,
	RECORD_SEAT_MOUNT_A     = 590,
	RECORD_SEAT_OBJECT_TYPE = 594,
	RECORD_SEAT_AIM_A       = 598,
	RECORD_SEAT_AIM_B       = 602,
};

/* The reserved object types that have an empty craft record of their own. */
static const int g_reservedSlots[] = { 33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  49,  66,  67,  68,
									   69,  70,  86,  87,  88,  89,  90,  116, 117, 118, 119, 120, 121, 122,
									   123, 124, 125, 151, 152, 184, 185, 186, 187, 188, 189, 190, 191 };

static int16_t record_i16(const uint8_t* record, int offset, int seat) {
	return (int16_t)(record[offset + 2 * seat] | record[offset + 2 * seat + 1] << 8);
}

/* Whether the installation has FlightModels/<name>.opt. */
static bool has_opt(const char* game, const char* name) {
	char rel[400], path[1024];
	snprintf(rel, sizeof rel, "FlightModels/%s.opt", name);
	return Xwau_FindFile(game, rel, path, sizeof path);
}

/* Sets the cockpit of `seat` to `name`, or to the stock cockpit when the
   installation doesn't have its OPT. */
static void set_cockpit(const char* game, const char* source, XwauGunnerSeat* seat, const char* name) {
	if (!has_opt(game, name)) {
		Report(source, "[Turrets]: FlightModels/%s.opt doesn't exist; XWAU loads " STOCK_COCKPIT " instead",
			   name);
		name = STOCK_COCKPIT;
	}
	snprintf(seat->cockpit, sizeof seat->cockpit, "%s", name);
}

/* Reads the seat of `line` into `seat`. Returns false when the line isn't a
   seat, or when its numbers can't be read, which is reported. */
static bool read_seat(const char* game, const char* source, const char* line, XwauGunnerSeat* seat) {
	XwauLines tokens = { 0 };
	long long value[9];
	bool      ok = Xwau_Tokenize(line, XWAU_RULES_CPP, &tokens) && tokens.count >= 9;
	for (int i = 0; ok && i < 9; ++i) {
		if (i != 7 && !Xwau_StoiAuto(tokens.items[i], &value[i])) {
			Report(source,
				   "[Turrets]: \"%s\" has values that aren't numbers, which would crash XWAU; the line "
				   "is left out",
				   line);
			ok = false;
		}
	}
	if (ok) {
		memset(seat, 0, sizeof *seat);
		seat->y             = (int16_t)value[0];
		seat->x             = (int16_t)value[1];
		seat->z             = (int16_t)value[2];
		seat->mount_angle_b = (int16_t)value[3];
		seat->mount_angle_a = (int16_t)value[4];
		seat->aim_limit_a   = (int16_t)value[5];
		seat->aim_limit_b   = (int16_t)value[6];
		seat->inverted      = value[8] != 0;
		set_cockpit(game, source, seat, tokens.items[7]);
	}
	XwauLines_Free(&tokens);
	return ok;
}

/* Builds the seats of `model` from the record of the first object type that
   loads it, as the hook does without a section. */
static void record_seats(const char* game, const XwauGameTables* tables, const char* model,
						 XwauGunnerSeats* out) {
	char key[256];
	LowerCopy(key, sizeof key, model);
	for (int t = 0; tables && t < XWAU_GAME_OBJECT_TYPES; ++t) {
		const XwauGameType type = XwauGameTables_Type(tables, t);
		if (!XwauGameType_HasRow(&type) || type.record < 0 || type.record >= XWAU_GAME_RECORDS ||
			strcmp(XwauGameTables_Model(tables, t), key) != 0)
			continue;
		const uint8_t* record = tables->records[type.record];
		for (int s = 0; s < XWAU_GUNNER_SEAT_MAX; ++s) {
			if (record_i16(record, RECORD_SEAT_OBJECT_TYPE, s) == 0)
				continue;
			XwauGunnerSeat* seat = &out->seats[out->count++];
			char            name[300];
			seat->x             = record_i16(record, RECORD_SEAT_X, s);
			seat->y             = record_i16(record, RECORD_SEAT_Y, s);
			seat->z             = record_i16(record, RECORD_SEAT_Z, s);
			seat->mount_angle_a = record_i16(record, RECORD_SEAT_MOUNT_A, s);
			seat->mount_angle_b = record_i16(record, RECORD_SEAT_MOUNT_B, s);
			seat->aim_limit_a   = record_i16(record, RECORD_SEAT_AIM_A, s);
			seat->aim_limit_b   = record_i16(record, RECORD_SEAT_AIM_B, s);
			seat->inverted      = s == 1;
			snprintf(name, sizeof name, "%sGunner%s", model, s == 1 ? "2" : "");
			if (s == 1 && !has_opt(game, name))
				snprintf(name, sizeof name, "%sGunner", model);
			snprintf(seat->cockpit, sizeof seat->cockpit, "%s", has_opt(game, name) ? name : STOCK_COCKPIT);
		}
		out->has = out->count != 0;
		return;
	}
}

void XwauGunnerSeats_Read(const char* game, const XwauGameTables* tables, const char* model,
						  const char* turrets_txt, const char* ini, XwauGunnerSeats* out) {
	XwauLines   lines  = { 0 };
	const char* source = turrets_txt;
	memset(out, 0, sizeof *out);
	out->forward_fire = true;
	if (turrets_txt)
		XwauLines_Read(turrets_txt, "", XWAU_RULES_CPP, &lines);
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		source = ini;
		if (ini)
			XwauLines_Read(ini, "Turrets", XWAU_RULES_CPP, &lines);
	}
	if (lines.count == 0) {
		XwauLines_Free(&lines);
		record_seats(game, tables, model, out);
		return;
	}
	out->has     = true;
	size_t extra = 0;
	for (size_t i = 0; i < lines.count; ++i) {
		XwauGunnerSeat seat;
		if (!read_seat(game, source, lines.items[i], &seat))
			continue;
		if (out->count == XWAU_GUNNER_SEAT_MAX)
			++extra;
		else
			out->seats[out->count++] = seat;
	}
	if (extra)
		Report(source, "[Turrets]: the game has room for %d seats; %zu more are left out",
			   XWAU_GUNNER_SEAT_MAX, extra);
	char      value[64];
	long long number;
	XwauLines_Value(&lines, "TurretForwardFire", XWAU_RULES_CPP, value, sizeof value);
	if (value[0] && Xwau_StoiAuto(value, &number))
		out->forward_fire = number != 0;
	XwauLines_Free(&lines);
}

bool XwauGunnerSeats_Write(FILE* f, const XwauGunnerSeats* seats, XwauLines* cockpits) {
	if (!seats->has)
		return true;
	fputs("gunner_seats:\n", f);
	if (!seats->forward_fire)
		fputs("  forward_fire: false\n", f);
	fputs(seats->count ? "  seats:\n" : "  seats: []\n", f);
	for (size_t i = 0; i < seats->count; ++i) {
		const XwauGunnerSeat* seat = &seats->seats[i];
		char                  key[256];
		LowerCopy(key, sizeof key, seat->cockpit);
		fprintf(f,
				"    - {x: %d, y: %d, z: %d, mount_angle_a: %d, mount_angle_b: %d, aim_limit_a: %d, "
				"aim_limit_b: %d, cockpit: %s, inverted: %s}\n",
				seat->x, seat->y, seat->z, seat->mount_angle_a, seat->mount_angle_b, seat->aim_limit_a,
				seat->aim_limit_b, key, seat->inverted ? "true" : "false");
		bool seen = !cockpits;
		for (size_t k = 0; !seen && k < cockpits->count; ++k)
			seen = strcmp(cockpits->items[k], key) == 0;
		if (!seen && !XwauLines_Add(cockpits, key, strlen(key)))
			return false;
	}
	return true;
}

/* Reports the cockpit `name` when its OPT doesn't have exactly 2 laser
   hardpoints: the game takes the seat's shots from the first two. */
static void check_cockpit(const char* game, const char* name) {
	char rel[400], path[1024];
	snprintf(rel, sizeof rel, "FlightModels/%s.opt", name);
	if (!Xwau_FindFile(game, rel, path, sizeof path))
		return;
	opt_error_t err;
	opt_file_t* opt = opt_load_file(path, &err);
	if (!opt)
		return;
	int lasers = 0;
	for (int32_t m = 0; m < opt->mesh_count; ++m)
		for (int32_t h = 0; h < opt->meshes[m].hardpoint_count; ++h) {
			const int type = (int)opt->meshes[m].hardpoints[h].type;
			lasers += (type >= 1 && type <= 4) || type == 9 || type == 10;
		}
	opt_free(opt);
	if (lasers != 2)
		Report(rel,
			   "has %d laser hardpoints; a gunner cockpit needs 2, and the game fires from the first two",
			   lasers);
}

bool XwauGunnerSeats_WriteCockpits(const char* game, const char* stock_game, const char* output_dir,
								   const XwauLines* cockpits) {
	if (!cockpits->count)
		return true;
	XwauGameTables* stock = XwauGameTables_Load(stock_game);
	XwauGameTables* mod   = stock ? XwauGameTables_Load(game) : NULL;
	if (!stock || !mod) {
		XwauGameTables_Free(stock);
		XwauGameTables_Free(mod);
		return false;
	}
	char   dir[1100];
	bool   ok   = PathFormat(dir, sizeof dir, "%s/object_types", output_dir) && MakeDirs(dir);
	size_t slot = 0, written = 0, dropped = 0;
	for (size_t i = 0; ok && i < cockpits->count; ++i) {
		const char* key = cockpits->items[i];
		if (strcasecmp(key, STOCK_COCKPIT) == 0)
			continue;
		check_cockpit(game, key);
		/* The next reserved slot that loads no model in the mod, and whose
		   record no type that loads a model uses. A mod can give a slot an
		   empty row. */
		int t = -1;
		for (; t < 0 && slot < sizeof g_reservedSlots / sizeof g_reservedSlots[0]; ++slot) {
			const int          c = g_reservedSlots[slot];
			const XwauGameType s = XwauGameTables_Type(stock, c);
			bool used = XwauGameTables_Model(mod, c)[0] || s.record < 0 || s.record >= XWAU_GAME_RECORDS;
			for (int o = 0; !used && o < XWAU_GAME_OBJECT_TYPES; ++o) {
				const XwauGameType other = XwauGameTables_Type(mod, o);
				used = o != c && XwauGameTables_Model(mod, o)[0] && other.record == s.record;
			}
			if (!used)
				t = c;
		}
		if (t < 0) {
			Report(key,
				   "no reserved object type is left for this gunner cockpit; its seats show " STOCK_COCKPIT);
			++dropped;
			continue;
		}
		char path[1300], glb[1300];
		ok = PathFormat(path, sizeof path, "%s/%s.yaml", dir, key);
		if (ok && IsFile(path))
			ok = PathFormat(path, sizeof path, "%s/%s_%d.yaml", dir, key, t);
		FILE* f = ok ? fopen(path, "wb") : NULL;
		if (!f) {
			Report(path, "can't be written");
			ok = false;
			break;
		}
		fprintf(f,
				"# Written by xwau_import craft: the cockpit of a gunner seat.\nversion: 1\nobject_type: %d\n"
				"model: %s\nlist: equipment\ngenus: 2\nfamily: 6\nflags: 0x0\ngunner_cockpit: true\n",
				t, key);
		ok = fclose(f) == 0;
		written += ok;
		if (ok && PathFormat(glb, sizeof glb, "%s/models/%s.glb", output_dir, key) && !IsFile(glb))
			Report(glb, "doesn't exist, so the game can't load this gunner cockpit until it is converted");
	}
	XwauGameTables_Free(stock);
	XwauGameTables_Free(mod);
	if (ok)
		Report(NULL, "gunner seats: %zu cockpits get an object type of their own", written);
	if (ok && dropped)
		Report(NULL, "gunner seats: %zu cockpits have no free object type", dropped);
	return ok;
}
