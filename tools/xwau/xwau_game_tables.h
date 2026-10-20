#ifndef AERON_XWAU_GAME_TABLES_H
#define AERON_XWAU_GAME_TABLES_H

/*
 * Reads the craft tables of a game folder: its XWINGALLIANCE.EXE (XWA 2.02,
 * stock or patched by a mod) and its FlightModels/Spacecraft0.LST and
 * Equipment0.LST. The tables are read with the original layouts:
 *   - the object type table (0x5FB240), 557 entries of 24 bytes;
 *   - the craft type table (0x5B0F70), 232 words: the object type of each
 *     craft type;
 *   - the craft records (0x5BB480), 265 records of 987 bytes, packed, with
 *     4-byte pointers;
 *   - the four sound switches that pick an engine or launch sound case by
 *     object type, and the test that takes the beam away from six craft.
 *
 * An object type's model is the name of its list row, in lower case and
 * without the folder and the extension. Its list is its textureGroup (0 for
 * Spacecraft0.LST, 1 for Equipment0.LST) and its row its frameCount, when
 * bit 0x02 of its recordFlags is set.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	XWAU_GAME_OBJECT_TYPES     = 557,
	XWAU_GAME_OBJECT_TYPE_SIZE = 24,
	XWAU_GAME_CRAFT_TYPES      = 232,
	XWAU_GAME_RECORDS          = 265,
	XWAU_GAME_RECORD_SIZE      = 987,
	XWAU_GAME_SOUND_SWITCHES   = 4,
	/* Object types 1 to 65 have a sound case; the others take the default
	   case. */
	XWAU_GAME_SOUND_CASES = 65,
};

/* The `jz` of the first object type test that takes the beam away from six
   craft (mission.c:6624-6627). XWAU replaces it with a jump past the test. */
#define XWAU_GAME_BEAM_TEST 0x41C6BFu
#define XWAU_GAME_BEAM_TEST_END 0x41C6EAu

typedef struct XwauGameTables {
	uint8_t  types[XWAU_GAME_OBJECT_TYPES][XWAU_GAME_OBJECT_TYPE_SIZE];
	uint8_t  records[XWAU_GAME_RECORDS][XWAU_GAME_RECORD_SIZE];
	uint16_t craft[XWAU_GAME_CRAFT_TYPES];
	/* The word after the craft type table, which XWAU writes as craft type
	   232 (craft_definitions_plan.md 4.4). */
	uint16_t craftPastEnd;
	/* The case of each object type in each sound switch, and the switch's
	   table of jump addresses. */
	uint8_t  soundCases[XWAU_GAME_SOUND_SWITCHES][XWAU_GAME_SOUND_CASES];
	uint8_t* soundJumps[XWAU_GAME_SOUND_SWITCHES];
	size_t   soundJumpSize[XWAU_GAME_SOUND_SWITCHES];
	/* The two bytes at XWAU_GAME_BEAM_TEST. */
	uint8_t beam[2];
	char**  rows[2]; /* the model names of the two lists, by row */
	size_t  rowCount[2];
} XwauGameTables;

/* The fields of an entry of the object type table. */
typedef struct XwauGameType {
	uint8_t  recordFlags, assetFlags, family, genus;
	uint16_t flags;
	int16_t  record;
	uint16_t list, row;
} XwauGameType;

/* Reads the tables of the game at `root`. Returns NULL, with a report, when
 * a file is missing or can't be read. */
XwauGameTables* XwauGameTables_Load(const char* root);
void            XwauGameTables_Free(XwauGameTables* tables);

XwauGameType XwauGameTables_Type(const XwauGameTables* tables, int object_type);
/* Whether the object type has a list row. */
bool XwauGameType_HasRow(const XwauGameType* type);

/* The model of `object_type`, or "" when it has none. */
const char* XwauGameTables_Model(const XwauGameTables* tables, int object_type);
/* The model of the object type that `craft_type` maps to, or "" when it has
 * none. */
const char* XwauGameTables_CraftModel(const XwauGameTables* tables, int craft_type);

#endif /* AERON_XWAU_GAME_TABLES_H */
