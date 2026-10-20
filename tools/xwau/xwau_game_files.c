#include "xwau_game_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"

static const char* const kMissionDirs[] = { "Missions", "Melee", "Combat" };
static const char* const kVoiceDirs[]   = { "Wave/MISSIONVOICE", "Wave/FRONTEND" };

typedef struct Copy {
	const char* game;
	const char* stock_game;
	const char* out;
	size_t      missions, lists, voices;
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
static bool copy_file(const Copy* c, const char* path, bool if_changed, bool* copied) {
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
	char        target[2048], dir[2048];
	bool        ok    = PathFormat(target, sizeof target, "%s/%s", c->out, rel);
	const char* slash = ok ? strrchr(target, '/') : NULL;
	ok = ok && slash && PathFormat(dir, sizeof dir, "%.*s", (int)(slash - target), target) && MakeDirs(dir);
	if (ok && !(ok = WriteWholeFile(target, data, size)))
		Report(NULL, "cannot write %s", target);
	free(data);
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
		if (!(ok = PathFormat(path, sizeof path, "%s/%s", dir, entries[i])))
			break;
		if (IsDir(path)) {
			ok = copy_changed_tree(c, path);
		} else if (IsFile(path)) {
			bool copied = false;
			ok          = copy_file(c, path, true, &copied);
			c->voices += copied;
		}
	}
	FreeList(entries, count);
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
	for (size_t d = 0; ok && d < sizeof kVoiceDirs / sizeof kVoiceDirs[0]; ++d) {
		char dir[1024];
		if (Xwau_FindDir(game, kVoiceDirs[d], dir, sizeof dir))
			ok = copy_changed_tree(&c, dir);
	}
	if (ok)
		Report(NULL, "copied %zu mission files, %zu changed lists and %zu changed voice files into %s",
			   c.missions, c.lists, c.voices, out);
	return ok;
}
