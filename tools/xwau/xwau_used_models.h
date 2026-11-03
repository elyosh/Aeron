#ifndef AERON_XWAU_USED_MODELS_H
#define AERON_XWAU_USED_MODELS_H

/*
 * The models (FlightModels/<name>.opt) that the game can load with the mod.
 * The import only converts these. XWA with XWAU loads an OPT in these ways:
 *   1. a row of FlightModels/Spacecraft0.LST or Equipment0.LST;
 *   2. the cockpit and the exterior of the player's craft:
 *      <record name>Cockpit.opt and <record name>Exterior.opt, where the name
 *      is the craft record's cockpit name. A missing cockpit falls back to
 *      TieCockpit.opt for TIE craft, then to CombatSimCockpit.opt;
 *   3. the tech library and briefing preview: <row>Exterior.opt, else the
 *      row itself;
 *   4. a swap of hook_mission_objects ([Objects] in a mission .ini,
 *      <mission>_Objects.txt, FlightModels/Objects.txt and default.ini). A
 *      swap target can be a craft, so its gunner cockpits and its hangar
 *      count too;
 *   5. the hangar of the command ship (hook_hangars): <ship>Hangar.opt, else
 *      Hangar.opt. The proving grounds use ProvingGroundHangarModel of
 *      Hooks.ini;
 *   6. a HangarObjects swap (the [HangarObjects*] sections and the
 *      *HangarObjects*.txt files);
 *   7. a gunner seat: column 8 of a [Turrets] line or *Turrets.txt, else
 *      <ship>Gunner.opt and <ship>Gunner2.opt, and CorellianTransportGunner.opt
 *      as the last fallback;
 *   8. a weapon model of hook_mission_tie ([OptReplace] and *OptReplace.txt).
 * The hangar map, the shuttle and the droids use list rows, so way 1 covers
 * them.
 *
 * Every list row counts, because skirmish missions can use any craft. The
 * section and file names are matched loosely, and a name that has no OPT
 * does no harm, so the set can only hold too many models, not too few. A
 * swap target outside FlightModels is reported, because the import only
 * converts the OPTs of FlightModels.
 */

#include <stdbool.h>

#include "xwau_config.h"
#include "xwau_game_tables.h"

/* Adds to `keys` the model keys (Xwau_ModelKey) of the models that the mod
 * at `root`, with the tables `tables`, can load. Returns false when out of
 * memory. */
bool XwauUsedModels_Find(const char* root, const XwauGameTables* tables, XwauLines* keys);

/* Whether `keys` holds `key`. */
bool XwauUsedModels_Has(const XwauLines* keys, const char* key);

#endif /* AERON_XWAU_USED_MODELS_H */
