#include "xwau_sound_banks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "xwau_config.h"

/* How a bank's entries are indexed. */
enum {
	BANK_BY_NAME     = -1, /* fixed names, g_soundNames */
	BANK_BY_POSITION = 0,  /* the line's position */
	BANK_BY_TYPE     = 1   /* object type, or the number of entries for each type */
};

typedef struct BankInfo {
	const char* name; /* Wave/Sfx_<name>.lst */
	const char* key;  /* the key in sounds.yaml */
	int         layout;
	const char* hook; /* the hook DLL that reads the bank */
} BankInfo;

/* The banks that XWAU's hooks read. */
static const BankInfo g_banks[] = {
	{ "CanopyOpening", "canopy_opening", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "EngineSlowing", "engine_slowing", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "EngineWash", "engine_wash", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "EnterHyperspace", "enter_hyperspace", BANK_BY_TYPE, "hook_hyperspace.dll" },
	{ "ExitHyperspace", "exit_hyperspace", BANK_BY_TYPE, "hook_hyperspace.dll" },
	{ "FlyBy", "fly_by", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "HangarAmbient", "hangar_ambient", 10, "hook_hangars.dll" },
	{ "HyperAbort", "hyper_abort", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "HyperEnd", "hyper_end", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "HyperStart", "hyper_start", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "HyperZoom", "hyper_zoom", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "Interior", "interior", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "SFoil", "s_foil", BANK_BY_TYPE, "hook_sfoils.dll" },
	{ "ShuttleBlastOff", "shuttle_blast_off", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "ShuttleShutDown", "shuttle_shut_down", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "ShuttleTakeOff", "shuttle_take_off", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "Sounds", "sounds", BANK_BY_NAME, "hook_engine_sound.dll" },
	{ "StarshipAmbient", "starship_ambient", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "TakeOff", "take_off", BANK_BY_TYPE, "hook_engine_sound.dll" },
	{ "Weapon", "weapon", 10, "hook_engine_sound.dll" },
	{ "WeaponExplosion", "weapon_explosion", 8, "hook_engine_sound.dll" },
	{ "WeaponRange", "weapon_range", BANK_BY_POSITION, "hook_engine_sound.dll" },
};

/* The entries of the Sounds bank, in hook_engine_sound's order. */
static const char* const g_soundNames[] = { "player_targeted", "map_enter", "map_exit", "turret_switch",
											"target_select" };

enum { DEFAULT_VOLUME = 127, DEFAULT_DISTANCE = 8192 };

bool XwauSoundBanks_ParseLine(const char* line, XwauSoundLine* out) {
	char buffer[256];
	snprintf(buffer, sizeof buffer, "%s", line);
	/* XWAU's hook cuts the line at ';' before the game reads it. */
	char* cursor = strchr(buffer, ';');
	if (cursor)
		*cursor = '\0';
	out->volume   = DEFAULT_VOLUME;
	out->distance = DEFAULT_DISTANCE;

	/* The game's reader (fsfx_LoadSfxList). */
	cursor = buffer;
	while (*cursor && *cursor != '\n' && *cursor != '\r' && *cursor != ' ' && *cursor != '\t')
		++cursor;
	if (*cursor == ' ' || *cursor == '\t') {
		while (*cursor == ' ' || *cursor == '\t')
			*cursor++ = '\0';
		out->volume = (uint8_t)atoi(cursor);
	}
	while (*cursor && *cursor != '\n' && *cursor != '\r' && *cursor != ' ' && *cursor != '\t')
		++cursor;
	if (*cursor == ' ' || *cursor == '\t') {
		while (*cursor == ' ' || *cursor == '\t')
			*cursor++ = '\0';
		uint16_t distance = 0;
		for (;; ++cursor) {
			const char ch = *cursor;
			if (ch >= '0' && ch <= '9')
				distance = (uint16_t)(distance * 16 + ch - '0');
			else if (ch >= 'a' && ch <= 'f')
				distance = (uint16_t)(distance * 16 + ch - 'a' + 10);
			else if (ch == 'F')
				distance = (uint16_t)(distance * 16 + 15);
			else
				break;
		}
		out->distance = distance;
	}
	*cursor = '\0';
	snprintf(out->name, sizeof out->name, "%s", buffer);
	return out->name[0] != '\0';
}

/* ---- WAV files ------------------------------------------------------------ */

static unsigned read16(const uint8_t* p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

static uint32_t read32(const uint8_t* p) { return read16(p) | (uint32_t)read16(p + 2) << 16; }

static void write16(uint8_t* p, unsigned v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void write32(uint8_t* p, uint32_t v) {
	write16(p, v & 0xffff);
	write16(p + 2, v >> 16);
}

static int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* A 16-bit PCM WAV file with `frames` frames of `channels` channels. */
static uint8_t* new_wav(unsigned channels, uint32_t rate, size_t frames, size_t* size) {
	const size_t bytes = frames * channels * 2;
	uint8_t*     wav   = (uint8_t*)calloc(1, 44 + bytes);
	if (!wav)
		return NULL;
	memcpy(wav, "RIFF", 4);
	write32(wav + 4, (uint32_t)(36 + bytes));
	memcpy(wav + 8, "WAVEfmt ", 8);
	write32(wav + 16, 16);
	write16(wav + 20, 1);
	write16(wav + 22, channels);
	write32(wav + 24, rate);
	write32(wav + 28, rate * channels * 2);
	write16(wav + 32, channels * 2);
	write16(wav + 34, 16);
	memcpy(wav + 36, "data", 4);
	write32(wav + 40, (uint32_t)bytes);
	*size = 44 + bytes;
	return wav;
}

/* The sample of a PCM or float frame's channel, as 16 bits. */
static int pcm_sample(const uint8_t* p, unsigned bits, bool is_float) {
	if (is_float) {
		float value;
		memcpy(&value, p, 4);
		return clamp16((int)(value * 32767.0f));
	}
	switch (bits) {
		case 8:
			return (p[0] - 128) * 256;
		case 16:
			return (int16_t)read16(p);
		case 24:
			return (int16_t)read16(p + 1);
		default:
			return (int16_t)read16(p + 2);
	}
}

/* Decodes MS ADPCM blocks into 16-bit samples. */
static uint8_t* decode_ms_adpcm(const uint8_t* fmt, uint32_t fmt_size, const uint8_t* data, size_t size,
								size_t* out_size) {
	static const int adaptation[16] = { 230, 230, 230, 230, 307, 409, 512, 614,
										768, 614, 512, 409, 307, 230, 230, 230 };
	const unsigned   channels       = read16(fmt + 2);
	const unsigned   block_align    = read16(fmt + 12);
	if (fmt_size < 22 || channels < 1 || channels > 2 || block_align < 7 * channels)
		return NULL;
	const unsigned per_block = read16(fmt + 18);
	const unsigned coefs     = read16(fmt + 20);
	if (per_block < 2 || fmt_size < 22 + coefs * 4)
		return NULL;
	const size_t blocks = (size + block_align - 1) / block_align;
	uint8_t*     wav    = new_wav(channels, read32(fmt + 4), blocks * per_block, out_size);
	if (!wav)
		return NULL;
	uint8_t* out    = wav + 44;
	size_t   frames = 0;
	for (size_t at = 0; at < size; at += block_align) {
		const uint8_t* block = data + at;
		const size_t   left  = size - at < block_align ? size - at : block_align;
		if (left < 7 * channels)
			break;
		int c1[2], c2[2], delta[2], s1[2], s2[2];
		for (unsigned ch = 0; ch < channels; ++ch) {
			const unsigned predictor = block[ch] < coefs ? block[ch] : 0;
			c1[ch]                   = (int16_t)read16(fmt + 22 + predictor * 4);
			c2[ch]                   = (int16_t)read16(fmt + 24 + predictor * 4);
			delta[ch]                = (int16_t)read16(block + channels + ch * 2);
			s1[ch]                   = (int16_t)read16(block + channels * 3 + ch * 2);
			s2[ch]                   = (int16_t)read16(block + channels * 5 + ch * 2);
		}
		for (unsigned ch = 0; ch < channels; ++ch)
			write16(out + (frames * channels + ch) * 2, (uint16_t)s2[ch]);
		for (unsigned ch = 0; ch < channels; ++ch)
			write16(out + ((frames + 1) * channels + ch) * 2, (uint16_t)s1[ch]);
		size_t   block_frames = 2;
		unsigned ch           = 0;
		for (size_t i = 7 * channels; i < left && block_frames < per_block; ++i) {
			for (int half = 0; half < 2; ++half) {
				const int nibble    = half == 0 ? block[i] >> 4 : block[i] & 15;
				const int sum       = s1[ch] * c1[ch] + s2[ch] * c2[ch];
				const int predicted = sum >= 0 ? sum / 256 : -((255 - sum) / 256); /* rounded down */
				const int sample    = clamp16(predicted + (nibble >= 8 ? nibble - 16 : nibble) * delta[ch]);
				s2[ch]              = s1[ch];
				s1[ch]              = sample;
				delta[ch]           = adaptation[nibble] * delta[ch] / 256;
				if (delta[ch] < 16)
					delta[ch] = 16;
				write16(out + ((frames + block_frames) * channels + ch) * 2, (uint16_t)sample);
				if (++ch == channels) {
					ch = 0;
					++block_frames;
				}
			}
		}
		frames += block_frames;
	}
	const size_t bytes = frames * channels * 2;
	write32(wav + 4, (uint32_t)(36 + bytes));
	write32(wav + 40, (uint32_t)bytes);
	*out_size = 44 + bytes;
	return wav;
}

XwauWavResult XwauSoundBanks_ConvertWav(const uint8_t* data, size_t size, uint8_t** out, size_t* out_size) {
	*out = NULL;
	if (size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
		return XWAU_WAV_BAD;
	const uint8_t* fmt      = NULL;
	const uint8_t* samples  = NULL;
	uint32_t       fmt_size = 0, sample_bytes = 0;
	for (size_t at = 12; at + 8 <= size;) {
		const uint32_t chunk = read32(data + at + 4);
		const size_t   left  = size - at - 8;
		if (memcmp(data + at, "fmt ", 4) == 0 && chunk <= left) {
			fmt      = data + at + 8;
			fmt_size = chunk;
		} else if (memcmp(data + at, "data", 4) == 0) {
			samples      = data + at + 8;
			sample_bytes = chunk <= left ? chunk : (uint32_t)left;
		}
		if (chunk > left)
			break;
		at += 8 + (size_t)chunk + (chunk & 1);
	}
	if (!fmt || fmt_size < 16 || !samples)
		return XWAU_WAV_BAD;
	unsigned       tag      = read16(fmt);
	const unsigned channels = read16(fmt + 2);
	const uint32_t rate     = read32(fmt + 4);
	const unsigned align    = read16(fmt + 12);
	const unsigned bits     = read16(fmt + 14);
	if (channels == 0 || rate == 0)
		return XWAU_WAV_BAD;
	if (tag == 2) {
		*out = decode_ms_adpcm(fmt, fmt_size, samples, sample_bytes, out_size);
		return *out ? XWAU_WAV_CONVERTED : XWAU_WAV_BAD;
	}
	const bool extensible = tag == 0xfffe;
	if (extensible) {
		if (fmt_size < 40)
			return XWAU_WAV_BAD;
		tag = read16(fmt + 24); /* the sub-format GUID starts with the format tag */
	}
	const bool is_float = tag == 3 && bits == 32;
	if ((tag != 1 && !is_float) || (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
		align != channels * bits / 8)
		return XWAU_WAV_BAD;
	if (!extensible && tag == 1 && (bits == 8 || bits == 16) && channels <= 2)
		return XWAU_WAV_PLAYABLE;

	/* Channels past the second are mixed into the first channel (even
	   channels) or the second channel (odd channels). */
	const unsigned out_channels = channels > 2 ? 2 : channels;
	const size_t   frames       = sample_bytes / align;
	uint8_t*       wav          = new_wav(out_channels, rate, frames, out_size);
	if (!wav)
		return XWAU_WAV_BAD;
	for (size_t frame = 0; frame < frames; ++frame) {
		int sums[2] = { 0, 0 }, counts[2] = { 0, 0 };
		for (unsigned ch = 0; ch < channels; ++ch) {
			sums[ch % out_channels] += pcm_sample(samples + frame * align + ch * bits / 8, bits, is_float);
			++counts[ch % out_channels];
		}
		for (unsigned ch = 0; ch < out_channels; ++ch)
			write16(wav + 44 + (frame * out_channels + ch) * 2, (uint16_t)(sums[ch] / counts[ch]));
	}
	*out = wav;
	return XWAU_WAV_CONVERTED;
}

/* ---- Banks ---------------------------------------------------------------- */

typedef struct Entry {
	char path[300]; /* the game path, Wave\Sfx_<Bank>\<name> */
	int  volume;
	int  distance;
} Entry;

/* A file that an entry names. */
typedef struct Known {
	char* rel; /* its path in the installation */
	bool  playable;
} Known;

typedef struct Import {
	const char* game;
	const char* game_files;
	Known*      known; /* the files already copied or found bad */
	size_t      known_count;
	int         files, converted, missing;
} Import;

/* Returns the number of lines that XWAU counts in a list, which is the
 * number of lines that aren't blank. */
static int xwau_line_count(const char* text) {
	int count = 0;
	for (const char* line = text; *line;) {
		const char* end = strchr(line, '\n');
		if (!end)
			end = line + strlen(line);
		for (const char* c = line; c < end; ++c) {
			if (!strchr(" \t\r\f\v", *c)) {
				++count;
				break;
			}
		}
		line = *end ? end + 1 : end;
	}
	return count;
}

static const Known* find_known(const Import* im, const char* rel) {
	for (size_t i = 0; i < im->known_count; ++i)
		if (strcasecmp(im->known[i].rel, rel) == 0)
			return &im->known[i];
	return NULL;
}

static void add_known(Import* im, const char* rel, bool playable) {
	char* copy = StrDup(rel);
	if (copy && ArrayGrow((void**)&im->known, im->known_count, sizeof *im->known))
		im->known[im->known_count++] = (Known) { copy, playable };
	else
		free(copy);
}

/* Copies the file at `path` (`rel` in the installation) into the game files
 * folder. If the game can't play the file, the copy is converted. Returns
 * false if the file isn't a WAV file that the game can play; the entry is
 * then silent. */
static bool copy_sound(Import* im, const char* path, const char* rel, const char* list, bool* error) {
	const Known* known = find_known(im, rel);
	if (known)
		return known->playable;
	size_t   size = 0, out_size = 0;
	uint8_t* data = (uint8_t*)ReadWholeFile(path, &size);
	uint8_t* out  = NULL;
	if (!data) {
		Report(path, "cannot be read");
		*error = true;
		return false;
	}
	/* An empty file, such as XU's Blank.wav, is a way to silence an entry. */
	if (size == 0) {
		add_known(im, rel, false);
		free(data);
		return false;
	}
	const XwauWavResult result = XwauSoundBanks_ConvertWav(data, size, &out, &out_size);
	if (result == XWAU_WAV_BAD) {
		Report(list, "%s isn't a WAV file that the game can play, so its entries are silent", rel);
		add_known(im, rel, false);
		free(data);
		return false;
	}
	char target[2048], dir[2048];
	bool ok = PathFormat(target, sizeof target, "%s/%s", im->game_files, rel);
	for (char* c = target; ok && *c; ++c)
		if (*c == '\\')
			*c = '/';
	const char* slash = ok ? strrchr(target, '/') : NULL;
	ok = ok && slash && PathFormat(dir, sizeof dir, "%.*s", (int)(slash - target), target) && MakeDirs(dir);
	if (ok && !(ok = out ? WriteWholeFile(target, out, out_size) : WriteWholeFile(target, data, size)))
		Report(NULL, "cannot write %s", target);
	im->converted += ok && out;
	free(data);
	free(out);
	if (!ok) {
		*error = true;
		return false;
	}
	add_known(im, rel, true);
	++im->files;
	return true;
}

static void write_entry(FILE* f, const Entry* entry) {
	fprintf(f, "{file: ");
	Xwau_WriteString(f, entry->path);
	fprintf(f, ", volume: %d, distance: %d}\n", entry->volume, entry->distance);
}

/* Writes the entries of one bank. `entries[i]` is NULL for a silent slot. */
static void write_bank(FILE* f, const BankInfo* bank, const char* list, int count, Entry* const* entries,
					   int slots) {
	static const char* const layouts[] = { "by name", "by position", "by object type" };
	if (bank->layout > 1)
		fprintf(f, "  %s: # %s, %d by object type\n", bank->key, list, bank->layout);
	else
		fprintf(f, "  %s: # %s, %s\n", bank->key, list, layouts[bank->layout + 1]);
	fprintf(f, "    count: %d\n    sounds:", count);
	bool any   = false;
	int  group = -1;
	for (int slot = 0; slot < slots; ++slot) {
		if (!entries[slot])
			continue;
		if (!any)
			fprintf(f, "\n");
		any = true;
		if (bank->layout == BANK_BY_NAME) {
			fprintf(f, "      %s: ", g_soundNames[slot]);
		} else if (bank->layout > 1) {
			if (slot / bank->layout != group) {
				group = slot / bank->layout;
				fprintf(f, "      %d:\n", group);
			}
			fprintf(f, "        %d: ", slot % bank->layout);
		} else {
			fprintf(f, "      %d: ", slot);
		}
		write_entry(f, entries[slot]);
	}
	if (!any)
		fprintf(f, " {}\n");
}

/* Reads one bank list, copies its files and writes the bank. */
static bool import_bank(Import* im, FILE* f, const BankInfo* bank, const char* list_path, const char* stem,
						int* entry_count) {
	char   list[300];
	size_t size = 0;
	snprintf(list, sizeof list, "Wave/%s.lst", stem);
	char* text = ReadWholeFile(list_path, &size);
	if (!text) {
		Report(list_path, "cannot be read");
		return false;
	}
	const int count = xwau_line_count(text);
	int slots = bank->layout == BANK_BY_NAME ? (int)(sizeof g_soundNames / sizeof g_soundNames[0]) : count;
	if (slots > count)
		slots = count;
	Entry*  entries = (Entry*)calloc(slots ? (size_t)slots : 1, sizeof *entries);
	Entry** used    = (Entry**)calloc(slots ? (size_t)slots : 1, sizeof *used);
	bool    error   = !entries || !used;
	int     slot = 0, missing = 0, past = 0;
	/* The files that the entries name and the installation doesn't have. */
	XwauLines missing_names = { 0 };

	/* The game reads the list with fgets and a 256-byte buffer, so a longer
	   line is read in parts. */
	for (const char* line = text; !error && *line;) {
		const char* end = line;
		while (*end && *end != '\n' && end - line < 255)
			++end;
		if (*end == '\n' && end - line < 255)
			++end;
		char part[256];
		memcpy(part, line, (size_t)(end - line));
		part[end - line] = '\0';
		line             = end;
		XwauSoundLine parsed;
		if (!XwauSoundBanks_ParseLine(part, &parsed))
			continue;
		const int this_slot = slot++;
		if (this_slot >= slots) {
			past += bank->layout != BANK_BY_NAME;
			continue;
		}
		char rel[600], path[1024];
		snprintf(rel, sizeof rel, "Wave/%s/%s", stem, parsed.name);
		for (char* c = rel; *c; ++c)
			if (*c == '\\')
				*c = '/';
		if (!Xwau_FindFile(im->game, rel, path, sizeof path)) {
			if (strcasecmp(parsed.name, "unused") != 0) {
				bool known = false;
				for (size_t i = 0; !known && i < missing_names.count; ++i)
					known = strcasecmp(missing_names.items[i], parsed.name) == 0;
				if (!known)
					error = !XwauLines_Add(&missing_names, parsed.name, strlen(parsed.name));
				++missing;
			}
			continue;
		}
		/* The file's own name, in the installation's case. */
		const char* found = path + strlen(path) - strlen(rel);
		if (!copy_sound(im, path, found, list, &error))
			continue;
		Entry* entry = &entries[this_slot];
		snprintf(entry->path, sizeof entry->path, "Wave\\%s\\%s", stem, parsed.name);
		entry->volume   = parsed.volume;
		entry->distance = parsed.distance;
		used[this_slot] = entry;
		*entry_count += 1;
	}
	if (missing) {
		char names[600] = "";
		for (size_t i = 0; i < missing_names.count && i < 5; ++i)
			snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? ", " : "",
					 missing_names.items[i]);
		Report(list, "%d entries name %zu files that aren't in the installation, so they're silent: %s%s",
			   missing, missing_names.count, names, missing_names.count > 5 ? " and more" : "");
	}
	XwauLines_Free(&missing_names);
	im->missing += missing;
	if (past)
		Report(list, "%d entries are past the %d lines that XWAU counts, and no hook reads them", past,
			   count);
	if (!error)
		write_bank(f, bank, list, count, used, slots);
	free(entries);
	free(used);
	free(text);
	return !error;
}

int XwauSoundBanks_LineCount(const char* game, const char* name) {
	char hook[1024], rel[300], path[1024];
	snprintf(rel, sizeof rel, "Wave/Sfx_%s.lst", name);
	if (!Xwau_FindFile(game, "hook_sounds_count.dll", hook, sizeof hook) ||
		!Xwau_FindFile(game, rel, path, sizeof path))
		return 0;
	size_t size  = 0;
	char*  text  = ReadWholeFile(path, &size);
	int    count = text ? xwau_line_count(text) : 0;
	free(text);
	return count;
}

bool XwauSoundBanks_Write(const char* game, const char* output_dir, const char* game_files) {
	char hook[1024], wave[1024];
	if (!Xwau_FindFile(game, "hook_sounds_count.dll", hook, sizeof hook) ||
		!Xwau_FindDir(game, "Wave", wave, sizeof wave))
		return true;
	size_t count = 0;
	char** names = ListDir(wave, true, &count);
	Import im    = { .game = game, .game_files = game_files };
	char   path[1024];
	bool   ok    = PathFormat(path, sizeof path, "%s/sounds.yaml", output_dir) && MakeDirs(output_dir);
	FILE*  f     = ok ? fopen(path, "wb") : NULL;
	int    banks = 0, entries = 0;
	if (ok && !f) {
		Report(path, "can't be written");
		ok = false;
	}
	if (f)
		fprintf(f,
				"# Written by xwau_import craft from the Wave/Sfx_*.lst sound banks.\nversion: 1\nbanks:\n");
	for (size_t i = 0; ok && i < count; ++i) {
		const size_t length = strlen(names[i]);
		if (length <= 8 || strncasecmp(names[i], "sfx_", 4) != 0 || !EndsWithNoCase(names[i], ".lst"))
			continue;
		char stem[256], list_path[2048];
		snprintf(stem, sizeof stem, "%.*s", (int)(length - 4), names[i]);
		if (!PathFormat(list_path, sizeof list_path, "%s/%s", wave, names[i]) || !IsFile(list_path))
			continue;
		const BankInfo* bank = NULL;
		for (size_t b = 0; !bank && b < sizeof g_banks / sizeof g_banks[0]; ++b)
			if (strcasecmp(stem + 4, g_banks[b].name) == 0)
				bank = &g_banks[b];
		char dll[1024];
		if (!bank) {
			Report(NULL, "Wave/%s: no hook reads this sound bank, so it isn't imported", names[i]);
			continue;
		}
		if (!Xwau_FindFile(game, bank->hook, dll, sizeof dll)) {
			Report(NULL,
				   "Wave/%s: %s reads this sound bank, and the installation doesn't have it, so it "
				   "isn't imported",
				   names[i], bank->hook);
			continue;
		}
		ok = import_bank(&im, f, bank, list_path, stem, &entries);
		++banks;
	}
	if (f && fclose(f) != 0 && ok) {
		Report(path, "can't be written");
		ok = false;
	}
	FreeList(names, count);
	for (size_t i = 0; i < im.known_count; ++i)
		free(im.known[i].rel);
	free(im.known);
	if (ok)
		Report(NULL,
			   "sounds.yaml: %d banks, %d entries; %d files copied into %s, %d of them converted to 16-bit "
			   "PCM; %d entries name files that aren't in the installation",
			   banks, entries, im.files, game_files, im.converted, im.missing);
	return ok;
}
