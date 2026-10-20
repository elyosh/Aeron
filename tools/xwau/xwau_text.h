#ifndef AERON_XWAU_TEXT_H
#define AERON_XWAU_TEXT_H

/*
 * Writes the texts that a mod changes in STRINGS.TXT, FRONTTXT.TXT,
 * SHIPLIST.TXT and SPECDESC.TXT (xwau_import craft --strings FILE).
 *
 * Each line of these files is "!ID!text", and the game looks up every line by
 * its ID. In OpenXWA, text/strings.yaml can replace the text of any ID. So
 * the file lists each ID whose text is different in the mod:
 *
 *   version: 1
 *   strings:
 *     # STRINGS.TXT
 *     MSG_LASERS_ARMED: '\03Primary cannons armed'
 *
 * The text is everything after the ID, without the line end, exactly as the
 * file writes it. It is written in single quotes, where YAML keeps
 * backslashes as they are. The files are read as Latin-1 and written as
 * UTF-8.
 *
 * Upper and lower case don't matter in IDs, as in the game. These cases are
 * reported:
 *   - an ID that only the mod's file has is skipped. The game reads the stock
 *     file, so it never looks up that ID (XWAU's SPECIES_232 and SPEC_232_*);
 *   - an ID that the mod's file doesn't have keeps the stock text;
 *   - when the mod's file has an ID more than once, the first text is used;
 *   - when the mod's file has its lines in a different order: the game still
 *     reads the lines in the stock order, and each line gets the mod's text
 *     for its ID;
 *   - a text with a control character other than a tab is skipped, because
 *     YAML can't hold it in single quotes.
 * The ship list and tech room texts (SPECIES_<n> and SPEC_<n>_*) of craft
 * types 184 to 227 and 231 are also skipped, unless a craft type file carries
 * them. The stock game has no craft for these craft types, and mods put their
 * new craft there. Without a craft type file, such a craft type still has no
 * craft, and its texts would list a craft that the game can't load.
 * A text file that the mod doesn't have is skipped.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct XwauTextRecord {
	char* id;   /* as the file writes it */
	char* text; /* Latin-1 */
} XwauTextRecord;

typedef struct XwauTextFile {
	XwauTextRecord* records;
	size_t          count;
} XwauTextFile;

/* Reads the "!ID!text" lines of `path`, in order. Lines end at '\n', and the
 * '\r' before it is dropped. Lines that start with "//" and lines without an
 * ID are skipped. Returns false when the file can't be read or when out of
 * memory. */
bool XwauText_ReadFile(const char* path, XwauTextFile* out);
void XwauText_FreeFile(XwauTextFile* file);

/* Writes the Latin-1 `text` to `f` in single quotes, in UTF-8. Returns false,
 * and writes nothing, when the text has a control character other than a
 * tab. */
bool XwauText_WriteQuoted(FILE* f, const char* text);

/* A set of text IDs. Upper and lower case don't matter. */
typedef struct XwauTextIds {
	char** ids;
	size_t count;
} XwauTextIds;

/* Adds `id`. Returns false when out of memory. */
bool XwauTextIds_Add(XwauTextIds* ids, const char* id);
bool XwauTextIds_Has(const XwauTextIds* ids, const char* id);
void XwauTextIds_Free(XwauTextIds* ids);

/* Writes `output_file` from the text files of the mod at `mod_root` and of
 * the stock game at `stock_game`. The IDs of `moved` (it can be NULL) are left
 * out, because the object type and craft type files carry their texts
 * (xwau_objects.h). Returns
 * false when a stock file is missing or can't be read, or when the file can't
 * be written. */
bool XwauText_Write(const char* mod_root, const char* stock_game, const char* output_file,
					const XwauTextIds* moved);

#endif /* AERON_XWAU_TEXT_H */
