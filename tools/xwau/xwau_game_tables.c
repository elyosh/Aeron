#include "xwau_game_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"

#define IMAGE_BASE 0x400000u
#define OBJECT_TYPE_TABLE 0x5FB240u
#define CRAFT_TYPE_TABLE 0x5B0F70u
#define RECORD_TABLE 0x5BB480u

/* Each switch reads its case number from a table of 65 bytes, by object type
   minus 1, then jumps through a table of addresses (`jmp [table + case * 4]`). */
static const struct {
	uint32_t cases;
	uint32_t jump;
} g_soundSwitches[XWAU_GAME_SOUND_SWITCHES] = {
	{ 0x43D458u, 0x43D117u }, /* the player's engine loop, fsfx.c:1923 */
	{ 0x43D4F8u, 0x43D359u }, /* the player's engine loop in the map view, fsfx.c:2061 */
	{ 0x43DCC4u, 0x43DA0Cu }, /* the flight sounds of other craft, fsfx.c:2411 */
	{ 0x459678u, 0x458E81u }, /* the hangar launch, hangar.c:2534 */
};

static const char* const g_listNames[2] = { "Spacecraft0.LST", "Equipment0.LST" };

/* ---- Executable --------------------------------------------------------- */

typedef struct Image {
	const uint8_t* data;
	size_t         size;
	uint32_t       base;
	size_t         sectionTable;
	unsigned       sectionCount;
} Image;

static uint32_t read_u32(const uint8_t* p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t read_u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool image_open(const uint8_t* data, size_t size, Image* image) {
	if (size < 0x40 || data[0] != 'M' || data[1] != 'Z')
		return false;
	const uint32_t pe = read_u32(data + 0x3C);
	if (pe > size - 24 || memcmp(data + pe, "PE\0\0", 4) != 0)
		return false;
	const unsigned optional_size = read_u16(data + pe + 20);
	image->data                  = data;
	image->size                  = size;
	image->sectionCount          = read_u16(data + pe + 6);
	image->sectionTable          = pe + 24 + optional_size;
	image->base                  = optional_size >= 32 ? read_u32(data + pe + 24 + 28) : IMAGE_BASE;
	return image->sectionTable + 40 * (size_t)image->sectionCount <= size;
}

/* The bytes of the file at address `address`, or NULL when they aren't in a
   section's data. */
static const uint8_t* image_at(const Image* image, uint32_t address, size_t size) {
	if (address < image->base)
		return NULL;
	const uint32_t rva = address - image->base;
	for (unsigned i = 0; i < image->sectionCount; ++i) {
		const uint8_t* section  = image->data + image->sectionTable + 40 * (size_t)i;
		const uint32_t start    = read_u32(section + 12);
		const uint32_t raw_size = read_u32(section + 16);
		const uint32_t raw      = read_u32(section + 20);
		if (rva >= start && rva - start + size <= raw_size &&
			(size_t)raw + (rva - start) + size <= image->size)
			return image->data + raw + (rva - start);
	}
	return NULL;
}

/* ---- Lists --------------------------------------------------------------- */

/* Reads a FlightModels list the way the game does: lines end at '\r' or
   '\n', empty lines are skipped, and rows count from 0. Each row becomes its
   file's name, in lower case, without the folder and the extension. */
static bool read_list(const char* path, char*** rows, size_t* count) {
	size_t size = 0;
	char*  data = ReadWholeFile(path, &size);
	*rows       = NULL;
	*count      = 0;
	if (!data)
		return false;
	bool ok = true;
	for (char* line = data; ok && line < data + size;) {
		size_t length = strcspn(line, "\r\n");
		char*  next   = line + length;
		while (next < data + size && (*next == '\r' || *next == '\n'))
			++next;
		line[length] = '\0';
		if (length > 0) {
			const char* base = line;
			for (const char* c = line; *c; ++c)
				if (*c == '\\' || *c == '/')
					base = c + 1;
			const char* dot = strrchr(base, '.');
			char**      row = ARRAY_PUSH(*rows, *count);
			ok              = row && (*row = StrNDup(base, dot ? (size_t)(dot - base) : strlen(base)));
			if (ok)
				LowerInPlace(*row);
		}
		line = next;
	}
	free(data);
	return ok;
}

/* ---- Tables -------------------------------------------------------------- */

static bool read_table(const Image* image, const char* exe, uint32_t address, void* out, size_t size) {
	const uint8_t* bytes = image_at(image, address, size);
	if (!bytes) {
		Report(exe, "has no data at 0x%X; it isn't an XWA 2.02 executable", (unsigned)address);
		return false;
	}
	memcpy(out, bytes, size);
	return true;
}

static bool read_sound_switch(const Image* image, const char* exe, int index, XwauGameTables* tables) {
	if (!read_table(image, exe, g_soundSwitches[index].cases, tables->soundCases[index],
					XWAU_GAME_SOUND_CASES))
		return false;
	const uint8_t* jump = image_at(image, g_soundSwitches[index].jump, 7);
	if (!jump || jump[0] != 0xFF || jump[1] != 0x24) {
		Report(exe, "has no switch jump at 0x%X", (unsigned)g_soundSwitches[index].jump);
		return false;
	}
	unsigned cases = 0;
	for (int i = 0; i < XWAU_GAME_SOUND_CASES; ++i)
		if (tables->soundCases[index][i] + 1u > cases)
			cases = tables->soundCases[index][i] + 1u;
	tables->soundJumpSize[index] = 4 * (size_t)cases;
	tables->soundJumps[index]    = malloc(tables->soundJumpSize[index]);
	return tables->soundJumps[index] && read_table(image, exe, read_u32(jump + 3), tables->soundJumps[index],
												   tables->soundJumpSize[index]);
}

XwauGameTables* XwauGameTables_Load(const char* root) {
	char exe[1100];
	if (!Xwau_FindFile(root, "XWINGALLIANCE.EXE", exe, sizeof exe)) {
		Report(root, "has no XWINGALLIANCE.EXE");
		return NULL;
	}
	XwauGameTables* tables = calloc(1, sizeof *tables);
	size_t          size   = 0;
	uint8_t*        data   = (uint8_t*)ReadWholeFile(exe, &size);
	Image           image;
	bool            ok = tables && data;
	if (ok && !image_open(data, size, &image)) {
		Report(exe, "isn't a Windows executable");
		ok = false;
	}
	uint8_t craft[2 * XWAU_GAME_CRAFT_TYPES + 2];
	ok = ok && read_table(&image, exe, OBJECT_TYPE_TABLE, tables->types, sizeof tables->types) &&
		 read_table(&image, exe, RECORD_TABLE, tables->records, sizeof tables->records) &&
		 read_table(&image, exe, CRAFT_TYPE_TABLE, craft, sizeof craft) &&
		 read_table(&image, exe, XWAU_GAME_BEAM_TEST, tables->beam, sizeof tables->beam);
	for (int i = 0; ok && i < XWAU_GAME_SOUND_SWITCHES; ++i)
		ok = read_sound_switch(&image, exe, i, tables);
	if (ok) {
		for (int i = 0; i < XWAU_GAME_CRAFT_TYPES; ++i)
			tables->craft[i] = read_u16(craft + 2 * i);
		tables->craftPastEnd = read_u16(craft + 2 * XWAU_GAME_CRAFT_TYPES);
	}
	for (int list = 0; ok && list < 2; ++list) {
		char rel[64], path[1100];
		snprintf(rel, sizeof rel, "FlightModels/%s", g_listNames[list]);
		if (!Xwau_FindFile(root, rel, path, sizeof path) ||
			!read_list(path, &tables->rows[list], &tables->rowCount[list])) {
			Report(root, "has no readable %s", rel);
			ok = false;
		}
	}
	free(data);
	if (!ok) {
		XwauGameTables_Free(tables);
		return NULL;
	}
	return tables;
}

void XwauGameTables_Free(XwauGameTables* tables) {
	if (!tables)
		return;
	for (int i = 0; i < XWAU_GAME_SOUND_SWITCHES; ++i)
		free(tables->soundJumps[i]);
	for (int list = 0; list < 2; ++list)
		FreeList(tables->rows[list], tables->rowCount[list]);
	free(tables);
}

XwauGameType XwauGameTables_Type(const XwauGameTables* tables, int object_type) {
	const uint8_t* b = tables->types[object_type];
	return (XwauGameType) { b[0],
							b[1],
							b[2],
							b[3],
							read_u16(b + 16),
							(int16_t)read_u16(b + 18),
							read_u16(b + 20),
							read_u16(b + 22) };
}

bool XwauGameType_HasRow(const XwauGameType* type) {
	return (type->recordFlags & 0x02) != 0 && type->list < 2;
}

const char* XwauGameTables_Model(const XwauGameTables* tables, int object_type) {
	if (object_type < 0 || object_type >= XWAU_GAME_OBJECT_TYPES)
		return "";
	const XwauGameType type = XwauGameTables_Type(tables, object_type);
	if (!XwauGameType_HasRow(&type) || type.row >= tables->rowCount[type.list])
		return "";
	return tables->rows[type.list][type.row];
}

const char* XwauGameTables_CraftModel(const XwauGameTables* tables, int craft_type) {
	if (craft_type < 0 || craft_type >= XWAU_GAME_CRAFT_TYPES)
		return "";
	return XwauGameTables_Model(tables, tables->craft[craft_type]);
}
