#ifndef AERON_XWAU_MISSION_SOUNDS_H
#define AERON_XWAU_MISSION_SOUNDS_H

/*
 * Reads the sound replacements of a mission, which hook_engine_sound uses.
 * They are in <mission>_Sounds.txt, or else in the [Sounds] section of
 * <mission>.ini. During the mission, a line "from = to" makes the game load
 * the file `to` whenever it loads a sound from the file `from`:
 *
 *   Wave\Sfx_HangarAmbient\FamilyHangar0.wav = Wave\Custom\Evacuate.wav
 *
 * The hook compares `from` with the path that the game builds. Upper and
 * lower case count as the same, but '/' and '\' don't. If two lines have the
 * same `from`, the first one is used. If the `to` file isn't in the
 * installation, the line does nothing; it is reported and left out.
 *
 * Lines for hangar voices (Wave/HANGARVOICESFX.LST) never match, because the
 * game builds those paths with a '/'. They are also reported and left out.
 *
 * The lines that work go into the mission file:
 *
 *   sounds:
 *     - ["Wave\\Sfx_HangarAmbient\\FamilyHangar0.wav", "Wave\\Custom\\Evacuate.wav"]
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct XwauMissionSound {
	char* from;
	char* to;
	char  to_path[1024]; /* `to` in the installation */
} XwauMissionSound;

typedef struct XwauMissionSounds {
	XwauMissionSound* items;
	size_t            count;
} XwauMissionSounds;

/* Reads the sound replacements of mission `base` ("Missions/1b0m7fw") of the
 * installation `game`, when it has hook_engine_sound.dll. `stock_game` is
 * the stock game's folder, for the hangar voices. With `report`, the lines
 * left out are reported. Returns false when out of memory. */
bool XwauMissionSounds_Read(const char* game, const char* stock_game, const char* base, bool report,
							XwauMissionSounds* out);
void XwauMissionSounds_Free(XwauMissionSounds* sounds);

/* Writes the `sounds` list of a mission file, when there are lines. */
void XwauMissionSounds_Write(FILE* f, const XwauMissionSounds* sounds);

#endif /* AERON_XWAU_MISSION_SOUNDS_H */
