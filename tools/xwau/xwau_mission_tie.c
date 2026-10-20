#include "xwau_mission_tie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static int compare_flight_groups(const void* a, const void* b) {
	const int x = ((const XwauMissionTieFlightGroup*)a)->fg, y = ((const XwauMissionTieFlightGroup*)b)->fg;
	return (x > y) - (x < y);
}

/* The values of flight group `fg`, added when missing. NULL when out of
 * memory. */
static XwauMissionTieFlightGroup* flight_group(XwauMissionTie* tie, int fg) {
	for (size_t i = 0; i < tie->fg_count; ++i)
		if (tie->fgs[i].fg == fg)
			return &tie->fgs[i];
	XwauMissionTieFlightGroup* added = ARRAY_PUSH(tie->fgs, tie->fg_count);
	if (added)
		added->fg = fg;
	return added;
}

/* Reads word `index` of `words` as std::stoi does into `out`. When it isn't
 * a number, reports it, since XWAU would fail on it. */
static bool number(const XwauLines* words, size_t index, const char* line, const char* base, bool report,
				   long long* out) {
	if (Xwau_Stoi(words->items[index], out))
		return true;
	if (report)
		Report(base,
			   "mission_tie: '%s' isn't a number in line '%s', which XWAU would fail on; the line is skipped",
			   words->items[index], line);
	return false;
}

/* Reads an "fg, N, …" line. Returns false when out of memory. */
static bool read_fg_line(XwauMissionTie* tie, const XwauLines* words, size_t line_number, const char* line,
						 const char* base, bool report) {
	if (words->count < 4) {
		if (report)
			Report(base, "mission_tie: line '%s' has fewer than 4 words, so XWAU ignores it", line);
		return true;
	}
	long long   fg  = 0;
	const char* key = words->items[2];
	if (!number(words, 1, line, base, report, &fg))
		return true;
	if (Xwau_SameName(key, "index")) {
		long long craft = 0, markings = 0;
		if (words->count < 6 || !Xwau_SameName(words->items[4], "markings"))
			return true; /* an index line for another hook */
		if (!number(words, 3, line, base, report, &craft) || !number(words, 5, line, base, report, &markings))
			return true;
		for (size_t i = 0; i < tie->craft_marking_count; ++i) {
			XwauMissionTieCraftMarkings* entry = &tie->craft_markings[i];
			if (entry->fg == fg && entry->craft == craft) {
				entry->markings = (int)markings;
				return true;
			}
		}
		XwauMissionTieCraftMarkings* added = ARRAY_PUSH(tie->craft_markings, tie->craft_marking_count);
		if (!added)
			return false;
		*added = (XwauMissionTieCraftMarkings) { (int)fg, (int)craft, (int)markings };
		return true;
	}
	const bool markings = Xwau_SameName(key, "markings"), iff = Xwau_SameName(key, "iff");
	const bool voice  = Xwau_SameName(key, "pilotvoice");
	const bool sfoils = Xwau_SameName(key, "close_SFoils"), gear = Xwau_SameName(key, "open_LandingGears");
	if (!markings && !iff && !voice && !sfoils && !gear) {
		/* Other keys are for other hooks, but a number is no key at all: a
		   comma is missing. */
		if (report && ((key[0] >= '0' && key[0] <= '9') || key[0] == '-'))
			Report(base,
				   "mission_tie: line '%s' has the number '%s' where its key should be, so XWAU ignores it",
				   line, key);
		return true;
	}
	long long value = 0;
	if (!voice && !number(words, 3, line, base, report, &value))
		return true;
	XwauMissionTieFlightGroup* entry = flight_group(tie, (int)fg);
	if (!entry)
		return false;
	if (markings) {
		entry->has_markings = true;
		entry->markings     = (int)value;
	} else if (iff) {
		entry->has_iff = true;
		entry->iff     = (int)value;
	} else if (voice) {
		entry->has_pilot_voice = true;
		snprintf(entry->pilot_voice, sizeof entry->pilot_voice, "%s", words->items[3]);
	} else if (value == 1 && sfoils && !entry->close_sfoils) {
		entry->close_sfoils = line_number + 1;
	} else if (value == 1 && gear && !entry->open_landing_gear) {
		entry->open_landing_gear = line_number + 1;
	}
	return true;
}

bool XwauMissionTie_Read(const char* game, const char* base, bool report, XwauMissionTie* out) {
	memset(out, 0, sizeof *out);
	char      rel[1100], path[1024];
	XwauLines lines;
	if (!PathFormat(rel, sizeof rel, "%s.txt", base) ||
		!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP,
						&lines))
		return false;
	bool ok = true;
	if (!lines.count) {
		ok = PathFormat(rel, sizeof rel, "%s.ini", base) &&
			 XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "mission_tie",
							XWAU_RULES_CPP, &lines);
	}
	for (size_t i = 0; ok && i < lines.count; ++i) {
		const char* line = lines.items[i];
		XwauLines   words;
		if (!(ok = Xwau_Tokenize(line, XWAU_RULES_CPP, &words)))
			break;
		if (words.count && Xwau_SameName(words.items[0], "fg"))
			ok = read_fg_line(out, &words, i, line, base, report);
		else if (words.count && Xwau_SameName(words.items[0], "craft"))
			ok = XwauLines_Add(&out->craft_lines, line, strlen(line));
		else if (strchr(line, '='))
			ok = XwauLines_Add(&out->settings, line, strlen(line));
		XwauLines_Free(&words);
	}
	XwauLines_Free(&lines);
	if (ok && out->fg_count > 1)
		qsort(out->fgs, out->fg_count, sizeof *out->fgs, compare_flight_groups);
	if (!ok)
		XwauMissionTie_Free(out);
	return ok;
}

void XwauMissionTie_Free(XwauMissionTie* tie) {
	free(tie->fgs);
	free(tie->craft_markings);
	XwauLines_Free(&tie->craft_lines);
	XwauLines_Free(&tie->settings);
	memset(tie, 0, sizeof *tie);
}

const XwauMissionTieFlightGroup* XwauMissionTie_FlightGroup(const XwauMissionTie* tie, int fg) {
	for (size_t i = 0; i < tie->fg_count; ++i)
		if (tie->fgs[i].fg == fg)
			return &tie->fgs[i];
	return NULL;
}
