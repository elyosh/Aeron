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
	size_t      missions, lists, sounds, replacements, converted;
	XwauLines   targets; /* the sound replacement files seen */
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
	if (EndsWithNoCase(rel, ".wav") && XwauSoundBanks_ConvertWav((const uint8_t*)data, size, &converted,
																 &converted_size) == XWAU_WAV_CONVERTED)
		++c->converted;
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

/* Copies the files in Wave/ that differ from the stock game's. This covers
 * the stock files directly in Wave/, and every file in a folder that the
 * stock game also has. Other files and folders are skipped, because the game
 * doesn't read them. xwau_import craft copies the sound banks (Wave/Sfx_*). */
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
	FreeList(entries, count);
	return ok;
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
	ok = ok && copy_wave(&c);
	if (ok)
		Report(NULL,
			   "copied %zu mission files, %zu changed lists, %zu changed sound files and %zu files of the "
			   "missions' sound replacements into %s; %zu sounds converted to 16-bit PCM",
			   c.missions, c.lists, c.sounds, c.replacements, out, c.converted);
	XwauLines_Free(&c.targets);
	return ok;
}
