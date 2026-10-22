#ifndef AERON_XWAU_GAME_FILES_H
#define AERON_XWAU_GAME_FILES_H

/*
 * Copies a mod's game files into the mod folder's game/ folder
 * (xwau_import missions --game-files DIR). The game reads these files from
 * there instead of from the game folder. The copied files are:
 *   - every .tie in Missions/, Melee/ and Combat/;
 *   - the MISSION.LST of these folders, Missions/EMAIL.TXT and
 *     Frontres/Cutscene/cutscene.txt, if they differ from the stock game's;
 *   - the files in Wave/ that differ from the stock game's. This covers the
 *     stock files directly in Wave/, such as SFXBLASTNEW.LST, and every file
 *     in a folder that the stock game also has: mission voices
 *     (Wave/MISSIONVOICE/), briefing and concourse voices (Wave/FRONTEND/)
 *     and sound effects (Wave/FE_HIGH_RES/). Other files and folders in
 *     Wave/ are skipped, because the game doesn't read them. The sound banks
 *     (Wave/Sfx_*) are copied by xwau_import craft;
 *   - the files that the missions' sound replacements load, such as
 *     Wave/Custom/Evacuate.wav, if they differ from the stock game's.
 *
 * WAV files that the game can't play are converted to 16-bit PCM
 * (XwauSoundBanks_ConvertWav).
 *
 * Each file keeps its path from the mod, so Missions/1b7m2w.tie goes to
 * <out>/Missions/1b7m2w.tie. The game finds it whatever the case of the
 * path. Files already in <out> are replaced, and other files are kept.
 */

#include <stdbool.h>

/* Copies the files of the installation `game` into `out`. `stock_game` is the
 * stock game's folder, to compare with. Returns false on an error. */
bool XwauGameFiles_Copy(const char* game, const char* stock_game, const char* out);

#endif /* AERON_XWAU_GAME_FILES_H */
