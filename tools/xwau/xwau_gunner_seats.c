#include "xwau_gunner_seats.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
   installation doesn't have its OPT, as XWAU does. */
static void set_cockpit(const char* game, XwauGunnerSeat* seat, const char* name) {
	if (!has_opt(game, name))
		name = STOCK_COCKPIT;
	snprintf(seat->cockpit, sizeof seat->cockpit, "%s", name);
}

/* Reads the seat of `line` into `seat`. Returns false when the line isn't a
   seat, or when its numbers can't be read, which would crash XWAU. */
static bool read_seat(const char* game, const char* line, XwauGunnerSeat* seat) {
	XwauLines tokens = { 0 };
	long long value[9];
	bool      ok = Xwau_Tokenize(line, XWAU_RULES_CPP, &tokens) && tokens.count >= 9;
	for (int i = 0; ok && i < 9; ++i) {
		if (i != 7 && !Xwau_StoiAutoInt(tokens.items[i], &value[i]))
			ok = false;
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
		set_cockpit(game, seat, tokens.items[7]);
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
	XwauSection section;
	memset(out, 0, sizeof *out);
	out->forward_fire = true;
	XwauSection_ReadPaths(turrets_txt, ini, "Turrets", XWAU_RULES_CPP, &section);
	const XwauLines* lines = &section.lines;
	if (lines->count == 0) {
		XwauSection_Free(&section);
		record_seats(game, tables, model, out);
		return;
	}
	out->has     = true;
	size_t extra = 0;
	for (size_t i = 0; i < lines->count; ++i) {
		XwauGunnerSeat seat;
		XwauLines      tokens = { 0 };
		const bool short_line = Xwau_Tokenize(lines->items[i], XWAU_RULES_CPP, &tokens) && tokens.count < 9;
		XwauLines_Free(&tokens);
		/* The hook reads only the TurretForwardFire key from such a line. */
		if (short_line)
			continue;
		if (!read_seat(game, lines->items[i], &seat))
			continue;
		if (out->count == XWAU_GUNNER_SEAT_MAX)
			++extra;
		else
			out->seats[out->count++] = seat;
	}
	if (extra)
		ReportFatal(section.source, "the section has %zu seats, but the game has room for only %d",
					out->count + extra, XWAU_GUNNER_SEAT_MAX);
	long long number;
	if (XwauSection_Int(&section, "TurretForwardFire", &number) == XWAU_VALUE_SET)
		out->forward_fire = number != 0;
	XwauSection_Free(&section);
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

void XwauGunnerSeats_CheckCockpits(const char* game, const char* output_dir, const XwauLines* cockpits) {
	size_t checked = 0;
	for (size_t i = 0; i < cockpits->count; ++i) {
		const char* key = cockpits->items[i];
		char        glb[1300];
		if (strcasecmp(key, STOCK_COCKPIT) == 0)
			continue;
		if (PathFormat(glb, sizeof glb, "%s/models/%s.glb", output_dir, key) && !IsFile(glb))
			Report(glb, "doesn't exist, so the game can't load this gunner cockpit until it is converted");
		checked++;
	}
	ReportDetail(NULL, "gunner seats use %zu cockpits besides the stock one", checked);
}
