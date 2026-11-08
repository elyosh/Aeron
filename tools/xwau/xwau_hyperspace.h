#ifndef AERON_XWAU_HYPERSPACE_H
#define AERON_XWAU_HYPERSPACE_H

/*
 * Imports what XWAU changes about hyperspace jumps:
 *
 *   - hook_interdictor: a model's [Interdictor] IsInterdictor, from
 *     FlightModels/<model>Interdictor.txt or else the section of its INI,
 *     becomes `hyperspace.gravity_well` in its model settings. As in the
 *     hook, a section without the key gives false, and a model without the
 *     section keeps the rule of its object type.
 *
 *   - hook_hyperspace: ShortHyperspaceEffect, from hook_hyperspace.cfg or
 *     the [hook_hyperspace] section of Hooks.ini, and from each mission's
 *     <mission>_Hyperspace.txt or [Hyperspace] section, where -1 means the
 *     global value. The hook only uses the speeds of a model's
 *     [Hyperspace] section, from <model>Hyperspace.txt or its INI, when the
 *     mode is 1. So the import only writes speeds when at least one
 *     mission uses mode 1:
 *       - the hook's default speeds, 0x7000, as the `hyperspace` section of
 *         flight/flight.yaml, with a departure acceleration that reaches
 *         them in about 0.1 s. The hook doubles a departing craft's speed on
 *         every frame, which takes a few frames;
 *       - each model's OutOfHyperspaceSpeed and IntoHyperspaceSpeed, other
 *         than -1, as its arrival and departure speeds;
 *       - use_hyperspace_speeds: false for the missions where the mode is 0.
 *         When the global mode is 0, this goes into mission_settings, and
 *         the missions where the mode is 1 get true.
 *     A speed above 32767 wraps to a negative speed in the hook. It is
 *     reported as an error in the mod and left out.
 *     OutOfHyperspaceSetPosition isn't converted: the game lands an arriving
 *     craft on its point without a snap.
 *
 *   - ddraw's interdiction regions, read from the [Interdiction] section of
 *     <mission>.ini the way ddraw_effects.dll reads it
 *     (XwauHyperspace_DdrawRegions).
 */

#include <stdbool.h>
#include <stdio.h>

/* What the installation's hooks do with hyperspace. */
typedef struct XwauHyperspaceModes {
	bool interdictor;       /* hook_interdictor.dll */
	bool hyperspace;        /* hook_hyperspace.dll */
	int  global_mode;       /* ShortHyperspaceEffect, 0 or 1 */
	bool any_short;         /* at least one mission uses mode 1 */
	bool interdiction_look; /* ddraw_effects.dll reads [Interdiction] */
} XwauHyperspaceModes;

/* Reads the hooks and modes of the installation `game`. Returns false when
 * out of memory. */
bool XwauHyperspace_ReadModes(const char* game, XwauHyperspaceModes* out);

/* Returns the ShortHyperspaceEffect mode of mission `base`
 * ("<dir>/<name>"): 0 or 1. */
int XwauHyperspace_MissionMode(const char* game, const char* base, const XwauHyperspaceModes* modes);

/* Returns the use_hyperspace_speeds value that mission `base` needs: 0 or
 * 1, or -1 when the default of flight/flight.yaml is right for it. */
int XwauHyperspace_MissionUseSpeeds(const char* game, const char* base, const XwauHyperspaceModes* modes);

/* Writes the `hyperspace` section of flight/flight.yaml. Returns whether
 * it wrote one. */
bool XwauHyperspace_WriteFlightSettings(FILE* f, const XwauHyperspaceModes* modes);
/* Whether `mission_settings` needs `use_hyperspace_speeds: false`. */
bool XwauHyperspace_DefaultUseSpeedsOff(const XwauHyperspaceModes* modes);

/* The `hyperspace` group of a model. */
typedef struct XwauCraftHyperspace {
	bool has_gravity_well;
	bool gravity_well;
	int  arrival_speed;   /* game units; 0 for none */
	int  departure_speed; /* game units; 0 for none */
} XwauCraftHyperspace;

/* Reads the [Interdictor] and [Hyperspace] sections of model `model`:
 * `interdictor_txt` and `hyperspace_txt` when they have lines, else the
 * sections of `ini`. Any path can be NULL. */
void XwauHyperspace_ReadModel(const XwauHyperspaceModes* modes, const char* model,
							  const char* interdictor_txt, const char* hyperspace_txt, const char* ini,
							  XwauCraftHyperspace* out);
bool XwauHyperspace_HasModel(const XwauCraftHyperspace* model);
/* Writes the `hyperspace` group of `model`. */
void XwauHyperspace_WriteModel(FILE* f, const XwauCraftHyperspace* model);

/* The highest region that a mission's hyperspace_regions can list. */
#define XWAU_HYPERSPACE_MAX_REGION 3

/* Sets bit r of `*regions` for each region r in the [Interdiction] section
 * of <base>.ini, and returns whether the file has the section. It reads the
 * section the way ddraw does:
 *   - ddraw only reads <base>.ini, not <base>_Interdiction.txt;
 *   - a line only counts when its first word is Region, in any case;
 *   - the text after the '=' is split at commas, and each piece is read
 *     with atoi;
 *   - ddraw only keeps regions 0 to 7. */
bool XwauHyperspace_DdrawRegions(const char* game, const char* base, unsigned* regions);

#endif /* AERON_XWAU_HYPERSPACE_H */
