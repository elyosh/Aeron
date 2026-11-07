#include "xwau_mission_sounds.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"

/* Whether paths `a` and `b` are the same, ignoring case and treating '/' and
 * '\' as the same character. */
static bool same_path(const char* a, const char* b) {
	for (; *a && *b; ++a, ++b) {
		const char ca = *a == '/' ? '\\' : Xwau_Lower(*a);
		const char cb = *b == '/' ? '\\' : Xwau_Lower(*b);
		if (ca != cb)
			return false;
	}
	return *a == *b;
}

/* Whether `from` is a hangar voice. The game loads hangar voices from paths
 * with a '/' (Wave\Rebel_Pilot8/G4P8024.wav). */
static bool hangar_voice(const char* game, const char* stock_game, const char* from) {
	char path[1024];
	if (!Xwau_FindFile(game, "Wave/HANGARVOICESFX.LST", path, sizeof path) &&
		!Xwau_FindFile(stock_game, "Wave/HANGARVOICESFX.LST", path, sizeof path))
		return false;
	XwauLines lines = { 0 };
	XwauLines_Read(path, "", XWAU_RULES_CPP, &lines);
	bool found = false;
	for (size_t i = 0; !found && i < lines.count; ++i) {
		char name[600];
		snprintf(name, sizeof name, "wave\\%s", lines.items[i]);
		/* The game's reader ends the name at the first space or tab. */
		name[strcspn(name, " \t\r\n")] = '\0';
		found                          = strchr(name, '/') && same_path(name, from);
	}
	XwauLines_Free(&lines);
	return found;
}

bool XwauMissionSounds_Read(const char* game, const char* stock_game, const char* base, bool report,
							XwauMissionSounds* out) {
	memset(out, 0, sizeof *out);
	char path[1024];
	/* Without the hook, XWAU reads no line. */
	if (!Xwau_FindFile(game, "hook_engine_sound.dll", path, sizeof path))
		return true;
	XwauSection section;
	bool        ok = XwauSection_ReadMission(game, base, "Sounds", XWAU_RULES_CPP, &section);
	for (size_t i = 0; ok && i < section.lines.count; ++i) {
		/* XWAU ignores a line that isn't a "from = to" line, and the first
		   line for a path wins, even when that line is left out. */
		char from[512], to[512];
		if (!XwauSection_KeyAt(&section, i, from, sizeof from))
			continue;
		XwauSection_Text(&section, from, to, sizeof to);
		/* A line whose file isn't in the installation does nothing, and a
		   hangar voice never matches, in XWAU too. */
		char found[1024];
		if (!to[0] || !Xwau_FindFile(game, to, found, sizeof found)) {
			if (to[0] && stock_game && Xwau_FindFile(stock_game, to, found, sizeof found) &&
				!hangar_voice(game, stock_game, from) && report)
				ReportFatal(base,
							"[Sounds]: '%s' replaces %s with a file of the stock game, which the "
							"importer doesn't copy",
							section.lines.items[i], from);
			continue;
		}
		if (hangar_voice(game, stock_game, from))
			continue;
		XwauMissionSound* item = (XwauMissionSound*)ARRAY_PUSH(out->items, out->count);
		if (!item) {
			ok = false;
			break;
		}
		item->from = StrDup(from);
		item->to   = StrDup(to);
		snprintf(item->to_path, sizeof item->to_path, "%s", found);
		ok = item->from && item->to;
	}
	XwauSection_Free(&section);
	if (!ok)
		Report(NULL, "out of memory");
	return ok;
}

void XwauMissionSounds_Free(XwauMissionSounds* sounds) {
	for (size_t i = 0; i < sounds->count; ++i) {
		free(sounds->items[i].from);
		free(sounds->items[i].to);
	}
	free(sounds->items);
	memset(sounds, 0, sizeof *sounds);
}

/* Writes `path` as a yaml string, with '/' between folders. */
static void write_path(FILE* f, const char* path) {
	char text[512];
	snprintf(text, sizeof text, "%s", path);
	for (char* c = text; *c; ++c)
		if (*c == '\\')
			*c = '/';
	Xwau_WriteString(f, text);
}

void XwauMissionSounds_Write(FILE* f, const XwauMissionSounds* sounds) {
	for (size_t i = 0; i < sounds->count; ++i) {
		fputs(i ? "  - [" : "sounds:\n  - [", f);
		write_path(f, sounds->items[i].from);
		fputs(", ", f);
		write_path(f, sounds->items[i].to);
		fputs("]\n", f);
	}
}
