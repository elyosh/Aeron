#ifndef AERON_XWAU_SOUND_BANKS_H
#define AERON_XWAU_SOUND_BANKS_H

/*
 * Imports XWAU's sound banks (xwau_import craft --game-files DIR). An
 * installation with hook_sounds_count.dll loads each Wave/Sfx_<Bank>.lst as a
 * bank of sounds, from Wave/Sfx_<Bank>/. Other hooks pick a bank's sound by
 * object type: hook_engine_sound, hook_hyperspace, hook_sfoils and
 * hook_hangars.
 *
 * A bank is imported only when the installation has the hook that reads it.
 * The importer writes the banks into <output_dir>/sounds.yaml:
 *
 *   version: 1
 *   banks:
 *     interior:                       # Wave/Sfx_Interior.lst, by object type
 *       count: 225
 *       sounds:
 *         1: {file: 'Wave\Sfx_Interior\EngineIntXW.wav', volume: 76, distance: 8192}
 *     weapon:                         # 10 by object type
 *       count: 2230
 *       sounds:
 *         1:
 *           0: {file: ...}
 *
 * `count` is the number of lines that XWAU counts in the list: the hooks
 * use a bank entry only below it. An entry without a sound is silent: a line
 * such as "Unused", or a file that the installation doesn't have. The banks
 * by object type are listed in xwau_sound_banks.c.
 *
 * A list line is read as the game reads it, after XWAU cuts it at ';': the
 * file name ends at the first space or tab, then come the volume in decimal
 * (127 without it) and the distance in hex (8192 without it). The hex reader
 * only knows the digits 0-9, a-f and F, as in the game. A line without a
 * file name takes no slot.
 *
 * The WAV files of the entries go into DIR, with their path in the
 * installation (Wave/Sfx_Interior/EngineIntXW.wav). The game finds them in
 * any case. Files that the game can't play are converted to 16-bit PCM:
 * 24-bit, float, WAVE_FORMAT_EXTENSIBLE and MS ADPCM files, and files with
 * more than 2 channels.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Writes <output_dir>/sounds.yaml and copies the files into `game_files`,
 * when the installation `game` has hook_sounds_count.dll. Returns false on
 * an error. */
bool XwauSoundBanks_Write(const char* game, const char* output_dir, const char* game_files);

#endif /* AERON_XWAU_SOUND_BANKS_H */
