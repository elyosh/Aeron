#ifndef AERON_XWAU_GAME_FILES_H
#define AERON_XWAU_GAME_FILES_H

/*
 * Copies the files that come with a mod's missions into the mod folder's
 * game/ folder (xwau_import missions --game-files DIR). The game reads these
 * files from there instead of the game folder:
 *   - every .tie of Missions/, Melee/ and Combat/;
 *   - the MISSION.LST of these folders, Missions/EMAIL.TXT and
 *     Frontres/Cutscene/cutscene.txt, when they differ from the stock game's;
 *   - the files of Wave/MISSIONVOICE/ and Wave/FRONTEND/ (the mission voices,
 *     and the briefing, debriefing and concourse voices) that differ from the
 *     stock game's or that the stock game doesn't have.
 *
 * A file keeps its path in the mod, so Missions/1b7m2w.tie goes to
 * <out>/Missions/1b7m2w.tie. The game finds it in any case. Files that are
 * already in <out> are replaced, and the other ones are kept.
 */

#include <stdbool.h>

/* Copies the files of the installation `game` into `out`. `stock_game` is the
 * stock game's folder, to compare with. Returns false on an error. */
bool XwauGameFiles_Copy(const char* game, const char* stock_game, const char* out);

#endif /* AERON_XWAU_GAME_FILES_H */
