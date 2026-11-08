#ifndef AERON_XWAU_SOUND_BANKS_H
#define AERON_XWAU_SOUND_BANKS_H

/*
 * Imports XWAU's sound banks, in the mod importer's craft phase. When an
 * installation has hook_sounds_count.dll, XWAU loads each
 * Wave/Sfx_<Bank>.lst as a bank of sounds, with the files in
 * Wave/Sfx_<Bank>/. Other hooks then pick sounds from the banks by object
 * type: hook_engine_sound, hook_hyperspace, hook_sfoils and hook_hangars.
 *
 * A bank is imported only if the installation has the hook that reads it.
 * The importer writes the banks to <output_dir>/sounds.yaml. Each bank
 * lists its sounds by the game's object type names, with each sound's file,
 * volume and distance.
 *
 * XWAU's hooks only use the entries within the list's line count. Within the
 * list, an entry without a sound is silent: lines such as "Unused", and
 * files that the installation doesn't have. Past the end of the list, each
 * hook either plays nothing or plays the stock sound. A bank where XWAU
 * plays nothing gets `unlisted: silent`. For the other banks, the stock
 * sound plays, so a silent entry within the list gets `{silent: true}`. The
 * hangar_ambient and weapon_explosion lists keep their silent entries,
 * because the game picks one entry of the list at random. The weapon_range
 * entries are named after their position in the list, such as range_3.
 *
 * The EnterHyperspace and ExitHyperspace banks of hook_hyperspace play when
 * other craft jump away or end their arrival in the player's region. The
 * stock game plays nothing then, and neither does XWAU past the end of the
 * lists.
 *
 * Entries for object types that the game has no name for are left out,
 * since no craft can use them.
 *
 * Each list line is read the way the game reads it, after XWAU cuts it at
 * ';'. The file name ends at the first space or tab. Then come the volume in
 * decimal (127 if missing) and the distance in hex, in game units (8192 if
 * missing). Like the game, the hex reader only accepts the digits 0-9, a-f
 * and F. A line without a file name doesn't use a slot. sounds.yaml has the
 * distance in metres.
 *
 * The WAV files go into DIR, with the same path as in the installation
 * (Wave/Sfx_Interior/EngineIntXW.wav). The game finds them whatever the
 * case of the path. Files that the game can't play are converted to 16-bit
 * PCM: 24-bit, float, WAVE_FORMAT_EXTENSIBLE and MS ADPCM files, and files
 * with more than 2 channels. A WAV file in another format makes the import
 * fail. A file that isn't a WAV file is silent in XWAU too, so its entries
 * are silent.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xwau_names.h"

/* One line of a bank list. */
typedef struct XwauSoundLine {
	char name[256]; /* the file name, in the bank's folder */
	int  volume;
	int  distance;
} XwauSoundLine;

/* Reads one list line, with its line end or without it. Returns false when
 * the line takes no slot. */
bool XwauSoundBanks_ParseLine(const char* line, XwauSoundLine* out);

typedef enum XwauWavResult {
	XWAU_WAV_PLAYABLE,  /* the game plays the file as it is */
	XWAU_WAV_CONVERTED, /* `*out` holds a 16-bit PCM copy */
	XWAU_WAV_BAD        /* not a WAV file that can be read or converted */
} XwauWavResult;

/* Checks the WAV file in `data`. When the game can't play it, writes a
 * 16-bit PCM copy into `*out` (to free) and `*out_size`. */
XwauWavResult XwauSoundBanks_ConvertWav(const uint8_t* data, size_t size, uint8_t** out, size_t* out_size);

/* Returns the number of lines that XWAU counts in the bank list
 * Wave/Sfx_<name>.lst, or 0 when the installation doesn't load that bank. */
int XwauSoundBanks_LineCount(const char* game, const char* name);

/* Writes the name of the weapon_range entry at `position` in XWAU's list,
 * such as "range_3". */
void XwauSoundBanks_RangeName(int position, char* out, size_t cap);

/* When the installation `game` has hook_sounds_count.dll, writes
 * <output_dir>/sounds.yaml and copies the sound files into `game_files`.
 * The banks use the names in `game_names`. Returns false on an error. */
bool XwauSoundBanks_Write(const char* game, const XwauNames* game_names, const char* output_dir,
						  const char* game_files);

#endif /* AERON_XWAU_SOUND_BANKS_H */
