#include "internal.h"

#include <math.h>
#include <string.h>

/* Generic SDL3 software mixer. See include/aeron/audio.h for the model.
 *
 * The mixer mutex guards samples, voices, queued streams and controls. Cursor
 * snapshots and playback transitions take the SDL stream lock first, then the
 * mixer mutex, matching the SDL callback's lock order. The SDL stream lock
 * covers mixing and submission so a snapshot observes complete output chunks.
 * Device initialization/shutdown run on the control thread with producers stopped.
 */

#define AERON_AUDIO_DEVICE_RATE 48000
#define AERON_AUDIO_DEVICE_CHANNELS 2
#define AERON_AUDIO_MAX_CLIPS 2048
#define AERON_AUDIO_MAX_VOICES 96
#define AERON_AUDIO_MAX_STREAMS 8
#define AERON_AUDIO_MIX_CHUNK 1024
#define AERON_AUDIO_SOUND_SPEED 343.0f /* metres/second, for doppler */

typedef struct AeronClipData {
	int16_t*       pcm; /* interleaved S16, channels = channels */
	size_t         frames;
	int            rate;
	int            channels;
	int            refcount;        /* voices currently referencing the clip */
	int            pending_destroy; /* destroy requested while still referenced */
	int            mutable_pcm;
	AeronPcmFormat format;
	int            in_use;
	uint16_t       gen;
} AeronClipData;

typedef struct AeronVoiceSlot {
	int       in_use;
	uint64_t  gen;
	AeronClip clip;
	double    pos;      /* fractional frame position within the clip */
	double    advanced; /* source frames mixed since the last seek */
	size_t    start_frame;
	uint64_t  mixed_until; /* output frame immediately after this voice's last sample */
	int       draining;
	float     gain;
	float     pan;   /* 2D pan [-1..1], used when !is3d */
	float     pitch; /* playback-rate ratio */
	int       loop;
	int       is3d;
	float     pos3[3];
	float     vel3[3];
	float     min_dist;
	float     max_dist;
} AeronVoiceSlot;

typedef struct AeronAudioStreamSlot {
	int      in_use;
	uint16_t gen;
	uint8_t* pcm;
	size_t   capacity_frames;
	int      rate;
	int      channels;
	int      bits;
	int      block_align;
	float    gain;
	int      playing;
	uint64_t write_frame;
	double   read_position;
	double   submitted_frames;
	double   audible_frames;
	uint64_t audible_updated_ns;
	uint64_t underruns;
} AeronAudioStreamSlot;

typedef struct AeronAudioSystem {
	int               initialized;
	SDL_AudioStream*  device; /* output device stream */
	SDL_AudioDeviceID device_id;
	int               device_buffer_input_frames;
	int               device_paused;
	uint64_t          device_paused_ns;
	SDL_Mutex*        lock;
	SDL_Condition*    stream_space_available;
	uint64_t          submitted_frames; /* includes silence mixed after a voice ends */

	AeronClipData        clips[AERON_AUDIO_MAX_CLIPS];
	AeronVoiceSlot       voices[AERON_AUDIO_MAX_VOICES];
	AeronAudioStreamSlot streams[AERON_AUDIO_MAX_STREAMS];

	float              master_gain;
	AeronAudioListener listener;
	int                has_listener;
	float              distance_factor;
	float              rolloff_factor;
	float              doppler_factor;

	float   mix[AERON_AUDIO_MIX_CHUNK * 2];
	int16_t stage[AERON_AUDIO_MIX_CHUNK * 2];
} AeronAudioSystem;

static AeronAudioSystem g_audio;

/* Both locks are required for output-clock snapshots. */
static int Aeron_AudioLockOutput(void) {
	if (!g_audio.initialized || !SDL_LockAudioStream(g_audio.device)) {
		return 0;
	}
	SDL_LockMutex(g_audio.lock);
	return 1;
}

static void Aeron_AudioUnlockOutput(void) {
	SDL_UnlockMutex(g_audio.lock);
	SDL_UnlockAudioStream(g_audio.device);
}

static uint64_t Aeron_AudioPlayedOutputFramesLocked(void) {
	int      queued  = SDL_GetAudioStreamQueued(g_audio.device);
	uint64_t pending = (uint64_t)g_audio.device_buffer_input_frames;
	if (queued > 0) {
		pending += (unsigned int)queued / (AERON_AUDIO_DEVICE_CHANNELS * sizeof(int16_t));
	}
	return g_audio.submitted_frames > pending ? g_audio.submitted_frames - pending : 0;
}

/* Handle packing: gen in the high 16 bits (never 0), index in the low 16. */
static uint32_t Aeron_AudioPackHandle(uint16_t index, uint16_t gen) {
	return ((uint32_t)gen << 16) | (uint32_t)index;
}

static uint16_t Aeron_AudioHandleIndex(uint32_t handle) { return (uint16_t)(handle & 0xFFFFu); }

static uint16_t Aeron_AudioHandleGen(uint32_t handle) { return (uint16_t)(handle >> 16); }

static uint16_t Aeron_AudioNextGen(uint16_t gen) {
	uint16_t next = (uint16_t)(gen + 1u);
	return next == 0u ? 1u : next;
}

/* Voice buffers can retain finished handles across many thousands of plays. */
static uint64_t Aeron_AudioNextVoiceGen(uint64_t gen) {
	uint64_t next = (gen + 1u) & (UINT64_MAX >> 16);
	return next == 0u ? 1u : next;
}

/* --- clip helpers (caller holds the lock) -------------------------------- */

static AeronClipData* Aeron_AudioResolveClip(AeronClip clip) {
	uint16_t index = Aeron_AudioHandleIndex(clip);
	if (clip == 0 || index >= AERON_AUDIO_MAX_CLIPS) {
		return NULL;
	}
	AeronClipData* data = &g_audio.clips[index];
	if (!data->in_use || data->gen != Aeron_AudioHandleGen(clip)) {
		return NULL;
	}
	return data;
}

static void Aeron_AudioFreeClipData(AeronClipData* data) {
	SDL_free(data->pcm);
	uint16_t gen = Aeron_AudioNextGen(data->gen);
	memset(data, 0, sizeof(*data));
	data->gen = gen;
}

static void Aeron_AudioClipAddRef(AeronClipData* data) { data->refcount++; }

static void Aeron_AudioClipRelease(AeronClipData* data) {
	if (--data->refcount <= 0 && data->pending_destroy) {
		Aeron_AudioFreeClipData(data);
	}
}

static void Aeron_AudioFreeVoiceLocked(AeronVoiceSlot* voice) {
	AeronClipData* clip = Aeron_AudioResolveClip(voice->clip);
	if (clip) {
		Aeron_AudioClipRelease(clip);
	}
	uint64_t gen = Aeron_AudioNextVoiceGen(voice->gen);
	memset(voice, 0, sizeof(*voice));
	voice->gen = gen;
}

static int Aeron_AudioVoiceFinished(const AeronVoiceSlot* voice, uint64_t played) {
	return voice->draining && played >= voice->mixed_until;
}

/* --- vector helpers ------------------------------------------------------ */

static float Aeron_AudioDot(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void Aeron_AudioCross(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float Aeron_AudioNormalize(float v[3]) {
	float len = sqrtf(Aeron_AudioDot(v, v));
	if (len > 1e-6f) {
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
	return len;
}

/* Equal-power panning for positional audio. */
static void Aeron_AudioEqualPowerPanGains(float gain, float pan, float* out_l, float* out_r) {
	float angle = (pan + 1.0f) * 0.25f * 3.14159265358979323846f;
	*out_l      = cosf(angle) * gain;
	*out_r      = sinf(angle) * gain;
}

/* Amplitude-preserving balance for non-positional audio. At the centre both
 * channels remain at the requested gain; moving away only attenuates the
 * opposite channel. */
static void Aeron_AudioBalanceGains(float gain, float balance, float* out_l, float* out_r) {
	if (balance < -1.0f) {
		balance = -1.0f;
	} else if (balance > 1.0f) {
		balance = 1.0f;
	}
	if (balance <= 0.0f) {
		*out_l = gain;
		*out_r = gain * (1.0f + balance);
	} else {
		*out_l = gain * (1.0f - balance);
		*out_r = gain;
	}
}

/* Computes per-voice left/right gains and the doppler-adjusted step. */
static void Aeron_AudioComputeVoiceMix(const AeronVoiceSlot* voice, const AeronClipData* clip, float* out_l,
									   float* out_r, double* out_step) {
	float base_step = ((float)clip->rate / (float)AERON_AUDIO_DEVICE_RATE) * voice->pitch;

	if (!voice->is3d) {
		Aeron_AudioBalanceGains(voice->gain, voice->pan, out_l, out_r);
		*out_step = base_step;
		return;
	}
	if (!g_audio.has_listener) {
		Aeron_AudioEqualPowerPanGains(voice->gain, 0.0f, out_l, out_r);
		*out_step = base_step;
		return;
	}

	float to_src[3] = { voice->pos3[0] - g_audio.listener.pos[0], voice->pos3[1] - g_audio.listener.pos[1],
						voice->pos3[2] - g_audio.listener.pos[2] };
	float dist      = Aeron_AudioNormalize(to_src) * g_audio.distance_factor;

	/* Inverse-distance attenuation (DirectSound3D / OpenAL model). */
	float atten    = 1.0f;
	float min_dist = voice->min_dist > 0.0f ? voice->min_dist : 1.0f;
	float clamped  = dist;
	if (voice->max_dist > 0.0f && clamped > voice->max_dist) {
		clamped = voice->max_dist;
	}
	if (clamped > min_dist) {
		atten = min_dist / (min_dist + g_audio.rolloff_factor * (clamped - min_dist));
	}

	/* Azimuth pan from the listener basis (right = front x top). */
	float front[3] = { g_audio.listener.front[0], g_audio.listener.front[1], g_audio.listener.front[2] };
	float top[3]   = { g_audio.listener.top[0], g_audio.listener.top[1], g_audio.listener.top[2] };
	float right[3];
	Aeron_AudioNormalize(front);
	Aeron_AudioNormalize(top);
	Aeron_AudioCross(front, top, right);
	Aeron_AudioNormalize(right);
	float pan = Aeron_AudioDot(to_src, right); /* to_src already normalized */
	if (pan < -1.0f) {
		pan = -1.0f;
	}
	if (pan > 1.0f) {
		pan = 1.0f;
	}
	Aeron_AudioEqualPowerPanGains(voice->gain * atten, pan, out_l, out_r);

	/* Doppler: shift along the source->listener line. */
	double step = base_step;
	if (g_audio.doppler_factor > 0.0f) {
		float sl[3] = { -to_src[0], -to_src[1], -to_src[2] }; /* source -> listener (unit) */
		float vls   = Aeron_AudioDot(g_audio.listener.vel, sl) * g_audio.distance_factor;
		float vss   = Aeron_AudioDot(voice->vel3, sl) * g_audio.distance_factor;
		float num   = AERON_AUDIO_SOUND_SPEED - g_audio.doppler_factor * vls;
		float den   = AERON_AUDIO_SOUND_SPEED - g_audio.doppler_factor * vss;
		if (den > 1.0f && num > 1.0f) {
			float ratio = num / den;
			if (ratio < 0.5f) {
				ratio = 0.5f;
			}
			if (ratio > 2.0f) {
				ratio = 2.0f;
			}
			step *= ratio;
		}
	}
	*out_step = step;
}

/* Linear-interpolated sample fetch from a clip channel. */
static float Aeron_AudioSampleClip(const AeronClipData* clip, double pos, int channel, int loop) {
	size_t i0   = (size_t)pos;
	size_t i1   = i0 + 1;
	float  frac = (float)(pos - (double)i0);
	if (i1 >= clip->frames) {
		i1 = loop ? 0 : clip->frames - 1;
	}
	const int16_t* p  = clip->pcm;
	int            c  = clip->channels;
	float          s0 = (float)p[i0 * c + channel];
	float          s1 = (float)p[i1 * c + channel];
	return s0 + (s1 - s0) * frac;
}

/* The SDL callback holds its stream lock while this voice is mixed. */
static void Aeron_AudioMixVoice(AeronVoiceSlot* voice, int frames) {
	AeronClipData* clip = Aeron_AudioResolveClip(voice->clip);
	if (!clip || !clip->frames) {
		Aeron_AudioFreeVoiceLocked(voice);
		return;
	}
	float  gl, gr;
	double step;
	Aeron_AudioComputeVoiceMix(voice, clip, &gl, &gr, &step);

	int mixed = 0;
	for (; mixed < frames; ++mixed) {
		if (voice->pos >= (double)clip->frames) {
			if (voice->loop) {
				voice->pos = fmod(voice->pos, (double)clip->frames);
			} else {
				break;
			}
		}
		float left, right;
		if (clip->channels >= 2) {
			left  = Aeron_AudioSampleClip(clip, voice->pos, 0, voice->loop) * gl;
			right = Aeron_AudioSampleClip(clip, voice->pos, 1, voice->loop) * gr;
		} else {
			float mono = Aeron_AudioSampleClip(clip, voice->pos, 0, voice->loop);
			left       = mono * gl;
			right      = mono * gr;
		}
		g_audio.mix[mixed * 2] += left;
		g_audio.mix[mixed * 2 + 1] += right;
		voice->pos += step;
		voice->advanced += step;
	}
	voice->mixed_until = g_audio.submitted_frames + (uint64_t)mixed;
	voice->draining    = !voice->loop && voice->pos >= (double)clip->frames;
}

static AeronAudioStreamSlot* Aeron_AudioResolveStream(AeronAudioStream stream) {
	uint16_t index = Aeron_AudioHandleIndex(stream);
	if (stream == 0 || index >= AERON_AUDIO_MAX_STREAMS) {
		return NULL;
	}
	AeronAudioStreamSlot* slot = &g_audio.streams[index];
	if (!slot->in_use || slot->gen != Aeron_AudioHandleGen(stream)) {
		return NULL;
	}
	return slot;
}

static size_t Aeron_AudioStreamQueuedFramesLocked(const AeronAudioStreamSlot* stream) {
	const uint64_t read_frame = (uint64_t)floor(stream->read_position);
	if (stream->write_frame <= read_frame) {
		return 0;
	}
	return (size_t)(stream->write_frame - read_frame);
}

static void Aeron_AudioStreamUpdateAudibleLocked(AeronAudioStreamSlot* stream, uint64_t now_ns) {
	double elapsed_frames;
	double available_frames;

	if (g_audio.device_paused || !stream->playing || stream->audible_updated_ns == 0 ||
		now_ns <= stream->audible_updated_ns) {
		return;
	}
	elapsed_frames   = (double)(now_ns - stream->audible_updated_ns) * (double)stream->rate / 1000000000.0;
	available_frames = stream->submitted_frames - stream->audible_frames;
	if (available_frames < 0.0) {
		available_frames = 0.0;
	}
	if (elapsed_frames > available_frames) {
		elapsed_frames = available_frames;
	}
	stream->audible_frames += elapsed_frames;
	/* Discard elapsed time without submitted media so underruns do not make
	 * later writes jump the audible clock forward. */
	stream->audible_updated_ns = now_ns;
}

static float Aeron_AudioSampleStream(const AeronAudioStreamSlot* stream, uint64_t frame, int channel) {
	const size_t index = (size_t)(frame % stream->capacity_frames);
	if (stream->bits == 8) {
		return (float)(((int)stream->pcm[index * (size_t)stream->channels + (size_t)channel] - 128) * 256);
	}
	return (float)((const int16_t*)stream->pcm)[index * (size_t)stream->channels + (size_t)channel];
}

static void Aeron_AudioMixStream(AeronAudioStreamSlot* stream, int frames) {
	const double old_position = stream->read_position;
	const double step         = (double)stream->rate / (double)AERON_AUDIO_DEVICE_RATE;
	int          mixed        = 0;

	Aeron_AudioStreamUpdateAudibleLocked(stream, SDL_GetTicksNS());
	for (int i = 0; i < frames; ++i) {
		uint64_t i0;
		uint64_t i1;
		float    fraction;
		float    left;
		float    right;

		if (stream->read_position >= (double)stream->write_frame) {
			break;
		}

		i0 = (uint64_t)floor(stream->read_position);
		i1 = i0 + 1u;
		if (i1 >= stream->write_frame) {
			i1 = i0;
		}
		fraction = (float)(stream->read_position - (double)i0);
		if (stream->channels >= 2) {
			const float l0 = Aeron_AudioSampleStream(stream, i0, 0);
			const float l1 = Aeron_AudioSampleStream(stream, i1, 0);
			const float r0 = Aeron_AudioSampleStream(stream, i0, 1);
			const float r1 = Aeron_AudioSampleStream(stream, i1, 1);
			left           = l0 + (l1 - l0) * fraction;
			right          = r0 + (r1 - r0) * fraction;
		} else {
			const float s0 = Aeron_AudioSampleStream(stream, i0, 0);
			const float s1 = Aeron_AudioSampleStream(stream, i1, 0);
			left = right = s0 + (s1 - s0) * fraction;
		}
		g_audio.mix[i * 2] += left * stream->gain;
		g_audio.mix[i * 2 + 1] += right * stream->gain;
		stream->read_position += step;
		if (stream->read_position > (double)stream->write_frame) {
			stream->read_position = (double)stream->write_frame;
		}
		++mixed;
	}

	stream->submitted_frames += stream->read_position - old_position;
	if (mixed < frames) {
		++stream->underruns;
	}
	if ((uint64_t)floor(stream->read_position) > (uint64_t)floor(old_position)) {
		SDL_BroadcastCondition(g_audio.stream_space_available);
	}
}

/* Mixes one chunk under the lock and stages it as S16. */
static void Aeron_AudioMixChunk(int frames) {
	SDL_LockMutex(g_audio.lock);

	memset(g_audio.mix, 0, (size_t)frames * 2 * sizeof(float));

	uint64_t played = Aeron_AudioPlayedOutputFramesLocked();
	for (int v = 0; v < AERON_AUDIO_MAX_VOICES; ++v) {
		AeronVoiceSlot* voice = &g_audio.voices[v];
		if (!voice->in_use) {
			continue;
		}
		if (Aeron_AudioVoiceFinished(voice, played)) {
			Aeron_AudioFreeVoiceLocked(voice);
		} else if (!voice->draining) {
			Aeron_AudioMixVoice(voice, frames);
		}
	}

	for (int s = 0; s < AERON_AUDIO_MAX_STREAMS; ++s) {
		AeronAudioStreamSlot* stream = &g_audio.streams[s];
		if (stream->in_use && stream->playing) {
			Aeron_AudioMixStream(stream, frames);
		}
	}

	float master = g_audio.master_gain;
	SDL_UnlockMutex(g_audio.lock);

	for (int i = 0; i < frames * 2; ++i) {
		float sample = g_audio.mix[i] * master;
		if (sample > 32767.0f) {
			sample = 32767.0f;
		}
		if (sample < -32768.0f) {
			sample = -32768.0f;
		}
		g_audio.stage[i] = (int16_t)sample;
	}
}

static void SDLCALL Aeron_AudioCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
										int total_amount) {
	(void)userdata;
	(void)total_amount;

	int frames_remaining = additional_amount / (AERON_AUDIO_DEVICE_CHANNELS * (int)sizeof(int16_t));
	while (frames_remaining > 0) {
		int chunk = frames_remaining < AERON_AUDIO_MIX_CHUNK ? frames_remaining : AERON_AUDIO_MIX_CHUNK;
		Aeron_AudioMixChunk(chunk);
		SDL_PutAudioStreamData(stream, g_audio.stage,
							   chunk * AERON_AUDIO_DEVICE_CHANNELS * (int)sizeof(int16_t));
		SDL_LockMutex(g_audio.lock);
		g_audio.submitted_frames += (uint64_t)chunk;
		SDL_UnlockMutex(g_audio.lock);
		frames_remaining -= chunk;
	}
}

/* --- lifecycle ----------------------------------------------------------- */

int Aeron_AudioInit(void) {
	memset(&g_audio, 0, sizeof(g_audio));
	g_audio.master_gain     = 1.0f;
	g_audio.distance_factor = 1.0f;
	g_audio.rolloff_factor  = 1.0f;
	g_audio.doppler_factor  = 1.0f;

	g_audio.lock = SDL_CreateMutex();
	if (!g_audio.lock) {
		Aeron_LogError("aeron.audio", "SDL_CreateMutex failed: %s", SDL_GetError());
		return 0;
	}
	g_audio.stream_space_available = SDL_CreateCondition();
	if (!g_audio.stream_space_available) {
		Aeron_LogError("aeron.audio", "SDL_CreateCondition failed: %s", SDL_GetError());
		SDL_DestroyMutex(g_audio.lock);
		g_audio.lock = NULL;
		return 0;
	}

	SDL_AudioSpec spec;
	spec.format   = SDL_AUDIO_S16;
	spec.channels = AERON_AUDIO_DEVICE_CHANNELS;
	spec.freq     = AERON_AUDIO_DEVICE_RATE;

	g_audio.device =
		SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Aeron_AudioCallback, NULL);
	if (!g_audio.device) {
		Aeron_LogError("aeron.audio", "SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
		SDL_DestroyCondition(g_audio.stream_space_available);
		g_audio.stream_space_available = NULL;
		SDL_DestroyMutex(g_audio.lock);
		g_audio.lock = NULL;
		return 0;
	}
	g_audio.device_id = SDL_GetAudioStreamDevice(g_audio.device);
	if (g_audio.device_id) {
		SDL_AudioSpec actual;
		int           sample_frames = 0;
		if (SDL_GetAudioDeviceFormat(g_audio.device_id, &actual, &sample_frames) && actual.freq > 0 &&
			sample_frames > 0) {
			g_audio.device_buffer_input_frames =
				(int)(((int64_t)sample_frames * AERON_AUDIO_DEVICE_RATE + actual.freq - 1) / actual.freq);
		}
	}

	if (!SDL_ResumeAudioStreamDevice(g_audio.device)) {
		Aeron_LogError("aeron.audio", "SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
	}

	g_audio.initialized = 1;
	Aeron_LogInfo("aeron.audio", "audio device opened: %d Hz, %d ch, S16 buffer=%d frames",
				  AERON_AUDIO_DEVICE_RATE, AERON_AUDIO_DEVICE_CHANNELS, g_audio.device_buffer_input_frames);
	return 1;
}

void Aeron_AudioSetPaused(int paused) {
	uint64_t now_ns;

	if (!g_audio.initialized || !g_audio.device) {
		return;
	}

	if (paused) {
		SDL_LockMutex(g_audio.lock);
		if (g_audio.device_paused) {
			SDL_UnlockMutex(g_audio.lock);
			return;
		}
		SDL_UnlockMutex(g_audio.lock);

		if (!SDL_PauseAudioStreamDevice(g_audio.device)) {
			Aeron_LogError("aeron.audio", "SDL_PauseAudioStreamDevice failed: %s", SDL_GetError());
			return;
		}

		SDL_LockMutex(g_audio.lock);
		now_ns = SDL_GetTicksNS();
		for (int i = 0; i < AERON_AUDIO_MAX_STREAMS; ++i) {
			AeronAudioStreamSlot* stream = &g_audio.streams[i];
			if (stream->in_use && stream->playing) {
				Aeron_AudioStreamUpdateAudibleLocked(stream, now_ns);
			}
		}
		g_audio.device_paused    = 1;
		g_audio.device_paused_ns = now_ns;
		SDL_UnlockMutex(g_audio.lock);
		return;
	}

	SDL_LockMutex(g_audio.lock);
	if (!g_audio.device_paused) {
		SDL_UnlockMutex(g_audio.lock);
		return;
	}
	now_ns = SDL_GetTicksNS();
	for (int i = 0; i < AERON_AUDIO_MAX_STREAMS; ++i) {
		AeronAudioStreamSlot* stream = &g_audio.streams[i];
		if (stream->in_use && stream->playing && stream->audible_updated_ns != 0) {
			stream->audible_updated_ns += now_ns - g_audio.device_paused_ns;
		}
	}
	g_audio.device_paused    = 0;
	g_audio.device_paused_ns = 0;
	SDL_UnlockMutex(g_audio.lock);

	if (!SDL_ResumeAudioStreamDevice(g_audio.device)) {
		Aeron_LogError("aeron.audio", "SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
		SDL_LockMutex(g_audio.lock);
		g_audio.device_paused    = 1;
		g_audio.device_paused_ns = now_ns;
		SDL_UnlockMutex(g_audio.lock);
	}
}

void Aeron_AudioShutdown(void) {
	if (!g_audio.initialized) {
		return;
	}

	/* Destroying the device stream pauses + unbinds it, so the callback cannot
	 * run again before we tear down the tables. */
	SDL_DestroyAudioStream(g_audio.device);
	g_audio.device = NULL;

	/* Producers have joined before the device and sample storage are released. */
	SDL_LockMutex(g_audio.lock);
	g_audio.initialized = 0;
	void* clip_pcm[AERON_AUDIO_MAX_CLIPS];
	void* stream_pcm[AERON_AUDIO_MAX_STREAMS];
	for (int c = 0; c < AERON_AUDIO_MAX_CLIPS; ++c) {
		clip_pcm[c]             = g_audio.clips[c].in_use ? g_audio.clips[c].pcm : NULL;
		g_audio.clips[c].in_use = 0;
		g_audio.clips[c].pcm    = NULL;
	}
	for (int s = 0; s < AERON_AUDIO_MAX_STREAMS; ++s) {
		stream_pcm[s]              = g_audio.streams[s].in_use ? g_audio.streams[s].pcm : NULL;
		g_audio.streams[s].in_use  = 0;
		g_audio.streams[s].playing = 0;
		g_audio.streams[s].pcm     = NULL;
	}
	SDL_UnlockMutex(g_audio.lock);

	for (int c = 0; c < AERON_AUDIO_MAX_CLIPS; ++c) {
		SDL_free(clip_pcm[c]);
	}
	for (int s = 0; s < AERON_AUDIO_MAX_STREAMS; ++s) {
		SDL_free(stream_pcm[s]);
	}

	SDL_BroadcastCondition(g_audio.stream_space_available);
	SDL_DestroyCondition(g_audio.stream_space_available);
	g_audio.stream_space_available = NULL;
	SDL_DestroyMutex(g_audio.lock);
	g_audio.lock        = NULL;
	g_audio.initialized = 0;
}

/* --- clips --------------------------------------------------------------- */

static AeronClip Aeron_AudioCreateClip(const void* pcm, size_t frame_count, int sample_rate, int channels,
									   AeronPcmFormat fmt, int mutable_pcm) {
	if (!g_audio.initialized || (!pcm && !mutable_pcm) || frame_count == 0 ||
		(channels != 1 && channels != 2) || sample_rate <= 0 ||
		(fmt != AERON_PCM_U8 && fmt != AERON_PCM_S16) ||
		frame_count > SIZE_MAX / (size_t)channels / sizeof(int16_t)) {
		return 0;
	}

	size_t   sample_count = frame_count * (size_t)channels;
	int16_t* converted    = (int16_t*)SDL_malloc(sample_count * sizeof(int16_t));
	if (!converted) {
		return 0;
	}
	if (!pcm) {
		memset(converted, 0, sample_count * sizeof(int16_t));
	} else if (fmt == AERON_PCM_S16) {
		memcpy(converted, pcm, sample_count * sizeof(int16_t));
	} else {
		const uint8_t* src = (const uint8_t*)pcm;
		for (size_t i = 0; i < sample_count; ++i) {
			converted[i] = (int16_t)(((int)src[i] - 128) * 256);
		}
	}

	SDL_LockMutex(g_audio.lock);
	AeronClip result = 0;
	for (int i = 0; i < AERON_AUDIO_MAX_CLIPS; ++i) {
		AeronClipData* data = &g_audio.clips[i];
		if (!data->in_use) {
			data->pcm             = converted;
			data->frames          = frame_count;
			data->rate            = sample_rate;
			data->channels        = channels;
			data->refcount        = 0;
			data->pending_destroy = 0;
			data->mutable_pcm     = mutable_pcm;
			data->format          = fmt;
			data->in_use          = 1;
			if (data->gen == 0) {
				data->gen = 1;
			}
			result = Aeron_AudioPackHandle((uint16_t)i, data->gen);
			break;
		}
	}
	SDL_UnlockMutex(g_audio.lock);

	if (result == 0) {
		SDL_free(converted);
	}
	return result;
}

AeronClip Aeron_AudioClipCreate(const void* pcm, size_t frame_count, int sample_rate, int channels,
								AeronPcmFormat fmt) {
	return Aeron_AudioCreateClip(pcm, frame_count, sample_rate, channels, fmt, 0);
}

AeronClip Aeron_AudioClipCreateMutable(size_t frame_count, int sample_rate, int channels,
									   AeronPcmFormat fmt) {
	return Aeron_AudioCreateClip(NULL, frame_count, sample_rate, channels, fmt, 1);
}

int Aeron_AudioClipWrite(AeronClip clip, const AeronPcmWrite* writes, size_t count) {
	if (!g_audio.initialized || (!writes && count)) {
		return 0;
	}
	SDL_LockMutex(g_audio.lock);
	AeronClipData* data = Aeron_AudioResolveClip(clip);
	if (!data || !data->mutable_pcm || data->pending_destroy) {
		SDL_UnlockMutex(g_audio.lock);
		return 0;
	}
	size_t capacity = data->frames * (size_t)data->channels * (data->format == AERON_PCM_U8 ? 1u : 2u);
	for (size_t i = 0; i < count; ++i) {
		if ((!writes[i].pcm && writes[i].bytes) || writes[i].offset > capacity ||
			writes[i].bytes > capacity - writes[i].offset) {
			SDL_UnlockMutex(g_audio.lock);
			return 0;
		}
	}
	for (size_t i = 0; i < count; ++i) {
		if (!writes[i].bytes) {
			continue;
		}
		if (data->format == AERON_PCM_S16) {
			memcpy((uint8_t*)data->pcm + writes[i].offset, writes[i].pcm, writes[i].bytes);
		} else {
			const uint8_t* src = (const uint8_t*)writes[i].pcm;
			int16_t*       dst = data->pcm + writes[i].offset;
			for (size_t j = 0; j < writes[i].bytes; ++j) {
				dst[j] = (int16_t)(((int)src[j] - 128) * 256);
			}
		}
	}
	SDL_UnlockMutex(g_audio.lock);
	return 1;
}

void Aeron_AudioClipDestroy(AeronClip clip) {
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronClipData* data = Aeron_AudioResolveClip(clip);
	if (data) {
		if (data->refcount > 0) {
			data->pending_destroy = 1;
		} else {
			Aeron_AudioFreeClipData(data);
		}
	}
	SDL_UnlockMutex(g_audio.lock);
}

/* --- voices -------------------------------------------------------------- */

static AeronVoice Aeron_AudioStartVoice(AeronClip clip, float gain, float pan, float pitch, int loop,
										int is3d, const float pos[3], float min_dist, float max_dist,
										size_t frame) {
	if (clip == 0 || !Aeron_AudioLockOutput()) {
		return 0;
	}

	uint64_t       played = Aeron_AudioPlayedOutputFramesLocked();
	AeronVoice     result = 0;
	AeronClipData* data   = Aeron_AudioResolveClip(clip);
	if (data && !data->pending_destroy && frame < data->frames) {
		for (int i = 0; i < AERON_AUDIO_MAX_VOICES; ++i) {
			AeronVoiceSlot* voice = &g_audio.voices[i];
			if (voice->in_use && Aeron_AudioVoiceFinished(voice, played)) {
				Aeron_AudioFreeVoiceLocked(voice);
			}
			if (!voice->in_use) {
				voice->in_use      = 1;
				voice->clip        = clip;
				voice->pos         = (double)frame;
				voice->start_frame = frame;
				voice->advanced    = 0.0;
				voice->mixed_until = g_audio.submitted_frames;
				voice->draining    = 0;
				voice->gain        = gain;
				voice->pan         = pan;
				voice->pitch       = pitch > 0.0f ? pitch : 1.0f;
				voice->loop        = loop;
				voice->is3d        = is3d;
				voice->min_dist    = min_dist;
				voice->max_dist    = max_dist;
				if (is3d && pos) {
					voice->pos3[0] = pos[0];
					voice->pos3[1] = pos[1];
					voice->pos3[2] = pos[2];
				}
				if (voice->gen == 0) {
					voice->gen = 1;
				}
				Aeron_AudioClipAddRef(data);
				result = (voice->gen << 16) | (uint16_t)i;
				break;
			}
		}
	}
	Aeron_AudioUnlockOutput();
	return result;
}

AeronVoice Aeron_AudioVoicePlay(AeronClip clip, float gain, float pan, float pitch, int loop) {
	return Aeron_AudioStartVoice(clip, gain, pan, pitch, loop, 0, NULL, 0.0f, 0.0f, 0);
}

AeronVoice Aeron_AudioVoicePlay3D(AeronClip clip, float gain, float pitch, int loop, const float pos[3],
								  float min_dist, float max_dist) {
	return Aeron_AudioStartVoice(clip, gain, 0.0f, pitch, loop, 1, pos, min_dist, max_dist, 0);
}

AeronVoice Aeron_AudioVoicePlayFrom(AeronClip clip, float gain, float pan, float pitch, int loop,
									size_t frame) {
	return Aeron_AudioStartVoice(clip, gain, pan, pitch, loop, 0, NULL, 0.0f, 0.0f, frame);
}

AeronVoice Aeron_AudioVoicePlay3DFrom(AeronClip clip, float gain, float pitch, int loop, const float pos[3],
									  float min_dist, float max_dist, size_t frame) {
	return Aeron_AudioStartVoice(clip, gain, 0.0f, pitch, loop, 1, pos, min_dist, max_dist, frame);
}

static AeronVoiceSlot* Aeron_AudioResolveVoice(AeronVoice voice) {
	uint16_t index = (uint16_t)voice;
	if (voice == 0 || index >= AERON_AUDIO_MAX_VOICES) {
		return NULL;
	}
	AeronVoiceSlot* slot = &g_audio.voices[index];
	if (!slot->in_use || slot->gen != (voice >> 16)) {
		return NULL;
	}
	return slot;
}

/* The output snapshot keeps completed voices valid until their tail has played. */
static AeronVoiceSlot* Aeron_AudioResolvePlayingVoiceLocked(AeronVoice voice, uint64_t played) {
	AeronVoiceSlot* slot = Aeron_AudioResolveVoice(voice);
	if (slot && Aeron_AudioVoiceFinished(slot, played)) {
		Aeron_AudioFreeVoiceLocked(slot);
		slot = NULL;
	}
	return slot;
}

static size_t Aeron_AudioVoiceCursorLocked(const AeronVoiceSlot* voice, const AeronClipData* clip,
										   uint64_t played) {
	float  left, right;
	double step;
	Aeron_AudioComputeVoiceMix(voice, clip, &left, &right, &step);
	uint64_t pending = voice->mixed_until > played ? voice->mixed_until - played : 0;
	double   heard   = voice->advanced - (double)pending * step;
	double   frame   = (double)voice->start_frame + (heard > 0.0 ? heard : 0.0);
	if (voice->loop) {
		return (size_t)fmod(frame, (double)clip->frames);
	}
	return frame < (double)clip->frames ? (size_t)frame : clip->frames - 1;
}

int Aeron_AudioVoiceGetCursors(AeronVoice voice, size_t* play, size_t* write) {
	if (play)
		*play = 0;
	if (write)
		*write = 0;
	if (!Aeron_AudioLockOutput()) {
		return 0;
	}
	uint64_t        played = Aeron_AudioPlayedOutputFramesLocked();
	AeronVoiceSlot* slot   = Aeron_AudioResolvePlayingVoiceLocked(voice, played);
	AeronClipData*  clip   = slot ? Aeron_AudioResolveClip(slot->clip) : NULL;
	if (clip) {
		if (play)
			*play = Aeron_AudioVoiceCursorLocked(slot, clip, played);
		if (write)
			*write = (size_t)fmod(slot->pos, (double)clip->frames);
	}
	Aeron_AudioUnlockOutput();
	return clip != NULL;
}

int Aeron_AudioVoiceSetPosition(AeronVoice voice, size_t frame) {
	if (!Aeron_AudioLockOutput()) {
		return 0;
	}
	AeronVoiceSlot* slot = Aeron_AudioResolvePlayingVoiceLocked(voice, Aeron_AudioPlayedOutputFramesLocked());
	AeronClipData*  clip = slot ? Aeron_AudioResolveClip(slot->clip) : NULL;
	int             valid = clip && frame < clip->frames;
	if (valid) {
		slot->pos         = (double)frame;
		slot->start_frame = frame;
		slot->advanced    = 0.0;
		slot->mixed_until = g_audio.submitted_frames;
		slot->draining    = 0;
	}
	Aeron_AudioUnlockOutput();
	return valid;
}

int Aeron_AudioVoiceSetLooping(AeronVoice voice, int loop) {
	if (!Aeron_AudioLockOutput()) {
		return 0;
	}
	AeronVoiceSlot* slot = Aeron_AudioResolvePlayingVoiceLocked(voice, Aeron_AudioPlayedOutputFramesLocked());
	if (slot) {
		slot->loop = loop != 0;
		if (slot->loop)
			slot->draining = 0;
	}
	int valid = slot != NULL;
	Aeron_AudioUnlockOutput();
	return valid;
}

void Aeron_AudioVoiceSetGain(AeronVoice voice, float gain) {
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronVoiceSlot* slot = Aeron_AudioResolveVoice(voice);
	if (slot) {
		slot->gain = gain;
	}
	SDL_UnlockMutex(g_audio.lock);
}

void Aeron_AudioVoiceSetPan(AeronVoice voice, float pan) {
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronVoiceSlot* slot = Aeron_AudioResolveVoice(voice);
	if (slot) {
		slot->pan = pan;
	}
	SDL_UnlockMutex(g_audio.lock);
}

void Aeron_AudioVoiceSetPitch(AeronVoice voice, float pitch) {
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronVoiceSlot* slot = Aeron_AudioResolveVoice(voice);
	if (slot && pitch > 0.0f) {
		slot->pitch = pitch;
	}
	SDL_UnlockMutex(g_audio.lock);
}

void Aeron_AudioVoiceSet3DPosition(AeronVoice voice, const float pos[3]) {
	if (!g_audio.initialized || !pos) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronVoiceSlot* slot = Aeron_AudioResolveVoice(voice);
	if (slot) {
		slot->pos3[0] = pos[0];
		slot->pos3[1] = pos[1];
		slot->pos3[2] = pos[2];
	}
	SDL_UnlockMutex(g_audio.lock);
}

void Aeron_AudioVoiceSet3DVelocity(AeronVoice voice, const float vel[3]) {
	if (!g_audio.initialized || !vel) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronVoiceSlot* slot = Aeron_AudioResolveVoice(voice);
	if (slot) {
		slot->vel3[0] = vel[0];
		slot->vel3[1] = vel[1];
		slot->vel3[2] = vel[2];
	}
	SDL_UnlockMutex(g_audio.lock);
}

size_t Aeron_AudioVoiceStopAndGetPosition(AeronVoice voice) {
	size_t cursor = 0;
	if (!Aeron_AudioLockOutput()) {
		return 0;
	}
	AeronVoiceSlot* slot = Aeron_AudioResolveVoice(voice);
	if (slot) {
		AeronClipData* clip = Aeron_AudioResolveClip(slot->clip);
		if (clip && !slot->draining) {
			cursor = (size_t)fmod(slot->pos, (double)clip->frames);
		}
		Aeron_AudioFreeVoiceLocked(slot);
	}
	Aeron_AudioUnlockOutput();
	return cursor;
}

void Aeron_AudioVoiceStop(AeronVoice voice) { (void)Aeron_AudioVoiceStopAndGetPosition(voice); }

int Aeron_AudioVoiceIsPlaying(AeronVoice voice) {
	if (!Aeron_AudioLockOutput()) {
		return 0;
	}
	int playing = Aeron_AudioResolvePlayingVoiceLocked(voice, Aeron_AudioPlayedOutputFramesLocked()) != NULL;
	Aeron_AudioUnlockOutput();
	return playing;
}

void Aeron_AudioSetListener(const AeronAudioListener* listener) {
	if (!g_audio.initialized || !listener) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	g_audio.listener     = *listener;
	g_audio.has_listener = 1;
	SDL_UnlockMutex(g_audio.lock);
}

void Aeron_AudioSetDistanceModel(float distance_factor, float rolloff_factor, float doppler_factor) {
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	g_audio.distance_factor = distance_factor > 0.0f ? distance_factor : 1.0f;
	g_audio.rolloff_factor  = rolloff_factor >= 0.0f ? rolloff_factor : 1.0f;
	g_audio.doppler_factor  = doppler_factor >= 0.0f ? doppler_factor : 0.0f;
	SDL_UnlockMutex(g_audio.lock);
}

/* --- queued streams ------------------------------------------------------ */

AeronAudioStream Aeron_AudioStreamOpen(int rate, int channels, AeronPcmFormat format, size_t capacity_frames,
									   float gain) {
	size_t           bytes;
	uint8_t*         storage;
	AeronAudioStream result = 0;
	const int        bits   = format == AERON_PCM_U8 ? 8 : format == AERON_PCM_S16 ? 16 : 0;

	if (!g_audio.initialized || rate <= 0 || (channels != 1 && channels != 2) || bits == 0 ||
		capacity_frames == 0 || capacity_frames > SIZE_MAX / ((size_t)channels * (size_t)(bits / 8))) {
		return 0;
	}

	bytes   = capacity_frames * (size_t)channels * (size_t)(bits / 8);
	storage = (uint8_t*)SDL_malloc(bytes);
	if (!storage) {
		return 0;
	}
	memset(storage, bits == 8 ? 128 : 0, bytes);

	SDL_LockMutex(g_audio.lock);
	for (int i = 0; i < AERON_AUDIO_MAX_STREAMS; ++i) {
		AeronAudioStreamSlot* slot = &g_audio.streams[i];
		if (!slot->in_use) {
			slot->in_use             = 1;
			slot->pcm                = storage;
			slot->capacity_frames    = capacity_frames;
			slot->rate               = rate;
			slot->channels           = channels;
			slot->bits               = bits;
			slot->block_align        = channels * (bits / 8);
			slot->gain               = gain < 0.0f ? 0.0f : gain;
			slot->playing            = 0;
			slot->write_frame        = 0;
			slot->read_position      = 0.0;
			slot->submitted_frames   = 0.0;
			slot->audible_frames     = 0.0;
			slot->audible_updated_ns = 0;
			slot->underruns          = 0;
			if (slot->gen == 0) {
				slot->gen = 1;
			}
			result = Aeron_AudioPackHandle((uint16_t)i, slot->gen);
			break;
		}
	}
	SDL_UnlockMutex(g_audio.lock);

	if (result == 0) {
		SDL_free(storage);
	}
	return result;
}

size_t Aeron_AudioStreamWrite(AeronAudioStream stream, const void* pcm, size_t frame_count) {
	AeronAudioStreamSlot* slot;
	size_t                queued;
	size_t                writable;
	size_t                first;
	size_t                frame_bytes;

	if (!g_audio.initialized || !pcm || frame_count == 0) {
		return 0;
	}

	SDL_LockMutex(g_audio.lock);
	slot = Aeron_AudioResolveStream(stream);
	if (!slot) {
		SDL_UnlockMutex(g_audio.lock);
		return 0;
	}
	queued   = Aeron_AudioStreamQueuedFramesLocked(slot);
	writable = slot->capacity_frames - queued;
	if (frame_count < writable) {
		writable = frame_count;
	}
	frame_bytes = (size_t)slot->block_align;
	first       = slot->capacity_frames - (size_t)(slot->write_frame % slot->capacity_frames);
	if (first > writable) {
		first = writable;
	}
	memcpy(slot->pcm + (size_t)(slot->write_frame % slot->capacity_frames) * frame_bytes, pcm,
		   first * frame_bytes);
	if (writable > first) {
		memcpy(slot->pcm, (const uint8_t*)pcm + first * frame_bytes, (writable - first) * frame_bytes);
	}
	slot->write_frame += writable;
	SDL_UnlockMutex(g_audio.lock);
	return writable;
}

int Aeron_AudioStreamWaitWritable(AeronAudioStream stream, size_t required_frames) {
	int ready = 0;

	if (!g_audio.initialized || required_frames == 0) {
		return required_frames == 0;
	}

	SDL_LockMutex(g_audio.lock);
	for (;;) {
		AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
		size_t                writable;
		if (!slot || required_frames > slot->capacity_frames) {
			break;
		}
		writable = slot->capacity_frames - Aeron_AudioStreamQueuedFramesLocked(slot);
		if (writable >= required_frames) {
			ready = 1;
			break;
		}
		if (!slot->playing) {
			break;
		}
		SDL_WaitCondition(g_audio.stream_space_available, g_audio.lock);
	}
	SDL_UnlockMutex(g_audio.lock);
	return ready;
}

size_t Aeron_AudioStreamWritableFrames(AeronAudioStream stream) {
	size_t writable = 0;
	if (!g_audio.initialized) {
		return 0;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		writable = slot->capacity_frames - Aeron_AudioStreamQueuedFramesLocked(slot);
	}
	SDL_UnlockMutex(g_audio.lock);
	return writable;
}

size_t Aeron_AudioStreamQueuedFrames(AeronAudioStream stream) {
	size_t queued = 0;
	if (!g_audio.initialized) {
		return 0;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		queued = Aeron_AudioStreamQueuedFramesLocked(slot);
	}
	SDL_UnlockMutex(g_audio.lock);
	return queued;
}

size_t Aeron_AudioStreamCapacityFrames(AeronAudioStream stream) {
	size_t capacity = 0;
	if (!g_audio.initialized) {
		return 0;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		capacity = slot->capacity_frames;
	}
	SDL_UnlockMutex(g_audio.lock);
	return capacity;
}

int Aeron_AudioStreamEnsureCapacity(AeronAudioStream stream, size_t minimum_capacity_frames) {
	AeronAudioStreamSlot* slot;
	uint8_t*              new_storage;
	uint8_t*              old_storage = NULL;
	size_t                frame_bytes;
	size_t                bytes;
	size_t                queued;
	size_t                first;
	size_t                read_frame;
	int                   channels;
	int                   bits;
	int                   result = 0;

	if (!g_audio.initialized || minimum_capacity_frames == 0) {
		return 0;
	}

	SDL_LockMutex(g_audio.lock);
	slot = Aeron_AudioResolveStream(stream);
	if (!slot) {
		SDL_UnlockMutex(g_audio.lock);
		return 0;
	}
	if (minimum_capacity_frames <= slot->capacity_frames) {
		SDL_UnlockMutex(g_audio.lock);
		return 1;
	}
	/* Resizing after consumption would have to preserve a fractional
	 * resampling cursor. Video prebuffer growth only occurs before play. */
	if (slot->playing || slot->read_position != 0.0 || slot->submitted_frames != 0.0) {
		SDL_UnlockMutex(g_audio.lock);
		return 0;
	}
	channels = slot->channels;
	bits     = slot->bits;
	SDL_UnlockMutex(g_audio.lock);

	frame_bytes = (size_t)channels * (size_t)(bits / 8);
	if (frame_bytes == 0 || minimum_capacity_frames > SIZE_MAX / frame_bytes) {
		return 0;
	}
	bytes       = minimum_capacity_frames * frame_bytes;
	new_storage = (uint8_t*)SDL_malloc(bytes);
	if (!new_storage) {
		return 0;
	}
	memset(new_storage, bits == 8 ? 128 : 0, bytes);

	SDL_LockMutex(g_audio.lock);
	slot = Aeron_AudioResolveStream(stream);
	if (!slot || slot->playing || slot->read_position != 0.0 || slot->submitted_frames != 0.0 ||
		slot->channels != channels || slot->bits != bits) {
		SDL_UnlockMutex(g_audio.lock);
		SDL_free(new_storage);
		return 0;
	}
	if (minimum_capacity_frames <= slot->capacity_frames) {
		SDL_UnlockMutex(g_audio.lock);
		SDL_free(new_storage);
		return 1;
	}

	queued     = Aeron_AudioStreamQueuedFramesLocked(slot);
	read_frame = (size_t)floor(slot->read_position);
	first      = slot->capacity_frames - read_frame % slot->capacity_frames;
	if (first > queued) {
		first = queued;
	}
	memcpy(new_storage, slot->pcm + (read_frame % slot->capacity_frames) * frame_bytes, first * frame_bytes);
	if (queued > first) {
		memcpy(new_storage + first * frame_bytes, slot->pcm, (queued - first) * frame_bytes);
	}

	old_storage           = slot->pcm;
	slot->pcm             = new_storage;
	slot->capacity_frames = minimum_capacity_frames;
	slot->read_position   = 0.0;
	slot->write_frame     = queued;
	SDL_BroadcastCondition(g_audio.stream_space_available);
	result = 1;
	SDL_UnlockMutex(g_audio.lock);

	SDL_free(old_storage);
	return result;
}

void Aeron_AudioStreamPlay(AeronAudioStream stream) {
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		if (!slot->playing) {
			const uint64_t latency_ns =
				(uint64_t)g_audio.device_buffer_input_frames * 1000000000u / AERON_AUDIO_DEVICE_RATE;
			const uint64_t baseline_ns = g_audio.device_paused ? g_audio.device_paused_ns : SDL_GetTicksNS();
			slot->playing              = 1;
			slot->audible_updated_ns   = baseline_ns + latency_ns;
		}
	}
	SDL_UnlockMutex(g_audio.lock);
}

void Aeron_AudioStreamPause(AeronAudioStream stream) {
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		Aeron_AudioStreamUpdateAudibleLocked(slot, SDL_GetTicksNS());
		slot->playing = 0;
		SDL_BroadcastCondition(g_audio.stream_space_available);
	}
	SDL_UnlockMutex(g_audio.lock);
}

void Aeron_AudioStreamFlush(AeronAudioStream stream) {
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		slot->write_frame        = 0;
		slot->read_position      = 0.0;
		slot->submitted_frames   = 0.0;
		slot->audible_frames     = 0.0;
		slot->audible_updated_ns = 0;
		slot->underruns          = 0;
		SDL_BroadcastCondition(g_audio.stream_space_available);
	}
	SDL_UnlockMutex(g_audio.lock);
}

void Aeron_AudioStreamSetGain(AeronAudioStream stream, float gain) {
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		slot->gain = gain < 0.0f ? 0.0f : gain;
	}
	SDL_UnlockMutex(g_audio.lock);
}

int Aeron_AudioStreamIsPlaying(AeronAudioStream stream) {
	int playing = 0;
	if (!g_audio.initialized) {
		return 0;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		playing = slot->playing;
	}
	SDL_UnlockMutex(g_audio.lock);
	return playing;
}

uint64_t Aeron_AudioStreamConsumedFrames(AeronAudioStream stream) {
	uint64_t consumed = 0;
	if (!g_audio.initialized) {
		return 0;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot && slot->submitted_frames > 0.0) {
		consumed = (uint64_t)slot->submitted_frames;
	}
	SDL_UnlockMutex(g_audio.lock);
	return consumed;
}

uint64_t Aeron_AudioStreamAudibleFrames(AeronAudioStream stream) {
	uint64_t audible = 0;

	if (!g_audio.initialized) {
		return 0;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		Aeron_AudioStreamUpdateAudibleLocked(slot, SDL_GetTicksNS());
		if (slot->audible_frames > 0.0) {
			audible = (uint64_t)slot->audible_frames;
		}
	}
	SDL_UnlockMutex(g_audio.lock);
	return audible;
}

uint64_t Aeron_AudioStreamUnderrunCount(AeronAudioStream stream) {
	uint64_t underruns = 0;
	if (!g_audio.initialized) {
		return 0;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		underruns = slot->underruns;
	}
	SDL_UnlockMutex(g_audio.lock);
	return underruns;
}

void Aeron_AudioStreamClose(AeronAudioStream stream) {
	uint8_t* storage = NULL;
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	AeronAudioStreamSlot* slot = Aeron_AudioResolveStream(stream);
	if (slot) {
		const uint16_t gen = Aeron_AudioNextGen(slot->gen);
		storage            = slot->pcm;
		memset(slot, 0, sizeof(*slot));
		slot->gen = gen;
		SDL_BroadcastCondition(g_audio.stream_space_available);
	}
	SDL_UnlockMutex(g_audio.lock);
	SDL_free(storage);
}

void Aeron_AudioSetMasterGain(float gain) {
	if (!g_audio.initialized) {
		return;
	}
	SDL_LockMutex(g_audio.lock);
	g_audio.master_gain = gain < 0.0f ? 0.0f : gain;
	SDL_UnlockMutex(g_audio.lock);
}
