#include "audio_hrtf.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

/* HRTF data sets and filtering. See audio_hrtf.h.
 *
 * The file format is OpenAL Soft's (docs/hrtf.txt and core/hrtf_loader.cpp
 * in its sources). All fields are little-endian:
 *   - "MinPHR03": u32 rate, u8 channel type (0 = left ear only, 1 = both),
 *     u8 response size, u8 field count;
 *   - "MinPHR02": u32 rate, u8 sample type (0 = S16, 1 = S24), then the same;
 *   - for each field: u16 distance in mm, u8 elevation count, then u8
 *     azimuth counts, one per elevation ring from -90° upwards;
 *   - the responses of every field, [response][tap][ear], then the delays,
 *     [response][ear]. MinPHR03 stores S24 taps and delays in 6.2 fixed
 *     point; MinPHR02 stores whole-frame delays.
 * Azimuths start at the front and go clockwise, toward the right. MinPHR03
 * stores the farthest field first, MinPHR02 the nearest. Game sounds are far
 * away, so only the farthest field is used.
 *
 * Debug builds also compile this file with -O3 (see aeron/CMakeLists.txt),
 * so they can mix many voices in real time. */

#if defined(_MSC_VER)
#define AERON_RESTRICT __restrict
#else
#define AERON_RESTRICT restrict
#endif

#define AERON_HRTF_RATE 48000
#define AERON_HRTF_MAX_RINGS 181
#define AERON_HRTF_MAX_DELAY 63
#define AERON_HRTF_PI 3.14159265358979323846f

struct AeronHrtf {
	int      taps; /* rounded up to a multiple of 8 */
	int      rings;
	int      mirrored; /* only the left ear is stored; the right ear is its mirror image */
	int      responses;
	int      az_count[AERON_HRTF_MAX_RINGS];
	int      first[AERON_HRTF_MAX_RINGS]; /* the first response of each ring */
	float*   coeffs;                      /* [response][tap][ear] */
	uint8_t* delays;                      /* [response][ear], 6.2 fixed point */
};

extern const unsigned char aeron_hrtf_default[];
extern const unsigned long aeron_hrtf_default_size;

const uint8_t* Aeron_HrtfBuiltinData(size_t* size) {
	*size = (size_t)aeron_hrtf_default_size;
	return aeron_hrtf_default;
}

/* --- loading -------------------------------------------------------------- */

typedef struct HrtfReader {
	const uint8_t* data;
	size_t         size;
	size_t         pos;
} HrtfReader;

static int Hrtf_Has(const HrtfReader* r, size_t bytes) { return r->size - r->pos >= bytes; }

static unsigned Hrtf_U8(HrtfReader* r) { return r->data[r->pos++]; }

static unsigned Hrtf_U16(HrtfReader* r) {
	unsigned v = (unsigned)r->data[r->pos] | (unsigned)r->data[r->pos + 1] << 8;
	r->pos += 2;
	return v;
}

static uint32_t Hrtf_U32(HrtfReader* r) {
	uint32_t v = (uint32_t)r->data[r->pos] | (uint32_t)r->data[r->pos + 1] << 8 |
				 (uint32_t)r->data[r->pos + 2] << 16 | (uint32_t)r->data[r->pos + 3] << 24;
	r->pos += 4;
	return v;
}

/* A signed tap of `bytes` bytes (2 or 3), as a float in [-1, 1). */
static float Hrtf_Tap(HrtfReader* r, int bytes) {
	if (bytes == 2) {
		const int16_t v = (int16_t)Hrtf_U16(r);
		return (float)v / 32768.0f;
	}
	uint32_t v =
		(uint32_t)r->data[r->pos] | (uint32_t)r->data[r->pos + 1] << 8 | (uint32_t)r->data[r->pos + 2] << 16;
	r->pos += 3;
	const int32_t s = (int32_t)(v ^ 0x800000u) - 0x800000;
	return (float)s / 8388608.0f;
}

static AeronHrtf* Hrtf_Fail(AeronHrtf* hrtf, char* error, size_t error_size, const char* message) {
	if (error && error_size) {
		snprintf(error, error_size, "%s", message);
	}
	Aeron_HrtfFree(hrtf);
	return NULL;
}

void Aeron_HrtfFree(AeronHrtf* hrtf) {
	if (hrtf) {
		SDL_free(hrtf->coeffs);
		SDL_free(hrtf->delays);
		SDL_free(hrtf);
	}
}

int Aeron_HrtfTaps(const AeronHrtf* hrtf) { return hrtf->taps; }

int Aeron_HrtfResponses(const AeronHrtf* hrtf) { return hrtf->responses; }

/* Uses the same limits as OpenAL Soft. */
AeronHrtf* Aeron_HrtfLoad(const uint8_t* data, size_t size, char* error, size_t error_size) {
	HrtfReader r = { data, data ? size : 0, 0 };
	char       message[160];
	int        v3;

	if (error && error_size) {
		error[0] = '\0';
	}
	if (!Hrtf_Has(&r, 8)) {
		return Hrtf_Fail(NULL, error, error_size, "the file is too short");
	}
	if (memcmp(data, "MinPHR03", 8) == 0) {
		v3 = 1;
	} else if (memcmp(data, "MinPHR02", 8) == 0) {
		v3 = 0;
	} else {
		return Hrtf_Fail(NULL, error, error_size, "not a MinPHR03 or MinPHR02 .mhr file");
	}
	r.pos = 8;
	if (!Hrtf_Has(&r, v3 ? 7u : 8u)) {
		return Hrtf_Fail(NULL, error, error_size, "the file ends early");
	}
	const uint32_t rate        = Hrtf_U32(&r);
	const unsigned sample_type = v3 ? 1u : Hrtf_U8(&r);
	const unsigned channels    = Hrtf_U8(&r);
	const unsigned ir_size     = Hrtf_U8(&r);
	const unsigned fields      = Hrtf_U8(&r);
	if (rate != AERON_HRTF_RATE) {
		snprintf(message, sizeof message,
				 "the data set is at %u Hz, and must be at 48000 Hz (convert it with makemhr -r 48000)",
				 (unsigned)rate);
		return Hrtf_Fail(NULL, error, error_size, message);
	}
	if (sample_type > 1 || channels > 1) {
		return Hrtf_Fail(NULL, error, error_size, "unsupported sample or channel type");
	}
	if (ir_size < 8 || ir_size > AERON_HRTF_MAX_TAPS) {
		snprintf(message, sizeof message, "unsupported response size %u (8 to %d)", ir_size,
				 AERON_HRTF_MAX_TAPS);
		return Hrtf_Fail(NULL, error, error_size, message);
	}
	if (fields < 1 || fields > 16) {
		return Hrtf_Fail(NULL, error, error_size, "unsupported number of fields");
	}

	AeronHrtf* hrtf = (AeronHrtf*)SDL_calloc(1, sizeof(AeronHrtf));
	if (!hrtf) {
		return Hrtf_Fail(NULL, error, error_size, "out of memory");
	}
	hrtf->taps     = (int)((ir_size + 7u) & ~7u);
	hrtf->mirrored = channels == 0;

	/* The field to use, and the responses before it in the file. */
	const unsigned used      = v3 ? 0u : fields - 1u;
	unsigned       skip      = 0;
	unsigned       total     = 0;
	unsigned       last_dist = 0;
	for (unsigned f = 0; f < fields; ++f) {
		if (!Hrtf_Has(&r, 3)) {
			return Hrtf_Fail(hrtf, error, error_size, "the file ends early");
		}
		const unsigned distance = Hrtf_U16(&r);
		const unsigned rings    = Hrtf_U8(&r);
		if (distance < 50 || distance > 2500 || rings < 5 || rings > AERON_HRTF_MAX_RINGS) {
			return Hrtf_Fail(hrtf, error, error_size, "unsupported field distance or elevation count");
		}
		if (f > 0 && (v3 ? distance >= last_dist : distance <= last_dist)) {
			return Hrtf_Fail(hrtf, error, error_size, "the fields are not in order of distance");
		}
		last_dist = distance;
		if (!Hrtf_Has(&r, rings)) {
			return Hrtf_Fail(hrtf, error, error_size, "the file ends early");
		}
		unsigned field_total = 0;
		for (unsigned e = 0; e < rings; ++e) {
			const unsigned count = Hrtf_U8(&r);
			if (count < 1) {
				return Hrtf_Fail(hrtf, error, error_size, "an elevation has no azimuth");
			}
			if (f == used) {
				hrtf->az_count[e] = (int)count;
				hrtf->first[e]    = (int)field_total;
			}
			field_total += count;
		}
		if (f == used) {
			hrtf->rings     = (int)rings;
			hrtf->responses = (int)field_total;
		} else if (f < used) {
			skip += field_total;
		}
		total += field_total;
	}

	const unsigned ears       = channels == 0 ? 1u : 2u;
	const unsigned tap_bytes  = sample_type == 0 ? 2u : 3u;
	const size_t   coef_bytes = (size_t)total * ir_size * ears * tap_bytes;
	const size_t   delay_size = (size_t)total * ears;
	if (!Hrtf_Has(&r, coef_bytes + delay_size)) {
		return Hrtf_Fail(hrtf, error, error_size, "the file ends early");
	}
	hrtf->coeffs = (float*)SDL_calloc((size_t)hrtf->responses * (size_t)hrtf->taps * 2u, sizeof(float));
	hrtf->delays = (uint8_t*)SDL_calloc((size_t)hrtf->responses * 2u, 1);
	if (!hrtf->coeffs || !hrtf->delays) {
		return Hrtf_Fail(hrtf, error, error_size, "out of memory");
	}

	const size_t coef_start = r.pos;
	r.pos                   = coef_start + (size_t)skip * ir_size * ears * tap_bytes;
	for (int h = 0; h < hrtf->responses; ++h) {
		float* out = hrtf->coeffs + (size_t)h * (size_t)hrtf->taps * 2u;
		for (unsigned k = 0; k < ir_size; ++k) {
			for (unsigned ear = 0; ear < ears; ++ear) {
				out[k * 2 + ear] = Hrtf_Tap(&r, (int)tap_bytes);
			}
		}
	}
	r.pos = coef_start + coef_bytes + (size_t)skip * ears;
	for (int h = 0; h < hrtf->responses; ++h) {
		for (unsigned ear = 0; ear < ears; ++ear) {
			/* MinPHR02 stores delays in whole frames; convert them to 6.2
			   fixed point. */
			const unsigned delay = Hrtf_U8(&r);
			if (delay > (v3 ? AERON_HRTF_MAX_DELAY << 2 : AERON_HRTF_MAX_DELAY)) {
				return Hrtf_Fail(hrtf, error, error_size, "a delay is longer than 63 frames");
			}
			hrtf->delays[h * 2 + (int)ear] = (uint8_t)(v3 ? delay : delay << 2);
		}
	}

	/* The right ear's response at azimuth j is the left ear's at -j. */
	if (hrtf->mirrored) {
		for (int e = 0; e < hrtf->rings; ++e) {
			const int count = hrtf->az_count[e];
			for (int j = 0; j < count; ++j) {
				const int    left  = hrtf->first[e] + j;
				const int    right = hrtf->first[e] + (count - j) % count;
				const float* src   = hrtf->coeffs + (size_t)left * (size_t)hrtf->taps * 2u;
				float*       dst   = hrtf->coeffs + (size_t)right * (size_t)hrtf->taps * 2u;
				for (int k = 0; k < hrtf->taps; ++k) {
					dst[k * 2 + 1] = src[k * 2];
				}
				hrtf->delays[right * 2 + 1] = hrtf->delays[left * 2];
			}
		}
	}
	return hrtf;
}

/* --- filters -------------------------------------------------------------- */

/* Finds the ring just below an elevation, and the blend weight of the ring
 * above it. As in OpenAL Soft, the rings are evenly spaced from -90° to
 * +90°. */
static void Hrtf_RingIndex(int rings, float elevation, int* index, float* blend) {
	const float e = (elevation / AERON_HRTF_PI + 0.5f) * (float)(rings - 1);
	int         i = e > 0.0f ? (int)e : 0;
	if (i > rings - 1) {
		i = rings - 1;
	}
	*index = i;
	*blend = e - (float)i;
}

/* Finds the measured azimuth just before a direction, on a ring of `count`
 * azimuths, and the blend weight of the next one. */
static void Hrtf_AzimuthIndex(int count, float azimuth, int* index, float* blend) {
	const float a = (azimuth / (2.0f * AERON_HRTF_PI) + 1.0f) * (float)count;
	const int   i = a > 0.0f ? (int)a : 0;
	*index        = i % count;
	*blend        = a - (float)i;
}

/* Blends the four responses around a direction, as OpenAL Soft's
 * HrtfStore::getCoeffs does. */
static void Hrtf_Blend(const AeronHrtf* hrtf, float azimuth, float elevation, AeronHrtfFilter* out) {
	int   e0, a0, a1;
	float eb, ab0, ab1;
	Hrtf_RingIndex(hrtf->rings, elevation, &e0, &eb);
	const int e1 = e0 + 1 < hrtf->rings ? e0 + 1 : hrtf->rings - 1;
	Hrtf_AzimuthIndex(hrtf->az_count[e0], azimuth, &a0, &ab0);
	Hrtf_AzimuthIndex(hrtf->az_count[e1], azimuth, &a1, &ab1);

	const int index[4] = {
		hrtf->first[e0] + a0,
		hrtf->first[e0] + (a0 + 1) % hrtf->az_count[e0],
		hrtf->first[e1] + a1,
		hrtf->first[e1] + (a1 + 1) % hrtf->az_count[e1],
	};
	const float blend[4] = {
		(1.0f - eb) * (1.0f - ab0),
		(1.0f - eb) * ab0,
		eb * (1.0f - ab1),
		eb * ab1,
	};

	for (int ear = 0; ear < 2; ++ear) {
		float d = 0.0f;
		for (int c = 0; c < 4; ++c) {
			d += (float)hrtf->delays[index[c] * 2 + ear] * blend[c];
		}
		out->delay[ear] = (int)(d * 0.25f + 0.5f);
	}
	const int n = hrtf->taps * 2;
	memset(out->coeffs, 0, sizeof out->coeffs);
	for (int c = 0; c < 4; ++c) {
		const float* src = hrtf->coeffs + (size_t)index[c] * (size_t)n;
		for (int k = 0; k < n; ++k) {
			out->coeffs[k] += src[k] * blend[c];
		}
	}
}

void Aeron_HrtfGetFilter(const AeronHrtf* hrtf, float azimuth, float elevation, AeronHrtfFilter* out) {
	/* A mirrored data set is symmetric, so a direction on the left is the one
	   on the right with the ears swapped. Working it out that way makes the
	   two sides match exactly. */
	if (!hrtf->mirrored || azimuth >= 0.0f) {
		Hrtf_Blend(hrtf, azimuth, elevation, out);
		return;
	}
	Hrtf_Blend(hrtf, -azimuth, elevation, out);
	for (int k = 0; k < hrtf->taps; ++k) {
		const float left       = out->coeffs[k * 2];
		out->coeffs[k * 2]     = out->coeffs[k * 2 + 1];
		out->coeffs[k * 2 + 1] = left;
	}
	const int delay = out->delay[0];
	out->delay[0]   = out->delay[1];
	out->delay[1]   = delay;
}

/* --- mixing --------------------------------------------------------------- */

/* Adds one filtered input frame to the accumulator. This is the
 * accumulation form of the convolution, as in OpenAL Soft. The loop has no
 * reduction, so it vectorizes without reordering the sums: clang vectorizes
 * it at -O2 and -O3, GCC only at -O3. Of the loop forms tried, this one was
 * the fastest with clang (arm64 and x86_64), and within 30% of the fastest
 * with GCC -O3. */
static void Hrtf_AddFrame(float* AERON_RESTRICT acc, const float* AERON_RESTRICT coeffs, int taps, float left,
						  float right) {
	for (int k = 0; k < taps; ++k) {
		acc[2 * k] += coeffs[2 * k] * left;
		acc[2 * k + 1] += coeffs[2 * k + 1] * right;
	}
}

/* Filters `frames` frames from `in` (after the history) into `acc`. */
static void Hrtf_Filter(float* acc, const float* in, int frames, const AeronHrtfFilter* filter, int taps) {
	const float* left_in  = in + AERON_HRTF_HISTORY - filter->delay[0];
	const float* right_in = in + AERON_HRTF_HISTORY - filter->delay[1];
	for (int i = 0; i < frames; ++i) {
		Hrtf_AddFrame(acc + 2 * i, filter->coeffs, taps, left_in[i], right_in[i]);
	}
}

/* The same, during a crossfade from `from` to `to`. `fade` is the crossfade
 * position of the first frame. Each frame adds the old filter, then the new
 * one, and the weights are computed from the position, so the sums don't
 * depend on how the frames are split into chunks. */
static void Hrtf_FilterFade(float* acc, const float* in, int frames, const AeronHrtfFilter* from,
							const AeronHrtfFilter* to, int taps, int fade) {
	const float* from_left  = in + AERON_HRTF_HISTORY - from->delay[0];
	const float* from_right = in + AERON_HRTF_HISTORY - from->delay[1];
	const float* to_left    = in + AERON_HRTF_HISTORY - to->delay[0];
	const float* to_right   = in + AERON_HRTF_HISTORY - to->delay[1];
	for (int i = 0; i < frames; ++i) {
		const float rising  = (float)(fade + i) * (1.0f / AERON_HRTF_FADE_FRAMES);
		const float falling = (float)(AERON_HRTF_FADE_FRAMES - fade - i) * (1.0f / AERON_HRTF_FADE_FRAMES);
		Hrtf_AddFrame(acc + 2 * i, from->coeffs, taps, from_left[i] * falling, from_right[i] * falling);
		Hrtf_AddFrame(acc + 2 * i, to->coeffs, taps, to_left[i] * rising, to_right[i] * rising);
	}
}

/* Starts a voice with the filter for a direction, from silence. */
static void Hrtf_StartVoice(const AeronHrtf* hrtf, AeronHrtfVoice* voice, float azimuth, float elevation) {
	Aeron_HrtfGetFilter(hrtf, azimuth, elevation, &voice->filter[0]);
	voice->current      = 0;
	voice->fade         = -1;
	voice->azimuth[0]   = azimuth;
	voice->elevation[0] = elevation;
	memset(voice->history, 0, sizeof voice->history);
	memset(voice->tail, 0, sizeof voice->tail);
}

/* Starts a crossfade to a new direction, unless one is in progress: a new
 * direction waits for it to end. */
static void Hrtf_FollowDirection(const AeronHrtf* hrtf, AeronHrtfVoice* voice, float azimuth,
								 float elevation) {
	const int current = voice->current;
	if (voice->fade < 0 && (azimuth != voice->azimuth[current] || elevation != voice->elevation[current])) {
		Aeron_HrtfGetFilter(hrtf, azimuth, elevation, &voice->filter[!current]);
		voice->azimuth[!current]   = azimuth;
		voice->elevation[!current] = elevation;
		voice->fade                = 0;
	}
}

/* How many of the next `frames` frames are still part of the crossfade in
 * progress. */
static int Hrtf_FadeFrames(const AeronHrtfVoice* voice, int frames) {
	const int left = AERON_HRTF_FADE_FRAMES - voice->fade;
	return left < frames ? left : frames;
}

/* Moves the crossfade forward by `frames` frames, and ends it once it reaches
 * its full length. */
static void Hrtf_AdvanceFade(AeronHrtfVoice* voice, int frames) {
	voice->fade += frames;
	if (voice->fade == AERON_HRTF_FADE_FRAMES) {
		voice->current = !voice->current;
		voice->fade    = -1;
	}
}

void Aeron_HrtfMixVoice(const AeronHrtf* hrtf, AeronHrtfVoice* voice, int start, float azimuth,
						float elevation, float* input, int frames, float* scratch, float* mix, int channels) {
	const int taps = hrtf->taps;

	if (start) {
		Hrtf_StartVoice(hrtf, voice, azimuth, elevation);
	}
	memcpy(input, voice->history, sizeof voice->history);
	memcpy(scratch, voice->tail, (size_t)taps * 2u * sizeof(float));
	memset(scratch + taps * 2, 0, (size_t)frames * 2u * sizeof(float));

	int done = 0;
	while (done < frames) {
		Hrtf_FollowDirection(hrtf, voice, azimuth, elevation);
		const int current = voice->current;
		if (voice->fade < 0) {
			Hrtf_Filter(scratch + 2 * done, input + done, frames - done, &voice->filter[current], taps);
			break;
		}
		const int n = Hrtf_FadeFrames(voice, frames - done);
		Hrtf_FilterFade(scratch + 2 * done, input + done, n, &voice->filter[current],
						&voice->filter[!current], taps, voice->fade);
		Hrtf_AdvanceFade(voice, n);
		done += n;
	}

	for (int i = 0; i < frames; ++i) {
		mix[i * channels] += scratch[2 * i];
		mix[i * channels + 1] += scratch[2 * i + 1];
	}
	memcpy(voice->tail, scratch + frames * 2, (size_t)taps * 2u * sizeof(float));
	memcpy(voice->history, input + frames, sizeof voice->history);
}

void Aeron_HrtfSkipVoice(const AeronHrtf* hrtf, AeronHrtfVoice* voice, int start, float azimuth,
						 float elevation, int frames) {
	if (start) {
		Hrtf_StartVoice(hrtf, voice, azimuth, elevation);
	}
	int done = 0;
	while (done < frames) {
		Hrtf_FollowDirection(hrtf, voice, azimuth, elevation);
		if (voice->fade < 0) {
			break;
		}
		const int n = Hrtf_FadeFrames(voice, frames - done);
		Hrtf_AdvanceFade(voice, n);
		done += n;
	}
}
