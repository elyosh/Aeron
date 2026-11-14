/*
 * Tests the audio mixer and the DirectSound shim without an audio device.
 * The mixer runs headless (Aeron_AudioInitHeadless), so every mixed frame
 * counts as played at once and the results don't depend on the machine.
 *
 * The checks cover the spatial audio work (docs/spatial_audio_plan.md): the
 * inverse-distance curve, the direction of positional panning, the
 * conversion of DirectSound's coordinates, the DirectSound buffer cursors,
 * the fade before the max distance, 3D batches and deferred settings, the
 * gain ramps, output that doesn't depend on the chunk size, the resampler,
 * the limiter, and voices that change while they mix outside the mixer
 * mutex.
 *
 * With --bench, it measures the cost of mixing 32 positional voices instead,
 * as a percentage of real time on one core, and how long a call from another
 * thread waits while they mix.
 *
 * Built with the tools as aeron_audio_test. It prints each check and exits
 * with a non-zero status when a check fails.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "aeron/audio.h"
#include "aeron/compat/dsound.h"
#include "internal.h"

enum { RATE = 48000, MAX_FRAMES = 4096 };

/* Half of full scale: the level of the test clips. */
#define CLIP_LEVEL 0.5f
/* One S16 step is 1/32768, about 3e-5. */
#define TOLERANCE 1e-4f

static int   g_failures;
static float g_out[MAX_FRAMES * 2];
/* The output limiter delays everything by this many frames: a sound mixed
 * from the start of a render is heard from this frame of it on. */
static int g_delay;

static void check(int ok, const char* what) {
	printf("%s %s\n", ok ? "ok" : "FAILED", what);
	g_failures += !ok;
}

static int close_to(float value, float expected) { return fabsf(value - expected) <= TOLERANCE; }

static void render(int frames, int chunk_frames) {
	if (frames > MAX_FRAMES || !Aeron_AudioRenderHeadless(g_out, frames, chunk_frames)) {
		printf("FAILED could not render %d frames\n", frames);
		exit(1);
	}
}

static float left_at(int frame) { return g_out[frame * 2]; }

static float right_at(int frame) { return g_out[frame * 2 + 1]; }

/* Plays out what is still in the limiter's delay. With nothing playing, the
 * next render then starts from silence. */
static void flush_delay(void) { render(g_delay, g_delay); }

/* A listener at the origin, looking down -z with +y up, so its right is +x
 * (the mixer is right-handed). */
static void set_mixer_listener(void) {
	AeronAudioListener listener;
	memset(&listener, 0, sizeof listener);
	listener.front[2] = -1.0f;
	listener.top[1]   = 1.0f;
	Aeron_AudioSetListener(&listener);
}

static AeronClip make_constant_clip(int frames) {
	int16_t* pcm = (int16_t*)malloc((size_t)frames * sizeof(int16_t));
	for (int i = 0; i < frames; ++i) {
		pcm[i] = (int16_t)(CLIP_LEVEL * 32768.0f);
	}
	AeronClip clip = Aeron_AudioClipCreate(pcm, (size_t)frames, RATE, 1, AERON_PCM_S16);
	free(pcm);
	return clip;
}

/* The inverse-distance clamped model: min / (min + rolloff·(d − min)), with d
 * clamped to [min, max]. */
static float expected_attenuation(float distance, float min_dist, float max_dist, float rolloff) {
	if (distance > max_dist) {
		distance = max_dist;
	}
	if (distance <= min_dist) {
		return 1.0f;
	}
	return min_dist / (min_dist + rolloff * (distance - min_dist));
}

static AeronAudioSource3D make_source(const float pos[3], float min_dist, float max_dist) {
	AeronAudioSource3D source;
	memset(&source, 0, sizeof source);
	memcpy(source.pos, pos, sizeof source.pos);
	source.min_dist = min_dist;
	source.max_dist = max_dist;
	return source;
}

static AeronVoice play_source(AeronClip clip, float gain, const AeronAudioSource3D* source) {
	AeronVoice voice = Aeron_AudioVoicePlay3DSource(clip, gain, 1.0f, 1, source, 0);
	if (!voice) {
		printf("FAILED could not start a positional voice\n");
		exit(1);
	}
	return voice;
}

/* Plays a looping positional voice of a constant clip and returns the steady
 * left and right levels. The low-pass of rear sources settles well within
 * 256 frames. */
static void measure_source(AeronClip clip, const AeronAudioSource3D* source, float* left, float* right) {
	AeronVoice voice = play_source(clip, 1.0f, source);
	render(256, 256);
	*left  = left_at(255);
	*right = right_at(255);
	Aeron_AudioVoiceStop(voice);
}

static void measure_positional(AeronClip clip, const float pos[3], float min_dist, float max_dist,
							   float* left, float* right) {
	const AeronAudioSource3D source = make_source(pos, min_dist, max_dist);
	measure_source(clip, &source, left, right);
}

static void test_distance_curve(void) {
	static const float distances[] = { 50.0f, 100.0f, 200.0f, 400.0f, 1000.0f, 2000.0f };
	const float        min_dist    = 100.0f;
	const float        max_dist    = 1000.0f;
	AeronClip          clip        = make_constant_clip(4800);
	char               what[128];

	set_mixer_listener();
	for (size_t i = 0; i < sizeof distances / sizeof distances[0]; ++i) {
		/* Straight ahead: both channels get cos(45°) of the gain. */
		const float pos[3] = { 0.0f, 0.0f, -distances[i] };
		const float expected =
			CLIP_LEVEL * sqrtf(0.5f) * expected_attenuation(distances[i], min_dist, max_dist, 1.0f);
		float left, right;
		measure_positional(clip, pos, min_dist, max_dist, &left, &right);
		snprintf(what, sizeof what, "distance %.0f in front: both channels at %.4f (got %.4f, %.4f)",
				 distances[i], expected, left, right);
		check(close_to(left, expected) && close_to(right, expected), what);
	}

	Aeron_AudioSetDistanceModel(1.0f, 2.0f, 1.0f);
	{
		const float pos[3] = { 0.0f, 0.0f, -200.0f };
		const float expected =
			CLIP_LEVEL * sqrtf(0.5f) * expected_attenuation(200.0f, min_dist, max_dist, 2.0f);
		float left, right;
		measure_positional(clip, pos, min_dist, max_dist, &left, &right);
		snprintf(what, sizeof what, "rolloff 2 at distance 200: both channels at %.4f (got %.4f, %.4f)",
				 expected, left, right);
		check(close_to(left, expected) && close_to(right, expected), what);
	}
	Aeron_AudioSetDistanceModel(1.0f, 1.0f, 1.0f);
	Aeron_AudioClipDestroy(clip);
}

static void test_mixer_directions(void) {
	const float min_dist = 100.0f;
	const float max_dist = 1000.0f;
	const float level    = CLIP_LEVEL * expected_attenuation(500.0f, min_dist, max_dist, 1.0f);
	AeronClip   clip     = make_constant_clip(4800);
	float       left, right;
	char        what[128];

	set_mixer_listener();
	{
		const float pos[3] = { 500.0f, 0.0f, 0.0f };
		measure_positional(clip, pos, min_dist, max_dist, &left, &right);
		snprintf(what, sizeof what, "source at +90 degrees: right only (got %.4f, %.4f)", left, right);
		check(close_to(left, 0.0f) && close_to(right, level), what);
	}
	{
		const float pos[3] = { -500.0f, 0.0f, 0.0f };
		measure_positional(clip, pos, min_dist, max_dist, &left, &right);
		snprintf(what, sizeof what, "source at -90 degrees: left only (got %.4f, %.4f)", left, right);
		check(close_to(left, level) && close_to(right, 0.0f), what);
	}
	{
		/* Right behind: centred, with the rear gain of 0.7. The rear low-pass
		   doesn't change a constant signal. */
		const float pos[3]   = { 0.0f, 0.0f, 500.0f };
		const float expected = level * sqrtf(0.5f) * 0.7f;
		measure_positional(clip, pos, min_dist, max_dist, &left, &right);
		snprintf(what, sizeof what, "source behind: both channels at %.4f (got %.4f, %.4f)", expected, left,
				 right);
		check(close_to(left, expected) && close_to(right, expected), what);
	}
	Aeron_AudioClipDestroy(clip);
}

/* --- DirectSound shim ---------------------------------------------------- */

/* The DirectSound3D interfaces, at the slots the shim fills. */
typedef struct TestDs3dBufferVtbl {
	void* unused0[2];
	int (*Release)(void* self);                                                /* 2 */
	void* unused3[13];                                                         /* 3..15 */
	int (*SetMaxDistance)(void* self, float dist, uint32_t apply);             /* 16 */
	int (*SetMinDistance)(void* self, float dist, uint32_t apply);             /* 17 */
	int (*SetMode)(void* self, uint32_t mode, uint32_t apply);                 /* 18 */
	int (*SetPosition)(void* self, float x, float y, float z, uint32_t apply); /* 19 */
	int (*SetVelocity)(void* self, float x, float y, float z, uint32_t apply); /* 20 */
} TestDs3dBufferVtbl;

typedef struct TestDs3dBuffer {
	const TestDs3dBufferVtbl* lpVtbl;
} TestDs3dBuffer;

typedef struct TestDs3dListenerVtbl {
	void* unused0[2];
	int (*Release)(void* self);                                        /* 2 */
	void* unused3[8];                                                  /* 3..10 */
	int (*SetDistanceFactor)(void* self, float value, uint32_t apply); /* 11 */
	int (*SetDopplerFactor)(void* self, float value, uint32_t apply);  /* 12 */
	int (*SetOrientation)(void* self, float fx, float fy, float fz, float tx, float ty, float tz,
						  uint32_t apply);                                     /* 13 */
	int (*SetPosition)(void* self, float x, float y, float z, uint32_t apply); /* 14 */
	void* unused15;                                                            /* 15 */
	int (*SetVelocity)(void* self, float x, float y, float z, uint32_t apply); /* 16 */
	int (*CommitDeferredSettings)(void* self);                                 /* 17 */
} TestDs3dListenerVtbl;

typedef struct TestDs3dListener {
	const TestDs3dListenerVtbl* lpVtbl;
} TestDs3dListener;

enum { DS3D_IMMEDIATE = 0, DS3D_DEFERRED = 1 };

/* A mono 16-bit secondary buffer filled with the constant test level. */
static IDirectSoundBuffer* make_ds_buffer(IDirectSound* device, uint32_t flags, uint32_t frames) {
	DSWaveFormat        format;
	DSBufferDesc        desc;
	IDirectSoundBuffer* buffer = NULL;
	void*               p1;
	void*               p2;
	uint32_t            b1, b2;

	memset(&format, 0, sizeof format);
	format.wFormatTag      = 1;
	format.nChannels       = 1;
	format.nSamplesPerSec  = RATE;
	format.nAvgBytesPerSec = RATE * 2;
	format.nBlockAlign     = 2;
	format.wBitsPerSample  = 16;
	memset(&desc, 0, sizeof desc);
	desc.dwSize        = sizeof desc;
	desc.dwFlags       = flags;
	desc.dwBufferBytes = frames * 2;
	desc.lpwfxFormat   = &format;
	if (device->lpVtbl->CreateSoundBuffer(device, &desc, &buffer, NULL) < 0 || !buffer) {
		printf("FAILED could not create a DirectSound buffer\n");
		exit(1);
	}
	if (buffer->lpVtbl->Lock(buffer, 0, 0, &p1, &b1, &p2, &b2, DSBLOCK_ENTIREBUFFER) < 0) {
		printf("FAILED could not lock a DirectSound buffer\n");
		exit(1);
	}
	for (uint32_t i = 0; i < b1 / 2; ++i) {
		((int16_t*)p1)[i] = (int16_t)(CLIP_LEVEL * 32768.0f);
	}
	buffer->lpVtbl->Unlock(buffer, p1, b1, p2, b2);
	return buffer;
}

static IDirectSound* create_ds_device(void) {
	void* device = NULL;
	if (DirectSoundCreate(NULL, &device, NULL) < 0 || !device) {
		printf("FAILED could not create the DirectSound device\n");
		exit(1);
	}
	return (IDirectSound*)device;
}

/* Creates the primary buffer and gets the listener from it, as the game does.
 * The listener is at the origin with DirectSound's default orientation:
 * looking down +z with +y up. */
static TestDs3dListener* open_ds_listener(IDirectSound* device, IDirectSoundBuffer** primary) {
	DSBufferDesc      primary_desc;
	TestDs3dListener* listener = NULL;

	memset(&primary_desc, 0, sizeof primary_desc);
	primary_desc.dwSize  = sizeof primary_desc;
	primary_desc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D;
	*primary             = NULL;
	if (device->lpVtbl->CreateSoundBuffer(device, &primary_desc, primary, NULL) < 0 ||
		(*primary)->lpVtbl->QueryInterface(*primary, &IID_IDirectSound3DListener, (void**)&listener) < 0) {
		printf("FAILED could not get the DirectSound3D listener\n");
		exit(1);
	}
	listener->lpVtbl->SetPosition(listener, 0.0f, 0.0f, 0.0f, DS3D_DEFERRED);
	listener->lpVtbl->SetVelocity(listener, 0.0f, 0.0f, 0.0f, DS3D_DEFERRED);
	listener->lpVtbl->SetOrientation(listener, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, DS3D_DEFERRED);
	listener->lpVtbl->CommitDeferredSettings(listener);
	return listener;
}

static TestDs3dBuffer* get_ds3d_buffer(IDirectSoundBuffer* buffer) {
	TestDs3dBuffer* buffer3d = NULL;
	if (buffer->lpVtbl->QueryInterface(buffer, &IID_IDirectSound3DBuffer, (void**)&buffer3d) < 0) {
		printf("FAILED could not get a DirectSound3D buffer\n");
		exit(1);
	}
	return buffer3d;
}

/* The flags of the game's positional sound effects (frontend_sound.c). */
#define DS_GAME_3D_FLAGS                                                                                     \
	(DSBCAPS_STATIC | DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME |                          \
	 DSBCAPS_MUTE3DATMAXDISTANCE)

/* DirectSound is left-handed: +x right, +y up, +z forward. The shim negates z
 * for the mixer, so a source keeps its side. */
static void test_shim_directions(void) {
	static const struct {
		float       pos[3];
		const char* name;
		float       left_scale;
		float       right_scale;
	} cases[] = {
		{ { 500.0f, 0.0f, 0.0f }, "DirectSound source at +x: right only", 0.0f, 1.0f },
		{ { -500.0f, 0.0f, 0.0f }, "DirectSound source at -x: left only", 1.0f, 0.0f },
		{ { 0.0f, 0.0f, 500.0f }, "DirectSound source at +z (in front): centred", 0.70710678f, 0.70710678f },
		{ { 0.0f, 0.0f, -500.0f },
		  "DirectSound source at -z (behind): centred, rear gain",
		  0.70710678f * 0.7f,
		  0.70710678f * 0.7f },
	};

	IDirectSound*       device = create_ds_device();
	IDirectSoundBuffer* primary;
	TestDs3dListener*   listener = open_ds_listener(device, &primary);
	const float         level    = CLIP_LEVEL * expected_attenuation(500.0f, 100.0f, 1e9f, 1.0f);
	char                what[160];

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		IDirectSoundBuffer* buffer = make_ds_buffer(
			device, DSBCAPS_STATIC | DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME, 4800);
		TestDs3dBuffer* buffer3d = get_ds3d_buffer(buffer);
		buffer3d->lpVtbl->SetMinDistance(buffer3d, 100.0f, DS3D_IMMEDIATE);
		buffer3d->lpVtbl->SetPosition(buffer3d, cases[i].pos[0], cases[i].pos[1], cases[i].pos[2],
									  DS3D_IMMEDIATE);
		buffer->lpVtbl->Play(buffer, 0, 0, DSBPLAY_LOOPING);
		render(256, 256);
		snprintf(what, sizeof what, "%s (got %.4f, %.4f)", cases[i].name, left_at(255), right_at(255));
		check(close_to(left_at(255), level * cases[i].left_scale) &&
				  close_to(right_at(255), level * cases[i].right_scale),
			  what);
		buffer->lpVtbl->Stop(buffer);
		buffer3d->lpVtbl->Release(buffer3d);
		buffer->lpVtbl->Release(buffer);
	}

	listener->lpVtbl->Release(listener);
	primary->lpVtbl->Release(primary);
	device->lpVtbl->Release(device);
}

static void check_cursors(IDirectSoundBuffer* buffer, uint32_t play_expected, uint32_t write_expected,
						  const char* when) {
	uint32_t play  = 0xffffffffu;
	uint32_t write = 0xffffffffu;
	char     what[160];
	buffer->lpVtbl->GetCurrentPosition(buffer, &play, &write);
	snprintf(what, sizeof what, "%s: play %u, write %u (got %u, %u)", when, play_expected, write_expected,
			 play, write);
	check(play == play_expected && write == write_expected, what);
}

static void check_status(IDirectSoundBuffer* buffer, uint32_t expected, const char* when) {
	uint32_t status = 0xffffffffu;
	char     what[160];
	buffer->lpVtbl->GetStatus(buffer, &status);
	snprintf(what, sizeof what, "%s: status %u (got %u)", when, expected, status);
	check(status == expected, what);
}

/* A 2D looping buffer at the mixer's rate, so one source frame is mixed per
 * output frame and the cursors are exact. The cursors are in bytes: two per
 * frame. While the buffer plays, the write cursor is 3 frames (6 bytes) past
 * the position being mixed, since the resampler reads two frames ahead. The
 * play cursor counts what has been heard, so it is the limiter's delay
 * (`lag` bytes) behind that position. */
static void test_shim_cursors(void) {
	enum { FRAMES = 48000, BYTES = FRAMES * 2 };

	IDirectSound*       device = create_ds_device();
	IDirectSoundBuffer* buffer =
		make_ds_buffer(device, DSBCAPS_STATIC | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME, FRAMES);
	const uint32_t lag = (uint32_t)g_delay * 2;
	char           what[160];

	check_cursors(buffer, 0, 0, "new buffer");
	buffer->lpVtbl->Play(buffer, 0, 0, DSBPLAY_LOOPING);
	check_cursors(buffer, 0, 6, "after Play");
	check_status(buffer, DSBSTATUS_PLAYING | DSBSTATUS_LOOPING, "after Play");
	render(1000, 1000);
	check_cursors(buffer, 2000 - lag, 2006, "after 1000 frames");

	buffer->lpVtbl->SetCurrentPosition(buffer, 4000);
	check_cursors(buffer, 4000, 4006, "after SetCurrentPosition(4000)");
	render(500, 128);
	check_cursors(buffer, 5000 - lag, 5006, "500 more frames, mixed in chunks of 128");

	buffer->lpVtbl->Stop(buffer);
	check_cursors(buffer, 5000, 5000, "after Stop");
	check_status(buffer, 0, "after Stop");
	render(100, 100);
	check_cursors(buffer, 5000, 5000, "100 frames after Stop");

	buffer->lpVtbl->Play(buffer, 0, 0, DSBPLAY_LOOPING);
	render(100, 100);
	check_cursors(buffer, 5200 - lag, 5206, "100 frames after resuming");

	buffer->lpVtbl->SetCurrentPosition(buffer, BYTES - 400);
	render(300, 300);
	check_cursors(buffer, 200 - lag, 206, "300 frames from 200 frames before the end: wrapped");
	buffer->lpVtbl->SetCurrentPosition(buffer, BYTES - 2);
	check_cursors(buffer, BYTES - 2, 4, "at the last frame: the write cursor wraps to frame 2");

	/* Not looping: the voice ends after 200 frames, which are heard from the
	   limiter's delay on. */
	buffer->lpVtbl->Stop(buffer);
	buffer->lpVtbl->SetCurrentPosition(buffer, BYTES - 400);
	buffer->lpVtbl->Play(buffer, 0, 0, 0);
	check_status(buffer, DSBSTATUS_PLAYING, "after Play without looping");
	render(400, 400);
	snprintf(what, sizeof what, "the last frame plays and the next is silent (got %.4f, %.4f)",
			 left_at(g_delay + 199), left_at(g_delay + 200));
	check(close_to(left_at(g_delay + 199), CLIP_LEVEL) && close_to(left_at(g_delay + 200), 0.0f), what);
	check_status(buffer, 0, "after the end without looping");
	check_cursors(buffer, 0, 0, "after the end without looping");

	buffer->lpVtbl->Release(buffer);
	device->lpVtbl->Release(device);
}

/* --- range fade and batches ---------------------------------------------- */

/* A muted source with min 100 and max 400 fades from fade_start to silence at
 * max. Straight ahead, so both channels get cos(45°) of the gain. */
static void test_range_fade(void) {
	static const struct {
		float distance;
		int   mute;
		float fade_start;
		float fade; /* the expected fade factor */
	} cases[] = {
		{ 200.0f, 1, 300.0f, 1.0f },  { 300.0f, 1, 300.0f, 1.0f }, { 350.0f, 1, 300.0f, 0.5f },
		{ 399.0f, 1, 300.0f, 0.01f }, { 400.0f, 1, 300.0f, 0.0f }, { 500.0f, 1, 300.0f, 0.0f },
		{ 399.0f, 1, 0.0f, 1.0f },    { 401.0f, 1, 0.0f, 0.0f },   { 500.0f, 0, 300.0f, 1.0f },
	};

	AeronClip clip = make_constant_clip(4800);
	char      what[160];

	set_mixer_listener();
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		const float        pos[3] = { 0.0f, 0.0f, -cases[i].distance };
		AeronAudioSource3D source = make_source(pos, 100.0f, 400.0f);
		source.mute_beyond_max    = cases[i].mute;
		source.fade_start         = cases[i].fade_start;
		const float expected = CLIP_LEVEL * sqrtf(0.5f) *
							   expected_attenuation(cases[i].distance, 100.0f, 400.0f, 1.0f) * cases[i].fade;
		float       left, right;
		measure_source(clip, &source, &left, &right);
		snprintf(what, sizeof what, "%s, fade from %.0f, at %.0f: fade %.2f, level %.5f (got %.5f, %.5f)",
				 cases[i].mute ? "muted past 400" : "not muted", cases[i].fade_start, cases[i].distance,
				 cases[i].fade, expected, left, right);
		check(close_to(left, expected) && close_to(right, expected), what);
	}

	/* A muted voice keeps playing, and is heard again when it comes back. */
	{
		const float        far_pos[3]  = { 0.0f, 0.0f, -500.0f };
		const float        near_pos[3] = { 0.0f, 0.0f, -200.0f };
		AeronAudioSource3D source      = make_source(far_pos, 100.0f, 400.0f);
		source.mute_beyond_max         = 1;
		source.fade_start              = 300.0f;
		AeronVoice voice               = play_source(clip, 1.0f, &source);
		render(256, 256);
		check(close_to(left_at(255), 0.0f) && Aeron_AudioVoiceIsPlaying(voice),
			  "a voice past its max distance is silent and still playing");
		memcpy(source.pos, near_pos, sizeof source.pos);
		Aeron_AudioVoiceSet3DSource(voice, &source);
		render(256, 256);
		const float expected = CLIP_LEVEL * sqrtf(0.5f) * expected_attenuation(200.0f, 100.0f, 400.0f, 1.0f);
		snprintf(what, sizeof what, "moved back to 200, it is heard again at %.4f (got %.4f)", expected,
				 left_at(255));
		check(close_to(left_at(255), expected), what);
		Aeron_AudioVoiceStop(voice);
	}
	Aeron_AudioClipDestroy(clip);
}

/* The listener faces -z and the source is at its right. Inside a batch the
 * source moves in front (-z) and the listener turns to face +x, which puts
 * the source on its left. Either change alone would centre the source. No
 * chunk mixed inside the batch may use either change. */
static void test_batch(void) {
	const float        right_pos[3] = { 500.0f, 0.0f, 0.0f };
	const float        front_pos[3] = { 0.0f, 0.0f, -500.0f };
	const float        level        = CLIP_LEVEL * expected_attenuation(500.0f, 100.0f, 1000.0f, 1.0f);
	AeronClip          clip         = make_constant_clip(4800);
	AeronAudioSource3D source       = make_source(right_pos, 100.0f, 1000.0f);
	AeronAudioListener turned;
	char               what[160];

	set_mixer_listener();
	AeronVoice voice = play_source(clip, 1.0f, &source);
	render(256, 256);
	check(close_to(left_at(255), 0.0f) && close_to(right_at(255), level), "before the batch: right only");

	memset(&turned, 0, sizeof turned);
	turned.front[0] = 1.0f;
	turned.top[1]   = 1.0f;
	memcpy(source.pos, front_pos, sizeof source.pos);

	Aeron_Audio3DBeginBatch();
	Aeron_AudioVoiceSet3DSource(voice, &source);
	render(256, 256);
	snprintf(what, sizeof what, "the source moved inside the batch: still right only (got %.4f, %.4f)",
			 left_at(255), right_at(255));
	check(close_to(left_at(255), 0.0f) && close_to(right_at(255), level), what);
	Aeron_Audio3DBeginBatch();
	Aeron_AudioSetListener(&turned);
	Aeron_Audio3DEndBatch();
	render(256, 256);
	snprintf(what, sizeof what,
			 "the listener turned inside a nested batch, which ended: still right only (got %.4f, %.4f)",
			 left_at(255), right_at(255));
	check(close_to(left_at(255), 0.0f) && close_to(right_at(255), level), what);
	Aeron_Audio3DEndBatch();
	render(256, 256);
	snprintf(what, sizeof what, "after the outer batch ends: left only (got %.4f, %.4f)", left_at(255),
			 right_at(255));
	check(close_to(left_at(255), level) && close_to(right_at(255), 0.0f), what);

	Aeron_AudioVoiceStop(voice);
	Aeron_AudioClipDestroy(clip);
}

/* The game's positional buffers have DSBCAPS_MUTE3DATMAXDISTANCE. The shim
 * gives them a fade over the last quarter of their max distance, and the
 * duplicates the game plays keep it. */
static void test_shim_range_fade(void) {
	IDirectSound*       device = create_ds_device();
	IDirectSoundBuffer* primary;
	TestDs3dListener*   listener = open_ds_listener(device, &primary);
	IDirectSoundBuffer* muted    = make_ds_buffer(device, DS_GAME_3D_FLAGS, 4800);
	IDirectSoundBuffer* plain =
		make_ds_buffer(device, DS_GAME_3D_FLAGS & ~(uint32_t)DSBCAPS_MUTE3DATMAXDISTANCE, 4800);
	IDirectSoundBuffer* duplicate = NULL;
	TestDs3dBuffer*     muted3d   = get_ds3d_buffer(muted);
	TestDs3dBuffer*     plain3d   = get_ds3d_buffer(plain);
	const float         at350 = CLIP_LEVEL * sqrtf(0.5f) * expected_attenuation(350.0f, 100.0f, 400.0f, 1.0f);
	char                what[160];

	muted3d->lpVtbl->SetMinDistance(muted3d, 100.0f, DS3D_IMMEDIATE);
	muted3d->lpVtbl->SetMaxDistance(muted3d, 400.0f, DS3D_IMMEDIATE);
	muted3d->lpVtbl->SetPosition(muted3d, 0.0f, 0.0f, 350.0f, DS3D_IMMEDIATE);
	muted->lpVtbl->Play(muted, 0, 0, DSBPLAY_LOOPING);
	render(256, 256);
	snprintf(what, sizeof what, "muted buffer at 350 of 400: half the distance gain, %.5f (got %.5f)",
			 at350 * 0.5f, left_at(255));
	check(close_to(left_at(255), at350 * 0.5f) && close_to(right_at(255), at350 * 0.5f), what);
	muted->lpVtbl->Stop(muted);

	if (device->lpVtbl->DuplicateSoundBuffer(device, muted, &duplicate) < 0 || !duplicate) {
		printf("FAILED could not duplicate a DirectSound buffer\n");
		exit(1);
	}
	duplicate->lpVtbl->Play(duplicate, 0, 0, DSBPLAY_LOOPING);
	render(256, 256);
	snprintf(what, sizeof what, "its duplicate fades the same way (got %.5f)", left_at(255));
	check(close_to(left_at(255), at350 * 0.5f), what);
	{
		TestDs3dBuffer* duplicate3d = get_ds3d_buffer(duplicate);
		duplicate3d->lpVtbl->SetPosition(duplicate3d, 0.0f, 0.0f, 500.0f, DS3D_IMMEDIATE);
		duplicate3d->lpVtbl->Release(duplicate3d);
	}
	render(256, 256);
	check(close_to(left_at(255), 0.0f), "the duplicate moved past its max distance: silent");
	check_status(duplicate, DSBSTATUS_PLAYING | DSBSTATUS_LOOPING, "the silent duplicate");
	duplicate->lpVtbl->Stop(duplicate);

	plain3d->lpVtbl->SetMinDistance(plain3d, 100.0f, DS3D_IMMEDIATE);
	plain3d->lpVtbl->SetMaxDistance(plain3d, 400.0f, DS3D_IMMEDIATE);
	plain3d->lpVtbl->SetPosition(plain3d, 0.0f, 0.0f, 500.0f, DS3D_IMMEDIATE);
	plain->lpVtbl->Play(plain, 0, 0, DSBPLAY_LOOPING);
	render(256, 256);
	{
		const float expected = CLIP_LEVEL * sqrtf(0.5f) * expected_attenuation(500.0f, 100.0f, 400.0f, 1.0f);
		snprintf(what, sizeof what, "a buffer without the mute flag at 500: clamped at 400, %.5f (got %.5f)",
				 expected, left_at(255));
		check(close_to(left_at(255), expected), what);
	}
	plain->lpVtbl->Stop(plain);

	muted3d->lpVtbl->Release(muted3d);
	plain3d->lpVtbl->Release(plain3d);
	duplicate->lpVtbl->Release(duplicate);
	muted->lpVtbl->Release(muted);
	plain->lpVtbl->Release(plain);
	listener->lpVtbl->Release(listener);
	primary->lpVtbl->Release(primary);
	device->lpVtbl->Release(device);
}

/* The game sets every position and the listener with DS3D_DEFERRED, then
 * commits. Nothing reaches the mixer before the commit, and everything
 * reaches it together. The cases are those of test_batch, in DirectSound's
 * coordinates: the listener faces +z, and turns to face +x, which puts +z on
 * its left. */
static void test_shim_deferred(void) {
	IDirectSound*       device = create_ds_device();
	IDirectSoundBuffer* primary;
	TestDs3dListener*   listener = open_ds_listener(device, &primary);
	IDirectSoundBuffer* buffer   = make_ds_buffer(device, DS_GAME_3D_FLAGS, 4800);
	TestDs3dBuffer*     buffer3d = get_ds3d_buffer(buffer);
	const float         level    = CLIP_LEVEL * expected_attenuation(500.0f, 100.0f, 1000.0f, 1.0f);
	char                what[160];

	buffer3d->lpVtbl->SetMinDistance(buffer3d, 100.0f, DS3D_IMMEDIATE);
	buffer3d->lpVtbl->SetMaxDistance(buffer3d, 1000.0f, DS3D_IMMEDIATE);
	buffer3d->lpVtbl->SetPosition(buffer3d, 500.0f, 0.0f, 0.0f, DS3D_IMMEDIATE);
	buffer->lpVtbl->Play(buffer, 0, 0, DSBPLAY_LOOPING);
	render(256, 256);
	check(close_to(left_at(255), 0.0f) && close_to(right_at(255), level),
		  "immediate position at +x: right only");

	buffer3d->lpVtbl->SetPosition(buffer3d, 0.0f, 0.0f, 500.0f, DS3D_DEFERRED);
	render(256, 256);
	snprintf(what, sizeof what, "deferred move to +z: still right only (got %.4f, %.4f)", left_at(255),
			 right_at(255));
	check(close_to(left_at(255), 0.0f) && close_to(right_at(255), level), what);
	listener->lpVtbl->SetOrientation(listener, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, DS3D_DEFERRED);
	render(256, 256);
	snprintf(what, sizeof what, "deferred turn of the listener: still right only (got %.4f, %.4f)",
			 left_at(255), right_at(255));
	check(close_to(left_at(255), 0.0f) && close_to(right_at(255), level), what);
	/* Buffers released with deferred settings leave the commit's list. */
	for (int i = 0; i < 3; ++i) {
		IDirectSoundBuffer* other   = make_ds_buffer(device, DS_GAME_3D_FLAGS, 4800);
		TestDs3dBuffer*     other3d = get_ds3d_buffer(other);
		other3d->lpVtbl->SetPosition(other3d, 1.0f, 2.0f, 3.0f, DS3D_DEFERRED);
		other3d->lpVtbl->Release(other3d);
		if (i != 1) {
			other->lpVtbl->Release(other);
		}
	}
	listener->lpVtbl->CommitDeferredSettings(listener);
	render(256, 256);
	snprintf(what, sizeof what, "after the commit: left only (got %.4f, %.4f)", left_at(255), right_at(255));
	check(close_to(left_at(255), level) && close_to(right_at(255), 0.0f), what);
	listener->lpVtbl->CommitDeferredSettings(listener);
	render(256, 256);
	check(close_to(left_at(255), level) && close_to(right_at(255), 0.0f), "a second commit with nothing new");

	buffer->lpVtbl->Stop(buffer);
	buffer3d->lpVtbl->Release(buffer3d);
	buffer->lpVtbl->Release(buffer);
	listener->lpVtbl->Release(listener);
	primary->lpVtbl->Release(primary);
	device->lpVtbl->Release(device);
}

/* --- gain ramps ----------------------------------------------------------- */

enum { RAMP_FRAMES = 64 };

/* A margin of two steps of 1/32768 between neighbouring samples, for the
 * float rounding of the gains. */
#define STEP_TOLERANCE (2.0f / 32768.0f)

static void render_into(float* out, int frames, int chunk_frames) {
	if (!Aeron_AudioRenderHeadless(out, frames, chunk_frames)) {
		printf("FAILED could not render %d frames\n", frames);
		exit(1);
	}
}

/* The largest change between neighbouring samples of one channel of `out`,
 * over `frames` frames. */
static float max_sample_step(const float* out, int channel, int frames) {
	float largest = 0.0f;
	for (int i = 1; i < frames; ++i) {
		const float step = fabsf(out[i * 2 + channel] - out[(i - 1) * 2 + channel]);
		if (step > largest) {
			largest = step;
		}
	}
	return largest;
}

/* A source that jumps from the left to the right moves over 64 frames, in
 * equal steps, instead of at once. */
static void test_ramp_jump(void) {
	const float        left_pos[3]  = { -500.0f, 0.0f, 0.0f };
	const float        right_pos[3] = { 500.0f, 0.0f, 0.0f };
	const float        level        = CLIP_LEVEL * expected_attenuation(500.0f, 100.0f, 1000.0f, 1.0f);
	AeronClip          clip         = make_constant_clip(4800);
	AeronAudioSource3D source       = make_source(left_pos, 100.0f, 1000.0f);
	char               what[192];

	/* The ramp starts with the second render's first mixed frame, which is
	   heard g_delay frames into it. */
	const int start = g_delay;

	set_mixer_listener();
	AeronVoice voice = play_source(clip, 1.0f, &source);
	render(256, 256);
	check(close_to(left_at(start), level) && close_to(right_at(start), 0.0f),
		  "a new voice starts at its level, without a ramp");

	/* The step into the first new frame counts too. */
	const float last_left  = left_at(255);
	const float last_right = right_at(255);
	memcpy(source.pos, right_pos, sizeof source.pos);
	Aeron_AudioVoiceSet3DSource(voice, &source);
	render(256, 256);
	const float largest = fmaxf(fmaxf(max_sample_step(g_out, 0, 256), max_sample_step(g_out, 1, 256)),
								fmaxf(fabsf(left_at(0) - last_left), fabsf(right_at(0) - last_right)));
	snprintf(what, sizeof what,
			 "jump from -90 to +90 degrees: no step above %.5f between neighbouring samples (largest %.5f)",
			 level / RAMP_FRAMES, largest);
	check(largest <= level / RAMP_FRAMES + STEP_TOLERANCE, what);
	snprintf(what, sizeof what, "halfway through the ramp, both channels at %.4f (got %.4f, %.4f)", level / 2,
			 left_at(start + RAMP_FRAMES / 2), right_at(start + RAMP_FRAMES / 2));
	check(close_to(left_at(start + RAMP_FRAMES / 2), level / 2) &&
			  close_to(right_at(start + RAMP_FRAMES / 2), level / 2),
		  what);
	snprintf(what, sizeof what, "after 64 frames, right only (got %.4f, %.4f)", left_at(start + RAMP_FRAMES),
			 right_at(start + RAMP_FRAMES));
	check(close_to(left_at(start + RAMP_FRAMES), 0.0f) && close_to(right_at(start + RAMP_FRAMES), level),
		  what);

	Aeron_AudioVoiceStop(voice);
	Aeron_AudioClipDestroy(clip);
}

/* A muted source moves out through its fade and past its max distance, one
 * position per 64-frame chunk, then jumps out of range and stays there for
 * one more chunk, so its last ramp can end. Each change of level spreads over
 * 64 frames, so no step between neighbouring samples is larger than the
 * largest change divided by 64. */
static void test_ramp_range_fade(void) {
	static const float distances[] = { 340.0f, 350.0f, 360.0f, 370.0f, 380.0f, 450.0f, 450.0f };

	enum { COUNT = sizeof distances / sizeof distances[0] };

	static float out[COUNT * RAMP_FRAMES * 2];
	AeronClip    clip     = make_constant_clip(4800);
	float        bound    = 0.0f;
	float        previous = 0.0f;
	char         what[192];
	AeronVoice   voice = 0;

	set_mixer_listener();
	for (int i = 0; i < COUNT; ++i) {
		const float        pos[3] = { 0.0f, 0.0f, -distances[i] };
		AeronAudioSource3D source = make_source(pos, 100.0f, 400.0f);
		source.mute_beyond_max    = 1;
		source.fade_start         = 300.0f;
		const float fade          = distances[i] >= 400.0f  ? 0.0f
									: distances[i] > 300.0f ? (400.0f - distances[i]) / 100.0f
															: 1.0f;
		const float level =
			CLIP_LEVEL * sqrtf(0.5f) * expected_attenuation(distances[i], 100.0f, 400.0f, 1.0f) * fade;
		if (i == 0) {
			voice = play_source(clip, 1.0f, &source);
		} else {
			Aeron_AudioVoiceSet3DSource(voice, &source);
			if (fabsf(level - previous) > bound) {
				bound = fabsf(level - previous);
			}
		}
		previous = level;
		render_into(out + i * RAMP_FRAMES * 2, RAMP_FRAMES, RAMP_FRAMES);
	}
	bound /= RAMP_FRAMES;
	/* From the voice's first heard frame: before it, the limiter's delay
	   still holds the previous test's sound. */
	const float largest = max_sample_step(out + g_delay * 2, 0, COUNT * RAMP_FRAMES - g_delay);
	snprintf(what, sizeof what, "moving out through the fade and past max: no step above %.6f (largest %.6f)",
			 bound, largest);
	check(largest <= bound + STEP_TOLERANCE, what);
	snprintf(what, sizeof what, "silent at the end (got %.6f)", out[(COUNT * RAMP_FRAMES - 1) * 2]);
	check(close_to(out[(COUNT * RAMP_FRAMES - 1) * 2], 0.0f), what);

	Aeron_AudioVoiceStop(voice);
	Aeron_AudioClipDestroy(clip);
}

/* The output doesn't depend on how the frames are split into chunks: the
 * ramps, the rear filter and the resampling carry over from one chunk to the
 * next. A noise clip at 22,050 Hz starts in front, then moves behind after
 * 100 frames, which starts a ramp and the rear filter. Every split gives the
 * same samples, bit for bit. */
static void test_chunk_independence(void) {
	enum { CLIP_RATE = 22050, FIRST = 100, TOTAL = 3000 };

	static const int chunk_sizes[] = { 1024, 1, 7, 64, 480 };
	static float     reference[TOTAL * 2];
	static float     out[TOTAL * 2];
	int16_t*         pcm  = (int16_t*)malloc(CLIP_RATE * sizeof(int16_t));
	uint32_t         seed = 7;
	char             what[128];

	for (int i = 0; i < CLIP_RATE; ++i) {
		seed   = seed * 1664525u + 1013904223u;
		pcm[i] = (int16_t)(((int)(seed >> 16) - 32768) / 2);
	}
	AeronClip clip = Aeron_AudioClipCreate(pcm, CLIP_RATE, CLIP_RATE, 1, AERON_PCM_S16);
	free(pcm);

	set_mixer_listener();
	for (size_t i = 0; i < sizeof chunk_sizes / sizeof chunk_sizes[0]; ++i) {
		const int          chunk     = chunk_sizes[i];
		const float        front[3]  = { 300.0f, 0.0f, -300.0f };
		const float        behind[3] = { -200.0f, 50.0f, 400.0f };
		float*             target    = i == 0 ? reference : out;
		AeronAudioSource3D source    = make_source(front, 100.0f, 1000.0f);
		/* Every run starts from silence in the limiter's delay. */
		flush_delay();
		AeronVoice voice = play_source(clip, 1.0f, &source);
		render_into(target, FIRST, chunk);
		memcpy(source.pos, behind, sizeof source.pos);
		Aeron_AudioVoiceSet3DSource(voice, &source);
		render_into(target + FIRST * 2, TOTAL - FIRST, chunk);
		Aeron_AudioVoiceStop(voice);
		if (i > 0) {
			snprintf(what, sizeof what, "chunks of %d frames give the same output as chunks of 1024", chunk);
			check(memcmp(reference, out, sizeof out) == 0, what);
		}
	}
	Aeron_AudioClipDestroy(clip);
}

/* --- resampler ------------------------------------------------------------ */

/* The clip stores p(n) truncated to S16, so each frame is off by up to one
 * step of 1/32768. The interpolation weights add up to at most 1.25 in size,
 * so the output can be off by up to 1.25 steps. */
#define SAMPLE_TOLERANCE (1.5f / 32768.0f)

/* The cubic interpolation reproduces a cubic polynomial exactly. A 30-frame
 * clip at 22,050 Hz holds p(n) = 100 (n - 15)^2 + 2 (n - 15)^3, and plays as
 * a centred 2D voice. Between frame 1 and frame 27 of the clip, where all
 * four frames around the position are inside it, the output is p(position).
 * Linear interpolation is off by up to about 25 S16 steps on this curve. */
static void test_resampler_cubic_exact(void) {
	enum { FRAMES = 30, CLIP_RATE = 22050, OUT = 64 };

	int16_t     pcm[FRAMES];
	const float step     = (float)CLIP_RATE / (float)RATE;
	float       largest  = 0.0f;
	int         measured = 0;
	char        what[160];

	for (int n = 0; n < FRAMES; ++n) {
		const float x = (float)(n - 15);
		pcm[n]        = (int16_t)(100.0f * x * x + 2.0f * x * x * x);
	}
	AeronClip  clip  = Aeron_AudioClipCreate(pcm, FRAMES, CLIP_RATE, 1, AERON_PCM_S16);
	AeronVoice voice = Aeron_AudioVoicePlay(clip, 1.0f, 0.0f, 1.0f, 0);
	render(OUT + g_delay, OUT + g_delay);
	for (int k = 0; k < OUT; ++k) {
		const double t = (double)k * step;
		if (t < 1.0 || t > (double)(FRAMES - 3)) {
			continue;
		}
		const double x     = t - 15.0;
		const float  ideal = (float)((100.0 * x * x + 2.0 * x * x * x) / 32768.0);
		const float  error = fmaxf(fabsf(left_at(g_delay + k) - ideal), fabsf(right_at(g_delay + k) - ideal));
		largest            = fmaxf(largest, error);
		++measured;
	}
	snprintf(what, sizeof what, "a cubic plays exactly over %d frames: largest error %.6f (limit %.6f)",
			 measured, largest, SAMPLE_TOLERANCE);
	check(measured > 40 && largest <= SAMPLE_TOLERANCE, what);
	Aeron_AudioVoiceStop(voice);
	Aeron_AudioClipDestroy(clip);
}

/* A 5 kHz sine in a looping 22,050 Hz clip, played to 48 kHz from 400 frames
 * before the loop point. The cubic interpolation is closer to the true sine
 * than linear interpolation of the same samples, which the mixer used before.
 * Its error at the loop point is no larger than elsewhere, so the wrap uses
 * the frames from the start of the clip. */
static void test_resampler_sine(void) {
	enum { CLIP_RATE = 22050, START = CLIP_RATE - 400, OUT = 2048 };

	const double frequency = 5000.0;
	const double amplitude = 16000.0;
	const double step      = (double)CLIP_RATE / (double)RATE;
	int16_t*     pcm       = (int16_t*)malloc(CLIP_RATE * sizeof(int16_t));
	double       cubic_sum = 0.0, linear_sum = 0.0;
	float        largest = 0.0f, largest_at_wrap = 0.0f;
	char         what[192];

	for (int n = 0; n < CLIP_RATE; ++n) {
		pcm[n] = (int16_t)lrint(amplitude * sin(2.0 * 3.14159265358979323846 * frequency * n / CLIP_RATE));
	}
	AeronClip  clip  = Aeron_AudioClipCreate(pcm, CLIP_RATE, CLIP_RATE, 1, AERON_PCM_S16);
	AeronVoice voice = Aeron_AudioVoicePlayFrom(clip, 1.0f, 0.0f, 1.0f, 1, START);
	render(OUT + g_delay, 1024);

	const int wrap = (int)((CLIP_RATE - START) / step);
	for (int k = 0; k < OUT; ++k) {
		const double t = START + k * step;
		const double ideal =
			amplitude * sin(2.0 * 3.14159265358979323846 * frequency * t / CLIP_RATE) / 32768.0;
		const size_t n      = (size_t)t % CLIP_RATE;
		const double mu     = t - floor(t);
		const double linear = (pcm[n] + (pcm[(n + 1) % CLIP_RATE] - pcm[n]) * mu) / 32768.0;
		const double heard  = left_at(g_delay + k);
		const float  error  = (float)fabs(heard - ideal);
		cubic_sum += (heard - ideal) * (heard - ideal);
		linear_sum += (linear - ideal) * (linear - ideal);
		if (k >= wrap - 4 && k <= wrap + 4) {
			largest_at_wrap = fmaxf(largest_at_wrap, error);
		} else {
			largest = fmaxf(largest, error);
		}
	}
	const double cubic_rms  = sqrt(cubic_sum / OUT);
	const double linear_rms = sqrt(linear_sum / OUT);
	snprintf(what, sizeof what, "5 kHz sine: error %.5f, half or less of linear interpolation's %.5f",
			 cubic_rms, linear_rms);
	check(cubic_rms <= 0.5 * linear_rms, what);
	snprintf(what, sizeof what, "across the loop point: largest error %.5f, at most the %.5f elsewhere",
			 largest_at_wrap, largest);
	check(largest_at_wrap <= largest, what);

	Aeron_AudioVoiceStop(voice);
	Aeron_AudioClipDestroy(clip);
	free(pcm);
}

/* --- positional voices mixed outside the mixer mutex --------------------- */

/* A clip of `frames` frames at `rate`, at the constant test level. */
static AeronClip make_rate_clip(int frames, int rate) {
	int16_t* pcm = (int16_t*)malloc((size_t)frames * sizeof(int16_t));
	for (int i = 0; i < frames; ++i) {
		pcm[i] = (int16_t)(CLIP_LEVEL * 32768.0f);
	}
	AeronClip clip = Aeron_AudioClipCreate(pcm, (size_t)frames, rate, 1, AERON_PCM_S16);
	free(pcm);
	return clip;
}

/* Straight ahead of the listener, closer than the min distance: full gain,
 * the same on both channels. */
static AeronAudioSource3D make_front_source(void) {
	const float pos[3] = { 0.0f, 0.0f, -10.0f };
	return make_source(pos, 100.0f, 131072.0f);
}

typedef struct JobStopTest {
	int        done;
	AeronVoice stopped;
	AeronVoice sought;
	AeronVoice started;
	AeronClip  stopped_clip;
	AeronClip  started_clip;
	AeronClip  probe;
	size_t     stopped_at;
	int        sought_ok;
} JobStopTest;

/* Runs once, after the jobs of the first chunk have mixed and before their
 * results go back to the voices. */
static void job_stop_hook(int stage, void* user) {
	JobStopTest* t = (JobStopTest*)user;
	if (stage != AERON_AUDIO_JOB_HOOK_AFTER || t->done) {
		return;
	}
	t->done                         = 1;
	t->stopped_at                   = Aeron_AudioVoiceStopAndGetPosition(t->stopped);
	t->sought_ok                    = Aeron_AudioVoiceSetPosition(t->sought, 100);
	const AeronAudioSource3D source = make_front_source();
	t->started = Aeron_AudioVoicePlay3DSource(t->started_clip, 0.5f, 1.0f, 1, &source, 50);
	Aeron_AudioClipDestroy(t->stopped_clip);
	t->probe = make_constant_clip(100);
}

/* While the jobs of a chunk mix, a voice is stopped, a second one is sought,
 * and a third one starts in the stopped one's slot. None of them gets the
 * state of another job, and the stopped voice's clip stays allocated until
 * its job ends. */
static void test_job_stop(void) {
	JobStopTest t;
	char        what[192];

	memset(&t, 0, sizeof t);
	set_mixer_listener();
	/* 300 frames at 22,050 Hz: the job's position wraps around, at a step
	   that isn't a whole number. */
	t.stopped_clip                  = make_rate_clip(300, 22050);
	AeronClip twin                  = make_rate_clip(300, 22050);
	AeronClip sought                = make_constant_clip(4800);
	t.started_clip                  = make_constant_clip(4800);
	const AeronAudioSource3D source = make_front_source();
	/* Low gains keep the sum below full scale, so the limiter doesn't act
	   on the next tests. */
	t.stopped             = play_source(t.stopped_clip, 0.25f, &source);
	t.sought              = play_source(sought, 0.25f, &source);
	AeronVoice twin_voice = play_source(twin, 0.25f, &source);

	Aeron_AudioSetJobHook(job_stop_hook, &t);
	render(1024, 1024);
	Aeron_AudioSetJobHook(NULL, NULL);

	const size_t twin_at = Aeron_AudioVoiceStopAndGetPosition(twin_voice);
	snprintf(what, sizeof what,
			 "stopping a voice during its job returns the position the job reaches (%zu, its twin %zu)",
			 t.stopped_at, twin_at);
	check(t.done && t.stopped_at == twin_at && twin_at != 0, what);
	const size_t sought_at = Aeron_AudioVoiceStopAndGetPosition(t.sought);
	snprintf(what, sizeof what, "a seek during the job wins over the job's position (%zu, expected 100)",
			 sought_at);
	check(t.sought_ok && sought_at == 100, what);
	check(t.started && (uint16_t)t.started == (uint16_t)t.stopped,
		  "the voice started during the job takes the stopped voice's slot");
	check(t.probe && (uint16_t)t.probe != (uint16_t)t.stopped_clip,
		  "the stopped voice's clip stays allocated until its job ends");

	/* Only the new voice plays now. Its first frame comes out after the
	   limiter's delay, at its own level: a new voice starts without a ramp.
	   With the stopped voice's ramps, it would start at that one's level. */
	render(1024, 1024);
	const size_t started_at = Aeron_AudioVoiceStopAndGetPosition(t.started);
	const float  expected   = CLIP_LEVEL * 0.5f * 0.70710678f;
	snprintf(what, sizeof what,
			 "the new voice in that slot keeps its own state: it starts at its level (%.4f, expected %.4f) "
			 "from frame 50 (at %zu, expected 1074)",
			 left_at(g_delay), expected, started_at);
	check(close_to(left_at(g_delay), expected) && started_at == 1074, what);

	Aeron_AudioClipDestroy(t.probe);
	Aeron_AudioClipDestroy(twin);
	Aeron_AudioClipDestroy(sought);
	Aeron_AudioClipDestroy(t.started_clip);
	flush_delay();
}

typedef struct JobWriteTest {
	AeronClip     clip;
	int16_t*      pcm; /* the new PCM */
	size_t        bytes;
	SDL_Thread*   writer;
	SDL_AtomicInt returned;
	int           write_ok;
	int           waited;
	int           returned_before_end;
	int           stage_done[2];
} JobWriteTest;

static int SDLCALL job_writer(void* user) {
	JobWriteTest*       t     = (JobWriteTest*)user;
	const AeronPcmWrite write = { 0, t->pcm, t->bytes };
	t->write_ok               = Aeron_AudioClipWrite(t->clip, &write, 1);
	SDL_SetAtomicInt(&t->returned, 1);
	return 0;
}

/* Before the jobs of the first chunk mix, a second thread writes to the
 * clip. The hook returns once the write is waiting. After the jobs, it
 * records whether the write has returned yet. */
static void job_write_hook(int stage, void* user) {
	JobWriteTest* t = (JobWriteTest*)user;
	if (t->stage_done[stage]) {
		return;
	}
	t->stage_done[stage] = 1;
	if (stage == AERON_AUDIO_JOB_HOOK_BEFORE) {
		t->writer = SDL_CreateThread(job_writer, "clip writer", t);
		for (int ms = 0; t->writer && ms < 2000 && !t->waited; ++ms) {
			t->waited = Aeron_AudioClipWritersWaiting() == 1;
			if (!t->waited) {
				SDL_Delay(1);
			}
		}
	} else {
		t->returned_before_end = SDL_GetAtomicInt(&t->returned);
	}
}

/* A clip write that starts while a job mixes the clip waits until the job
 * has ended. The chunk being mixed reads only the old PCM, and the next one
 * the new PCM. */
static void test_job_clip_write(void) {
	enum { FRAMES = 4800 };

	JobWriteTest t;
	char         what[192];

	memset(&t, 0, sizeof t);
	set_mixer_listener();
	int16_t* old_pcm = (int16_t*)malloc(FRAMES * sizeof(int16_t));
	t.pcm            = (int16_t*)malloc(FRAMES * sizeof(int16_t));
	t.bytes          = FRAMES * sizeof(int16_t);
	for (int i = 0; i < FRAMES; ++i) {
		old_pcm[i] = (int16_t)(0.25f * 32768.0f);
		t.pcm[i]   = (int16_t)(0.5f * 32768.0f);
	}
	t.clip                                 = Aeron_AudioClipCreateMutable(FRAMES, RATE, 1, AERON_PCM_S16);
	const AeronPcmWrite      first_write   = { 0, old_pcm, t.bytes };
	const int                first_written = Aeron_AudioClipWrite(t.clip, &first_write, 1);
	const AeronAudioSource3D source        = make_front_source();
	AeronVoice               voice         = play_source(t.clip, 1.0f, &source);
	/* Straight ahead, equal-power panning gives each channel cos(45°). */
	const float side = 0.70710678f;

	Aeron_AudioSetJobHook(job_write_hook, &t);
	render(1024, 1024);
	Aeron_AudioSetJobHook(NULL, NULL);
	if (t.writer) {
		SDL_WaitThread(t.writer, NULL);
	}

	check(first_written && t.waited, "a clip write that starts during a job waits for it");
	check(t.stage_done[1] && !t.returned_before_end, "the write returns only after the job has ended");
	snprintf(what, sizeof what, "the chunk being mixed reads only the old PCM (%.4f, expected %.4f)",
			 left_at(600), 0.25f * side);
	check(close_to(left_at(600), 0.25f * side), what);
	render(1024, 1024);
	snprintf(what, sizeof what, "the next chunk reads the new PCM (%.4f, expected %.4f)", left_at(600),
			 0.5f * side);
	check(t.write_ok && close_to(left_at(600), 0.5f * side), what);

	Aeron_AudioVoiceStop(voice);
	Aeron_AudioClipDestroy(t.clip);
	free(old_pcm);
	free(t.pcm);
	flush_delay();
}

/* --- output and limiter --------------------------------------------------- */

/* The output can have 6 channels. A 2D voice plays on the first two, front
 * left and front right. The count can't change while a voice exists. */
static void test_output_channels(void) {
	static float out[256 * 8];
	AeronClip    clip = make_constant_clip(4800);
	char         what[192];

	check(Aeron_AudioOutputChannels() == 2, "the output starts with 2 channels");
	check(!Aeron_AudioSetOutputChannels(4) && Aeron_AudioOutputChannels() == 2, "4 channels are refused");
	check(Aeron_AudioSetOutputChannels(6) && Aeron_AudioOutputChannels() == 6,
		  "the output switches to 6 channels while nothing plays");

	AeronVoice voice = Aeron_AudioVoicePlay(clip, 1.0f, 0.0f, 1.0f, 1);
	render_into(out, 256, 256);
	const float* frame  = out + 200 * 6;
	int          others = 1;
	for (int c = 2; c < 6; ++c) {
		others = others && frame[c] == 0.0f;
	}
	snprintf(what, sizeof what,
			 "a 2D voice plays on front left and right only (got %.4f %.4f %.4f %.4f %.4f %.4f)", frame[0],
			 frame[1], frame[2], frame[3], frame[4], frame[5]);
	check(close_to(frame[0], CLIP_LEVEL) && close_to(frame[1], CLIP_LEVEL) && others, what);
	check(!Aeron_AudioSetOutputChannels(8) && Aeron_AudioOutputChannels() == 6,
		  "the count can't change while a voice exists");

	Aeron_AudioVoiceStop(voice);
	check(Aeron_AudioSetOutputChannels(2) && Aeron_AudioOutputChannels() == 2,
		  "back to 2 channels once the voice is stopped");
	Aeron_AudioClipDestroy(clip);
}

/* An impulse comes out exactly the limiter's lookahead later, unchanged:
 * below full scale, the limiter only delays. */
static void test_limiter_delay(void) {
	int16_t pcm[100];
	int     first = -1;
	char    what[160];

	memset(pcm, 0, sizeof pcm);
	pcm[0]         = (int16_t)(CLIP_LEVEL * 32768.0f);
	AeronClip clip = Aeron_AudioClipCreate(pcm, 100, RATE, 1, AERON_PCM_S16);
	flush_delay();
	AeronVoice voice = Aeron_AudioVoicePlay(clip, 1.0f, 0.0f, 1.0f, 0);
	render(2 * g_delay, 2 * g_delay);
	for (int k = 0; k < 2 * g_delay && first < 0; ++k) {
		if (fabsf(left_at(k)) > 1e-6f) {
			first = k;
		}
	}
	check(g_delay == 48, "the limiter delays the output by 48 frames (1 ms)");
	snprintf(what, sizeof what, "an impulse comes out at frame %d, unchanged (got frame %d, %.4f)", g_delay,
			 first, first >= 0 ? left_at(first) : 0.0f);
	check(first == g_delay && close_to(left_at(first), CLIP_LEVEL), what);
	Aeron_AudioVoiceStop(voice);
	Aeron_AudioClipDestroy(clip);
}

/* A full-scale 1 kHz sine played at twice its gain (+6 dB) for 2 seconds.
 * No sample leaves [-1, 1]. Once the limiter has settled, it holds a gain of
 * 1/2, so the second second is the clean sine at full scale, delayed, and
 * not a clipped one with flat tops. The final clamp alone would keep the
 * samples in range, but it would clip.
 * This runs last: the limiter then takes about a second to recover. */
static void test_limiter_peak(void) {
	enum { SECONDS = 2, PIECE = 4000 };

	int16_t* pcm        = (int16_t*)malloc(RATE * sizeof(int16_t));
	float    largest    = 0.0f;
	float    late_error = 0.0f;
	char     what[160];

	for (int n = 0; n < RATE; ++n) {
		pcm[n] = (int16_t)lrint(32767.0 * sin(2.0 * 3.14159265358979323846 * 1000.0 * n / RATE));
	}
	/* Two neighbouring samples at full scale are a flat top: clipping. The
	   samples next to a peak of a clean 1 kHz sine are at 0.991. */
	int   flat_tops = 0;
	float previous  = 0.0f;

	AeronClip  clip  = Aeron_AudioClipCreate(pcm, RATE, RATE, 1, AERON_PCM_S16);
	AeronVoice voice = Aeron_AudioVoicePlay(clip, 2.0f, 0.0f, 1.0f, 1);
	for (int done = 0; done < SECONDS * RATE; done += PIECE) {
		render(PIECE, 1024);
		for (int k = 0; k < PIECE; ++k) {
			largest = fmaxf(largest, fmaxf(fabsf(left_at(k)), fabsf(right_at(k))));
			flat_tops += fabsf(left_at(k)) >= 0.9999f && fabsf(previous) >= 0.9999f;
			previous = left_at(k);
			if (done >= (SECONDS * RATE) / 2) {
				/* Output frame done + k was mixed g_delay frames earlier, from
				   the clip frame of the same number, at a step of 1. */
				const float clean = (float)pcm[(done + k - g_delay) % RATE] / 32768.0f;
				late_error        = fmaxf(late_error, fabsf(left_at(k) - clean));
			}
		}
	}
	snprintf(what, sizeof what, "a +6 dB sine never leaves [-1, 1] (largest %.6f)", largest);
	check(largest <= 1.0f, what);
	snprintf(what, sizeof what, "no flat tops from the first peak on: not clipped (%d found)", flat_tops);
	check(flat_tops == 0, what);
	snprintf(what, sizeof what,
			 "in the second second, it is the clean sine at a gain of 1/2 (largest error %.5f)", late_error);
	check(late_error <= 0.01f, what);
	Aeron_AudioVoiceStop(voice);
	Aeron_AudioClipDestroy(clip);
	free(pcm);
}

/* --- benchmark ----------------------------------------------------------- */

/* A second thread that calls Aeron_AudioVoiceIsPlaying over and over, as the
 * game thread calls GetStatus, and records how long each call takes, in
 * microseconds. The last bucket counts the calls of 1 ms or more. */
enum { PROBE_BUCKETS = 1001 };

typedef struct WaitProbe {
	SDL_AtomicInt stop;
	AeronVoice    voice;
	uint64_t      calls;
	uint64_t      total_ticks;
	uint64_t      max_ticks;
	uint64_t      buckets[PROBE_BUCKETS];
} WaitProbe;

static int SDLCALL wait_probe(void* user) {
	WaitProbe* p = (WaitProbe*)user;
	while (!SDL_GetAtomicInt(&p->stop)) {
		const uint64_t start = SDL_GetPerformanceCounter();
		(void)Aeron_AudioVoiceIsPlaying(p->voice);
		const uint64_t ticks = SDL_GetPerformanceCounter() - start;
		p->total_ticks += ticks;
		p->max_ticks      = ticks > p->max_ticks ? ticks : p->max_ticks;
		const uint64_t us = ticks * 1000000u / SDL_GetPerformanceFrequency();
		++p->buckets[us < PROBE_BUCKETS - 1 ? us : PROBE_BUCKETS - 1];
		++p->calls;
	}
	return 0;
}

/* 32 positional voices of a 22,050 Hz noise clip, circling the listener at
 * different distances. The time spent mixing is compared with the duration of
 * the audio. Positions change once per chunk, as they would with per-frame
 * updates from the game. */
/* Moves the voices a little along their circles, in one batch, as the game
 * commits its updates once per frame. */
static void bench_move_voices(const AeronVoice* voices, const float* radius, float* angle, int count) {
	Aeron_Audio3DBeginBatch();
	for (int v = 0; v < count; ++v) {
		angle[v] += 0.02f;
		const float              pos[3] = { radius[v] * sinf(angle[v]), 0.0f, -radius[v] * cosf(angle[v]) };
		const AeronAudioSource3D source = make_source(pos, 100.0f, 131072.0f);
		Aeron_AudioVoiceSet3DSource(voices[v], &source);
	}
	Aeron_Audio3DEndBatch();
}

static int run_bench(void) {
	enum {
		VOICES         = 32,
		CLIP_RATE      = 22050,
		CHUNK          = 1024,
		WARMUP_SECONDS = 1,
		SECONDS        = 10,
		PROBE_SECONDS  = 10
	};

	int16_t*   pcm  = (int16_t*)malloc(CLIP_RATE * sizeof(int16_t));
	uint32_t   seed = 1;
	AeronClip  clip;
	AeronVoice voices[VOICES];
	float      radius[VOICES];
	float      angle[VOICES];
	uint64_t   ticks = 0;
	int        chunks;

	for (int i = 0; i < CLIP_RATE; ++i) {
		seed   = seed * 1664525u + 1013904223u;
		pcm[i] = (int16_t)(((int)(seed >> 16) - 32768) / 4);
	}
	clip = Aeron_AudioClipCreate(pcm, CLIP_RATE, CLIP_RATE, 1, AERON_PCM_S16);
	free(pcm);
	set_mixer_listener();
	for (int v = 0; v < VOICES; ++v) {
		radius[v]                       = 50.0f + 20.0f * (float)v;
		angle[v]                        = (float)v * 0.7f;
		const float              pos[3] = { radius[v] * sinf(angle[v]), 0.0f, -radius[v] * cosf(angle[v]) };
		const AeronAudioSource3D source = make_source(pos, 100.0f, 131072.0f);
		voices[v]                       = play_source(clip, 0.1f, &source);
	}

	chunks = (WARMUP_SECONDS + SECONDS) * RATE / CHUNK;
	for (int c = 0; c < chunks; ++c) {
		bench_move_voices(voices, radius, angle, VOICES);
		const uint64_t start = SDL_GetPerformanceCounter();
		render(CHUNK, CHUNK);
		if (c >= WARMUP_SECONDS * RATE / CHUNK) {
			ticks += SDL_GetPerformanceCounter() - start;
		}
	}

	const int    measured_chunks = chunks - WARMUP_SECONDS * RATE / CHUNK;
	const double mix_seconds     = (double)ticks / (double)SDL_GetPerformanceFrequency();
	const double audio_seconds   = (double)measured_chunks * CHUNK / RATE;
#ifdef NDEBUG
	const char* build = "optimized";
#else
	const char* build = "debug";
#endif
	printf("bench: %d positional voices, stereo panning: %.2f%% of one core, %.3f ms per %d-frame chunk (%s "
		   "build)\n",
		   VOICES, 100.0 * mix_seconds / audio_seconds, 1000.0 * mix_seconds / measured_chunks, CHUNK, build);

	/* The same mix, while a second thread checks a voice as fast as it can.
	   A check waits while the mixer holds its mutex, but not while the
	   positional voices mix. */
	WaitProbe probe;
	memset(&probe, 0, sizeof probe);
	probe.voice        = voices[0];
	SDL_Thread* thread = SDL_CreateThread(wait_probe, "wait probe", &probe);
	const int   probed = PROBE_SECONDS * RATE / CHUNK;
	for (int c = 0; thread && c < probed; ++c) {
		bench_move_voices(voices, radius, angle, VOICES);
		render(CHUNK, CHUNK);
	}
	SDL_SetAtomicInt(&probe.stop, 1);
	if (thread) {
		SDL_WaitThread(thread, NULL);
		/* The largest wait includes the times the system pauses one of
		   the threads, so the 99.9th percentile is printed too. */
		const double us_per_tick = 1e6 / (double)SDL_GetPerformanceFrequency();
		uint64_t     below       = 0;
		int          p999        = 0;
		while (p999 < PROBE_BUCKETS - 1 &&
			   (below += probe.buckets[p999]) < probe.calls - probe.calls / 1000) {
			++p999;
		}
		printf("bench: a voice status check from another thread, while mixing: mean %.3f us, 99.9%% under "
			   "%d us, at most %.1f us (%llu checks)\n",
			   probe.calls ? (double)probe.total_ticks * us_per_tick / (double)probe.calls : 0.0, p999 + 1,
			   (double)probe.max_ticks * us_per_tick, (unsigned long long)probe.calls);
	}

	for (int v = 0; v < VOICES; ++v) {
		Aeron_AudioVoiceStop(voices[v]);
	}
	Aeron_AudioClipDestroy(clip);
	return 0;
}

int main(int argc, char** argv) {
	const int bench = argc == 2 && strcmp(argv[1], "--bench") == 0;
	int       result;

	if (argc > 2 || (argc == 2 && !bench)) {
		fprintf(stderr, "usage: %s [--bench]\n", argv[0]);
		return 2;
	}
	if (!Aeron_AudioInitHeadless()) {
		printf("FAILED could not set up the headless mixer\n");
		return 1;
	}
	g_delay = Aeron_AudioOutputDelayFrames();
	if (bench) {
		result = run_bench();
		Aeron_AudioShutdown();
		return result;
	}

	test_distance_curve();
	test_mixer_directions();
	test_shim_directions();
	test_shim_cursors();
	test_range_fade();
	test_batch();
	test_shim_range_fade();
	test_shim_deferred();
	test_ramp_jump();
	test_ramp_range_fade();
	test_chunk_independence();
	test_resampler_cubic_exact();
	test_resampler_sine();
	test_job_stop();
	test_job_clip_write();
	test_output_channels();
	test_limiter_delay();
	test_limiter_peak();
	Aeron_AudioShutdown();

	if (g_failures) {
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
