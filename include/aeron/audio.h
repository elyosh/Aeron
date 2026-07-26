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
 *   - clips played as voices: immutable PCM uploaded once, played as instances
 *     with linear gain, stereo pan, a pitch ratio, and an optional 3D position;
 *   - rings: a fixed circular buffer a producer overwrites in place while the
 *     mixer reads it through a looping play cursor (for streamed audio such as
 *     voice-overs and the music engine).
 *
 * The API is intentionally game-agnostic: it knows only linear gain [0..1],
 * pan [-1..1], pitch ratios and a standard inverse-distance positional model.
 * No DirectSound, millibel, or game-specific concepts cross this boundary.
 *
 * Handles are generation-tagged 32-bit ids; the value 0 is always invalid.
 * Operations on a stale handle (whose slot has since been reused) are safe
 * no-ops. All entry points are safe to call from the engine main thread while
 * the SDL audio callback mixes on its own thread. */

typedef uint32_t AeronClip;  /* 0 == invalid */
typedef uint32_t AeronVoice; /* 0 == invalid */

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

/* Releases the clip. Memory is retained until any voices still referencing it
 * finish, so it is safe to destroy a clip while it is playing. */
void Aeron_AudioClipDestroy(AeronClip clip);

/* Voices ------------------------------------------------------------------ */

AeronVoice Aeron_AudioVoicePlay(AeronClip clip, float gain, float pan, float pitch, int loop);
AeronVoice Aeron_AudioVoicePlay3D(AeronClip clip, float gain, float pitch, int loop, const float pos[3],
								  float min_dist, float max_dist);

void Aeron_AudioVoiceSetGain(AeronVoice voice, float gain);
void Aeron_AudioVoiceSetPan(AeronVoice voice, float pan);
void Aeron_AudioVoiceSetPitch(AeronVoice voice, float pitch);
void Aeron_AudioVoiceSet3DPosition(AeronVoice voice, const float pos[3]);
void Aeron_AudioVoiceSet3DVelocity(AeronVoice voice, const float vel[3]);
void Aeron_AudioVoiceStop(AeronVoice voice);
int  Aeron_AudioVoiceIsPlaying(AeronVoice voice);

/* Positional model (shared listener + global tuning factors). */
void Aeron_AudioSetListener(const AeronAudioListener* listener);
void Aeron_AudioSetDistanceModel(float distance_factor, float rolloff_factor, float doppler_factor);

/* Ring sources --------------------------------------------------------------
 *
 * A fixed-size circular PCM buffer that the producer writes in place at byte
 * offsets while the mixer reads it linearly through an advancing play cursor
 * (optionally looping). This models a DirectSound streaming secondary buffer,
 * where the app overwrites the ring ahead of playback. The producer is expected
 * to keep its writes ahead of the play cursor; the ring bytes themselves are
 * not locked (a benign producer/mixer race separated in space), matching the
 * original lock-free streaming-buffer behaviour. */
typedef uint32_t AeronRing; /* 0 == invalid */

AeronRing Aeron_AudioRingOpen(int rate, int channels, int bits, size_t ring_bytes, float gain);
void*     Aeron_AudioRingBase(AeronRing ring); /* engine-owned ring storage to write into */
int       Aeron_AudioRingWrite(AeronRing ring, size_t offset, const void* src, size_t bytes);
size_t    Aeron_AudioRingPlayCursorBytes(AeronRing ring);
void      Aeron_AudioRingPlay(AeronRing ring, int looping);
void      Aeron_AudioRingStop(AeronRing ring);
int       Aeron_AudioRingIsPlaying(AeronRing ring);
void      Aeron_AudioRingSetGain(AeronRing ring, float gain);
void      Aeron_AudioRingClose(AeronRing ring);

/* Queued PCM streams -------------------------------------------------------
 *
 * A bounded FIFO for decoded or generated streaming audio. One producer may
 * write from a worker thread while the Aeron callback consumes the stream.
 * Unlike AeronRing, queued samples are never overwritten before playback. */
typedef uint32_t AeronAudioStream; /* 0 == invalid */

AeronAudioStream Aeron_AudioStreamOpen(int rate, int channels, AeronPcmFormat format, size_t capacity_frames,
									   float gain);
/* Writes as many complete frames as currently fit and returns that count. */
size_t Aeron_AudioStreamWrite(AeronAudioStream stream, const void* pcm, size_t frame_count);
/* Event-based producer wait. Returns zero when the stream is paused, closed,
 * or cannot ever hold required_frames. */
int      Aeron_AudioStreamWaitWritable(AeronAudioStream stream, size_t required_frames);
size_t   Aeron_AudioStreamWritableFrames(AeronAudioStream stream);
size_t   Aeron_AudioStreamQueuedFrames(AeronAudioStream stream);
size_t   Aeron_AudioStreamCapacityFrames(AeronAudioStream stream);
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

/* Pauses / resumes the output device. Ring play cursors freeze in place
 * (the mixer callback stops), so cursor-paced producers resume without a
 * skip. Used for host pause states (user pause, backgrounded window). */
void Aeron_AudioSetPaused(int paused);

#ifdef __cplusplus
}
#endif

#endif
