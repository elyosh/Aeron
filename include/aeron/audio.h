#ifndef AERON_AUDIO_H
#define AERON_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Aeron's generic software audio mixer.
 *
 * The engine owns one SDL3 output device and mixes two kinds of source:
 *   - clips played as voices: shared PCM, played as independent instances
 *     with linear gain, stereo pan, a pitch ratio, and an optional 3D position;
 *   - queued streams: a bounded PCM FIFO for decoder and synthesizer output.
 *
 * The API is intentionally game-agnostic: it knows only linear gain [0..1],
 * pan [-1..1], pitch ratios and a standard inverse-distance positional model.
 * No DirectSound, millibel, or game-specific concepts cross this boundary.
 *
 * The output is F32 at 48 kHz, with 2 channels by default (see
 * Aeron_AudioSetOutputChannels); SDL converts it to the device's format. A
 * limiter keeps every sample within [-1, 1]. It looks 1 ms ahead, so
 * everything is heard 1 ms after it is mixed, and the cursors and stream
 * clocks count that delay.
 *
 * Handles are generation-tagged ids; voices use 64 bits, other handles 32 bits.
 * The value 0 is always invalid.
 * Operations on a stale handle (whose slot has since been reused) are safe
 * no-ops. Playback and sample operations are synchronized with the SDL callback
 * and may be called by the game or producer threads. Device initialization,
 * shutdown and global pause run on the control thread. Join producers and
 * writable-space waiters before shutting down the audio system. */

typedef uint32_t AeronClip;  /* 0 == invalid */
typedef uint64_t AeronVoice; /* 0 == invalid */

typedef enum AeronPcmFormat { AERON_PCM_U8 = 0, AERON_PCM_S16 = 1 } AeronPcmFormat;

/* Positional audio uses right-handed coordinates, as OpenAL does: the
 * listener's right is front x top. Positions, velocities and the min and max
 * distances of a voice all use the same units. A positional voice plays a
 * stereo clip mixed down to mono. With stereo speakers, a sound behind the
 * listener is made quieter and duller to tell it apart from one in front.
 * With 5.1 and 7.1 speakers, it is panned between the speakers around it.
 * With headphones, an HRTF gives the direction (see
 * Aeron_AudioSetSpatialSettings). */
typedef struct AeronAudioListener {
	float pos[3];   /* world position */
	float front[3]; /* forward orientation vector (need not be normalized) */
	float top[3];   /* up orientation vector (need not be normalized) */
	float vel[3];   /* world velocity for doppler */
} AeronAudioListener;

/* Where a positional voice is and how its distance affects it. */
typedef struct AeronAudioSource3D {
	float pos[3];
	float vel[3]; /* units per second, for doppler */
	/* Inverse-distance attenuation: full gain up to min_dist, and no further
	   attenuation past max_dist. */
	float min_dist;
	float max_dist;
	/* When set, the voice is silent past max_dist. It keeps playing, and is
	   heard again when it comes back in range. */
	int mute_beyond_max;
	/* With mute_beyond_max: the gain falls linearly from fade_start to zero at
	   max_dist, so the voice doesn't go silent at once. 0 = no fade. */
	float fade_start;
} AeronAudioSource3D;

/* Clips ------------------------------------------------------------------- */

/* Copies frame_count frames of interleaved PCM into an engine-owned clip.
 * sample_rate/channels describe the source; the mixer resamples per voice.
 * Returns 0 on failure. */
AeronClip Aeron_AudioClipCreate(const void* pcm, size_t frame_count, int sample_rate, int channels,
								AeronPcmFormat fmt);

/* Mutable clips start silent. Writes use byte offsets in the original format
 * and preserve the playback positions of all active voices. */
AeronClip Aeron_AudioClipCreateMutable(size_t frame_count, int sample_rate, int channels, AeronPcmFormat fmt);

typedef struct AeronPcmWrite {
	size_t      offset;
	const void* pcm;
	size_t      bytes;
} AeronPcmWrite;

/* Validates every region before committing them together under the mixer lock.
 * While the mixer is mixing a positional voice of the clip, the write waits
 * until that chunk is mixed, so the chunk reads only the old or the new PCM. */
int Aeron_AudioClipWrite(AeronClip clip, const AeronPcmWrite* writes, size_t count);

/* Releases the clip. Memory is retained until any voices still referencing it
 * finish, so it is safe to destroy a clip while it is playing. */
void Aeron_AudioClipDestroy(AeronClip clip);

/* Voices ------------------------------------------------------------------ */

/* pan is an amplitude-preserving stereo balance in [-1, 1]: at zero both
 * channels play at full gain; either extreme silences the opposite channel. */
AeronVoice Aeron_AudioVoicePlay(AeronClip clip, float gain, float pan, float pitch, int loop);

/* Start playback at the specified source frame. */
AeronVoice Aeron_AudioVoicePlayFrom(AeronClip clip, float gain, float pan, float pitch, int loop,
									size_t frame);
/* Starts a positional voice at the specified source frame. */
AeronVoice Aeron_AudioVoicePlay3DSource(AeronClip clip, float gain, float pitch, int loop,
										const AeronAudioSource3D* source, size_t frame);
/* Cursors are source frames: play estimates audible position; write is the
 * first frame the mixer hasn't read yet, three past the next frame to mix,
 * since the resampler reads two frames ahead. It wraps around at the end of
 * the clip. Queued output is estimated at the current playback rate. */
int Aeron_AudioVoiceGetCursors(AeronVoice voice, size_t* play, size_t* write);
int Aeron_AudioVoiceSetPosition(AeronVoice voice, size_t frame);
/* Returns zero if the voice has already finished. */
int Aeron_AudioVoiceSetLooping(AeronVoice voice, int loop);
/* Releases the voice and returns the next source frame to mix for a later
 * resume. Output already submitted to the device finishes playing. Returns
 * zero after a non-looping voice has mixed its entire clip. */
size_t Aeron_AudioVoiceStopAndGetPosition(AeronVoice voice);

void Aeron_AudioVoiceSetGain(AeronVoice voice, float gain);
void Aeron_AudioVoiceSetPan(AeronVoice voice, float pan);
void Aeron_AudioVoiceSetPitch(AeronVoice voice, float pitch);
/* Inside a 3D batch, the change waits for Aeron_Audio3DEndBatch. */
void Aeron_AudioVoiceSet3DSource(AeronVoice voice, const AeronAudioSource3D* source);
void Aeron_AudioVoiceStop(AeronVoice voice);
/* Remains true while the voice's final output is queued for playback. It
 * uses the count of frames heard as of the last device callback, so it can
 * stay true up to one device buffer longer. */
int Aeron_AudioVoiceIsPlaying(AeronVoice voice);

/* Positional model (shared listener + global tuning factors). Inside a 3D
 * batch, a listener change waits for Aeron_Audio3DEndBatch. */
void Aeron_AudioSetListener(const AeronAudioListener* listener);

/* The listener and source changes made between Begin and End reach the mixer
 * together, so no mixed chunk uses some of them without the others. Batches
 * nest: the changes apply at the outermost End. Voices started inside a batch
 * use their source at once. */
void Aeron_Audio3DBeginBatch(void);
void Aeron_Audio3DEndBatch(void);
/* distance_factor is the number of metres in one unit. Only doppler uses it,
 * to compare speeds with the speed of sound. */
void Aeron_AudioSetDistanceModel(float distance_factor, float rolloff_factor, float doppler_factor);

/* Queued PCM streams -------------------------------------------------------
 *
 * A bounded FIFO for decoded or generated streaming audio. One producer may
 * write from a worker thread while the Aeron callback consumes the stream.
 * Samples remain queued until consumed by the mixer. */
typedef uint32_t AeronAudioStream; /* 0 == invalid */

AeronAudioStream Aeron_AudioStreamOpen(int rate, int channels, AeronPcmFormat format, size_t capacity_frames,
									   float gain);
/* Writes as many complete frames as currently fit and returns that count. */
size_t Aeron_AudioStreamWrite(AeronAudioStream stream, const void* pcm, size_t frame_count);
/* Event-based producer wait. Returns zero when the stream is paused, closed,
 * or cannot ever hold required_frames. */
int    Aeron_AudioStreamWaitWritable(AeronAudioStream stream, size_t required_frames);
size_t Aeron_AudioStreamWritableFrames(AeronAudioStream stream);
size_t Aeron_AudioStreamQueuedFrames(AeronAudioStream stream);
size_t Aeron_AudioStreamCapacityFrames(AeronAudioStream stream);
/* Grows a paused, never-consumed stream while preserving queued samples.
 * Intended for bounded decoder prebuffering before playback starts. */
int      Aeron_AudioStreamEnsureCapacity(AeronAudioStream stream, size_t minimum_capacity_frames);
void     Aeron_AudioStreamPlay(AeronAudioStream stream);
void     Aeron_AudioStreamPause(AeronAudioStream stream);
void     Aeron_AudioStreamFlush(AeronAudioStream stream);
void     Aeron_AudioStreamSetGain(AeronAudioStream stream, float gain);
int      Aeron_AudioStreamIsPlaying(AeronAudioStream stream);
uint64_t Aeron_AudioStreamConsumedFrames(AeronAudioStream stream);
uint64_t Aeron_AudioStreamAudibleFrames(AeronAudioStream stream);
uint64_t Aeron_AudioStreamUnderrunCount(AeronAudioStream stream);
void     Aeron_AudioStreamClose(AeronAudioStream stream);

/* Master ------------------------------------------------------------------ */

void Aeron_AudioSetMasterGain(float gain);

/* Sets the number of channels of the output: 2, 6 (5.1) or 8 (7.1), in SDL's
 * order. With speakers, positional voices are panned over all of them but
 * LFE; with headphones, they play on the first two. 2D voices and streams
 * always play on the first two, front left and front right. Reopens the
 * device, so call it from the control thread while no
 * other thread uses audio. Fails, and keeps the current count, while any
 * voice or stream exists. */
int Aeron_AudioSetOutputChannels(int channels);
int Aeron_AudioOutputChannels(void);
/* The channel count of the device itself, as the system reports it when the
 * output opens. It can differ from the output's: SDL converts. 0 when
 * unknown, as with the DirectSound, ALSA and sndio drivers, which don't
 * report it. When the system switches to another device while the output is
 * open, it is read again only when the output reopens. */
int Aeron_AudioDeviceChannels(void);

/* Spatial output --------------------------------------------------------- */

typedef enum AeronAudioOutputMode {
	/* Positional voices are panned between the speakers. */
	AERON_AUDIO_OUTPUT_SPEAKERS = 0,
	/* Positional voices are filtered with an HRTF and play on the first two
	   channels. On headphones, they then seem to come from their direction,
	   including above and behind the listener. */
	AERON_AUDIO_OUTPUT_HEADPHONES = 1,
} AeronAudioOutputMode;

typedef struct AeronAudioSpatialSettings {
	AeronAudioOutputMode mode;
	/* High frequencies fade with distance, as they do through air. 0 turns
	   it off; 1 is the physical amount, -0.05 dB per metre at 5 kHz beyond
	   the min distance. It uses the distance factor of
	   Aeron_AudioSetDistanceModel to turn units into metres. */
	float air_absorption;
} AeronAudioSpatialSettings;

/* Applies at the next mixed chunk. Headphones use the HRTF data set loaded
 * with Aeron_AudioLoadHrtf, or the built-in one if none was loaded. */
void Aeron_AudioSetSpatialSettings(const AeronAudioSpatialSettings* settings);

/* Loads an OpenAL Soft .mhr data set (MinPHR03 or MinPHR02, at 48,000 Hz)
 * through the VFS. With a NULL path, loads the built-in data set, derived
 * from SADIE II subject D1. On failure, writes a message to `error`, keeps
 * the current data set, and returns 0. Call it from the control thread; it
 * can wait up to one mixed chunk for the voices that use the current data
 * set. */
int Aeron_AudioLoadHrtf(AeronVfsRoot root, const char* path, char* error, size_t error_size);

/* Pauses / resumes the output device. The mixer callback stops, freezing
 * voices, queued-stream consumption, and queued-stream
 * audible clocks. Buffered PCM is preserved for a continuous resume. */
void Aeron_AudioSetPaused(int paused);

#ifdef __cplusplus
}
#endif

#endif
