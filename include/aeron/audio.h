#ifndef AERON_AUDIO_H
#define AERON_AUDIO_H

#include <stddef.h>
#include <stdint.h>

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

typedef struct AeronAudioListener {
	float pos[3];   /* world position */
	float front[3]; /* forward orientation vector (need not be normalized) */
	float top[3];   /* up orientation vector (need not be normalized) */
	float vel[3];   /* world velocity for doppler */
} AeronAudioListener;

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

/* Validates every region before committing them together under the mixer lock. */
int Aeron_AudioClipWrite(AeronClip clip, const AeronPcmWrite* writes, size_t count);

/* Releases the clip. Memory is retained until any voices still referencing it
 * finish, so it is safe to destroy a clip while it is playing. */
void Aeron_AudioClipDestroy(AeronClip clip);

/* Voices ------------------------------------------------------------------ */

/* pan is an amplitude-preserving stereo balance in [-1, 1]: at zero both
 * channels play at full gain; either extreme silences the opposite channel. */
AeronVoice Aeron_AudioVoicePlay(AeronClip clip, float gain, float pan, float pitch, int loop);
AeronVoice Aeron_AudioVoicePlay3D(AeronClip clip, float gain, float pitch, int loop, const float pos[3],
								  float min_dist, float max_dist);

/* Start playback at the specified source frame. */
AeronVoice Aeron_AudioVoicePlayFrom(AeronClip clip, float gain, float pan, float pitch, int loop,
									size_t frame);
AeronVoice Aeron_AudioVoicePlay3DFrom(AeronClip clip, float gain, float pitch, int loop, const float pos[3],
									  float min_dist, float max_dist, size_t frame);
/* Cursors are source frames: play estimates audible position; write is the
 * next frame to mix. Queued output is estimated at the current playback rate. */
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
void Aeron_AudioVoiceSet3DPosition(AeronVoice voice, const float pos[3]);
void Aeron_AudioVoiceSet3DVelocity(AeronVoice voice, const float vel[3]);
void Aeron_AudioVoiceStop(AeronVoice voice);
/* Remains true while the voice's final output is queued for playback. */
int Aeron_AudioVoiceIsPlaying(AeronVoice voice);

/* Positional model (shared listener + global tuning factors). */
void Aeron_AudioSetListener(const AeronAudioListener* listener);
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

/* Pauses / resumes the output device. The mixer callback stops, freezing
 * voices, queued-stream consumption, and queued-stream
 * audible clocks. Buffered PCM is preserved for a continuous resume. */
void Aeron_AudioSetPaused(int paused);

#ifdef __cplusplus
}
#endif

#endif
