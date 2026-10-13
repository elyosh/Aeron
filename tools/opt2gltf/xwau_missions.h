#ifndef AERON_OPT2GLTF_XWAU_MISSIONS_H
#define AERON_OPT2GLTF_XWAU_MISSIONS_H

/*
 * What the XWAU missions (Missions/ and Melee/ of the game) give models,
 * read once for all the models converted:
 *
 *   - skin lines (hook_32bpp): FlightModels/Skins.txt, else [Skins] of
 *     FlightModels/default.ini (the global context), then each mission
 *     with a .ini or _Skins.txt, by lower-case path: its own lines
 *     (XwauMissions_OwnSkinLines), else the global lines. Read with the C#
 *     rules.
 *
 *   - articulation lists (hook_sfoils): a mission's [<Section>_fg_N]
 *     sections (<mission>_<Section>_fg_N.txt, else that section of its
 *     .ini; <Section> one of the model sections, xwau_model.h) give flight
 *     group N its own lists. The flight group's craft type, read from the
 *     mission's .TIE, gives its model through the craft list
 *     (FlightModels/Spacecraft0.LST). Each distinct set of lists of a model
 *     is a variant, named after the first flight group with it, missions in
 *     lower-case name order, Missions/ then Melee/ ("1b0m7fw_fg13"): its
 *     articulation groups are `variant.<variant>.<group>`, which the mission
 *     selects. A mesh listed twice keeps its first line.
 *
 *   - start lines (hook_sfoils): "fg, N, close_SFoils, 1" and "fg, N,
 *     open_LandingGears, 1" (<mission>.txt, else [mission_tie] of its .ini)
 *     start flight group N with its S-foils or landing gear deployed.
 */

#include <stdbool.h>
#include <stddef.h>

#include "opt_gltf.h"
#include "xwau_config.h"

typedef struct XwauMissions XwauMissions;

/* Reads the installation's missions; NULL when out of memory. With
 * `report_problems`, the problems of the missions' files go to stderr (a
 * model conversion leaves them out: every model would repeat them). */
XwauMissions* XwauMissions_Load(const char* game, bool report_problems);
void          XwauMissions_Free(XwauMissions* missions);

/* The lower-case names of the missions of folder `dir` (Missions or Melee)
 * with a .ini, _Objects.txt, _Skins.txt, _SFoils*.txt or (beside its .TIE)
 * .txt file, sorted. False when out of memory. */
bool XwauMissions_Names(const char* game, const char* dir, XwauLines* out);

/* The skin lines of mission `base` ("<dir>/<name>"): <mission>_Skins.txt,
 * else [Skins] of <mission>.ini; none when it has neither. */
bool XwauMissions_OwnSkinLines(const char* game, const char* base, XwauLines* out);

/* The skin contexts: 0 is the global one. */
size_t           XwauMissions_SkinContextCount(const XwauMissions* missions);
const XwauLines* XwauMissions_SkinContext(const XwauMissions* missions, size_t index);

/* Appends the variant articulations of `model` (by variant name, then the
 * lists' group order and mesh order) to `out`, which holds `*count` of
 * `cap`. False when they do not fit. */
bool XwauMissions_Articulations(const XwauMissions* missions, const char* model,
								OptGltfComponentArticulation* out, size_t* count, size_t cap,
								const char* label);

/* What a mission selects for one of its flight groups. */
typedef struct XwauFlightGroupArticulation {
	int         flight_group; /* in the XWAU mission */
	const char* start[2];     /* the groups it starts deployed: "sfoils", "landing_gear" */
	size_t      start_count;
	char        variant[128]; /* its lists' variant; "" when none */
} XwauFlightGroupArticulation;

/* The flight groups of mission `base` ("<dir>/<lower-case name>") with
 * start lines or a variant, by flight group. */
size_t XwauMissions_FlightGroupArticulations(const XwauMissions* missions, const char* base,
											 const XwauFlightGroupArticulation** out);

/* A flight group of an XWA .TIE. */
typedef struct XwauTieFlightGroup {
	char name[21]; /* ISO-8859-1 */
	int  craft;    /* craft type */
	int  markings;
} XwauTieFlightGroup;

/* The flight groups of the .TIE `path`; NULL when it cannot be read. */
XwauTieFlightGroup* XwauTie_FlightGroups(const char* path, size_t* count);

#endif /* AERON_OPT2GLTF_XWAU_MISSIONS_H */
