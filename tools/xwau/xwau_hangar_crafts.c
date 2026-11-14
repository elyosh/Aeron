#include "xwau_hangar_crafts.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"

/* OpenXWA keeps at most this many craft in a list (XWA_HANGAR_CRAFTS_MAX in
   xwa_runtime/models/hangar_crafts.h). XWAU has no limit. */
#define HANGAR_CRAFTS_MAX 49

static const struct {
	const char* file;
	const char* key;
} kLists[] = {
	{ "CraftSelectionFighters.txt", "fighters" },
	{ "CraftSelectionTransports.txt", "transports" },
	{ "CraftSelectionCrafts.txt", "all" },
};

/* Reads `line` as the hook does, with (unsigned short)std::stoi(line).
 * Returns false when std::stoi would throw: no number at the start, or a
 * number out of the int range. */
static bool read_index(const char* line, int* out) {
	char* end;
	errno            = 0;
	const long value = strtol(line, &end, 10);
	if (end == line || errno == ERANGE || value < INT_MIN || value > INT_MAX)
		return false;
	*out = (unsigned short)(int)value;
	return true;
}

/* Writes the list of the file at `path` as `key`. */
static void write_list(FILE* f, const char* label, const char* path, const char* key, const XwauNames* names,
					   bool* ok) {
	XwauLines lines = { 0 };
	if (!XwauLines_Read(path, "", XWAU_RULES_CPP, &lines)) {
		*ok = false;
		return;
	}
	int    types[HANGAR_CRAFTS_MAX];
	size_t count = 0;
	for (size_t i = 0; i < lines.count; ++i) {
		int type;
		if (!read_index(lines.items[i], &type)) {
			Report(label, "'%s' isn't a model index, and XWAU fails to start with it. It is left out",
				   lines.items[i]);
			continue;
		}
		const char* name = XwauNames_ObjectType(names, type);
		if (type == 0 || !name) {
			Report(label, "%d isn't an object type. It is left out", type);
			continue;
		}
		bool repeated = false;
		for (size_t j = 0; j < count && !repeated; ++j)
			repeated = types[j] == type;
		if (repeated) {
			ReportDetail(label, "%s is listed twice. XWAU shows it twice, and OpenXWA once", name);
			continue;
		}
		if (count == HANGAR_CRAFTS_MAX) {
			Report(label, "lists more than %d craft. OpenXWA shows the first %d", HANGAR_CRAFTS_MAX,
				   HANGAR_CRAFTS_MAX);
			break;
		}
		types[count++] = type;
	}
	XwauLines_Free(&lines);
	fprintf(f, "  %s: [", key);
	for (size_t i = 0; i < count; ++i)
		fprintf(f, "%s%s", i ? ", " : "", XwauNames_ObjectType(names, types[i]));
	fputs("]\n", f);
}

bool XwauHangarCrafts_Write(FILE* f, const char* game, const XwauNames* names) {
	char dll[1024];
	if (!Xwau_FindFile(game, "hook_hangars.dll", dll, sizeof dll))
		return false;
	bool any = false, ok = true;
	for (size_t i = 0; i < sizeof kLists / sizeof kLists[0]; ++i) {
		char path[1024];
		if (!Xwau_FindFile(game, kLists[i].file, path, sizeof path))
			continue;
		if (!any)
			fputs("hangar_crafts:\n", f);
		any = true;
		write_list(f, kLists[i].file, path, kLists[i].key, names, &ok);
	}
	if (!ok)
		Report(NULL, "out of memory while reading the craft selection files");
	return any;
}
