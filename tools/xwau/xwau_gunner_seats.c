#include "xwau_gunner_seats.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "opt.h"
#include "util.h"

/* The stock gunner cockpit, which object type 317 loads. */
#define STOCK_COCKPIT "CorellianTransportGunner"

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
		if (i != 7 && !Xwau_StoiAutoInt(tokens.items[i], &value[i])) {
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
	size_t extra = 0, other = 0;
	for (size_t i = 0; i < lines.count; ++i) {
		XwauGunnerSeat seat;
		XwauLines      tokens = { 0 };
		const bool short_line = Xwau_Tokenize(lines.items[i], XWAU_RULES_CPP, &tokens) && tokens.count < 9;
		XwauLines_Free(&tokens);
		if (short_line) {
			/* The hook reads only the TurretForwardFire key from such a line. */
			const char* eq = strchr(lines.items[i], '=');
			char        name[64];
			name[0] = '\0';
			if (eq && (size_t)(eq - lines.items[i]) < sizeof name) {
				snprintf(name, sizeof name, "%.*s", (int)(eq - lines.items[i]), lines.items[i]);
				Xwau_Trim(name, XWAU_RULES_CPP);
			}
			if (!Xwau_SameName(name, "TurretForwardFire"))
				++other;
			continue;
		}
		if (!read_seat(game, source, lines.items[i], &seat))
			continue;
		if (out->count == XWAU_GUNNER_SEAT_MAX)
			++extra;
		else
			out->seats[out->count++] = seat;
	}
	if (other)
		Report(source,
			   "[Turrets]: %zu lines have fewer than 9 values and aren't TurretForwardFire. The hook doesn't "
			   "read them, so they are left out.",
			   other);
	if (extra)
		ReportFatal(source, "[Turrets]: the section has %zu seats, but the game has room for only %d",
					out->count + extra, XWAU_GUNNER_SEAT_MAX);
	char      value[64];
	long long number;
	XwauLines_Value(&lines, "TurretForwardFire", XWAU_RULES_CPP, value, sizeof value);
	if (value[0] && Xwau_StoiAutoInt(value, &number))
		out->forward_fire = number != 0;
	else if (value[0])
		Report(source,
			   "[Turrets]: TurretForwardFire = %s isn't a number, which would crash XWAU. It is left out.",
			   value);
	XwauLines_Free(&lines);
}

void XwauGunnerSeats_WriteAimLimit(FILE* f, const char* label, int steps) {
	if ((int16_t)steps < 0) {
		ReportFatal(label, "a seat's aim limit is %d, which the game reads as a negative angle",
					(uint16_t)steps);
		fputs("180", f);
		return;
	}
	Xwau_WriteDegrees(f, (int16_t)steps);
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
		fputs("    - {position_m: ", f);
		Xwau_WriteMetres3(f, seat->x, seat->y, seat->z);
		fputs(", mount_pitch_degrees: ", f);
		Xwau_WriteDegrees(f, seat->mount_angle_a);
		fputs(", mount_yaw_degrees: ", f);
		Xwau_WriteDegrees(f, seat->mount_angle_b);
		fputs(", aim_limit_pitch_degrees: ", f);
		XwauGunnerSeats_WriteAimLimit(f, key, seat->aim_limit_a);
		fputs(", aim_limit_yaw_degrees: ", f);
		XwauGunnerSeats_WriteAimLimit(f, key, seat->aim_limit_b);
		fprintf(f, ", cockpit: %s, inverted: %s}\n", key, seat->inverted ? "true" : "false");
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

void XwauGunnerSeats_CheckCockpits(const char* game, const char* output_dir, const XwauLines* cockpits) {
	size_t checked = 0;
	for (size_t i = 0; i < cockpits->count; ++i) {
		const char* key = cockpits->items[i];
		char        glb[1300];
		if (strcasecmp(key, STOCK_COCKPIT) == 0)
			continue;
		check_cockpit(game, key);
		if (PathFormat(glb, sizeof glb, "%s/models/%s.glb", output_dir, key) && !IsFile(glb))
			Report(glb, "doesn't exist, so the game can't load this gunner cockpit until it is converted");
		checked++;
	}
	Report(NULL, "gunner seats use %zu cockpits besides the stock one", checked);
}
