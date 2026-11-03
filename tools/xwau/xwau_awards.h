#ifndef AERON_XWAU_AWARDS_H
#define AERON_XWAU_AWARDS_H

/*
 * Writes frontend/awards.yaml, the medals, Kalidor Crescent levels,
 * promotion thresholds and family-room mementos of a mod
 * (the mod importer's missions phase). They come from the text files that
 * hook_concourse reads in the game folder:
 *
 * - MedalsDescriptions.txt gives `battle_medals`;
 * - KalidorCrescentPoints.txt and KalidorCrescentDescriptions.txt give
 *   `kalidor`. The points are whole bonus points, as in the file;
 * - RankPoints.txt and PilotRatingPoints.txt give the thresholds. The game
 *   has 8 rank thresholds and 25 rating thresholds, and the hook doesn't
 *   change these counts, so the values past them are left out;
 * - MedalDetailList.txt gives `mementos.images`, and
 *   FamilyMedalsDescriptions.txt gives `mementos.names`;
 * - the familyroom_ladyblue_mission and familyroom_cologne_mission keys of
 *   ConcourseDoors.txt give `mementos.lady_blue` and `mementos.cologne`.
 *   The importer only writes them when they aren't the stock missions, 23
 *   and 9.
 *
 * The files are read as the hook reads them. The name files are read line
 * by line, and an empty line is "Unknown". The other files skip empty lines
 * and comments (XWAU_RULES_CPP). In MedalDetailList.txt, "null" means that
 * the mission has no memento. A memento named "Null" or "null" is left out,
 * so it keeps the game's name. Without hook_concourse.dll or these files,
 * no file is written, since XWAU then never reads them.
 */

#include <stdbool.h>

/* Writes the awards of the installation `game` to `path`. Returns false
 * when a file can't be read or written, or when out of memory. */
bool XwauAwards_Write(const char* game, const char* path);

#endif /* AERON_XWAU_AWARDS_H */
