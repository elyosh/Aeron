#ifndef AERON_XWAU_MISSIONS_H
#define AERON_XWAU_MISSIONS_H

/*
 * Reads what the XWAU missions (in Missions/ and Melee/) give to the models.
 * The missions are read once for all the converted models:
 *
 *   - Skin lines (hook_32bpp), read with the C# rules. The global context
 *     comes from FlightModels/Skins.txt, or else from the [Skins] section of
 *     FlightModels/default.ini. Then each mission with a .ini or _Skins.txt
 *     file adds a context, in lower-case path order. The context holds the
 *     mission's own lines (XwauMissions_OwnSkinLines), or the global lines
 *     when it has none.
 *
 *   - Articulation lists (hook_sfoils). The [<Section>_fg_N] sections of a
 *     mission give flight group N its own lists. <Section> is one of the
 *     model sections (xwau_model.h). Each section is read from
 *     <mission>_<Section>_fg_N.txt, or else from that section of the
 *     mission's .ini. The craft type of the flight group, read from the
 *     mission's .TIE, gives its model through the craft list
 *     (FlightModels/Spacecraft0.LST). Each distinct set of lists for a model
 *     is a variant. The variant is named after the first flight group that
 *     uses it ("1b0m7fw_fg13"), with the missions in lower-case name order,
 *     first Missions/ then Melee/. Its articulation groups are
 *     `variant.<variant>.<group>`, and the mission selects them. When a mesh
 *     is listed twice, the first line is used.
 *
 *   - Start lines (hook_sfoils). The lines "fg, N, close_SFoils, 1" and
 *     "fg, N, open_LandingGears, 1" start flight group N with its S-foils or
 *     landing gear deployed. They are read from <mission>.txt, or else from
 *     the [mission_tie] section of the mission's .ini.
 */

#include <stdbool.h>
#include <stddef.h>

#include "opt_gltf.h"
#include "xwau_config.h"

typedef struct XwauMissions XwauMissions;

/* Reads the missions of the installation. Returns NULL when out of memory.
 * With `report_problems`, problems in the mission files are reported to
 * stderr. A model conversion turns this off, because every model would
 * repeat the same reports. */
XwauMissions* XwauMissions_Load(const char* game, bool report_problems);
void          XwauMissions_Free(XwauMissions* missions);

/* Lists the missions in folder `dir` (Missions or Melee) that have a .ini,
 * _Objects.txt, _Skins.txt or _SFoils*.txt file, or a .txt file next to
 * their .TIE. The names are in lower case and sorted. Returns false when out
 * of memory. */
bool XwauMissions_Names(const char* game, const char* dir, XwauLines* out);

/* Reads the skin lines of mission `base` ("<dir>/<name>") from
 * <mission>_Skins.txt, or else from the [Skins] section of <mission>.ini.
 * There are no lines when it has neither. */
bool XwauMissions_OwnSkinLines(const char* game, const char* base, XwauLines* out);

/* The skin contexts. Context 0 is the global one. */
size_t           XwauMissions_SkinContextCount(const XwauMissions* missions);
const XwauLines* XwauMissions_SkinContext(const XwauMissions* missions, size_t index);

/* Appends the variant articulations of `model` to `out`, which holds
 * `*count` entries out of `cap`. They are sorted by variant name, then by
 * group in list order, then by mesh. Returns false when they do not fit. */
bool XwauMissions_Articulations(const XwauMissions* missions, const char* model,
								OptGltfComponentArticulation* out, size_t* count, size_t cap,
								const char* label);

/* What a mission selects for one of its flight groups. */
typedef struct XwauFlightGroupArticulation {
	int         flight_group; /* in the XWAU mission */
	const char* start[2];     /* the groups it starts deployed: "sfoils", "landing_gear" */
	size_t      start_count;
	char        variant[128]; /* variant of its lists, or "" */
} XwauFlightGroupArticulation;

/* Returns the flight groups of mission `base` ("<dir>/<lower-case name>")
 * that have start lines or a variant, sorted by flight group. */
size_t XwauMissions_FlightGroupArticulations(const XwauMissions* missions, const char* base,
											 const XwauFlightGroupArticulation** out);

/* A flight group of an XWA .TIE. */
typedef struct XwauTieFlightGroup {
	char name[21]; /* ISO-8859-1 */
	int  craft;    /* craft type */
	int  markings;
} XwauTieFlightGroup;

/* Reads the flight groups of the .TIE file `path`. Returns NULL when it
 * cannot be read. */
XwauTieFlightGroup* XwauTie_FlightGroups(const char* path, size_t* count);

#endif /* AERON_XWAU_MISSIONS_H */
