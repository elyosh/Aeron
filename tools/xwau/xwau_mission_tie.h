#ifndef AERON_XWAU_MISSION_TIE_H
#define AERON_XWAU_MISSION_TIE_H

/*
 * Reads the [mission_tie] lines of a mission, as XWAU's hook_mission_tie
 * reads them. They come from <mission>.txt, or else from the [mission_tie]
 * section of <mission>.ini, with the hooks' C++ rules (XWAU_RULES_CPP): blank
 * lines and lines that start with '#', ';' or "//" are skipped.
 *
 * A line is split on ',' and ';', and each word is trimmed. Lines whose first
 * word is "fg" give a flight group's values, by its position in the mission's
 * .TIE:
 *   - fg, N, markings, V and fg, N, iff, V: numbers, read as std::stoi reads
 *     them, so a trailing comment is ignored;
 *   - fg, N, pilotvoice, S: the pilot's voice, up to 19 characters;
 *   - fg, N, index, W, markings, V: the markings of craft W of the flight
 *     group, in each of its waves;
 *   - fg, N, close_SFoils, 1 and fg, N, open_LandingGears, 1: the flight
 *     group starts with its S-foils closed or its landing gear open.
 * When two lines set the same value, the last one wins, as in XWAU.
 *
 * Lines whose first word is "craft" and lines "key = value" are kept as
 * they are, for the readers of their own.
 *
 * Reported, when `report` is set:
 *   - a line that XWAU ignores: one with fewer than four words, or one with
 *     a number where its key should be, such as "fg, 3 markings, 6 ;Grey 5",
 *     where the ';' of the comment makes a fourth word;
 *   - a value that isn't a number, which stops XWAU (std::stoi throws).
 */

#include <stdbool.h>
#include <stddef.h>

#include "xwau_config.h"

/* The values of one flight group. */
typedef struct XwauMissionTieFlightGroup {
	int  fg; /* its position in the .TIE */
	bool has_markings, has_iff, has_pilot_voice;
	int  markings, iff;
	char pilot_voice[20];
	/* The start lines: 1 + the number of the first line that sets them to 1,
	 * or 0, so that a reader can keep their order. */
	size_t close_sfoils, open_landing_gear;
} XwauMissionTieFlightGroup;

/* fg, N, index, W, markings, V */
typedef struct XwauMissionTieCraftMarkings {
	int fg, craft, markings;
} XwauMissionTieCraftMarkings;

typedef struct XwauMissionTie {
	XwauMissionTieFlightGroup*   fgs; /* sorted by position */
	size_t                       fg_count;
	XwauMissionTieCraftMarkings* craft_markings; /* in the order of the lines, one per (fg, craft) */
	size_t                       craft_marking_count;
	XwauLines                    craft_lines; /* the "craft, …" lines */
	XwauLines                    settings;    /* the "key = value" lines */
} XwauMissionTie;

/* Reads the [mission_tie] lines of mission `base` (such as
 * "Missions/1b7m2w") of the installation `game`. Problems are reported with
 * `base` as their label when `report` is set. Returns false when out of
 * memory. */
bool XwauMissionTie_Read(const char* game, const char* base, bool report, XwauMissionTie* out);
void XwauMissionTie_Free(XwauMissionTie* tie);

/* The values of flight group `fg`, or NULL. */
const XwauMissionTieFlightGroup* XwauMissionTie_FlightGroup(const XwauMissionTie* tie, int fg);

#endif /* AERON_XWAU_MISSION_TIE_H */
