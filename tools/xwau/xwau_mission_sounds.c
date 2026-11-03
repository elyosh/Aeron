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
	char       rel[1200], path[1024];
	const bool hook  = Xwau_FindFile(game, "hook_engine_sound.dll", path, sizeof path);
	XwauLines  lines = { 0 };
	bool      ok =
		PathFormat(rel, sizeof rel, "%s_Sounds.txt", base) &&
		XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP, &lines);
	if (ok && !lines.count) {
		XwauLines_Free(&lines);
		ok = PathFormat(rel, sizeof rel, "%s.ini", base) &&
			 XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "Sounds",
							XWAU_RULES_CPP, &lines);
	}
	/* Without the hook, XWAU reads no line. */
	if (ok && !hook) {
		XwauLines_Free(&lines);
		return true;
	}
	for (size_t i = 0; ok && i < lines.count; ++i) {
		/* XWAU ignores a line that isn't a "from = to" line. */
		const char* eq = strchr(lines.items[i], '=');
		if (!eq)
			continue;
		char from[512], to[512];
		snprintf(from, sizeof from, "%.*s", (int)(eq - lines.items[i]), lines.items[i]);
		snprintf(to, sizeof to, "%s", eq + 1);
		Xwau_Trim(from, XWAU_RULES_CPP);
		Xwau_Trim(to, XWAU_RULES_CPP);
		if (!from[0])
			continue;
		/* The first line for a path wins, even when that line is left out. */
		bool seen = false;
		for (size_t k = 0; !seen && k < i; ++k) {
			const char* other = strchr(lines.items[k], '=');
			char        name[512];
			if (!other)
				continue;
			snprintf(name, sizeof name, "%.*s", (int)(other - lines.items[k]), lines.items[k]);
			Xwau_Trim(name, XWAU_RULES_CPP);
			seen = Xwau_SameName(name, from);
		}
		if (seen)
			continue;
		/* A line whose file isn't in the installation does nothing, and a
		   hangar voice never matches, in XWAU too. */
		char found[1024];
		if (!to[0] || !Xwau_FindFile(game, to, found, sizeof found)) {
			if (to[0] && stock_game && Xwau_FindFile(stock_game, to, found, sizeof found) &&
				!hangar_voice(game, stock_game, from) && report)
				ReportFatal(base,
							"[Sounds]: '%s' replaces %s with a file of the stock game, which the "
							"importer doesn't copy",
							lines.items[i], from);
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
	XwauLines_Free(&lines);
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
