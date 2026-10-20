#include "xwau_objects.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"
#include "xwau_game_tables.h"

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

typedef enum FieldKind { FIELD_UNSIGNED, FIELD_SIGNED, FIELD_BOOL } FieldKind;

typedef struct Field {
	const char* key;
	unsigned    offset, size;
	FieldKind   kind;
} Field;

/* The single-value keys of `craft_record`, at their offsets in the packed
   987-byte record (model_def.h). */
static const Field g_fields[] = {
	{ "speed", 32, 2, FIELD_UNSIGNED },
	{ "acceleration", 34, 2, FIELD_UNSIGNED },
	{ "deceleration", 36, 2, FIELD_UNSIGNED },
	{ "yaw_rate", 38, 2, FIELD_SIGNED },
	{ "roll_rate", 42, 2, FIELD_SIGNED },
	{ "pitch_rate", 44, 2, FIELD_SIGNED },
	{ "auto_bank", 40, 2, FIELD_UNSIGNED },
	{ "max_tumble_angle", 46, 2, FIELD_UNSIGNED },
	{ "max_push_rate", 48, 2, FIELD_UNSIGNED },
	{ "hull", 21, 4, FIELD_SIGNED },
	{ "shields", 15, 4, FIELD_SIGNED },
	{ "has_shields", 14, 1, FIELD_BOOL },
	{ "has_hyperdrive", 12, 1, FIELD_BOOL },
	{ "system_strength", 29, 2, FIELD_UNSIGNED },
	{ "system_damage_hull_threshold", 25, 4, FIELD_SIGNED },
	{ "component_max_hp", 31, 1, FIELD_UNSIGNED },
	{ "rating_weight", 10, 2, FIELD_UNSIGNED },
	{ "laser_converge_mode", 13, 1, FIELD_UNSIGNED },
	{ "countermeasures", 563, 1, FIELD_UNSIGNED },
	{ "gunsight_offset_y", 564, 4, FIELD_SIGNED },
	{ "ai_hits_to_evade", 19, 1, FIELD_UNSIGNED },
};

/* The keys of a `lasers` entry: laserGroupWeaponType, laserGroupSlotCount,
   laserGroupWeaponKind, laserGroupFireRange, laserGroupFireCooldownTicks. */
static const Field g_laserFields[] = {
	{ "type", 306, 2, FIELD_UNSIGNED },     { "count", 318, 1, FIELD_UNSIGNED },
	{ "kind", 321, 1, FIELD_UNSIGNED },     { "range", 324, 4, FIELD_SIGNED },
	{ "cooldown", 336, 2, FIELD_UNSIGNED },
};

/* The keys of a `warheads` entry: warheadLauncherType, warheadLauncherSlotCount,
   warheadLauncherCapacity. */
static const Field g_warheadFields[] = {
	{ "type", 342, 2, FIELD_UNSIGNED },
	{ "count", 350, 1, FIELD_UNSIGNED },
	{ "capacity", 352, 1, FIELD_UNSIGNED },
};

/* The keys of a `turrets` entry, one for each of the two turret seats:
   turretSeatPosX, turretSeatPosY, turretSeatPosZ, turretMountAngleA,
   turretMountAngleB, turretObjectType (the object type of the seat's model),
   turretAimLimitA, turretAimLimitB. */
static const Field g_turretFields[] = {
	{ "x", 582, 2, FIELD_SIGNED },
	{ "y", 574, 2, FIELD_SIGNED },
	{ "z", 578, 2, FIELD_SIGNED },
	{ "mount_angle_a", 590, 2, FIELD_SIGNED },
	{ "mount_angle_b", 586, 2, FIELD_SIGNED },
	{ "object_type", 594, 2, FIELD_UNSIGNED },
	{ "aim_limit_a", 598, 2, FIELD_SIGNED },
	{ "aim_limit_b", 602, 2, FIELD_SIGNED },
};

#define NAME_OFFSET 50
#define NAME_SIZE 256
#define EYE_X 572
#define EYE_Y 568
#define EYE_Z 570
#define MESH_ATTACH 606
#define MESH_ATTACH_COUNT 11

/* The record's fields that no key sets and that the game doesn't rebuild
   from the model: the importer reports their changes. */
static const struct {
	const char* name;
	unsigned    offset, size;
} g_unkeyedFields[] = {
	{ "gap_14 (+0x14)", 20, 1 },
};

#define UNKEYED_COUNT (sizeof g_unkeyedFields / sizeof g_unkeyedFields[0])

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

static void write_field(FILE* f, const uint8_t* record, const Field* field, unsigned element) {
	const long long value = field_value(record, field, element);
	if (field->kind == FIELD_BOOL && (value == 0 || value == 1))
		fputs(value ? "true" : "false", f);
	else
		fprintf(f, "%lld", value);
}

static bool group_differs(const uint8_t* a, const uint8_t* b, const Field* fields, size_t count,
						  unsigned groups) {
	for (unsigned g = 0; g < groups; ++g)
		for (size_t i = 0; i < count; ++i)
			if (field_differs(a, b, &fields[i], g))
				return true;
	return false;
}

static void write_groups(FILE* f, const char* key, const uint8_t* record, const Field* fields, size_t count,
						 unsigned groups) {
	unsigned last = 0;
	for (unsigned g = 0; g < groups; ++g)
		for (size_t i = 0; i < count; ++i)
			if (field_value(record, &fields[i], g) != 0)
				last = g + 1;
	if (last == 0) {
		fprintf(f, "  %s: []\n", key);
		return;
	}
	fprintf(f, "  %s:\n", key);
	for (unsigned g = 0; g < last; ++g) {
		fputs("    - {", f);
		for (size_t i = 0; i < count; ++i) {
			fprintf(f, "%s%s: ", i ? ", " : "", fields[i].key);
			write_field(f, record, &fields[i], g);
		}
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

/* The mod's text of `id` when it differs from the stock text, else NULL. */
static const char* text_changed(const Texts* texts, int file, const char* id) {
	if (!texts->loaded[file])
		return NULL;
	const char* stock = text_find(&texts->stock[file], id);
	const char* mod   = text_find(&texts->mod[file], id);
	return stock && mod && strcmp(stock, mod) != 0 ? mod : NULL;
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

/* What the mod changes about one stock object type. */
typedef struct Change {
	int         objectType;
	const char* model; /* the mod's model, for the file name */
	int         list;  /* the list of the mod's row: 0 Spacecraft0.LST, 1 Equipment0.LST */
	bool        writeModel;
	bool        genus, family, flags, recordIndexChanged;
	int         recordIndex; /* the craft record that the type uses in the mod */
	bool        ownsRecord;  /* this file carries the record's fields and names */
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
static int beam_test_skipped(const XwauGameTables* stock, const XwauGameTables* mod, const char* mod_root) {
	if (memcmp(stock->beam, mod->beam, 2) == 0)
		return 0;
	if (mod->beam[0] == 0xEB && XWAU_GAME_BEAM_TEST + 2 + mod->beam[1] == XWAU_GAME_BEAM_TEST_END)
		return 1;
	Report(mod_root,
		   "changes the beam test at 0x%X in a way the import doesn't know (%02X %02X); no beam_allowed "
		   "is written",
		   (unsigned)XWAU_GAME_BEAM_TEST, mod->beam[0], mod->beam[1]);
	return 0;
}

/* Removes the object files that an earlier run wrote. */
static void remove_old_files(const char* dir) {
	size_t count = 0;
	char** names = ListDir(dir, true, &count);
	for (size_t i = 0; i < count; ++i) {
		char path[1300], line[64] = "";
		if (!EndsWithNoCase(names[i], ".yaml") || !PathFormat(path, sizeof path, "%s/%s", dir, names[i]))
			continue;
		FILE* f = fopen(path, "rb");
		if (!f)
			continue;
		const bool ours =
			fgets(line, sizeof line, f) && strncmp(line, "# Written by xwau_import craft", 30) == 0;
		fclose(f);
		if (ours)
			remove(path);
	}
	FreeList(names, count);
}

typedef struct Counts {
	int files, models, typeFields, records, sounds, beams, names, craftTypes, remaps;
} Counts;

static void write_quoted_or_empty(FILE* f, const char* text) {
	if (!XwauText_WriteQuoted(f, text))
		fputs("''", f);
}

static bool write_names(FILE* f, const Change* change, const Texts* texts, XwauTextIds* moved) {
	if (!change->ownsRecord || change->recordIndex >= SPECIES_WITH_OWN_RECORD)
		return false;
	const char* ids[3];
	const char* changed[3];
	bool        any = false;
	for (int t = 0; t < 3; ++t) {
		ids[t]     = species_id(texts, t, change->recordIndex);
		changed[t] = ids[t] ? text_changed(texts, TEXT_STRINGS, ids[t]) : NULL;
		any |= changed[t] != NULL;
	}
	if (!any)
		return false;
	if (changed[0] && !(changed[0][0] && strchr("mfn", changed[0][0]) && changed[0][1] == ':')) {
		Report(NULL, "object type %d: the long name %s has no gender; the names stay in strings.yaml",
			   change->objectType, ids[0]);
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
		if (changed[t])
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
	if (out->line && !parse_ship_line(out->line, &out->ship)) {
		Report(NULL, "craft type %d: its ship list line has too few fields; it stays in strings.yaml", c);
		out->line = NULL;
	}
	bool any_tech = false;
	for (int i = 0; i < 5; ++i) {
		char tech_id[64];
		snprintf(tech_id, sizeof tech_id, "SPEC_%d_%s", c, g_techRoomFields[i].suffix);
		out->tech[i] = text_changed(texts, TEXT_SPECDESC, tech_id);
		any_tech |= out->tech[i] != NULL;
	}
	return out->remap || out->line || any_tech;
}

/* The file name of a craft type: its ship list name in the mod, in lower case
   with '_' between words, or "" when it has none. */
static void craft_type_name(int c, const Texts* texts, char* name, size_t size) {
	char id[64];
	snprintf(id, sizeof id, "SPECIES_%d", c);
	const char* line = texts->loaded[TEXT_SHIPLIST] ? text_find(&texts->mod[TEXT_SHIPLIST], id) : NULL;
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

static bool write_craft_type_file(const char* path, int c, int object_type, const char* model,
								  const CraftTypeChange* change, XwauTextIds* moved) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "can't be written");
		return false;
	}
	fprintf(f, "# Written by xwau_import craft: what the mod changes about craft type %d.\n", c);
	fprintf(f, "version: 1\ncraft_type: %d\nobject_type: %d  # %s\n", c, object_type, model);
	if (change->line) {
		const ShipLine* ship = &change->ship;
		fputs("ship_list: {name: ", f);
		write_quoted_or_empty(f, ship->name);
		fputs(", category: ", f);
		write_quoted_or_empty(f, ship->category);
		fprintf(f, ", flyable: %s, known: %s, skirmish: %s, hidden: %s, icon: [%d, %d, %d, %d]}\n",
				ship->flyable, ship->known ? "true" : "false", ship->skirmish ? "true" : "false",
				ship->hidden ? "true" : "false", ship->icon[0], ship->icon[1], ship->icon[2], ship->icon[3]);
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
static bool write_craft_types(const char* dir, const XwauGameTables* stock, const XwauGameTables* mod,
							  const char* const* models, const Texts* texts, XwauTextIds* moved,
							  Counts* counts) {
	CraftTypeChange* changes = calloc(CRAFT_TYPE_COUNT, sizeof *changes);
	bool*            writes  = calloc(CRAFT_TYPE_COUNT, sizeof *writes);
	bool             ok      = changes && writes;
	for (int c = 1; ok && c < CRAFT_TYPE_COUNT; ++c) {
		const int t = mod->craft[c];
		writes[c] =
			t < OBJECT_TYPE_COUNT && models[t] && craft_type_change(c, stock, mod, texts, &changes[c]);
		if (writes[c])
			craft_type_name(c, texts, changes[c].name, sizeof changes[c].name);
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
			 write_craft_type_file(path, c, mod->craft[c], models[mod->craft[c]], &changes[c], moved);
		counts->craftTypes++;
		counts->remaps += changes[c].remap;
	}
	free(changes);
	free(writes);
	return ok;
}

static void write_craft_record(FILE* f, const uint8_t* stock, const uint8_t* mod) {
	fputs("craft_record:\n", f);
	for (size_t i = 0; i < sizeof g_fields / sizeof g_fields[0]; ++i) {
		if (!field_differs(stock, mod, &g_fields[i], 0))
			continue;
		fprintf(f, "  %s: ", g_fields[i].key);
		write_field(f, mod, &g_fields[i], 0);
		fputc('\n', f);
	}
	if (group_differs(stock, mod, g_laserFields, 5, 3))
		write_groups(f, "lasers", mod, g_laserFields, 5, 3);
	if (group_differs(stock, mod, g_warheadFields, 3, 2))
		write_groups(f, "warheads", mod, g_warheadFields, 3, 2);
	if (group_differs(stock, mod, g_turretFields, 8, 2))
		write_groups(f, "turrets", mod, g_turretFields, 8, 2);
	if (eye_differs(stock, mod))
		fprintf(f, "  cockpit_eye: [%d, %d, %d]\n", record_i16(mod, EYE_X), record_i16(mod, EYE_Y),
				record_i16(mod, EYE_Z));
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

static bool write_object_file(const char* path, const Change* change, const XwauGameTables* stock,
							  const XwauGameTables* mod, const Texts* texts, XwauTextIds* moved,
							  Counts* counts) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "can't be written");
		return false;
	}
	const XwauGameType m = XwauGameTables_Type(mod, change->objectType);
	fprintf(f, "# Written by xwau_import craft: what the mod changes about object type %d.\n",
			change->objectType);
	fprintf(f, "version: 1\nobject_type: %d\n", change->objectType);
	if (change->writeModel) {
		fprintf(f, "model: %s\n", change->model);
		if (change->list == 1)
			fputs("list: equipment\n", f);
		counts->models++;
	}
	if (change->genus)
		fprintf(f, "genus: %u\n", m.genus);
	if (change->family)
		fprintf(f, "family: %u\n", m.family);
	if (change->flags)
		fprintf(f, "flags: 0x%X\n", m.flags);
	if (change->recordIndexChanged)
		fprintf(f, "craft_record_index: %d\n", m.record);
	counts->typeFields += change->genus || change->family || change->flags || change->recordIndexChanged;
	if (change->ownsRecord && change->recordChanged) {
		write_craft_record(f, stock->records[change->recordIndex], mod->records[change->recordIndex]);
		counts->records++;
	}
	if (change->soundsLike > 0) {
		fprintf(f, "sounds_like: %d\n", change->soundsLike);
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

bool XwauObjects_Write(const char* mod_root, const char* stock_game, const char* output_dir,
					   XwauTextIds* moved) {
	XwauGameTables* stock = XwauGameTables_Load(stock_game);
	XwauGameTables* mod   = stock ? XwauGameTables_Load(mod_root) : NULL;
	if (!stock || !mod) {
		XwauGameTables_Free(stock);
		return false;
	}
	const bool sounds_comparable = sound_jumps_match(stock, mod);
	if (!sounds_comparable)
		Report(mod_root, "changes the sound cases themselves, not only which object types use them; no "
						 "sounds_like is written");
	const int beam = beam_test_skipped(stock, mod, mod_root);

	/* The rows that object types without a stock row load in the mod: the
	   rows of the mod's new craft. */
	bool*   new_craft_rows[2] = { calloc(mod->rowCount[0] + 1, 1), calloc(mod->rowCount[1] + 1, 1) };
	Change* changes           = calloc(OBJECT_TYPE_COUNT, sizeof *changes);
	int*    record_users      = calloc(RECORD_COUNT, sizeof *record_users);
	/* The model that each object type loads in the mod, or NULL. */
	const char** models = calloc(OBJECT_TYPE_COUNT, sizeof *models);
	bool         ok     = new_craft_rows[0] && new_craft_rows[1] && changes && record_users && models;
	for (int t = 0; ok && t < OBJECT_TYPE_COUNT; ++t) {
		const XwauGameType s = XwauGameTables_Type(stock, t), m = XwauGameTables_Type(mod, t);
		if (!XwauGameType_HasRow(&s) && XwauGameType_HasRow(&m) && m.row < mod->rowCount[m.list])
			new_craft_rows[m.list][m.row] = true;
		if ((m.recordFlags & 0x02) && valid_record(m.record))
			record_users[m.record]++;
	}

	int  new_slots = 0, new_placeholders = 0, empty_slots = 0, empty_placeholders = 0;
	int  equipment = 0, kept_models = 0, empty = 0;
	int  unkeyed[UNKEYED_COUNT]     = { 0 };
	bool record_owned[RECORD_COUNT] = { false };
	int  change_count               = 0;
	for (int t = 0; ok && t < OBJECT_TYPE_COUNT; ++t) {
		const XwauGameType s = XwauGameTables_Type(stock, t), m = XwauGameTables_Type(mod, t);
		const char*        stock_model = XwauGameTables_Model(stock, t);
		const char*        mod_model   = XwauGameTables_Model(mod, t);
		const bool         type_changed =
			memcmp(stock->types[t], mod->types[t], OBJECT_TYPE_SIZE) != 0 ||
			strcmp(stock_model, mod_model) != 0 ||
			(valid_record(m.record) && record_differs(stock->records[m.record], mod->records[m.record]));
		if (!XwauGameType_HasRow(&s)) {
			/* A reserved slot: the mod fills it with a new craft when it gives
			   it a row with a model. */
			if (!XwauGameType_HasRow(&m) || !mod_model[0]) {
				empty_slots += type_changed;
				continue;
			}
			new_slots++;
		} else if (is_placeholder(t)) {
			/* A placeholder loads the X-wing's row in stock. The mod fills it
			   when it gives it another model. */
			if (!mod_model[0]) {
				empty_placeholders += type_changed;
				continue;
			}
			new_placeholders += strcasecmp(stock_model, mod_model) != 0;
		}
		if (!mod_model[0]) {
			if (type_changed) {
				Report(mod_root, "object type %d loads no model in the mod; it is left out", t);
				empty++;
			}
			continue;
		}
		Change change     = { .objectType = t, .model = mod_model, .list = m.list, .recordIndex = m.record };
		change.writeModel = strcasecmp(stock_model, mod_model) != 0 || m.list != s.list;
		if (change.writeModel && m.list == s.list && m.row == s.row && new_craft_rows[m.list][m.row]) {
			Report(mod_root, "object type %d keeps its model '%s': the mod renames its row for a new craft",
				   t, stock_model);
			change.writeModel = false;
			change.model      = stock_model;
			kept_models++;
		}
		models[t]                 = change.model;
		change.genus              = s.genus != m.genus;
		change.family             = s.family != m.family;
		change.flags              = s.flags != m.flags;
		change.recordIndexChanged = s.record != m.record;
		change.recordChanged =
			valid_record(m.record) && record_differs(stock->records[m.record], mod->records[m.record]);
		change.soundsLike = sounds_comparable ? sounds_like(stock, mod, t) : 0;
		if (change.soundsLike < 0) {
			Report(mod_root,
				   "object type %d: no stock object type has its sound cases; no sounds_like is written", t);
			change.soundsLike = 0;
		}
		for (size_t i = 0; beam && i < sizeof g_beamTestTypes / sizeof g_beamTestTypes[0]; ++i)
			change.beamAllowed |= g_beamTestTypes[i] == t;
		if (!(change.writeModel || change.genus || change.family || change.flags ||
			  change.recordIndexChanged || change.recordChanged || change.soundsLike || change.beamAllowed))
			continue;
		if (valid_record(m.record) && !record_owned[m.record]) {
			change.ownsRecord      = true;
			record_owned[m.record] = true;
			if (record_users[m.record] > 1 && (change.recordChanged || m.record < SPECIES_WITH_OWN_RECORD))
				Report(mod_root,
					   "craft record %d is used by %d object types; its fields and names go into the file of "
					   "object type %d",
					   m.record, record_users[m.record], t);
		}
		if (change.ownsRecord && change.recordChanged)
			for (size_t i = 0; i < UNKEYED_COUNT; ++i)
				unkeyed[i] +=
					memcmp(stock->records[m.record] + g_unkeyedFields[i].offset,
						   mod->records[m.record] + g_unkeyedFields[i].offset, g_unkeyedFields[i].size) != 0;
		equipment += change.list == 1;
		changes[change_count++] = change;
	}
	for (size_t i = 0; i < UNKEYED_COUNT; ++i)
		if (unkeyed[i])
			Report(mod_root, "%s changes in %d craft records, and no key sets it; it isn't written",
				   g_unkeyedFields[i].name, unkeyed[i]);

	char dir[1100], craft_dir[1100];
	ok = ok && PathFormat(dir, sizeof dir, "%s/object_types", output_dir) && MakeDirs(dir) &&
		 PathFormat(craft_dir, sizeof craft_dir, "%s/craft_types", output_dir) && MakeDirs(craft_dir);
	if (ok) {
		remove_old_files(dir);
		remove_old_files(craft_dir);
	}
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
			 write_object_file(path, change, stock, mod, &texts, moved, &counts);
		char glb[1300];
		if (ok && change->writeModel &&
			PathFormat(glb, sizeof glb, "%s/models/%s.glb", output_dir, change->model) && !IsFile(glb)) {
			Report(glb, "doesn't exist, so the game skips %s until the model is converted", change->fileName);
			missing_glbs++;
		}
	}
	ok = ok && write_craft_types(craft_dir, stock, mod, models, &texts, moved, &counts);
	free_texts(&texts);
	if (ok) {
		Report(
			NULL,
			"object types: %d files: %d with a model, %d with a new genus, family, flags or craft record "
			"index, %d with craft record changes, %d with sounds_like, %d with beam_allowed, %d with names",
			counts.files, counts.models, counts.typeFields, counts.records, counts.sounds, counts.beams,
			counts.names);
		Report(NULL, "craft types: %d files, %d of them linked to another object type", counts.craftTypes,
			   counts.remaps);
		Report(NULL, "object types: %d new craft in reserved slots and %d in placeholders", new_slots,
			   new_placeholders);
		if (empty_slots || empty_placeholders)
			Report(NULL,
				   "object types: %d reserved slots and %d placeholders that the mod gives an empty row are "
				   "left out, with their craft types: they load nothing in the mod",
				   empty_slots, empty_placeholders);
		Report(NULL,
			   "object types: %d files for object types of Equipment0.LST; %d stock models kept, %d types "
			   "that load nothing in the mod",
			   equipment, kept_models, empty);
		if (missing_glbs)
			Report(NULL, "object types: %d files name a model that has no GLB in %s/models", missing_glbs,
				   output_dir);
		if (mod->craftPastEnd != stock->craftPastEnd)
			Report(
				mod_root,
				"also maps craft type %d to object type %u, past the end of the craft type table; the game's "
				"table has %d entries, so it is skipped",
				CRAFT_TYPE_COUNT, mod->craftPastEnd, CRAFT_TYPE_COUNT);
	}
	free(new_craft_rows[0]);
	free(new_craft_rows[1]);
	free(changes);
	free(record_users);
	free(models);
	XwauGameTables_Free(stock);
	XwauGameTables_Free(mod);
	return ok;
}
