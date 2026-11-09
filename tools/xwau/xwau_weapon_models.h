#ifndef AERON_XWAU_WEAPON_MODELS_H
#define AERON_XWAU_WEAPON_MODELS_H

/*
 * The models of a craft's shots, from the [OptReplace] section of
 * hook_mission_tie: FlightModels/<model>OptReplace.txt, else the
 * [OptReplace] section of FlightModels/<model>.ini. Each line has the form
 * `Weapon286 = FlightModels\CapitalTorpedo.opt`, for the weapon types 280 to
 * 307. When the craft fires that weapon type, the hook draws its shots with
 * that model instead of the weapon's own model. The import writes them as
 * the `weapons.models` group of the craft's model settings, and converts the
 * models as projectiles.
 *
 * Only newer builds of hook_mission_tie read the section.
 */

#include <stdbool.h>

#include "xwau_config.h"

#define XWAU_WEAPON_MODELS_FIRST 280
#define XWAU_WEAPON_MODELS_COUNT 28
#define XWAU_WEAPON_MODEL_KEY_MAX 64

/* For each weapon type, the key of the model that the craft's shots show,
 * or "" for the weapon's own model. */
typedef struct XwauWeaponModels {
	bool has;
	char key[XWAU_WEAPON_MODELS_COUNT][XWAU_WEAPON_MODEL_KEY_MAX];
} XwauWeaponModels;

/* Whether the installation's hook_mission_tie.dll reads [OptReplace]. */
bool XwauWeaponModels_HookReads(const char* game);

/* Reads the weapon models of `model` (a file name in FlightModels, without
 * the extension). With `report`, the values that the game can't use are
 * reported: a model outside FlightModels, a model that doesn't exist, and a
 * key that the hook doesn't read. Returns false when out of memory. */
bool XwauWeaponModels_Read(const char* game, const char* model, bool report, XwauWeaponModels* out);

/* Adds to `keys` the keys of every model that a craft of the installation
 * uses as a weapon model, without reports. Adds nothing when the hook
 * doesn't read [OptReplace]. Returns false when out of memory. */
bool XwauWeaponModels_FindAll(const char* game, XwauLines* keys);

#endif /* AERON_XWAU_WEAPON_MODELS_H */
