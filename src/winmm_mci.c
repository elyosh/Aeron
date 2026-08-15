#include "aeron/winmm/compat.h"
#include "aeron/winmm/mmsystem.h"

#include "aeron/audio.h"
#include "aeron/audio_decode.h"
#include "aeron/log.h"
#include "aeron/sync.h"

#include <SDL3/SDL.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum {
	AERON_WINMM_MAX_TRACKS     = 30,
	AERON_WINMM_DIRECTORY_SIZE = 512,
	AERON_WINMM_PATH_SIZE      = 1024,
	AERON_WINMM_DECODE_QUANTUM = 4096,
};

typedef enum AeronWinmmCommand {
	AERON_WINMM_COMMAND_NONE,
	AERON_WINMM_COMMAND_PLAY,
	AERON_WINMM_COMMAND_STOP,
	AERON_WINMM_COMMAND_QUIT,
} AeronWinmmCommand;

typedef struct AeronWinmmCdState {
	AeronVfs*    vfs;
	AeronVfsRoot root;
	char         directory[AERON_WINMM_DIRECTORY_SIZE];
	int          configured;
	MCIDEVICEID  open_id;
	MCIDEVICEID  next_id;
	int          time_format_tmsf;
	int          track_count;
	uint8_t      track_present[AERON_WINMM_MAX_TRACKS + 1];
	int64_t      track_duration_us[AERON_WINMM_MAX_TRACKS + 1];
	uint32_t     aux_volume;

	SDL_Mutex*        lock;
	SDL_Condition*    changed;
	AeronThread*      worker;
	AeronWinmmCommand command;
	uint64_t          request_generation;
	uint64_t          acknowledged_generation;
	AeronFile*        request_file;
	char              request_path[AERON_WINMM_PATH_SIZE];
	int64_t           request_from_us;
	int64_t           request_to_us;
	AeronAudioStream  stream;
} AeronWinmmCdState;

static AeronWinmmCdState g_cd;

static int AeronWinmm_TrackPath(char* path, size_t capacity, int track) {
	const int count = SDL_snprintf(path, capacity, "%s/Track%02d.ogg", g_cd.directory, track);
	return count > 0 && (size_t)count < capacity;
}

static int AeronWinmm_GenerationCurrent(uint64_t generation) {
	int current;
	SDL_LockMutex(g_cd.lock);
	current = g_cd.request_generation == generation;
	SDL_UnlockMutex(g_cd.lock);
	return current;
}

static void AeronWinmm_Acknowledge(uint64_t generation) {
	SDL_LockMutex(g_cd.lock);
	if (g_cd.acknowledged_generation < generation)
		g_cd.acknowledged_generation = generation;
	SDL_BroadcastCondition(g_cd.changed);
	SDL_UnlockMutex(g_cd.lock);
}

static int AeronWinmm_ReplaceStream(AeronAudioStream stream, uint64_t generation) {
	AeronAudioStream old_stream = 0;
	int              accepted   = 0;
	SDL_LockMutex(g_cd.lock);
	if (g_cd.request_generation == generation) {
		old_stream  = g_cd.stream;
		g_cd.stream = stream;
		Aeron_AudioStreamPlay(stream);
		accepted = 1;
	}
	SDL_UnlockMutex(g_cd.lock);
	if (old_stream)
		Aeron_AudioStreamClose(old_stream);
	if (!accepted)
		Aeron_AudioStreamClose(stream);
	return accepted;
}

static float AeronWinmm_CurrentGain(void) {
	uint32_t volume;
	SDL_LockMutex(g_cd.lock);
	volume = g_cd.aux_volume;
	SDL_UnlockMutex(g_cd.lock);
	return (float)(volume & 0xFFFFu) / 65535.0f;
}

static void AeronWinmm_DecodeRequest(AeronFile* file, const char* path, int64_t from_us, int64_t to_us,
									 uint64_t generation) {
	AeronAudioDecoder*    decoder;
	AeronAudioDecoderInfo info;
	AeronAudioStream      stream;
	int16_t               pcm[AERON_WINMM_DECODE_QUANTUM * 2];
	int64_t               remaining_frames;

	decoder = Aeron_AudioDecoderOpenFile(file, path);
	if (!decoder)
		return;
	if (!AeronWinmm_GenerationCurrent(generation)) {
		Aeron_AudioDecoderClose(decoder);
		return;
	}
	Aeron_AudioDecoderGetInfo(decoder, &info);
	if (info.sample_rate <= 0 || info.channels != 2 || !Aeron_AudioDecoderSeekUs(decoder, from_us)) {
		Aeron_AudioDecoderClose(decoder);
		return;
	}
	if (!AeronWinmm_GenerationCurrent(generation)) {
		Aeron_AudioDecoderClose(decoder);
		return;
	}
	stream = Aeron_AudioStreamOpen(info.sample_rate, 2, AERON_PCM_S16, (size_t)info.sample_rate,
								   AeronWinmm_CurrentGain());
	if (!stream) {
		Aeron_AudioDecoderClose(decoder);
		return;
	}
	if (!AeronWinmm_ReplaceStream(stream, generation)) {
		Aeron_AudioDecoderClose(decoder);
		return;
	}
	remaining_frames = (to_us - from_us) * info.sample_rate / 1000000;
	while (remaining_frames > 0 && AeronWinmm_GenerationCurrent(generation)) {
		size_t wanted = remaining_frames < AERON_WINMM_DECODE_QUANTUM ? (size_t)remaining_frames
																	  : (size_t)AERON_WINMM_DECODE_QUANTUM;
		if (!Aeron_AudioStreamWaitWritable(stream, wanted))
			break;
		if (!AeronWinmm_GenerationCurrent(generation))
			break;
		const size_t decoded = Aeron_AudioDecoderRead(decoder, pcm, wanted);
		if (decoded == 0)
			break;
		if (!AeronWinmm_GenerationCurrent(generation))
			break;
		if (Aeron_AudioStreamWrite(stream, pcm, decoded) != decoded)
			break;
		remaining_frames -= (int64_t)decoded;
	}
	Aeron_AudioDecoderClose(decoder);
}

static int AeronWinmm_WorkerMain(void* userdata) {
	uint64_t processed_generation = 0;
	(void)userdata;

	for (;;) {
		AeronWinmmCommand command;
		uint64_t          generation;
		AeronFile*        file = NULL;
		char              path[AERON_WINMM_PATH_SIZE];
		int64_t           from_us = 0;
		int64_t           to_us   = 0;

		SDL_LockMutex(g_cd.lock);
		while (g_cd.request_generation == processed_generation)
			SDL_WaitCondition(g_cd.changed, g_cd.lock);
		generation = g_cd.request_generation;
		command    = g_cd.command;
		if (command == AERON_WINMM_COMMAND_PLAY) {
			file              = g_cd.request_file;
			g_cd.request_file = NULL;
			SDL_strlcpy(path, g_cd.request_path, sizeof path);
			from_us = g_cd.request_from_us;
			to_us   = g_cd.request_to_us;
		}
		SDL_UnlockMutex(g_cd.lock);

		if (command == AERON_WINMM_COMMAND_QUIT) {
			if (file)
				AeronVfs_Close(file);
			AeronWinmm_Acknowledge(generation);
			break;
		}
		if (command == AERON_WINMM_COMMAND_PLAY && file)
			AeronWinmm_DecodeRequest(file, path, from_us, to_us, generation);
		else if (file)
			AeronVfs_Close(file);
		processed_generation = generation;
		AeronWinmm_Acknowledge(generation);
	}
	return 0;
}

static void AeronWinmm_ClearTracks(void) {
	g_cd.track_count = 0;
	memset(g_cd.track_present, 0, sizeof g_cd.track_present);
	memset(g_cd.track_duration_us, 0, sizeof g_cd.track_duration_us);
}

static int AeronWinmm_ScanTracks(void) {
	char path[AERON_WINMM_PATH_SIZE];
	AeronWinmm_ClearTracks();
	for (int track = 1; track <= AERON_WINMM_MAX_TRACKS; ++track) {
		AeronAudioDecoder*    decoder;
		AeronAudioDecoderInfo info;
		if (!AeronWinmm_TrackPath(path, sizeof path, track))
			return 0;
		if (!AeronVfs_Exists(g_cd.vfs, g_cd.root, path))
			continue;
		decoder = Aeron_AudioDecoderOpen(g_cd.vfs, g_cd.root, path);
		if (!decoder)
			return 0;
		Aeron_AudioDecoderGetInfo(decoder, &info);
		Aeron_AudioDecoderClose(decoder);
		if (info.duration_us <= 0)
			return 0;
		g_cd.track_present[track]     = 1;
		g_cd.track_duration_us[track] = info.duration_us;
		g_cd.track_count              = track;
	}
	return g_cd.track_count != 0;
}

static void AeronWinmm_PostStopAndWait(void) {
	uint64_t generation;
	if (!g_cd.worker)
		return;
	SDL_LockMutex(g_cd.lock);
	if (g_cd.request_file) {
		AeronVfs_Close(g_cd.request_file);
		g_cd.request_file = NULL;
	}
	if (g_cd.stream) {
		Aeron_AudioStreamPause(g_cd.stream);
		Aeron_AudioStreamFlush(g_cd.stream);
	}
	generation   = ++g_cd.request_generation;
	g_cd.command = AERON_WINMM_COMMAND_STOP;
	SDL_BroadcastCondition(g_cd.changed);
	while (g_cd.acknowledged_generation < generation)
		SDL_WaitCondition(g_cd.changed, g_cd.lock);
	SDL_UnlockMutex(g_cd.lock);
}

static void AeronWinmm_CloseDevice(void) {
	AeronAudioStream stream;
	if (!g_cd.open_id)
		return;
	AeronWinmm_PostStopAndWait();
	SDL_LockMutex(g_cd.lock);
	stream      = g_cd.stream;
	g_cd.stream = 0;
	SDL_UnlockMutex(g_cd.lock);
	if (stream)
		Aeron_AudioStreamClose(stream);
	g_cd.open_id          = 0;
	g_cd.time_format_tmsf = 0;
	AeronWinmm_ClearTracks();
}

static uint32_t AeronWinmm_DurationToMsf(int64_t duration_us) {
	const int64_t  frames = duration_us * 75 / 1000000;
	const uint32_t minute = (uint32_t)(frames / (75 * 60));
	const uint32_t second = (uint32_t)((frames / 75) % 60);
	const uint32_t frame  = (uint32_t)(frames % 75);
	return minute | (second << 8) | (frame << 16);
}

static int64_t AeronWinmm_TmsfPositionUs(uint32_t value) {
	const int64_t seconds = (int64_t)MCI_TMSF_MINUTE(value) * 60 + MCI_TMSF_SECOND(value);
	return seconds * 1000000 + (int64_t)MCI_TMSF_FRAME(value) * 1000000 / 75;
}

static MMRESULT AeronWinmm_Open(MciDwordPtr flags, MCI_OPEN_PARMSA* params) {
	if (!params || !(flags & MCI_OPEN_TYPE) || !params->lpstrDeviceType ||
		SDL_strcasecmp(params->lpstrDeviceType, "cdaudio") != 0 || !g_cd.configured)
		return MCIERR_INVALID_DEVICE_ID;
	AeronWinmm_CloseDevice();
	if (!AeronWinmm_ScanTracks())
		return MCIERR_INVALID_DEVICE_ID;
	if (!g_cd.worker) {
		g_cd.worker = Aeron_ThreadCreate("aeron-cdaudio", AeronWinmm_WorkerMain, NULL);
		if (!g_cd.worker) {
			AeronWinmm_ClearTracks();
			return MCIERR_INVALID_DEVICE_ID;
		}
	}
	if (++g_cd.next_id == 0)
		++g_cd.next_id;
	g_cd.open_id      = g_cd.next_id;
	params->wDeviceID = g_cd.open_id;
	return MMSYSERR_NOERROR;
}

static MMRESULT AeronWinmm_Status(MCIDEVICEID device_id, MciDwordPtr flags, MCI_STATUS_PARMS* params) {
	if (!params || device_id != g_cd.open_id || !g_cd.open_id)
		return MCIERR_INVALID_DEVICE_ID;
	if (!(flags & MCI_STATUS_ITEM))
		return MCIERR_UNSUPPORTED_FUNCTION;
	if (params->dwItem == MCI_STATUS_NUMBER_OF_TRACKS) {
		params->dwReturn = (MciDwordPtr)g_cd.track_count;
		return MMSYSERR_NOERROR;
	}
	if (params->dwItem == MCI_STATUS_LENGTH && (flags & MCI_TRACK)) {
		if (params->dwTrack < 1 || params->dwTrack > (uint32_t)g_cd.track_count)
			return MCIERR_OUTOFRANGE;
		params->dwReturn = (MciDwordPtr)AeronWinmm_DurationToMsf(g_cd.track_duration_us[params->dwTrack]);
		return MMSYSERR_NOERROR;
	}
	return MCIERR_UNSUPPORTED_FUNCTION;
}

static MMRESULT AeronWinmm_Play(MCIDEVICEID device_id, MciDwordPtr flags, MCI_PLAY_PARMS* params) {
	char       path[AERON_WINMM_PATH_SIZE];
	AeronFile* file = NULL;
	int        track;
	int64_t    from_us;
	int64_t    to_us;

	if (!params || device_id != g_cd.open_id || !g_cd.open_id || !g_cd.time_format_tmsf)
		return MCIERR_INVALID_DEVICE_ID;
	if (!(flags & MCI_FROM))
		return MCIERR_UNSUPPORTED_FUNCTION;
	track = MCI_TMSF_TRACK(params->dwFrom);
	if (track < 1 || track > g_cd.track_count || !g_cd.track_present[track])
		return MCIERR_OUTOFRANGE;
	from_us = AeronWinmm_TmsfPositionUs(params->dwFrom);
	to_us   = (flags & MCI_TO) ? AeronWinmm_TmsfPositionUs(params->dwTo) : g_cd.track_duration_us[track];
	if (to_us > g_cd.track_duration_us[track])
		to_us = g_cd.track_duration_us[track];
	if (from_us < 0 || from_us >= to_us)
		return MCIERR_OUTOFRANGE;
	if (!AeronWinmm_TrackPath(path, sizeof path, track) ||
		!AeronVfs_Open(g_cd.vfs, g_cd.root, path, AERON_VFS_READ, &file))
		return MCIERR_INVALID_DEVICE_ID;

	SDL_LockMutex(g_cd.lock);
	if (g_cd.request_file)
		AeronVfs_Close(g_cd.request_file);
	if (g_cd.stream) {
		Aeron_AudioStreamPause(g_cd.stream);
		Aeron_AudioStreamFlush(g_cd.stream);
	}
	g_cd.request_file = file;
	SDL_strlcpy(g_cd.request_path, path, sizeof g_cd.request_path);
	g_cd.request_from_us = from_us;
	g_cd.request_to_us   = to_us;
	++g_cd.request_generation;
	g_cd.command = AERON_WINMM_COMMAND_PLAY;
	SDL_BroadcastCondition(g_cd.changed);
	SDL_UnlockMutex(g_cd.lock);
	return MMSYSERR_NOERROR;
}

MMRESULT AERON_WINMMAPI AeronWinmm_MciSendCommandA(MCIDEVICEID device_id, uint32_t message, MciDwordPtr flags,
												   MciDwordPtr params_value) {
	void* params = (void*)(uintptr_t)params_value;
	switch (message) {
		case MCI_OPEN:
			return AeronWinmm_Open(flags, (MCI_OPEN_PARMSA*)params);
		case MCI_CLOSE:
			if (device_id != g_cd.open_id || !g_cd.open_id)
				return MCIERR_INVALID_DEVICE_ID;
			AeronWinmm_CloseDevice();
			return MMSYSERR_NOERROR;
		case MCI_PLAY:
			return AeronWinmm_Play(device_id, flags, (MCI_PLAY_PARMS*)params);
		case MCI_STOP:
			if (device_id != g_cd.open_id || !g_cd.open_id)
				return MCIERR_INVALID_DEVICE_ID;
			AeronWinmm_PostStopAndWait();
			return MMSYSERR_NOERROR;
		case MCI_SET: {
			MCI_SET_PARMS* set = (MCI_SET_PARMS*)params;
			if (device_id != g_cd.open_id || !g_cd.open_id)
				return MCIERR_INVALID_DEVICE_ID;
			if (!set || !(flags & MCI_SET_TIME_FORMAT) || set->dwTimeFormat != MCI_FORMAT_TMSF)
				return MCIERR_UNSUPPORTED_FUNCTION;
			g_cd.time_format_tmsf = 1;
			return MMSYSERR_NOERROR;
		}
		case MCI_STATUS:
			return AeronWinmm_Status(device_id, flags, (MCI_STATUS_PARMS*)params);
		default:
			Aeron_LogWarn("aeron.winmm", "unsupported MCI message 0x%x", message);
			return MCIERR_UNRECOGNIZED_COMMAND;
	}
}

uint32_t AERON_WINMMAPI AeronWinmm_AuxGetNumDevs(void) { return g_cd.configured ? 1u : 0u; }

MMRESULT AERON_WINMMAPI AeronWinmm_AuxGetDevCapsA(uintptr_t device_id, AUXCAPSA* caps, uint32_t size) {
	if (!g_cd.configured || device_id != 0)
		return MMSYSERR_BADDEVICEID;
	if (!caps || size < sizeof(*caps))
		return MCIERR_OUTOFRANGE;
	memset(caps, 0, sizeof(*caps));
	SDL_strlcpy(caps->szPname, "Aeron CD Audio", sizeof caps->szPname);
	caps->wTechnology = AUXCAPS_CDAUDIO;
	caps->dwSupport   = AUXCAPS_VOLUME | AUXCAPS_LRVOLUME;
	return MMSYSERR_NOERROR;
}

MMRESULT AERON_WINMMAPI AeronWinmm_AuxGetVolume(uintptr_t device_id, uint32_t* volume) {
	if (!g_cd.configured || device_id != 0)
		return MMSYSERR_BADDEVICEID;
	if (!volume)
		return MCIERR_OUTOFRANGE;
	SDL_LockMutex(g_cd.lock);
	*volume = g_cd.aux_volume;
	SDL_UnlockMutex(g_cd.lock);
	return MMSYSERR_NOERROR;
}

MMRESULT AERON_WINMMAPI AeronWinmm_AuxSetVolume(uintptr_t device_id, uint32_t volume) {
	if (!g_cd.configured || device_id != 0)
		return MMSYSERR_BADDEVICEID;
	SDL_LockMutex(g_cd.lock);
	g_cd.aux_volume = volume;
	if (g_cd.stream)
		Aeron_AudioStreamSetGain(g_cd.stream, (float)(volume & 0xFFFFu) / 65535.0f);
	SDL_UnlockMutex(g_cd.lock);
	return MMSYSERR_NOERROR;
}

int AeronWinmm_ConfigureCdAudio(const AeronWinmmCdAudioDesc* desc) {
	if (!desc || !desc->vfs || !desc->directory || !desc->directory[0])
		return 0;
	if (!g_cd.lock) {
		g_cd.lock    = SDL_CreateMutex();
		g_cd.changed = SDL_CreateCondition();
		if (!g_cd.lock || !g_cd.changed) {
			AeronWinmm_Shutdown();
			return 0;
		}
	}
	if (SDL_strlen(desc->directory) >= sizeof g_cd.directory)
		return 0;
	g_cd.vfs  = desc->vfs;
	g_cd.root = desc->root;
	SDL_strlcpy(g_cd.directory, desc->directory, sizeof g_cd.directory);
	g_cd.aux_volume = UINT32_MAX;
	g_cd.configured = 1;
	return 1;
}

void AeronWinmm_Shutdown(void) {
	AeronThread*     worker;
	AeronAudioStream stream = 0;

	if (!g_cd.lock) {
		if (g_cd.changed)
			SDL_DestroyCondition(g_cd.changed);
		memset(&g_cd, 0, sizeof g_cd);
		return;
	}
	AeronWinmm_CloseDevice();
	SDL_LockMutex(g_cd.lock);
	worker = g_cd.worker;
	if (worker) {
		if (g_cd.request_file) {
			AeronVfs_Close(g_cd.request_file);
			g_cd.request_file = NULL;
		}
		if (g_cd.stream) {
			Aeron_AudioStreamPause(g_cd.stream);
			Aeron_AudioStreamFlush(g_cd.stream);
		}
		++g_cd.request_generation;
		g_cd.command = AERON_WINMM_COMMAND_QUIT;
		SDL_BroadcastCondition(g_cd.changed);
	}
	SDL_UnlockMutex(g_cd.lock);
	if (worker)
		Aeron_ThreadJoin(worker);
	SDL_LockMutex(g_cd.lock);
	stream      = g_cd.stream;
	g_cd.stream = 0;
	SDL_UnlockMutex(g_cd.lock);
	if (stream)
		Aeron_AudioStreamClose(stream);
	if (g_cd.changed)
		SDL_DestroyCondition(g_cd.changed);
	if (g_cd.lock)
		SDL_DestroyMutex(g_cd.lock);
	memset(&g_cd, 0, sizeof g_cd);
}
