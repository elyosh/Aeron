#include "xwau_game_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"
#include "xwau_mission_sounds.h"
#include "xwau_sound_banks.h"

static const char* const kMissionDirs[] = { "Missions", "Melee", "Combat" };

typedef struct Copy {
	const char* game;
	const char* stock_game;
	const char* out;
	size_t      missions, lists, sounds, music, replacements, converted;
	XwauLines   targets; /* the sound replacement files seen */
	XwauLines   missing_stock_files; /* the stock files that the installation doesn't have */
} Copy;

/* The path of `path` relative to the installation `game`. `path` comes from
 * Xwau_FindFile or Xwau_FindDir, so it starts with `game`. */
static const char* relative(const char* game, const char* path) {
	const char* rel = path + strlen(game);
	while (*rel == '/')
		++rel;
	return rel;
}

/* Whether the stock game has file `rel` with the same bytes as `data`. */
static bool same_as_stock(const Copy* c, const char* rel, const char* data, size_t size) {
	char path[1024];
	if (!Xwau_FindFile(c->stock_game, rel, path, sizeof path))
		return false;
	size_t     stock_size = 0;
	char*      stock      = ReadWholeFile(path, &stock_size);
	const bool same       = stock && stock_size == size && memcmp(stock, data, size) == 0;
	free(stock);
	return same;
}

/* Copies the file at `path` to <out>/<rel>. With `if_changed`, a file that
 * the stock game has with the same bytes is skipped. Sets `*copied`. */
static bool copy_file(Copy* c, const char* path, bool if_changed, bool* copied) {
	*copied          = false;
	const char* rel  = relative(c->game, path);
	size_t      size = 0;
	char*       data = ReadWholeFile(path, &size);
	if (!data) {
		Report(path, "cannot be read");
		return false;
	}
	if (if_changed && same_as_stock(c, rel, data, size)) {
		free(data);
		return true;
	}
	/* A sound that the game can't play is converted to 16-bit PCM. */
	uint8_t* converted      = NULL;
	size_t   converted_size = 0;
	const XwauWavResult wav =
		EndsWithNoCase(rel, ".wav")
			? XwauSoundBanks_ConvertWav((const uint8_t*)data, size, &converted, &converted_size)
			: XWAU_WAV_PLAYABLE;
	if (wav == XWAU_WAV_CONVERTED)
		++c->converted;
	else if (wav == XWAU_WAV_BAD)
		ReportFatal(path,
					"isn't a WAV file that the import can read or convert to a sound the game can play");
	char        target[2048], dir[2048];
	bool        ok    = PathFormat(target, sizeof target, "%s/%s", c->out, rel);
	const char* slash = ok ? strrchr(target, '/') : NULL;
	ok = ok && slash && PathFormat(dir, sizeof dir, "%.*s", (int)(slash - target), target) && MakeDirs(dir);
	if (ok && !(ok = converted ? WriteWholeFile(target, converted, converted_size)
							   : WriteWholeFile(target, data, size)))
		Report(NULL, "cannot write %s", target);
	free(data);
	free(converted);
	*copied = ok;
	return ok;
}

/* Copies the files of folder `dir` and of its folders that differ from the
 * stock game's. */
static bool copy_changed_tree(Copy* c, const char* dir) {
	size_t count   = 0;
	char** entries = ListDir(dir, true, &count);
	bool   ok      = true;
	for (size_t i = 0; ok && i < count; ++i) {
		char path[2048];
		if (entries[i][0] == '.')
			continue;
		if (!(ok = PathFormat(path, sizeof path, "%s/%s", dir, entries[i])))
			break;
		if (IsDir(path)) {
			ok = copy_changed_tree(c, path);
		} else if (IsFile(path)) {
			bool copied = false;
			ok          = copy_file(c, path, true, &copied);
			c->sounds += copied;
		}
	}
	FreeList(entries, count);
	return ok;
}

/* Copies the files that the list Wave/<name> names and that the stock game
 * doesn't have. Each line starts with a path relative to Wave/. */
static bool copy_listed_sounds(Copy* c, const char* wave, const char* name, XwauLines* named) {
	char   path[2048];
	size_t size = 0;
	char*  data = PathFormat(path, sizeof path, "%s/%s", wave, name) ? ReadWholeFile(path, &size) : NULL;
	bool   ok   = true;
	if (!data) {
		Report(path, "cannot be read");
		return false;
	}
	for (size_t start = 0; ok && start < size;) {
		size_t end = start;
		while (end < size && data[end] != '\n' && data[end] != '\r')
			++end;
		/* The path ends at the first space, tab or ';'. */
		size_t length = 0;
		while (start + length < end && data[start + length] != ' ' && data[start + length] != '\t' &&
			   data[start + length] != ';')
			++length;
		char rel[600], stock[1024], file[1024];
		if (length > 0 && PathFormat(rel, sizeof rel, "Wave/%.*s", (int)length, data + start)) {
			for (char* p = rel; *p; ++p)
				if (*p == '\\')
					*p = '/';
			bool seen = false;
			for (size_t k = 0; !seen && k < named->count; ++k)
				seen = strcasecmp(named->items[k], rel) == 0;
			/* A path with ".." could name a file outside Wave/. */
			if (!seen && !strstr(rel, "..") && !Xwau_FindFile(c->stock_game, rel, stock, sizeof stock) &&
				Xwau_FindFile(c->game, rel, file, sizeof file)) {
				bool copied = false;
				ok          = copy_file(c, file, true, &copied) && XwauLines_Add(named, rel, strlen(rel));
				c->sounds += copied;
			}
		}
		start = end;
		while (start < size && (data[start] == '\n' || data[start] == '\r'))
			++start;
	}
	free(data);
	return ok;
}

/* Copies the files in Wave/ that differ from the stock game's: the stock
 * files directly in Wave/, every file in a folder that the stock game also
 * has, and the files that the lists in Wave/ name. The game doesn't read the
 * other files. The craft phase copies the sound banks (Wave/Sfx_*). */
static bool copy_wave(Copy* c) {
	char wave[1024];
	if (!Xwau_FindDir(c->game, "Wave", wave, sizeof wave))
		return true;
	size_t count   = 0;
	char** entries = ListDir(wave, true, &count);
	bool   ok      = true;
	for (size_t i = 0; ok && i < count; ++i) {
		char path[2048], rel[600], stock[1024];
		if (entries[i][0] == '.')
			continue;
		if (!(ok = PathFormat(path, sizeof path, "%s/%s", wave, entries[i]) &&
				   PathFormat(rel, sizeof rel, "Wave/%s", entries[i])))
			break;
		if (IsDir(path)) {
			if (Xwau_FindDir(c->stock_game, rel, stock, sizeof stock))
				ok = copy_changed_tree(c, path);
		} else if (IsFile(path) && Xwau_FindFile(c->stock_game, rel, stock, sizeof stock)) {
			bool copied = false;
			ok          = copy_file(c, path, true, &copied);
			c->sounds += copied;
		}
	}
	XwauLines named = { 0 };
	for (size_t i = 0; ok && i < count; ++i)
		if (EndsWithNoCase(entries[i], ".lst") && strncasecmp(entries[i], "Sfx_", 4) != 0)
			ok = copy_listed_sounds(c, wave, entries[i], &named);
	XwauLines_Free(&named);
	FreeList(entries, count);
	return ok;
}

/* Copies the music files in Music/ that differ from the stock game's. The
 * game only reads the stock files, so the other files are skipped. */
static bool copy_music(Copy* c) {
	char music[1024];
	if (!Xwau_FindDir(c->game, "Music", music, sizeof music))
		return true;
	size_t count   = 0;
	char** entries = ListDir(music, true, &count);
	bool   ok      = true;
	for (size_t i = 0; ok && i < count; ++i) {
		char path[2048], rel[600], stock[1024];
		if (entries[i][0] == '.' || !EndsWithNoCase(entries[i], ".imc"))
			continue;
		if (!(ok = PathFormat(path, sizeof path, "%s/%s", music, entries[i]) &&
				   PathFormat(rel, sizeof rel, "Music/%s", entries[i])))
			break;
		if (IsFile(path) && Xwau_FindFile(c->stock_game, rel, stock, sizeof stock)) {
			bool copied = false;
			ok          = copy_file(c, path, true, &copied);
			c->music += copied;
		}
	}
	FreeList(entries, count);
	return ok;
}

/* Adds every file in the stock game's folder `rel`, and in its subfolders,
 * that the installation doesn't have to `missing_stock_files`. TFTC moves the
 * stock Wave/Frontend folder aside and puts its own in its place, so the
 * stock voices that it leaves out are missing from the game it plays. */
static bool find_missing_stock_files(Copy* c, const char* rel) {
	char stock[1024];
	if (!Xwau_FindDir(c->stock_game, rel, stock, sizeof stock))
		return true;
	size_t count   = 0;
	char** entries = ListDir(stock, true, &count);
	bool   ok      = true;
	for (size_t i = 0; ok && i < count; ++i) {
		char path[2048], entry[1200], found[1024];
		if (entries[i][0] == '.')
			continue;
		if (!(ok = PathFormat(path, sizeof path, "%s/%s", stock, entries[i]) &&
				   PathFormat(entry, sizeof entry, "%s/%s", rel, entries[i])))
			break;
		if (IsDir(path))
			ok = find_missing_stock_files(c, entry);
		else if (IsFile(path) && !Xwau_FindFile(c->game, entry, found, sizeof found))
			ok = XwauLines_Add(&c->missing_stock_files, entry, strlen(entry));
	}
	FreeList(entries, count);
	return ok;
}

/* Writes the missing stock files to <out>/files.yaml under `removed`, so
 * that the game doesn't use them. Writes nothing when there are none. */
static bool write_removed_files(const Copy* c) {
	if (c->missing_stock_files.count == 0)
		return true;
	char path[2048];
	if (!PathFormat(path, sizeof path, "%s/files.yaml", c->out) || !MakeDirs(c->out))
		return false;
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "can't be written");
		return false;
	}
	fprintf(f, "# Made by the OpenXWA mod importer.\n"
			   "# The original game files that the installation doesn't have.\n"
			   "removed:\n");
	for (size_t i = 0; i < c->missing_stock_files.count; ++i) {
		fputs("  - ", f);
		Xwau_WriteString(f, c->missing_stock_files.items[i]);
		fputc('\n', f);
	}
	if (fclose(f) != 0) {
		Report(path, "can't be written");
		return false;
	}
	return true;
}

/* Copies the files that the sound replacements of mission `base` load, if
 * they differ from the stock game's. */
static bool copy_mission_sounds(Copy* c, const char* base) {
	XwauMissionSounds sounds;
	bool              ok = XwauMissionSounds_Read(c->game, c->stock_game, base, false, &sounds);
	for (size_t i = 0; ok && i < sounds.count; ++i) {
		const char* rel  = relative(c->game, sounds.items[i].to_path);
		bool        seen = false;
		for (size_t k = 0; !seen && k < c->targets.count; ++k)
			seen = strcasecmp(c->targets.items[k], rel) == 0;
		if (seen)
			continue;
		bool copied = false;
		ok          = copy_file(c, sounds.items[i].to_path, true, &copied) &&
					  XwauLines_Add(&c->targets, rel, strlen(rel));
		c->replacements += copied;
	}
	XwauMissionSounds_Free(&sounds);
	return ok;
}

/* Copies a list file, when the mod has it and it differs from the stock
 * game's. */
static bool copy_list(Copy* c, const char* rel) {
	char path[1024];
	if (!Xwau_FindFile(c->game, rel, path, sizeof path))
		return true;
	bool copied = false;
	bool ok     = copy_file(c, path, true, &copied);
	c->lists += copied;
	return ok;
}

bool XwauGameFiles_Copy(const char* game, const char* stock_game, const char* out) {
	Copy c  = { .game = game, .stock_game = stock_game, .out = out };
	bool ok = true;
	for (size_t d = 0; ok && d < sizeof kMissionDirs / sizeof kMissionDirs[0]; ++d) {
		char dir[1024];
		if (!Xwau_FindDir(game, kMissionDirs[d], dir, sizeof dir))
			continue;
		size_t count   = 0;
		char** entries = ListDir(dir, true, &count);
		for (size_t i = 0; ok && i < count; ++i) {
			char path[2048];
			/* A mission's sound replacements are in <mission>.ini or
			   <mission>_Sounds.txt. */
			const size_t length = strlen(entries[i]);
			const size_t suffix = EndsWithNoCase(entries[i], ".ini")          ? 4
								  : EndsWithNoCase(entries[i], "_sounds.txt") ? 11
																			  : 0;
			if (suffix && length > suffix) {
				char base[1200];
				ok = PathFormat(base, sizeof base, "%s/%.*s", kMissionDirs[d], (int)(length - suffix),
								entries[i]) &&
					 copy_mission_sounds(&c, base);
				continue;
			}
			if (!EndsWithNoCase(entries[i], ".tie"))
				continue;
			if (!(ok = PathFormat(path, sizeof path, "%s/%s", dir, entries[i])))
				break;
			bool copied = false;
			if (IsFile(path)) {
				ok = copy_file(&c, path, false, &copied);
				c.missions += copied;
			}
		}
		FreeList(entries, count);
		char rel[64];
		ok = ok && PathFormat(rel, sizeof rel, "%s/MISSION.LST", kMissionDirs[d]) && copy_list(&c, rel);
	}
	ok = ok && copy_list(&c, "Missions/EMAIL.TXT") && copy_list(&c, "Frontres/Cutscene/cutscene.txt");
	/* The battle medals, the medal images and the memento texts. */
	ok = ok && copy_list(&c, "Frontres/MEDALS/MEDALS.TXT") && copy_list(&c, "Frontres/MEDALS/MEDALS.LST") &&
		 copy_list(&c, "Frontres/MEDALS/FAMILYAWARDS.TXT");
	/* An installation with no Wave folder is incomplete. It doesn't mean
	   that every sound is removed. */
	char wave[1024];
	ok = ok && copy_music(&c) && copy_wave(&c) &&
		 (!Xwau_FindDir(game, "Wave", wave, sizeof wave) || find_missing_stock_files(&c, "Wave")) && write_removed_files(&c);
	if (ok)
		ReportDetail(
			NULL,
			"copied %zu mission files, %zu changed lists, %zu changed sound files, %zu changed music files "
			"and %zu files of the missions' sound replacements into %s; %zu sounds converted to 16-bit "
			"PCM; %zu stock sound files removed",
			c.missions, c.lists, c.sounds, c.music, c.replacements, out, c.converted,
			c.missing_stock_files.count);
	XwauLines_Free(&c.targets);
	XwauLines_Free(&c.missing_stock_files);
	return ok;
}
