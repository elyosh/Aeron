#include "xwau_objects.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"
#include "xwau_game_tables.h"
#include "xwau_gunner_seats.h"

enum {
	OBJECT_TYPE_COUNT  = XWAU_GAME_OBJECT_TYPES,
	OBJECT_TYPE_SIZE   = XWAU_GAME_OBJECT_TYPE_SIZE,
	CRAFT_TYPE_COUNT   = XWAU_GAME_CRAFT_TYPES,
	RECORD_COUNT       = XWAU_GAME_RECORDS,
	SOUND_SWITCH_COUNT = XWAU_GAME_SOUND_SWITCHES,
	SOUND_CASE_COUNT   = XWAU_GAME_SOUND_CASES,
	SPECIES_COUNT      = 221,
	SPECIES_WITH_OWN_RECORD =
		218, /* species records 0 to 217 belong to the craft record of the same number */
};

/* The craft whose beam the stock test takes away: the X-wing, Y-wing,
   A-wing, B-wing, TIE Fighter and Z-95. */
static const int g_beamTestTypes[] = { 1, 2, 3, 4, 5, 14 };

/* The stock placeholders that load the X-wing's row, which mods can fill
   with new craft, as TFTC does. */
static const int g_placeholders[] = { 103, 104, 108, 109, 112, 113, 114 };

static uint32_t read_u32(const uint8_t* p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t read_u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool valid_record(int record) { return record >= 0 && record < RECORD_COUNT; }

/* ---- Craft record fields -------------------------------------------------- */

typedef enum FieldKind {
	FIELD_UNSIGNED,
	FIELD_SIGNED,
	FIELD_BOOL,
	FIELD_WEAPON,     /* written as a weapon type name; 0 is none */
	FIELD_LASER_KIND, /* written as a laser group kind name */
} FieldKind;

/* A field of the record. A number with a unit is written in that unit. */
typedef struct Field {
	const char* key;
	unsigned    offset, size;
	FieldKind   kind;
	XwauUnit    unit;
} Field;

/* The single-value keys of `craft_record`, at their offsets in the packed
   987-byte record (model_def.h). */
static const Field g_fields[] = {
	{ "speed_mglt", 32, 2, FIELD_UNSIGNED, XWAU_UNIT_MGLT },
	{ "acceleration_mglt", 34, 2, FIELD_UNSIGNED, XWAU_UNIT_MGLT },
	{ "deceleration_mglt", 36, 2, FIELD_UNSIGNED, XWAU_UNIT_MGLT },
	{ "yaw_rate_degrees", 38, 2, FIELD_SIGNED, XWAU_UNIT_DEGREES },
	{ "roll_rate_degrees", 42, 2, FIELD_SIGNED, XWAU_UNIT_DEGREES },
	{ "pitch_rate_degrees", 44, 2, FIELD_SIGNED, XWAU_UNIT_DEGREES },
	{ "auto_bank_percent", 40, 2, FIELD_UNSIGNED, XWAU_UNIT_PERCENT_Q16 },
	{ "max_tumble_angle_degrees", 46, 2, FIELD_UNSIGNED, XWAU_UNIT_DEGREES },
	{ "max_push_rate", 48, 2, FIELD_UNSIGNED, XWAU_UNIT_NONE },
	{ "hull", 21, 4, FIELD_SIGNED, XWAU_UNIT_NONE },
	{ "shields", 15, 4, FIELD_SIGNED, XWAU_UNIT_NONE },
	{ "has_shields", 14, 1, FIELD_BOOL, XWAU_UNIT_NONE },
	{ "has_hyperdrive", 12, 1, FIELD_BOOL, XWAU_UNIT_NONE },
	{ "system_strength", 29, 2, FIELD_UNSIGNED, XWAU_UNIT_NONE },
	{ "system_damage_hull_threshold", 25, 4, FIELD_SIGNED, XWAU_UNIT_NONE },
	{ "component_max_hp", 31, 1, FIELD_UNSIGNED, XWAU_UNIT_NONE },
	{ "rating_weight", 10, 2, FIELD_UNSIGNED, XWAU_UNIT_NONE },
	{ "laser_converge_mode", 13, 1, FIELD_UNSIGNED, XWAU_UNIT_NONE },
	{ "countermeasures", 563, 1, FIELD_UNSIGNED, XWAU_UNIT_NONE },
	{ "gunsight_offset_y", 564, 4, FIELD_SIGNED, XWAU_UNIT_NONE },
	{ "ai_hits_to_evade", 19, 1, FIELD_UNSIGNED, XWAU_UNIT_NONE },
};

/* The keys of a `lasers` entry: laserGroupWeaponType, laserGroupSlotCount,
   laserGroupWeaponKind, laserGroupFireRange, laserGroupFireCooldownTicks. */
static const Field g_laserFields[] = {
	{ "type", 306, 2, FIELD_WEAPON, XWAU_UNIT_NONE },
	{ "count", 318, 1, FIELD_UNSIGNED, XWAU_UNIT_NONE },
	{ "kind", 321, 1, FIELD_LASER_KIND, XWAU_UNIT_NONE },
	{ "range_m", 324, 4, FIELD_SIGNED, XWAU_UNIT_METRES },
	{ "cooldown_ms", 336, 2, FIELD_UNSIGNED, XWAU_UNIT_MILLISECONDS },
};

/* The keys of a `warheads` entry: warheadLauncherType, warheadLauncherSlotCount,
   warheadLauncherCapacity. */
static const Field g_warheadFields[] = {
	{ "type", 342, 2, FIELD_WEAPON, XWAU_UNIT_NONE },
	{ "count", 350, 1, FIELD_UNSIGNED, XWAU_UNIT_NONE },
	{ "capacity", 352, 1, FIELD_UNSIGNED, XWAU_UNIT_NONE },
};

enum {
	TURRET_X,
	TURRET_Y,
	TURRET_Z,
	TURRET_PITCH,
	TURRET_YAW,
	TURRET_OBJECT_TYPE,
	TURRET_AIM_PITCH,
	TURRET_AIM_YAW
};

/* The fields of a turret seat, for each of the two seats. The object type is
   the type of the seat's cockpit model. A `turrets` entry writes them with
   the gunner seat keys, so the names here are the record's field names. */
static const Field g_turretFields[] = {
	[TURRET_X]           = { "turretSeatPosX", 582, 2, FIELD_SIGNED, XWAU_UNIT_NONE },
	[TURRET_Y]           = { "turretSeatPosY", 574, 2, FIELD_SIGNED, XWAU_UNIT_NONE },
	[TURRET_Z]           = { "turretSeatPosZ", 578, 2, FIELD_SIGNED, XWAU_UNIT_NONE },
	[TURRET_PITCH]       = { "turretMountAngleA", 590, 2, FIELD_SIGNED, XWAU_UNIT_NONE },
	[TURRET_YAW]         = { "turretMountAngleB", 586, 2, FIELD_SIGNED, XWAU_UNIT_NONE },
	[TURRET_OBJECT_TYPE] = { "turretObjectType", 594, 2, FIELD_UNSIGNED, XWAU_UNIT_NONE },
	[TURRET_AIM_PITCH]   = { "turretAimLimitA", 598, 2, FIELD_SIGNED, XWAU_UNIT_NONE },
	[TURRET_AIM_YAW]     = { "turretAimLimitB", 602, 2, FIELD_SIGNED, XWAU_UNIT_NONE },
};

#define NAME_OFFSET 50
#define NAME_SIZE 256
#define EYE_X 572
#define EYE_Y 568
#define EYE_Z 570
#define MESH_ATTACH 606
#define MESH_ATTACH_COUNT 11

static long long field_value(const uint8_t* record, const Field* field, unsigned element) {
	const uint8_t* p     = record + field->offset + element * field->size;
	uint32_t       value = 0;
	for (unsigned i = 0; i < field->size; ++i)
		value |= (uint32_t)p[i] << (8 * i);
	if (field->kind == FIELD_SIGNED && field->size < 4 && (value & (1u << (8 * field->size - 1))))
		value |= ~0u << (8 * field->size);
	return field->kind == FIELD_SIGNED ? (long long)(int32_t)value : (long long)value;
}

static bool field_differs(const uint8_t* a, const uint8_t* b, const Field* field, unsigned element) {
	return field_value(a, field, element) != field_value(b, field, element);
}

/* Writes field `field` of `record`. Problems are reported with `path`, the
   object type file. */
static void write_field(FILE* f, const char* path, const XwauNames* names, const uint8_t* record,
						const Field* field, unsigned element) {
	long long value = field_value(record, field, element);
	/* The game treats the turret kinds above 6 like 6, slow_turret. */
	if (field->kind == FIELD_LASER_KIND && value > 6)
		value = 6;
	const char* name = field->kind == FIELD_WEAPON       ? XwauNames_WeaponType(names, (int)value)
					   : field->kind == FIELD_LASER_KIND ? XwauNames_Name(&names->laser_kinds, (int)value)
														 : NULL;
	if (field->kind == FIELD_BOOL && (value == 0 || value == 1))
		fputs(value ? "true" : "false", f);
	else if (field->kind == FIELD_BOOL) {
		ReportFatal(path, "%s is %lld, but the game only accepts true or false", field->key, value);
		fprintf(f, "%lld", value);
	} else if (field->kind == FIELD_WEAPON && value == 0)
		fputs("none", f);
	else if (name)
		fputs(name, f);
	else if (field->kind == FIELD_WEAPON || field->kind == FIELD_LASER_KIND) {
		ReportFatal(path, "the game has no name for %s %lld", field->key, value);
		fputs("none", f);
	} else
		Xwau_WriteUnit(f, field->unit, value);
}

static bool group_differs(const uint8_t* a, const uint8_t* b, const Field* fields, size_t count,
						  unsigned groups) {
	for (unsigned g = 0; g < groups; ++g)
		for (size_t i = 0; i < count; ++i)
			if (field_differs(a, b, &fields[i], g))
				return true;
	return false;
}

/* Returns the index of the last non-empty group in `groups`, plus 1. */
static unsigned used_groups(const uint8_t* record, const Field* fields, size_t count, unsigned groups) {
	unsigned last = 0;
	for (unsigned g = 0; g < groups; ++g)
		for (size_t i = 0; i < count; ++i)
			if (field_value(record, &fields[i], g) != 0)
				last = g + 1;
	return last;
}

static void write_groups(FILE* f, const char* path, const XwauNames* names, const char* key,
						 const uint8_t* record, const Field* fields, size_t count, unsigned groups) {
	const unsigned last = used_groups(record, fields, count, groups);
	if (last == 0) {
		fprintf(f, "  %s: []\n", key);
		return;
	}
	fprintf(f, "  %s:\n", key);
	for (unsigned g = 0; g < last; ++g) {
		fputs("    - {", f);
		for (size_t i = 0; i < count; ++i) {
			fprintf(f, "%s%s: ", i ? ", " : "", fields[i].key);
			write_field(f, path, names, record, &fields[i], g);
		}
		fputs("}\n", f);
	}
}

/* Writes the turret seats with the gunner seat keys: the position in metres
   and the angles in degrees. */
static void write_turrets(FILE* f, const XwauNames* names, int object_type, const uint8_t* record) {
	const unsigned last = used_groups(record, g_turretFields, 8, 2);
	if (last == 0) {
		fputs("  turrets: []\n", f);
		return;
	}
	fputs("  turrets:\n", f);
	for (unsigned g = 0; g < last; ++g) {
		long long v[8];
		for (int i = 0; i < 8; ++i)
			v[i] = field_value(record, &g_turretFields[i], g);
		const char* cockpit = XwauNames_ObjectType(names, (int)v[TURRET_OBJECT_TYPE]);
		char        label[128];
		snprintf(label, sizeof label, "object type %s, turret %u", XwauNames_ObjectType(names, object_type),
				 g + 1);
		fputs("    - {position_m: ", f);
		Xwau_WriteMetres3(f, (int)v[TURRET_X], (int)v[TURRET_Y], (int)v[TURRET_Z]);
		fputs(", mount_pitch_degrees: ", f);
		Xwau_WriteDegrees(f, (int)v[TURRET_PITCH]);
		fputs(", mount_yaw_degrees: ", f);
		Xwau_WriteDegrees(f, (int)v[TURRET_YAW]);
		fputs(", aim_limit_pitch_degrees: ", f);
		XwauGunnerSeats_WriteAimLimit(f, label, (int)v[TURRET_AIM_PITCH]);
		fputs(", aim_limit_yaw_degrees: ", f);
		XwauGunnerSeats_WriteAimLimit(f, label, (int)v[TURRET_AIM_YAW]);
		if (cockpit)
			fprintf(f, ", object_type: %s", cockpit);
		else if (v[TURRET_OBJECT_TYPE] != 0)
			ReportFatal(NULL, "%s has cockpit object type %lld, which the game has no name for", label,
						v[TURRET_OBJECT_TYPE]);
		fputs("}\n", f);
	}
}

static int16_t record_i16(const uint8_t* record, unsigned offset) {
	return (int16_t)read_u16(record + offset);
}

static bool eye_differs(const uint8_t* a, const uint8_t* b) { return memcmp(a + EYE_Y, b + EYE_Y, 6) != 0; }

static bool name_differs(const uint8_t* a, const uint8_t* b) {
	return strncmp((const char*)a + NAME_OFFSET, (const char*)b + NAME_OFFSET, NAME_SIZE) != 0;
}

static bool mesh_attach_differs(const uint8_t* a, const uint8_t* b) {
	return memcmp(a + MESH_ATTACH, b + MESH_ATTACH, 4 * MESH_ATTACH_COUNT) != 0;
}

static bool record_differs(const uint8_t* stock, const uint8_t* mod) {
	for (size_t i = 0; i < sizeof g_fields / sizeof g_fields[0]; ++i)
		if (field_differs(stock, mod, &g_fields[i], 0))
			return true;
	return group_differs(stock, mod, g_laserFields, 5, 3) ||
		   group_differs(stock, mod, g_warheadFields, 3, 2) ||
		   group_differs(stock, mod, g_turretFields, 8, 2) || eye_differs(stock, mod) ||
		   name_differs(stock, mod) || mesh_attach_differs(stock, mod);
}

/* ---- [ExeCraftValues] ----------------------------------------------------- */

/* hook_mission_tie changes object types 0 to 218 whose craft record is below
   218. */
enum { EXE_VALUES_TYPES = 219, EXE_VALUES_RECORDS = 218 };

/* The object type flags that the section sets. */
#define FLAG_HARDPOINTS_MIRRORING 0x800
#define FLAG_AXIS_ALIGNED 0x1000

bool XwauObjects_HasExeCraftValues(const char* game, const char* model) {
	XwauSection section;
	const bool  has =
		XwauSection_ReadModel(game, model, "ExeCraftValues", XWAU_RULES_CPP, &section) && section.lines.count;
	XwauSection_Free(&section);
	return has;
}

/* Writes `value` into a field, cut to its size as the hook's casts do. */
static void set_field(uint8_t* record, const Field* field, unsigned element, long long value) {
	uint8_t* p = record + field->offset + element * field->size;
	for (unsigned i = 0; i < field->size; ++i)
		p[i] = (uint8_t)((unsigned long long)value >> (8 * i));
}

/* The model of object type `t` as hook_mission_tie finds it: its list row,
   without the test of recordFlags. */
static const char* exe_values_model(const XwauGameTables* tables, int t) {
	const XwauGameType type = XwauGameTables_Type(tables, t);
	return type.list < 2 && type.row < tables->rowCount[type.list] ? tables->rows[type.list][type.row] : "";
}

/* Applies the [ExeCraftValues] sections of the models to `tables`, as
   hook_mission_tie does at each mission load. When two object types share a
   craft record, the values of the later one win, as in XWAU. Writes the
   number of object types changed to `count`. Returns false when out of
   memory. */
static bool apply_exe_values(const char* game, XwauGameTables* tables, int* count) {
	*count = 0;
	/* The lasers and warheads keep the start-up record's values; the other
	   keys keep the current values. */
	uint8_t (*start)[XWAU_GAME_RECORD_SIZE] = malloc(sizeof tables->records[0] * EXE_VALUES_RECORDS);
	if (!start)
		return false;
	memcpy(start, tables->records, sizeof tables->records[0] * EXE_VALUES_RECORDS);
	bool loaded_ok = true;
	for (int t = 0; loaded_ok && t < EXE_VALUES_TYPES; ++t) {
		const XwauGameType type   = XwauGameTables_Type(tables, t);
		const uint16_t     record = (uint16_t)type.record;
		const char*        model  = exe_values_model(tables, t);
		if (record >= EXE_VALUES_RECORDS || !model[0])
			continue;
		XwauSection values;
		if (!(loaded_ok = XwauSection_ReadModel(game, model, "ExeCraftValues", XWAU_RULES_CPP, &values)))
			break;
		if (!values.lines.count) {
			XwauSection_Free(&values);
			continue;
		}
		uint16_t flags = type.flags;
		if (XwauSection_IntOr(&values, "GameOptions_IsHardpointsMirroring",
							  (flags & FLAG_HARDPOINTS_MIRRORING) != 0) != 0)
			flags |= FLAG_HARDPOINTS_MIRRORING;
		else
			flags &= (uint16_t)~FLAG_HARDPOINTS_MIRRORING;
		if (XwauSection_IntOr(&values, "GameOptions_IsAxisAligned", (flags & FLAG_AXIS_ALIGNED) != 0) != 0)
			flags |= FLAG_AXIS_ALIGNED;
		else
			flags &= (uint16_t)~FLAG_AXIS_ALIGNED;
		tables->types[t][16] = (uint8_t)flags;
		tables->types[t][17] = (uint8_t)(flags >> 8);
		uint8_t* r           = tables->records[record];
		/* LaserTypeId, LaserSequence, LaserRange, LaserFireRatio: the `type`,
		   `kind`, `range_m` and `cooldown_ms` of a `lasers` entry. */
		static const char* const kLaserKeys[]   = { "LaserTypeId", "LaserSequence", "LaserRange",
													"LaserFireRatio" };
		static const int         kLaserFields[] = { 0, 2, 3, 4 };
		for (unsigned g = 0; g < 3; ++g)
			for (int k = 0; k < 4; ++k) {
				char key[64];
				snprintf(key, sizeof key, "%s%u", kLaserKeys[k], g);
				const Field* field = &g_laserFields[kLaserFields[k]];
				set_field(r, field, g, XwauSection_IntOr(&values, key, field_value(start[record], field, g)));
			}
		/* WarheadTypeId and WarheadCapacity: the `type` and `capacity` of a
		   `warheads` entry. */
		static const char* const kWarheadKeys[]   = { "WarheadTypeId", "WarheadCapacity" };
		static const int         kWarheadFields[] = { 0, 2 };
		for (unsigned g = 0; g < 2; ++g)
			for (int k = 0; k < 2; ++k) {
				char key[64];
				snprintf(key, sizeof key, "%s%u", kWarheadKeys[k], g);
				const Field* field = &g_warheadFields[kWarheadFields[k]];
				set_field(r, field, g, XwauSection_IntOr(&values, key, field_value(start[record], field, g)));
			}
		for (size_t i = 0; i < sizeof g_fields / sizeof g_fields[0]; ++i) {
			const char* key = strcmp(g_fields[i].key, "countermeasures") == 0    ? "CounterMeasuresCount"
							  : strcmp(g_fields[i].key, "ai_hits_to_evade") == 0 ? "AIHitsTakenToEvade"
																				 : NULL;
			if (key)
				set_field(r, &g_fields[i], 0,
						  XwauSection_IntOr(&values, key, field_value(r, &g_fields[i], 0)));
		}
		XwauSection_Free(&values);
		(*count)++;
	}
	free(start);
	return loaded_ok;
}

/* ---- Text ------------------------------------------------------------------ */

typedef struct Texts {
	XwauTextFile stock[3], mod[3]; /* STRINGS.TXT, SHIPLIST.TXT, SPECDESC.TXT */
	bool         loaded[3];
	/* The positions in the stock STRINGS.TXT of the first records of the long,
	   plural and short species tables, or -1. */
	long species[3];
} Texts;

enum { TEXT_STRINGS, TEXT_SHIPLIST, TEXT_SPECDESC };

static const char* text_find(const XwauTextFile* file, const char* id) {
	for (size_t i = 0; i < file->count; ++i)
		if (strcasecmp(file->records[i].id, id) == 0)
			return file->records[i].text;
	return NULL;
}

/* The mod's text of `id` when it differs from the stock text, else NULL. A
   text that only the mod has counts as changed. In SHIPLIST.TXT and
   SPECDESC.TXT, this can only be a text of craft type 232, because the stock
   files have texts for every stock craft type. */
static const char* text_changed(const Texts* texts, int file, const char* id) {
	if (!texts->loaded[file])
		return NULL;
	const char* stock = text_find(&texts->stock[file], id);
	const char* mod   = text_find(&texts->mod[file], id);
	return mod && (!stock || strcmp(stock, mod) != 0) ? mod : NULL;
}

static void load_texts(const char* mod_root, const char* stock_game, Texts* texts) {
	static const char* const names[3] = { "STRINGS.TXT", "SHIPLIST.TXT", "SPECDESC.TXT" };
	memset(texts, 0, sizeof *texts);
	for (int i = 0; i < 3; ++i) {
		char stock_path[1100], mod_path[1100];
		texts->loaded[i] = Xwau_FindFile(stock_game, names[i], stock_path, sizeof stock_path) &&
						   Xwau_FindFile(mod_root, names[i], mod_path, sizeof mod_path) &&
						   XwauText_ReadFile(stock_path, &texts->stock[i]) &&
						   XwauText_ReadFile(mod_path, &texts->mod[i]);
	}
	static const char* const first[3] = { "KSPECXW", "KSPECXW_PLURAL", "KSPECXW_SHORT" };
	for (int t = 0; t < 3; ++t) {
		texts->species[t] = -1;
		for (size_t i = 0; texts->loaded[TEXT_STRINGS] && i < texts->stock[TEXT_STRINGS].count; ++i)
			if (strcasecmp(texts->stock[TEXT_STRINGS].records[i].id, first[t]) == 0 &&
				i + SPECIES_COUNT <= texts->stock[TEXT_STRINGS].count)
				texts->species[t] = (long)i;
	}
	if (texts->loaded[TEXT_STRINGS] &&
		(texts->species[0] < 0 || texts->species[1] < 0 || texts->species[2] < 0))
		Report(stock_game, "STRINGS.TXT has no species tables (KSPECXW); species names stay in strings.yaml");
}

static void free_texts(Texts* texts) {
	for (int i = 0; i < 3; ++i) {
		XwauText_FreeFile(&texts->stock[i]);
		XwauText_FreeFile(&texts->mod[i]);
	}
}

/* The ID of record `record` of species table `table` (0 long, 1 plural, 2
   short), or NULL. */
static const char* species_id(const Texts* texts, int table, int record) {
	if (texts->species[table] < 0 || record < 0 || record >= SPECIES_COUNT)
		return NULL;
	return texts->stock[TEXT_STRINGS].records[texts->species[table] + record].id;
}

/* A SHIPLIST.TXT line, split as ShipList_Load does (ship_list.c): fields end
   at ',' or a line end, and empty fields are skipped. */
typedef struct ShipLine {
	char        name[256];
	char        category[64];
	const char* flyable;
	bool        known, skirmish, hidden;
	int         icon[4];
} ShipLine;

static bool parse_ship_line(const char* text, ShipLine* out) {
	char copy[1024], *tokens[16];
	int  count = 0;
	snprintf(copy, sizeof copy, "%s", text);
	for (char* token = strtok(copy, ",\n"); token && count < 16; token = strtok(NULL, ",\n"))
		tokens[count++] = token;
	if (count < 13 || strlen(tokens[0]) >= sizeof out->name || strlen(tokens[1]) >= sizeof out->category)
		return false;
	out->hidden = tokens[0][0] == '*';
	snprintf(out->name, sizeof out->name, "%s", tokens[0] + out->hidden);
	snprintf(out->category, sizeof out->category, "%s", tokens[1]);
	out->flyable  = strcmp(tokens[2], "Flyable") == 0         ? "true"
					: strcmp(tokens[2], "GunnerFlyable") == 0 ? "gunner"
															  : "false";
	out->known    = strcmp(tokens[3], "Known") == 0;
	out->skirmish = strcmp(tokens[4], "Skirmish") == 0;
	for (int i = 0; i < 4; ++i)
		out->icon[i] = atoi(tokens[9 + i]);
	return true;
}

static const struct {
	const char* suffix;
	const char* key;
} g_techRoomFields[] = {
	{ "NAME", "name" },        { "MANU", "manufacturer" }, { "SIDE", "side" },
	{ "DESC", "description" }, { "CREW", "crew" },
};

/* ---- Object files ------------------------------------------------------- */

/* Asset flag of the object types whose list row is an OPT model. OpenXWA
   sets it on every object type that an object type file gives a model. */
#define ASSET_OPT_MODEL 0x01
/* Asset flag that the `always_loaded` key sets: every mission loads the
   model, as for the stock lasers and warheads. */
#define ASSET_ALWAYS_LOADED 0x08
/* The asset flags that the `missions` key sets: the model loads only in
   Death Star missions, or not in them. */
#define ASSET_DEATH_STAR_ONLY 0x20
#define ASSET_NOT_DEATH_STAR 0x80
#define ASSET_MISSION_FLAGS (ASSET_DEATH_STAR_ONLY | ASSET_NOT_DEATH_STAR)

/* What the mod changes about one stock object type. */
typedef struct Change {
	int         objectType;
	const char* model; /* the mod's model, for the file name */
	int         list;  /* the list of the mod's row: 0 Spacecraft0.LST, 1 Equipment0.LST */
	bool        writeModel;
	bool        genus, family, flags, missions, alwaysLoaded;
	int         recordIndex; /* the craft record that the type uses in the mod */
	int         stockRecord; /* the craft record that the type uses in the stock game */
	bool        recordChanged;
	int         soundsLike; /* 0: no change */
	bool        beamAllowed;
	char        fileName[160];
} Change;

static bool is_placeholder(int object_type) {
	for (size_t i = 0; i < sizeof g_placeholders / sizeof g_placeholders[0]; ++i)
		if (g_placeholders[i] == object_type)
			return true;
	return false;
}

/* The stock object type, 1 to 65, whose stock sound cases are the mod's cases
   of `object_type`, or 0 when its cases don't change. -1 when no stock object
   type has them. */
static int sounds_like(const XwauGameTables* stock, const XwauGameTables* mod, int object_type) {
	if (object_type < 1 || object_type > SOUND_CASE_COUNT)
		return 0;
	bool changed = false;
	for (int s = 0; s < SOUND_SWITCH_COUNT; ++s)
		changed |= stock->soundCases[s][object_type - 1] != mod->soundCases[s][object_type - 1];
	if (!changed)
		return 0;
	for (int other = 1; other <= SOUND_CASE_COUNT; ++other) {
		bool same = other != object_type;
		for (int s = 0; same && s < SOUND_SWITCH_COUNT; ++s)
			same = stock->soundCases[s][other - 1] == mod->soundCases[s][object_type - 1];
		if (same)
			return other;
	}
	return -1;
}

static bool sound_jumps_match(const XwauGameTables* stock, const XwauGameTables* mod) {
	for (int s = 0; s < SOUND_SWITCH_COUNT; ++s)
		if (stock->soundJumpSize[s] != mod->soundJumpSize[s] ||
			memcmp(stock->soundJumps[s], mod->soundJumps[s], stock->soundJumpSize[s]) != 0)
			return false;
	return true;
}

/* Whether the mod's code jumps past the beam test, as XWAU's does: `jmp` from
   XWAU_GAME_BEAM_TEST to XWAU_GAME_BEAM_TEST_END instead of the stock `jz`. */
static int beam_test_skipped(const XwauGameTables* stock, const XwauGameTables* mod) {
	if (memcmp(stock->beam, mod->beam, 2) == 0)
		return 0;
	if (mod->beam[0] == 0xEB && XWAU_GAME_BEAM_TEST + 2 + mod->beam[1] == XWAU_GAME_BEAM_TEST_END)
		return 1;
	ReportUnsupported("unknown changes to the beam test of XWINGALLIANCE.EXE");
	return 0;
}

typedef struct Counts {
	int files, models, typeFields, records, sounds, beams, names, craftTypes, remaps;
} Counts;

static void write_quoted_or_empty(FILE* f, const char* text) {
	if (!XwauText_WriteQuoted(f, text))
		fputs("''", f);
}

/* Writes `names` when the species names of the object type's record in the
   mod differ from the names of its stock record. The game writes them into
   the object type's own record. */
static bool write_names(FILE* f, const Change* change, const Texts* texts, XwauTextIds* moved) {
	if (!texts->loaded[TEXT_STRINGS] || change->recordIndex >= SPECIES_WITH_OWN_RECORD ||
		change->stockRecord >= SPECIES_WITH_OWN_RECORD)
		return false;
	const char* ids[3];
	const char* changed[3];
	bool        any = false;
	for (int t = 0; t < 3; ++t) {
		const char* stock_id = species_id(texts, t, change->stockRecord);
		const char* stock    = stock_id ? text_find(&texts->stock[TEXT_STRINGS], stock_id) : NULL;
		ids[t]               = species_id(texts, t, change->recordIndex);
		changed[t]           = ids[t] ? text_find(&texts->mod[TEXT_STRINGS], ids[t]) : NULL;
		if (!stock || !changed[t] || strcmp(stock, changed[t]) == 0)
			changed[t] = NULL;
		any |= changed[t] != NULL;
	}
	if (!any)
		return false;
	if (changed[0] && !(changed[0][0] && strchr("mfn", changed[0][0]) && changed[0][1] == ':')) {
		/* The names stay in strings.yaml. That only keeps them when the
		   object type uses its stock record in the mod too. */
		if (change->recordIndex != change->stockRecord)
			ReportFatal(NULL,
						"the long name %s of object type %d has no gender, so it can't go in the object type "
						"file, and its craft record in the mod isn't its stock record",
						ids[0], change->objectType);
		return false;
	}
	fputs("names: {", f);
	bool first = true;
	if (changed[2]) {
		fputs("short: ", f);
		write_quoted_or_empty(f, changed[2]);
		first = false;
	}
	if (changed[0]) {
		fprintf(f, "%slong: ", first ? "" : ", ");
		write_quoted_or_empty(f, changed[0] + 2);
		fprintf(f, ", gender: %c", changed[0][0]);
		first = false;
	}
	if (changed[1]) {
		fprintf(f, "%splural: ", first ? "" : ", ");
		write_quoted_or_empty(f, changed[1]);
	}
	fputs("}\n", f);
	for (int t = 0; t < 3; ++t)
		if (changed[t] && text_changed(texts, TEXT_STRINGS, ids[t]))
			XwauTextIds_Add(moved, ids[t]);
	return true;
}

/* ---- Craft type files ------------------------------------------------------ */

/* What the mod changes about one craft type. */
typedef struct CraftTypeChange {
	bool        remap; /* the mod links it to another object type */
	const char* line;  /* the changed SHIPLIST.TXT line, or NULL */
	ShipLine    ship;
	const char* tech[5]; /* the changed SPECDESC.TXT texts, or NULL */
	char        name[128];
} CraftTypeChange;

/* Finds what the mod changes about craft type `c`. Returns false when it
   changes nothing. */
static bool craft_type_change(int c, const XwauGameTables* stock, const XwauGameTables* mod,
							  const Texts* texts, CraftTypeChange* out) {
	memset(out, 0, sizeof *out);
	out->remap = stock->craft[c] != mod->craft[c];
	char id[64];
	snprintf(id, sizeof id, "SPECIES_%d", c);
	out->line = text_changed(texts, TEXT_SHIPLIST, id);
	/* A line with too few fields stays in strings.yaml. */
	if (out->line && !parse_ship_line(out->line, &out->ship))
		out->line = NULL;
	bool any_tech = false;
	for (int i = 0; i < 5; ++i) {
		char tech_id[64];
		snprintf(tech_id, sizeof tech_id, "SPEC_%d_%s", c, g_techRoomFields[i].suffix);
		out->tech[i] = text_changed(texts, TEXT_SPECDESC, tech_id);
		any_tech |= out->tech[i] != NULL;
	}
	return out->remap || out->line || any_tech;
}

/* Writes the ship list name of craft type `c` from SHIPLIST.TXT `file` to
   `name`, in lower case with '_' between words. Writes "" when the craft
   type has no ship list name. */
static void craft_type_ship_name(int c, const XwauTextFile* file, bool loaded, char* name, size_t size) {
	char id[64];
	snprintf(id, sizeof id, "SPECIES_%d", c);
	const char* line = loaded ? text_find(file, id) : NULL;
	ShipLine    ship;
	size_t      length = 0;
	name[0]            = '\0';
	if (!line || !parse_ship_line(line, &ship))
		return;
	for (const char* p = ship.name; *p && length + 1 < size; ++p) {
		const char ch = *p >= 'A' && *p <= 'Z' ? (char)(*p - 'A' + 'a') : *p;
		if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
			name[length++] = ch;
		else if (length > 0 && name[length - 1] != '_')
			name[length++] = '_';
	}
	while (length > 0 && name[length - 1] == '_')
		length--;
	name[length] = '\0';
}

/* Returns the category name for `text`, a category as SHIPLIST.TXT writes
   it, or NULL when `text` isn't a category. */
static const char* category_name(const XwauNames* names, const char* text) {
	return XwauNames_Name(&names->ship_list_categories,
						  XwauNames_Find(&names->ship_list_category_texts, text));
}

static bool write_craft_type_file(const char* path, const XwauNames* names, int c, int object_type,
								  const char* model, const CraftTypeChange* change, XwauTextIds* moved) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "can't be written");
		return false;
	}
	fputs("# Made by the OpenXWA mod importer.\n", f);
	fprintf(f, "craft_type: %d\nobject_type: %s  # %s\n", c, XwauNames_ObjectType(names, object_type), model);
	if (change->line) {
		const ShipLine* ship     = &change->ship;
		const char*     category = category_name(names, ship->category);
		/* The game leaves a line with an unknown category out of the ship
		   list, as it does with a hidden line. So such a craft is written as
		   hidden, without a category. */
		fputs("ship_list: {name: ", f);
		write_quoted_or_empty(f, ship->name);
		if (category)
			fprintf(f, ", category: %s", category);
		fprintf(f, ", flyable: %s, known: %s, skirmish: %s, hidden: %s, icon: [%d, %d, %d, %d]}\n",
				ship->flyable, ship->known ? "true" : "false", ship->skirmish ? "true" : "false",
				ship->hidden || !category ? "true" : "false", ship->icon[0], ship->icon[1], ship->icon[2],
				ship->icon[3]);
		char id[64];
		snprintf(id, sizeof id, "SPECIES_%d", c);
		XwauTextIds_Add(moved, id);
	}
	bool first = true;
	for (int i = 0; i < 5; ++i) {
		if (!change->tech[i])
			continue;
		char tech_id[64];
		snprintf(tech_id, sizeof tech_id, "SPEC_%d_%s", c, g_techRoomFields[i].suffix);
		fprintf(f, "%s%s: ", first ? "tech_room: {" : ", ", g_techRoomFields[i].key);
		write_quoted_or_empty(f, change->tech[i]);
		XwauTextIds_Add(moved, tech_id);
		first = false;
	}
	if (!first)
		fputs("}\n", f);
	if (fclose(f) != 0) {
		Report(path, "can't be written");
		return false;
	}
	return true;
}

/* Writes a file for each craft type that the mod links to another object
   type or whose texts it changes, when its object type loads a model in the
   mod (`models[object type]`, NULL when it loads nothing). */
static bool write_craft_types(const char* dir, const XwauNames* names, const XwauGameTables* stock,
							  const XwauGameTables* mod, const char* const* models, const Texts* texts,
							  XwauTextIds* moved, Counts* counts) {
	CraftTypeChange* changes = calloc(CRAFT_TYPE_COUNT, sizeof *changes);
	bool*            writes  = calloc(CRAFT_TYPE_COUNT, sizeof *writes);
	bool             ok      = changes && writes;
	for (int c = 1; ok && c < CRAFT_TYPE_COUNT; ++c) {
		const int t = mod->craft[c];
		const bool changed = craft_type_change(c, stock, mod, texts, &changes[c]);
		/* XWAU can't show a craft type whose object type loads nothing. */
		writes[c] = t < OBJECT_TYPE_COUNT && models[t] && changed;
		if (writes[c])
			craft_type_ship_name(c, &texts->mod[TEXT_SHIPLIST], texts->loaded[TEXT_SHIPLIST], changes[c].name,
								 sizeof changes[c].name);
	}
	for (int c = 1; ok && c < CRAFT_TYPE_COUNT; ++c) {
		if (!writes[c])
			continue;
		int same = 0;
		for (int other = 1; other < CRAFT_TYPE_COUNT; ++other)
			same += writes[other] && strcmp(changes[other].name, changes[c].name) == 0;
		char file_name[200], path[1300];
		if (!changes[c].name[0])
			snprintf(file_name, sizeof file_name, "craft_type_%d.yaml", c);
		else if (same > 1)
			snprintf(file_name, sizeof file_name, "%s_%d.yaml", changes[c].name, c);
		else
			snprintf(file_name, sizeof file_name, "%s.yaml", changes[c].name);
		ok = PathFormat(path, sizeof path, "%s/%s", dir, file_name) &&
			 write_craft_type_file(path, names, c, mod->craft[c], models[mod->craft[c]], &changes[c], moved);
		counts->craftTypes++;
		counts->remaps += changes[c].remap;
	}
	free(changes);
	free(writes);
	return ok;
}

static void write_craft_record(FILE* f, const char* path, const XwauNames* names, int object_type,
							   const uint8_t* stock, const uint8_t* mod) {
	fputs("craft_record:\n", f);
	for (size_t i = 0; i < sizeof g_fields / sizeof g_fields[0]; ++i) {
		if (!field_differs(stock, mod, &g_fields[i], 0))
			continue;
		fprintf(f, "  %s: ", g_fields[i].key);
		write_field(f, path, names, mod, &g_fields[i], 0);
		fputc('\n', f);
	}
	if (group_differs(stock, mod, g_laserFields, 5, 3))
		write_groups(f, path, names, "lasers", mod, g_laserFields, 5, 3);
	if (group_differs(stock, mod, g_warheadFields, 3, 2))
		write_groups(f, path, names, "warheads", mod, g_warheadFields, 3, 2);
	if (group_differs(stock, mod, g_turretFields, 8, 2))
		write_turrets(f, names, object_type, mod);
	if (eye_differs(stock, mod)) {
		fputs("  cockpit_eye_m: ", f);
		Xwau_WriteMetres3(f, record_i16(mod, EYE_X), record_i16(mod, EYE_Y), record_i16(mod, EYE_Z));
		fputc('\n', f);
	}
	if (name_differs(stock, mod)) {
		char name[NAME_SIZE + 1];
		memcpy(name, mod + NAME_OFFSET, NAME_SIZE);
		name[NAME_SIZE] = '\0';
		fputs("  cockpit_name: ", f);
		write_quoted_or_empty(f, name);
		fputc('\n', f);
	}
	if (mesh_attach_differs(stock, mod)) {
		fputs("  mesh_attach: [", f);
		for (unsigned i = 0; i < MESH_ATTACH_COUNT; ++i)
			fprintf(f, "%s%d", i ? ", " : "", (int32_t)read_u32(mod + MESH_ATTACH + 4 * i));
		fputs("]\n", f);
	}
}

/* If the mod turns a settable flag on or off, writes `flags` with all the
   settable flags that are on. A change to a named flag that a file can't set
   makes the import fail. Bits without a name are left out, because the game
   never reads them. */
static void write_flags(FILE* f, const XwauNames* names, int object_type, uint16_t stock, uint16_t mod) {
	const uint16_t settable = (uint16_t)names->settable_object_type_flags;
	for (int bit = 0; bit < 16; ++bit) {
		const uint16_t mask = (uint16_t)(1u << bit);
		const char*    name = XwauNames_Name(&names->object_type_flags, bit);
		if (((stock ^ mod) & mask) && name && !(settable & mask))
			ReportFatal(NULL,
						"the mod changes flag %s of object type %s, and an object type file can't set it",
						name, XwauNames_ObjectType(names, object_type));
	}
	if (!((stock ^ mod) & settable))
		return;
	bool first = true;
	fputs("flags: [", f);
	for (int bit = 0; bit < 16; ++bit) {
		const uint16_t mask = (uint16_t)(1u << bit);
		if ((mod & settable & mask) && XwauNames_Name(&names->object_type_flags, bit)) {
			fprintf(f, "%s%s", first ? "" : ", ", XwauNames_Name(&names->object_type_flags, bit));
			first = false;
		}
	}
	fputs("]\n", f);
}

static bool write_object_file(const char* path, const XwauNames* names, const Change* change,
							  const XwauGameTables* stock, const XwauGameTables* mod, const Texts* texts,
							  XwauTextIds* moved, Counts* counts) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "can't be written");
		return false;
	}
	const XwauGameType m = XwauGameTables_Type(mod, change->objectType);
	const XwauGameType s          = XwauGameTables_Type(stock, change->objectType);
	const char*        stock_name = XwauNames_Name(&names->object_types, change->objectType);
	const char*        name       = XwauNames_ObjectType(names, change->objectType);
	fputs("# Made by the OpenXWA mod importer.\n", f);
	fprintf(f, "object_type: %s\n", stock_name);
	if (strcmp(name, stock_name) != 0)
		fprintf(f, "name: %s\n", name);
	if (change->writeModel) {
		fprintf(f, "model: %s\n", change->model);
		if (change->list == 1)
			fputs("list: equipment\n", f);
		counts->models++;
	}
	const char* genus  = XwauNames_Name(&names->genus, m.genus);
	const char* family = XwauNames_Name(&names->family, m.family);
	if (change->genus && genus)
		fprintf(f, "genus: %s\n", genus);
	else if (change->genus)
		ReportFatal(path, "the game has no name for genus %u", m.genus);
	if (change->family && family)
		fprintf(f, "family: %s\n", family);
	else if (change->family)
		ReportFatal(path, "the game has no name for family %u", m.family);
	if (change->flags)
		write_flags(f, names, change->objectType, s.flags, m.flags);
	if (change->missions)
		fprintf(f, "missions: %s\n",
				(m.assetFlags & ASSET_DEATH_STAR_ONLY)  ? "death_star"
				: (m.assetFlags & ASSET_NOT_DEATH_STAR) ? "not_death_star"
														: "all");
	if (change->alwaysLoaded)
		fprintf(f, "always_loaded: %s\n", (m.assetFlags & ASSET_ALWAYS_LOADED) ? "true" : "false");
	counts->typeFields +=
		change->genus || change->family || change->flags || change->missions || change->alwaysLoaded;
	/* Compare with the object type's own stock record. If other object types
	   share this record, the game gives the object type its own copy. */
	if (change->recordChanged) {
		write_craft_record(f, path, names, change->objectType, stock->records[change->stockRecord],
						   mod->records[change->recordIndex]);
		counts->records++;
	}
	if (change->soundsLike > 0) {
		fprintf(f, "sounds_like: %s\n", XwauNames_ObjectType(names, change->soundsLike));
		counts->sounds++;
	}
	if (change->beamAllowed) {
		fputs("beam_allowed: true\n", f);
		counts->beams++;
	}
	counts->names += write_names(f, change, texts, moved);
	if (fclose(f) != 0) {
		Report(path, "can't be written");
		return false;
	}
	counts->files++;
	return true;
}

bool XwauObjects_Rename(const char* mod_root, const char* stock_game, const XwauGameTables* stock,
						const XwauGameTables* mod, XwauNames* names) {
	if (!stock || !mod)
		return true;
	const char** renames = calloc((size_t)names->object_types.count, sizeof *renames);
	if (!renames)
		return false;
	Texts texts;
	load_texts(mod_root, stock_game, &texts);
	int  count = 0;
	bool ok    = true;
	for (int t = 1; ok && t < OBJECT_TYPE_COUNT && t < names->object_types.count; ++t) {
		/* The slot's ship list name in each game, from the first craft type
		   that uses the slot. */
		char mod_name[128] = "", stock_name[128] = "";
		for (int c = 1; c < CRAFT_TYPE_COUNT && !mod_name[0]; ++c)
			if (mod->craft[c] == t)
				craft_type_ship_name(c, &texts.mod[TEXT_SHIPLIST], texts.loaded[TEXT_SHIPLIST], mod_name,
									 sizeof mod_name);
		for (int c = 1; c < CRAFT_TYPE_COUNT && !stock_name[0]; ++c)
			if (stock->craft[c] == t)
				craft_type_ship_name(c, &texts.stock[TEXT_SHIPLIST], texts.loaded[TEXT_SHIPLIST], stock_name,
									 sizeof stock_name);
		/* Names must be shorter than 64 characters, and the slot number may
		   be added later. */
		mod_name[56] = '\0';
		/* A slot that loads nothing in the mod gets no file, so it can't be
		   renamed. */
		if (!XwauGameTables_Model(mod, t)[0] || !mod_name[0] || strcmp(mod_name, stock_name) == 0 ||
			strcmp(mod_name, XwauNames_Name(&names->object_types, t) ? XwauNames_Name(&names->object_types, t)
																	 : "") == 0)
			continue;
		char name[160];
		snprintf(name, sizeof name, "%s", mod_name);
		for (int other = 0; other < names->object_types.count; ++other) {
			const char* taken = renames[other] ? renames[other] : XwauNames_Name(&names->object_types, other);
			if (other != t && taken && strcmp(taken, name) == 0) {
				snprintf(name, sizeof name, "%s_%d", mod_name, t);
				break;
			}
		}
		if (!(renames[t] = StrDup(name))) {
			ok = false;
			break;
		}
		ReportDetail(NULL, "object type %s is renamed to %s, after the mod's ship list name",
					 XwauNames_Name(&names->object_types, t), renames[t]);
		count++;
	}
	free_texts(&texts);
	if (!ok) {
		for (int t = 0; t < names->object_types.count; ++t)
			free((char*)renames[t]);
		free(renames);
		return false;
	}
	names->object_type_renames = renames;
	ReportDetail(NULL, "object types: %d renamed after the mod's ship list names", count);
	return true;
}

void XwauObjects_FreeRenames(XwauNames* names) {
	for (int t = 0; names->object_type_renames && t < names->object_types.count; ++t)
		free((char*)names->object_type_renames[t]);
	free(names->object_type_renames);
	names->object_type_renames = NULL;
}

/* The list rows of the mod's new craft: the rows that the mod gives to object
   types that have no row in the stock game. */
typedef struct NewCraftRows {
	bool* rows[2];
} NewCraftRows;

static bool new_craft_rows_find(const XwauGameTables* stock, const XwauGameTables* mod, NewCraftRows* out) {
	out->rows[0] = calloc(mod->rowCount[0] + 1, 1);
	out->rows[1] = calloc(mod->rowCount[1] + 1, 1);
	if (!out->rows[0] || !out->rows[1])
		return false;
	for (int t = 0; t < OBJECT_TYPE_COUNT; ++t) {
		const XwauGameType s = XwauGameTables_Type(stock, t), m = XwauGameTables_Type(mod, t);
		if (!XwauGameType_HasRow(&s) && XwauGameType_HasRow(&m) && m.row < mod->rowCount[m.list])
			out->rows[m.list][m.row] = true;
	}
	return true;
}

static void new_craft_rows_free(NewCraftRows* rows) {
	free(rows->rows[0]);
	free(rows->rows[1]);
}

/* Whether object type `t`, which loads `mod_model` in the mod, keeps its
   stock model in the import, because the mod renames the row that it loads
   for a new craft. */
static bool keeps_stock_model(const XwauGameTables* stock, const XwauGameTables* mod,
							  const NewCraftRows* rows, int t, const char* mod_model) {
	const XwauGameType s = XwauGameTables_Type(stock, t), m = XwauGameTables_Type(mod, t);
	return mod_model[0] && strcasecmp(XwauGameTables_Model(stock, t), mod_model) != 0 && m.list == s.list &&
		   m.row == s.row && rows->rows[m.list][m.row];
}

bool XwauObjects_ImportedModels(const XwauGameTables* stock, const XwauGameTables* mod, const char** models) {
	NewCraftRows rows = { { NULL, NULL } };
	const bool   ok   = !stock || new_craft_rows_find(stock, mod, &rows);
	for (int t = 0; ok && t < OBJECT_TYPE_COUNT; ++t) {
		models[t] = XwauGameTables_Model(mod, t);
		if (stock && keeps_stock_model(stock, mod, &rows, t, models[t]))
			models[t] = XwauGameTables_Model(stock, t);
	}
	new_craft_rows_free(&rows);
	return ok;
}

bool XwauObjects_Write(const char* mod_root, const char* stock_game, const XwauGameTables* stock,
					   XwauGameTables* mod, const XwauNames* names, const char* output_dir,
					   XwauTextIds* moved) {
	int exe_values = 0;
	if (!stock || !mod || !apply_exe_values(mod_root, mod, &exe_values))
		return false;
	const bool sounds_comparable = sound_jumps_match(stock, mod);
	if (!sounds_comparable)
		ReportUnsupported("changes to the object type sound cases of XWINGALLIANCE.EXE");
	const int beam = beam_test_skipped(stock, mod);

	NewCraftRows new_craft_rows = { { NULL, NULL } };
	Change*      changes        = calloc(OBJECT_TYPE_COUNT, sizeof *changes);
	/* The model that each object type loads in the mod, or NULL. */
	const char** models = calloc(OBJECT_TYPE_COUNT, sizeof *models);
	bool         ok     = changes && models && new_craft_rows_find(stock, mod, &new_craft_rows);

	int new_slots = 0, new_placeholders = 0;
	int equipment = 0, kept_models = 0, empty = 0;
	int change_count = 0;
	for (int t = 0; ok && t < OBJECT_TYPE_COUNT; ++t) {
		const XwauGameType s = XwauGameTables_Type(stock, t), m = XwauGameTables_Type(mod, t);
		const char*        stock_model = XwauGameTables_Model(stock, t);
		const char*        mod_model   = XwauGameTables_Model(mod, t);
		const bool         type_changed =
			memcmp(stock->types[t], mod->types[t], OBJECT_TYPE_SIZE) != 0 ||
			strcmp(stock_model, mod_model) != 0 ||
			(valid_record(m.record) && record_differs(stock->records[m.record], mod->records[m.record]));
		if (!XwauGameType_HasRow(&s)) {
			/* This is a free slot. The mod fills it with a new craft when it
			   gives it a row with a model. */
			if (!XwauGameType_HasRow(&m) || !mod_model[0])
				continue;
			new_slots++;
		} else if (is_placeholder(t)) {
			/* A placeholder loads the X-wing's row in stock. The mod fills it
			   when it gives it another model. */
			if (!mod_model[0])
				continue;
			new_placeholders += strcasecmp(stock_model, mod_model) != 0;
		}
		/* XWAU can't show an object type that loads no model. */
		if (!mod_model[0]) {
			empty += type_changed;
			continue;
		}
		/* No key sets recordFlags. Bit 0x02 comes from the model, and nothing
		   reads bit 0x01. For the asset flags, `always_loaded` sets
		   ASSET_ALWAYS_LOADED and `missions` sets ASSET_MISSION_FLAGS.
		   OpenXWA adds ASSET_OPT_MODEL to a free slot that gets a model, and
		   never removes it. `missions` can't set both of its flags: the model
		   would never load. */
		const uint8_t asset_changes = s.assetFlags ^ m.assetFlags;
		const uint8_t allowed_asset_changes =
			ASSET_ALWAYS_LOADED | ASSET_MISSION_FLAGS | (XwauGameType_HasRow(&s) ? 0 : ASSET_OPT_MODEL);
		if (((s.recordFlags ^ m.recordFlags) & ~0x03) || (asset_changes & ~allowed_asset_changes) ||
			((asset_changes & ASSET_OPT_MODEL) && !(m.assetFlags & ASSET_OPT_MODEL)) ||
			(m.assetFlags & ASSET_MISSION_FLAGS) == ASSET_MISSION_FLAGS)
			ReportFatal(
				mod_root,
				"changes the record flags of object type %d from 0x%02X to 0x%02X, or its asset flags "
				"from 0x%02X to 0x%02X, and no key sets them",
				t, s.recordFlags, m.recordFlags, s.assetFlags, m.assetFlags);
		Change change     = { .objectType  = t,
							  .model       = mod_model,
							  .list        = m.list,
							  .recordIndex = m.record,
							  .stockRecord = valid_record(s.record) ? s.record : m.record };
		change.writeModel = strcasecmp(stock_model, mod_model) != 0 || m.list != s.list;
		if (keeps_stock_model(stock, mod, &new_craft_rows, t, mod_model)) {
			ReportDetail(mod_root,
						 "object type %d keeps its model '%s': the mod renames its row for a new craft", t,
						 stock_model);
			change.writeModel = false;
			change.model      = stock_model;
			kept_models++;
		}
		models[t]                 = change.model;
		change.genus              = s.genus != m.genus;
		change.family             = s.family != m.family;
		change.flags              = s.flags != m.flags;
		change.missions           = (asset_changes & ASSET_MISSION_FLAGS) != 0;
		change.alwaysLoaded       = (asset_changes & ASSET_ALWAYS_LOADED) != 0;
		change.recordChanged = valid_record(m.record) &&
							   record_differs(stock->records[change.stockRecord], mod->records[m.record]);
		change.soundsLike = sounds_comparable ? sounds_like(stock, mod, t) : 0;
		if (change.soundsLike < 0) {
			ReportUnsupported("object type sound cases that no stock object type has");
			change.soundsLike = 0;
		}
		for (size_t i = 0; beam && i < sizeof g_beamTestTypes / sizeof g_beamTestTypes[0]; ++i)
			change.beamAllowed |= g_beamTestTypes[i] == t;
		if (!(change.writeModel || change.genus || change.family || change.flags || change.missions ||
			  change.alwaysLoaded || change.recordChanged || change.soundsLike || change.beamAllowed ||
			  XwauNames_IsRenamed(names, t)))
			continue;
		equipment += change.list == 1;
		changes[change_count++] = change;
	}

	char dir[1100], craft_dir[1100];
	ok = ok && PathFormat(dir, sizeof dir, "%s/object_types", output_dir) && MakeDirs(dir) &&
		 PathFormat(craft_dir, sizeof craft_dir, "%s/craft_types", output_dir) && MakeDirs(craft_dir);
	Texts texts;
	load_texts(mod_root, stock_game, &texts);
	Counts counts       = { 0 };
	int    missing_glbs = 0;
	for (int i = 0; ok && i < change_count; ++i) {
		Change* change = &changes[i];
		int     same   = 0;
		for (int j = 0; j < change_count; ++j)
			same += strcmp(changes[j].model, change->model) == 0;
		char path[1300];
		if (same > 1)
			snprintf(change->fileName, sizeof change->fileName, "%s_%d.yaml", change->model,
					 change->objectType);
		else
			snprintf(change->fileName, sizeof change->fileName, "%s.yaml", change->model);
		ok = PathFormat(path, sizeof path, "%s/%s", dir, change->fileName) &&
			 write_object_file(path, names, change, stock, mod, &texts, moved, &counts);
		char glb[1300];
		if (ok && change->writeModel &&
			PathFormat(glb, sizeof glb, "%s/models/%s.glb", output_dir, change->model) && !IsFile(glb)) {
			Report(glb, "doesn't exist, so the game skips %s until the model is converted", change->fileName);
			missing_glbs++;
		}
	}
	ok = ok && write_craft_types(craft_dir, names, stock, mod, models, &texts, moved, &counts);
	free_texts(&texts);
	if (ok) {
		ReportDetail(
			NULL,
			"object types: %d files: %d with a model, %d with a new genus, family, flags, missions or "
			"always_loaded, %d "
			"with craft record changes, %d with sounds_like, %d with beam_allowed, %d with names",
			counts.files, counts.models, counts.typeFields, counts.records, counts.sounds, counts.beams,
			counts.names);
		ReportDetail(NULL, "craft types: %d files, %d of them linked to another object type",
					 counts.craftTypes, counts.remaps);
		ReportDetail(NULL, "object types: %d new craft in free slots and %d in placeholders", new_slots,
					 new_placeholders);
		if (exe_values)
			ReportDetail(NULL, "object types: %d take the [ExeCraftValues] of their model", exe_values);
		ReportDetail(NULL,
					 "object types: %d files for object types of Equipment0.LST; %d stock models kept, %d "
					 "types that load nothing in the mod",
					 equipment, kept_models, empty);
		if (missing_glbs)
			ReportDetail(NULL, "object types: %d files name a model that has no GLB in %s/models",
						 missing_glbs, output_dir);
	}
	new_craft_rows_free(&new_craft_rows);
	free(changes);
	free(models);
	return ok;
}
