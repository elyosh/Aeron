#include "xwau_awards.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"

#define HEADER "# Made by the OpenXWA mod importer."

/* The counts that the game keeps: 8 rank thresholds and 25 rating
 * thresholds. */
#define RANK_COUNT 8
#define RATING_COUNT 25
/* The runtime's limits: 20 battle medals, 100 Kalidor levels, and mementos
 * for missions 0 to 255. */
#define BATTLE_MAX 20
#define KALIDOR_MAX 100
#define MEMENTO_MAX 256

/* The Tour of Duty missions that unlock the Lady Blue and the cologne in the
 * stock game. */
enum { STOCK_LADY_BLUE_MISSION = 23, STOCK_COLOGNE_MISSION = 9 };

/* Reads the lines of a name file as std::getline does on Windows: a line
 * ends at '\n', a '\r' before it is dropped, and the line isn't trimmed. An
 * empty line becomes "Unknown". `*found` tells whether the file exists. */
static bool read_names(const char* game, const char* name, XwauLines* out, bool* found) {
	memset(out, 0, sizeof *out);
	char path[1024];
	*found = Xwau_FindFile(game, name, path, sizeof path);
	if (!*found)
		return true;
	size_t size = 0;
	char*  data = ReadWholeFile(path, &size);
	if (!data) {
		Report(path, "cannot be read");
		return false;
	}
	bool ok = true;
	for (size_t start = 0; ok && start < size;) {
		const char* nl  = memchr(data + start, '\n', size - start);
		size_t      end = nl ? (size_t)(nl - data) : size;
		size_t      len = end - start;
		if (nl && len && data[end - 1] == '\r')
			--len;
		ok    = len ? XwauLines_Add(out, data + start, len) : XwauLines_Add(out, "Unknown", 7);
		start = end + 1;
	}
	free(data);
	if (!ok)
		Report(NULL, "out of memory (%s)", name);
	return ok;
}

/* Reads a list of numbers as the hook does (std::stoi of each line). A value
 * that isn't a number stops the hook, so it leaves the whole list out. A
 * value of 0 or less isn't a threshold that the game takes, and makes the
 * import fail. `*count` is 0 when the file doesn't exist or is left out.
 * `game_limit` tells whether the stock game also keeps only `max` values,
 * so that the values after them do nothing in XWAU either. */
static bool read_values(const char* game, const char* name, long long* values, size_t max, bool game_limit,
						size_t* count) {
	*count = 0;
	char path[1024];
	if (!Xwau_FindFile(game, name, path, sizeof path))
		return true;
	XwauLines lines;
	if (!XwauLines_Read(path, "", XWAU_RULES_CPP, &lines)) {
		Report(NULL, "out of memory (%s)", name);
		return false;
	}
	size_t n = 0;
	for (size_t i = 0; i < lines.count; ++i) {
		long long value;
		if (!Xwau_Stoi(lines.items[i], &value)) {
			Report(path, "'%s' isn't a number, which XWAU would fail on. The file is left out",
				   lines.items[i]);
			XwauLines_Free(&lines);
			return true;
		}
		if (value <= 0) {
			ReportFatal(path, "'%s' isn't above 0, and the game only takes thresholds above 0",
						lines.items[i]);
			XwauLines_Free(&lines);
			return true;
		}
		if (n < max)
			values[n++] = value;
	}
	if (lines.count > max && game_limit)
		Report(path, "%zu values. The game uses the first %zu, in XWAU too", lines.count, max);
	else if (lines.count > max)
		ReportFatal(path, "%zu values, and OpenXWA takes at most %zu", lines.count, max);
	XwauLines_Free(&lines);
	*count = n;
	return true;
}

/* Whether the installation has the image at `rel`. The game loads a .bmp
 * path from the .cbm file of the same name. */
static bool image_exists(const char* game, const char* rel) {
	char        found[1024], cbm[1024];
	const char* dot = strrchr(rel, '.');
	return Xwau_FindFile(game, rel, found, sizeof found) ||
		   (dot && PathFormat(cbm, sizeof cbm, "%.*s.cbm", (int)(dot - rel), rel) &&
			Xwau_FindFile(game, cbm, found, sizeof found));
}

static bool is_null_name(const char* name) { return strcmp(name, "Null") == 0 || strcmp(name, "null") == 0; }

static void write_values(FILE* f, const char* key, const long long* values, size_t count) {
	fprintf(f, "%s: [", key);
	for (size_t i = 0; i < count; ++i)
		fprintf(f, "%s%lld", i ? ", " : "", values[i]);
	fprintf(f, "]\n");
}

/* Returns the Tour of Duty mission that unlocks a family-room item: the
   value of `key` in ConcourseDoors.txt, or `stock` if the key is missing. A
   value that isn't a number makes the import fail. */
static long long unlock_mission(const XwauLines* doors, const char* key, long long stock) {
	char      value[64];
	long long mission;
	XwauLines_Value(doors, key, XWAU_RULES_CPP, value, sizeof value);
	if (!value[0])
		return stock;
	if (Xwau_Stoi(value, &mission))
		return mission;
	Report("ConcourseDoors.txt", "%s = '%s' isn't a number, so it is left out", key, value);
	return stock;
}

bool XwauAwards_Write(const char* game, const char* path) {
	char dll[1024];
	if (!Xwau_FindFile(game, "hook_concourse.dll", dll, sizeof dll)) {
		static const char* const kFiles[] = { "MedalsDescriptions.txt",
											  "KalidorCrescentDescriptions.txt",
											  "FamilyMedalsDescriptions.txt",
											  "KalidorCrescentPoints.txt",
											  "RankPoints.txt",
											  "PilotRatingPoints.txt",
											  "MedalDetailList.txt",
											  "ConcourseDoors.txt" };
		for (size_t i = 0; i < sizeof kFiles / sizeof kFiles[0]; ++i) {
			char found[1024];
			if (Xwau_FindFile(game, kFiles[i], found, sizeof found))
				Report(kFiles[i], "the mod has no hook_concourse.dll, so XWAU never reads this file");
		}
		return true;
	}
	XwauLines  medals = { 0 }, kalidor_names = { 0 }, memento_names = { 0 }, images = { 0 }, doors = { 0 };
	bool       has_medals = false, has_kalidor_names = false, has_memento_names = false;
	long long  kalidor_points[KALIDOR_MAX], ranks[RANK_COUNT], ratings[RATING_COUNT];
	size_t     kalidor_count = 0, rank_count = 0, rating_count = 0;
	char       images_path[1024];
	const bool has_images = Xwau_FindFile(game, "MedalDetailList.txt", images_path, sizeof images_path);
	bool       ok =
		read_names(game, "MedalsDescriptions.txt", &medals, &has_medals) &&
		read_names(game, "KalidorCrescentDescriptions.txt", &kalidor_names, &has_kalidor_names) &&
		read_names(game, "FamilyMedalsDescriptions.txt", &memento_names, &has_memento_names) &&
		read_values(game, "KalidorCrescentPoints.txt", kalidor_points, KALIDOR_MAX, false, &kalidor_count) &&
		read_values(game, "RankPoints.txt", ranks, RANK_COUNT, true, &rank_count) &&
		read_values(game, "PilotRatingPoints.txt", ratings, RATING_COUNT, true, &rating_count);
	if (ok && has_images && !(ok = XwauLines_Read(images_path, "", XWAU_RULES_CPP, &images)))
		Report(NULL, "out of memory (MedalDetailList.txt)");
	char doors_path[1024];
	if (ok && Xwau_FindFile(game, "ConcourseDoors.txt", doors_path, sizeof doors_path) &&
		!(ok = XwauLines_Read(doors_path, "", XWAU_RULES_CPP, &doors)))
		Report(NULL, "out of memory (ConcourseDoors.txt)");
	const long long lady_blue =
		unlock_mission(&doors, "familyroom_ladyblue_mission", STOCK_LADY_BLUE_MISSION);
	const long long cologne     = unlock_mission(&doors, "familyroom_cologne_mission", STOCK_COLOGNE_MISSION);
	const bool      unlocks     = lady_blue != STOCK_LADY_BLUE_MISSION || cologne != STOCK_COLOGNE_MISSION;
	size_t medal_count = medals.count;
	if (ok && medal_count > BATTLE_MAX) {
		ReportFatal("MedalsDescriptions.txt", "%zu names, and OpenXWA has at most %d battle medals",
					medal_count, BATTLE_MAX);
		medal_count = BATTLE_MAX;
	}

	/* Without KalidorCrescentPoints.txt, the hook keeps the 6 stock levels. */
	const size_t kalidor_levels = kalidor_count ? kalidor_count : (has_kalidor_names ? 6 : 0);
	size_t       memento_count  = images.count > memento_names.count ? images.count : memento_names.count;
	if (memento_count > MEMENTO_MAX) {
		ReportFatal("mementos", "%zu missions, and OpenXWA has mementos for at most %d missions",
					memento_count, MEMENTO_MAX);
		memento_count = MEMENTO_MAX;
	}
	size_t missing = 0;
	if (ok && (medal_count || kalidor_levels || rank_count || rating_count || has_images ||
			   memento_names.count || unlocks)) {
		char  dir[1024];
		char* slash = strrchr(path, '/');
		ok      = !slash || (PathFormat(dir, sizeof dir, "%.*s", (int)(slash - path), path) && MakeDirs(dir));
		FILE* f = ok ? fopen(path, "w") : NULL;
		if (!f) {
			Report(NULL, "cannot write %s", path);
			ok = false;
		} else {
			fprintf(f, "%s\nversion: 1\n", HEADER);
			if (medal_count) {
				fprintf(f, "battle_medals:\n");
				for (size_t i = 0; i < medal_count; ++i) {
					fprintf(f, "  - { name: ");
					Xwau_WriteString(f, medals.items[i]);
					fprintf(f, " }\n");
				}
			}
			if (kalidor_levels) {
				fprintf(f, "kalidor:\n");
				for (size_t i = 0; i < kalidor_levels; ++i) {
					const bool named = i < kalidor_names.count;
					fprintf(f, "  - {");
					if (kalidor_count)
						fprintf(f, " points: %lld%s", kalidor_points[i], named ? "," : "");
					if (named) {
						fprintf(f, " name: ");
						Xwau_WriteString(f, kalidor_names.items[i]);
					}
					fprintf(f, " }\n");
				}
			}
			if (rank_count)
				write_values(f, "rank_thresholds", ranks, rank_count);
			if (rating_count)
				write_values(f, "rating_thresholds", ratings, rating_count);
			if (has_images || memento_names.count || unlocks) {
				fprintf(f, "mementos:\n");
				if (has_images) {
					size_t written = 0;
					for (size_t i = 0; i < images.count && i < memento_count; ++i) {
						if (strcmp(images.items[i], "null") == 0)
							continue;
						if (!image_exists(game, images.items[i])) {
							Report(NULL, "MedalDetailList.txt: mission %zu: %s isn't in the installation", i,
								   images.items[i]);
							++missing;
						}
						fprintf(f, "%s    %zu: ", written++ ? "" : "  images:\n", i);
						Xwau_WriteString(f, images.items[i]);
						fprintf(f, "\n");
					}
					/* An empty map still replaces the stock images. */
					if (!written)
						fprintf(f, "  images: {}\n");
				}
				size_t written = 0;
				for (size_t i = 0; i < memento_names.count && i < memento_count; ++i) {
					if (is_null_name(memento_names.items[i]))
						continue;
					fprintf(f, "%s    %zu: ", written++ ? "" : "  names:\n", i);
					Xwau_WriteString(f, memento_names.items[i]);
					fprintf(f, "\n");
				}
				if (lady_blue != STOCK_LADY_BLUE_MISSION)
					fprintf(f, "  lady_blue: { mission: %lld }\n", lady_blue);
				if (cologne != STOCK_COLOGNE_MISSION)
					fprintf(f, "  cologne: { mission: %lld }\n", cologne);
			}
			if (fclose(f) != 0) {
				Report(NULL, "cannot write %s", path);
				ok = false;
			}
		}
		if (ok)
			Report(NULL,
				   "awards: %zu battle medal names, %zu Kalidor levels, %zu rank and %zu rating thresholds, "
				   "%s mementos; %zu memento images missing; written to %s",
				   medal_count, kalidor_levels, rank_count, rating_count, has_images ? "replaced" : "stock",
				   missing, path);
	}
	XwauLines_Free(&medals);
	XwauLines_Free(&kalidor_names);
	XwauLines_Free(&memento_names);
	XwauLines_Free(&images);
	XwauLines_Free(&doors);
	return ok;
}
