#ifndef AERON_OPT2GLTF_XWAU_MISSIONS_H
#define AERON_OPT2GLTF_XWAU_MISSIONS_H

/*
 * What the XWAU missions (Missions/ and Melee/ of the roots) give models,
 * read once for all the models converted:
 *
 *   - skin lines (hook_32bpp): FlightModels/Skins.txt, else [Skins] of
 *     FlightModels/default.ini (the global context), then each mission
 *     with a .ini or _Skins.txt, by lower-case path: its _Skins.txt, else
 *     [Skins] of its .ini, else the global lines. Read with the C# rules.
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
 */

#include <stdbool.h>
#include <stddef.h>

#include "opt2gltf.h"
#include "xwau_config.h"

typedef struct XwauMissions XwauMissions;

/* Reads the roots' missions; NULL when out of memory. Problems go to
 * stderr. */
XwauMissions* XwauMissions_Load(const XwauRoots* roots);
void          XwauMissions_Free(XwauMissions* missions);

/* The skin contexts: 0 is the global one. */
size_t           XwauMissions_SkinContextCount(const XwauMissions* missions);
const XwauLines* XwauMissions_SkinContext(const XwauMissions* missions, size_t index);

/* Appends the variant articulations of `model` (by variant name, then the
 * lists' group order and mesh order) to `out`, which holds `*count` of
 * `cap`. False when they do not fit. */
bool XwauMissions_Articulations(const XwauMissions* missions, const char* model,
								OptGltfComponentArticulation* out, size_t* count, size_t cap,
								const char* label);

#endif /* AERON_OPT2GLTF_XWAU_MISSIONS_H */
