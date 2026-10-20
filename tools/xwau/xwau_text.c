#include "xwau_text.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"

/* The number of craft types in the stock game. */
#define XWAU_TEXT_CRAFT_TYPES 232

/* The text files whose lines the game looks up by ID. */
static const char* const g_textFiles[] = { "STRINGS.TXT", "FRONTTXT.TXT", "SHIPLIST.TXT", "SPECDESC.TXT" };

bool XwauText_ReadFile(const char* path, XwauTextFile* out) {
	memset(out, 0, sizeof *out);
	size_t size = 0;
	char*  data = ReadWholeFile(path, &size);
	if (!data)
		return false;
	bool ok = true;
	for (char* line = data; ok && line < data + size;) {
		char* end  = memchr(line, '\n', (size_t)(data + size - line));
		char* next = end ? end + 1 : data + size;
		if (!end)
			end = data + size;
		while (end > line && end[-1] == '\r')
			--end;
		*end       = '\0';
		char* bang = line[0] == '!' ? strchr(line + 1, '!') : NULL;
		if (bang) {
			XwauTextRecord* record = ARRAY_PUSH(out->records, out->count);
			ok                     = record && (record->id = StrNDup(line + 1, (size_t)(bang - line - 1))) &&
									 (record->text = StrDup(bang + 1));
		}
		line = next;
	}
	free(data);
	if (!ok)
		XwauText_FreeFile(out);
	return ok;
}

void XwauText_FreeFile(XwauTextFile* file) {
	for (size_t i = 0; i < file->count; ++i) {
		free(file->records[i].id);
		free(file->records[i].text);
	}
	free(file->records);
	memset(file, 0, sizeof *file);
}

/* Whether `text` has a byte that YAML can't hold in single quotes. */
static bool has_control_character(const char* text) {
	for (const unsigned char* c = (const unsigned char*)text; *c; ++c)
		if ((*c < 0x20 && *c != '\t') || *c == 0x7f)
			return true;
	return false;
}

bool XwauText_WriteQuoted(FILE* f, const char* text) {
	if (has_control_character(text))
		return false;
	fputc('\'', f);
	for (const unsigned char* c = (const unsigned char*)text; *c; ++c) {
		if (*c == '\'') {
			fputs("''", f);
		} else if (*c < 0x80) {
			fputc(*c, f);
		} else {
			fputc(0xc0 | *c >> 6, f);
			fputc(0x80 | (*c & 0x3f), f);
		}
	}
	fputc('\'', f);
	return true;
}

/* Whether `id` can be written as a plain YAML key: a letter or '_', then
 * letters, digits and '_'. */
static bool is_plain_key(const char* id) {
	if (!(id[0] == '_' || (id[0] >= 'A' && id[0] <= 'Z') || (id[0] >= 'a' && id[0] <= 'z')))
		return false;
	for (const char* c = id; *c; ++c)
		if (!(*c == '_' || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')))
			return false;
	return true;
}

/* The craft type of a ship list ID (SPECIES_<n>) or a tech room ID
 * (SPEC_<n>_<field>), or -1 for another ID. */
static int craft_type_of_id(const char* id) {
	const bool  ship_list = strncasecmp(id, "SPECIES_", 8) == 0;
	const char* digits    = ship_list ? id + 8 : strncasecmp(id, "SPEC_", 5) == 0 ? id + 5 : NULL;
	if (!digits || *digits < '0' || *digits > '9')
		return -1;
	char*      end;
	const long craft_type = strtol(digits, &end, 10);
	if (ship_list ? *end != '\0' : *end != '_')
		return -1;
	return craft_type < XWAU_TEXT_CRAFT_TYPES ? (int)craft_type : -1;
}

/* Whether the stock game has no craft for `craft_type`. Its craft type table
 * (0x5B0F70) gives object type 0 to craft types 184 to 227 and 231, and mods
 * put their new craft there. */
static bool is_free_craft_type(int craft_type) {
	return (craft_type >= 184 && craft_type <= 227) || craft_type == 231;
}

/* The ID of a stock record and where the record is, to find it by ID. */
typedef struct StockEntry {
	const char* id;
	size_t      line; /* its position in the stock file's records */
} StockEntry;

static int compare_entries(const void* a, const void* b) {
	return strcasecmp(((const StockEntry*)a)->id, ((const StockEntry*)b)->id);
}

/* Writes the texts of the mod's file `name` that are different from the
 * stock file, and adds their count to `*written`. */
static bool write_file_texts(FILE* f, const char* name, const char* mod_path, const char* stock_path,
							 int* written) {
	XwauTextFile mod, stock;
	if (!XwauText_ReadFile(stock_path, &stock)) {
		Report(stock_path, "can't be read");
		return false;
	}
	if (!XwauText_ReadFile(mod_path, &mod)) {
		Report(mod_path, "can't be read");
		XwauText_FreeFile(&stock);
		return false;
	}
	StockEntry* entries = malloc((stock.count ? stock.count : 1) * sizeof *entries);
	bool*       seen    = calloc(stock.count ? stock.count : 1, sizeof *seen);
	if (!entries || !seen) {
		Report(NULL, "out of memory (%s)", name);
		free(entries);
		free(seen);
		XwauText_FreeFile(&mod);
		XwauText_FreeFile(&stock);
		return false;
	}
	for (size_t i = 0; i < stock.count; ++i)
		entries[i] = (StockEntry) { stock.records[i].id, i };
	qsort(entries, stock.count, sizeof *entries, compare_entries);

	int    changed = 0, skipped = 0, missing = 0, free_count = 0;
	size_t last_line                         = 0;
	bool   in_order                          = true;
	bool   free_types[XWAU_TEXT_CRAFT_TYPES] = { false };
	fprintf(f, "  # %s\n", name);
	for (size_t i = 0; i < mod.count; ++i) {
		const XwauTextRecord* record = &mod.records[i];
		const StockEntry      key    = { record->id, 0 };
		const StockEntry*     found  = bsearch(&key, entries, stock.count, sizeof *entries, compare_entries);
		if (!found) {
			Report(mod_path, "%s isn't in the stock file, so the game never reads it; it is skipped",
				   record->id);
			skipped++;
			continue;
		}
		if (seen[found->line]) {
			Report(mod_path, "%s is in the file more than once; only the first text is used", record->id);
			skipped++;
			continue;
		}
		in_order          = in_order && found->line >= last_line;
		last_line         = found->line;
		seen[found->line] = true;
		if (strcmp(record->text, stock.records[found->line].text) == 0)
			continue;
		const int craft_type = craft_type_of_id(record->id);
		if (craft_type >= 0 && is_free_craft_type(craft_type)) {
			free_count += !free_types[craft_type];
			free_types[craft_type] = true;
			skipped++;
			continue;
		}
		if (has_control_character(record->id) || has_control_character(record->text)) {
			Report(mod_path, "the line of %s has a control character; it keeps the stock text", record->id);
			skipped++;
			continue;
		}
		fputs("  ", f);
		if (is_plain_key(record->id))
			fputs(record->id, f);
		else
			XwauText_WriteQuoted(f, record->id);
		fputs(": ", f);
		XwauText_WriteQuoted(f, record->text);
		fputc('\n', f);
		changed++;
	}
	for (size_t i = 0; i < stock.count; ++i)
		missing += !seen[i];
	if (free_count) {
		char   list[XWAU_TEXT_CRAFT_TYPES * 5] = "";
		size_t used                            = 0;
		for (int t = 0; t < XWAU_TEXT_CRAFT_TYPES; ++t)
			if (free_types[t])
				used += (size_t)snprintf(list + used, sizeof list - used, "%s%d", used ? ", " : "", t);
		Report(mod_path,
			   "the ship list and tech room texts of craft types %s are skipped: the stock game has no "
			   "craft for them, so the game would list craft that it can't load",
			   list);
	}
	if (missing)
		Report(mod_path, "%d IDs of the stock file are missing; they keep the stock text", missing);
	if (!in_order)
		Report(mod_path, "has its lines in a different order from the stock file. The game still reads "
						 "the lines in the stock order, and each line gets the mod's text for its ID");
	if (skipped)
		Report(NULL, "%s: %d texts are different from the stock file, and %d lines are skipped", name,
			   changed, skipped);
	else
		Report(NULL, "%s: %d texts are different from the stock file", name, changed);
	*written += changed;
	free(entries);
	free(seen);
	XwauText_FreeFile(&mod);
	XwauText_FreeFile(&stock);
	return true;
}

bool XwauText_Write(const char* mod_root, const char* stock_game, const char* output_file) {
	char        dir[1024];
	const char* slash = strrchr(output_file, '/');
	if (slash &&
		!(PathFormat(dir, sizeof dir, "%.*s", (int)(slash - output_file), output_file) && MakeDirs(dir)))
		return false;
	FILE* f = fopen(output_file, "wb");
	if (!f) {
		Report(output_file, "can't be written");
		return false;
	}
	fprintf(f, "# Written by xwau_import craft: each ID whose text the mod changes in the\n"
			   "# game's text files.\nversion: 1\nstrings:\n");
	bool ok      = true;
	int  written = 0;
	for (size_t i = 0; ok && i < sizeof g_textFiles / sizeof g_textFiles[0]; ++i) {
		char mod_path[1100], stock_path[1100];
		if (!Xwau_FindFile(stock_game, g_textFiles[i], stock_path, sizeof stock_path)) {
			Report(stock_game, "has no %s", g_textFiles[i]);
			ok = false;
		} else if (!Xwau_FindFile(mod_root, g_textFiles[i], mod_path, sizeof mod_path)) {
			Report(mod_root, "has no %s; it is skipped", g_textFiles[i]);
		} else {
			ok = write_file_texts(f, g_textFiles[i], mod_path, stock_path, &written);
		}
	}
	if (written == 0)
		fputs("  {}\n", f);
	if (fclose(f) != 0) {
		Report(output_file, "can't be written");
		ok = false;
	}
	if (!ok) {
		/* Don't leave half a file: the game would get only some of the texts. */
		remove(output_file);
		return false;
	}
	Report(NULL, "%s: %d texts", output_file, written);
	return true;
}
