#ifndef AERON_AUDIO_HRTF_H
#define AERON_AUDIO_HRTF_H

#include <stddef.h>
#include <stdint.h>

/* HRTF filtering for the mixer's headphone output (audio_hrtf.c). The data
 * sets are OpenAL Soft .mhr files, and the methods follow OpenAL Soft's
 * direct HRTF mixing: bilinear blending of the four nearest measured
 * responses, whole-sample ear delays, and a 64-frame crossfade when the
 * filter of a voice changes. */

/* The longest response a data set may have, in taps. */
#define AERON_HRTF_MAX_TAPS 128
/* Input frames kept for the ear delays, which are at most 63 frames. */
#define AERON_HRTF_HISTORY 64
/* When a voice's filter changes, the old and new filters crossfade over this
 * many frames. */
#define AERON_HRTF_FADE_FRAMES 64

typedef struct AeronHrtf AeronHrtf;

/* The filter for one direction: a response for each ear, interleaved as
 * left/right pairs, and the delay of each ear in frames. */
typedef struct AeronHrtfFilter {
	float coeffs[AERON_HRTF_MAX_TAPS * 2];
	int   delay[2];
} AeronHrtfFilter;

/* The HRTF state that one voice carries from one chunk to the next. */
typedef struct AeronHrtfVoice {
	AeronHrtfFilter filter[2];
	int             current; /* the filter in use */
	/* How far a crossfade to filter[!current] has got, in frames, or -1
	   when there is no crossfade. */
	int   fade;
	float azimuth[2]; /* the direction each filter was made for */
	float elevation[2];
	float history[AERON_HRTF_HISTORY];   /* the last input frames */
	float tail[AERON_HRTF_MAX_TAPS * 2]; /* output still to come, left/right */
} AeronHrtfVoice;

/* Parses a MinPHR03 or MinPHR02 data set. It must be at 48,000 Hz. Returns
 * NULL and writes a message to `error` when the data is not usable. */
AeronHrtf* Aeron_HrtfLoad(const uint8_t* data, size_t size, char* error, size_t error_size);
void       Aeron_HrtfFree(AeronHrtf* hrtf);
/* The response length, rounded up to a multiple of 8 taps. */
int Aeron_HrtfTaps(const AeronHrtf* hrtf);
/* The number of measured responses in the field used. */
int Aeron_HrtfResponses(const AeronHrtf* hrtf);

/* The built-in data set: OpenAL Soft's default, embedded at build time. */
const uint8_t* Aeron_HrtfBuiltinData(size_t* size);

/* Computes the filter for a direction in radians: the azimuth is positive
 * to the right, and the elevation positive upwards. */
void Aeron_HrtfGetFilter(const AeronHrtf* hrtf, float azimuth, float elevation, AeronHrtfFilter* out);

/* Filters `frames` frames of one voice and adds them to the left and right
 * channels of `mix`, which has `channels` channels. `input` holds the
 * frames from index AERON_HRTF_HISTORY on; the first AERON_HRTF_HISTORY
 * floats are room for the voice's history, which this fills. With `start`
 * set, the voice starts from silence with the filter for the direction;
 * otherwise, a new direction crossfades from the filter in use. `scratch`
 * holds (frames + AERON_HRTF_MAX_TAPS) * 2 floats. */
void Aeron_HrtfMixVoice(const AeronHrtf* hrtf, AeronHrtfVoice* voice, int start, float azimuth,
						float elevation, float* input, int frames, float* scratch, float* mix, int channels);

/* Does what Aeron_HrtfMixVoice would do for a voice whose input, history
 * and tail are all zeros, without filtering anything: the filter and the
 * crossfade follow the direction in the same way, and nothing is added to
 * the mix. */
void Aeron_HrtfSkipVoice(const AeronHrtf* hrtf, AeronHrtfVoice* voice, int start, float azimuth,
						 float elevation, int frames);

#endif
