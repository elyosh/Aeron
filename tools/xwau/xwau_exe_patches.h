#ifndef AERON_XWAU_EXE_PATCHES_H
#define AERON_XWAU_EXE_PATCHES_H

/*
 * The code patches of a mod's XWINGALLIANCE.EXE that the importer converts.
 * Each patch is found by its bytes, as listed by XWAU's patcher. A patch is
 * applied when every part of it has the patched bytes. Other bytes are
 * reported as unsupported, and the patch counts as not applied.
 *
 * The patches come in two groups, one for each phase of the import that
 * uses them, so that each phase reports only its own patches.
 *
 * The craft phase (xwau_objects.h):
 *   - 0x41C6BF: the X-wing, Y-wing, A-wing, B-wing, Z-95 and TIE Fighter
 *     can carry a beam (mission.c:6624-6627).
 *
 * The flight settings phase, which writes the ejection rules
 * (xwau_mission_settings.h). TFTC's executables have all five patches, and
 * XWAU 2025's the last three:
 *   - 0x4042F4: no pilot ejects from a fighter;
 *   - 0x40434B: no escape pod ejects from a transport, starship or
 *     platform;
 *   - 0x404481 and 0x404693: pods and pilots are on team 9;
 *   - 0x404EEB: pods don't get their AI plan, and pilots keep the random
 *     speed and direction that every ejected craft starts with;
 *   - 0x5797B6: the craft of a skirmish mission can eject.
 */

#include <stdbool.h>

#include "xwau_game_tables.h"

typedef enum XwauExePatchGroup {
	XWAU_EXE_PATCHES_CRAFT,
	XWAU_EXE_PATCHES_FLIGHT,
} XwauExePatchGroup;

typedef struct XwauExePatches {
	/* The patch of the craft group. */
	bool craft_beams;
	/* The patches of the flight group. */
	bool no_pilot_ejections;
	bool no_escape_pod_ejections;
	bool ejected_craft_team_9;
	bool ejected_craft_skip_setup;
	bool skirmish_ejections;
} XwauExePatches;

/* Reads the patches of `group` in `exe`. The patches of the other group stay
 * false. Without an executable (NULL), there are none. */
void XwauExePatches_Read(const XwauGameExe* exe, XwauExePatchGroup group, XwauExePatches* out);

#endif /* AERON_XWAU_EXE_PATCHES_H */
