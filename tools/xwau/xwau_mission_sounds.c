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
	char rel[1200], path[1024];
	if (!Xwau_FindFile(game, "hook_engine_sound.dll", path, sizeof path))
		return true;
	XwauLines lines = { 0 };
	bool      ok =
		PathFormat(rel, sizeof rel, "%s_Sounds.txt", base) &&
		XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP, &lines);
	if (ok && !lines.count) {
		XwauLines_Free(&lines);
		ok = PathFormat(rel, sizeof rel, "%s.ini", base) &&
			 XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "Sounds",
							XWAU_RULES_CPP, &lines);
	}
	size_t missing = 0, voices = 0;
	for (size_t i = 0; ok && i < lines.count; ++i) {
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
		char found[1024];
		if (!to[0] || !Xwau_FindFile(game, to, found, sizeof found)) {
			++missing;
			continue;
		}
		if (hangar_voice(game, stock_game, from)) {
			++voices;
			continue;
		}
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
	if (missing && report)
		Report(base,
			   "[Sounds]: %zu lines name a file that isn't in the installation, so they do nothing; they are "
			   "left out",
			   missing);
	if (voices && report)
		Report(base,
			   "[Sounds]: %zu lines name hangar voices, which the game loads from paths with a '/', so they "
			   "never "
			   "match, in XWAU too; they are left out",
			   voices);
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

void XwauMissionSounds_Write(FILE* f, const XwauMissionSounds* sounds) {
	for (size_t i = 0; i < sounds->count; ++i) {
		fputs(i ? "  - [" : "sounds:\n  - [", f);
		Xwau_WriteString(f, sounds->items[i].from);
		fputs(", ", f);
		Xwau_WriteString(f, sounds->items[i].to);
		fputs("]\n", f);
	}
}
