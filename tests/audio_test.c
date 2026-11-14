/*
 * Tests the audio mixer and the DirectSound shim without an audio device.
 * The mixer runs headless (Aeron_AudioInitHeadless), so every mixed frame
 * counts as played at once and the results don't depend on the machine.
 *
 * The checks cover the spatial audio work (docs/spatial_audio_plan.md): the
 * inverse-distance curve, the direction of positional panning, the
 * conversion of DirectSound's coordinates, the DirectSound buffer cursors,
 * the fade before the max distance, and 3D batches and deferred settings.
 *
 * With --bench, it measures the cost of mixing 32 positional voices instead,
 * as a percentage of real time on one core.
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
 * frame. */
static void test_shim_cursors(void) {
	enum { FRAMES = 48000, BYTES = FRAMES * 2 };

	IDirectSound*       device = create_ds_device();
	IDirectSoundBuffer* buffer =
		make_ds_buffer(device, DSBCAPS_STATIC | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME, FRAMES);
	char what[160];

	check_cursors(buffer, 0, 0, "new buffer");
	buffer->lpVtbl->Play(buffer, 0, 0, DSBPLAY_LOOPING);
	check_cursors(buffer, 0, 0, "after Play");
	check_status(buffer, DSBSTATUS_PLAYING | DSBSTATUS_LOOPING, "after Play");
	render(1000, 1000);
	check_cursors(buffer, 2000, 2000, "after 1000 frames");

	buffer->lpVtbl->SetCurrentPosition(buffer, 4000);
	check_cursors(buffer, 4000, 4000, "after SetCurrentPosition(4000)");
	render(500, 128);
	check_cursors(buffer, 5000, 5000, "500 more frames, mixed in chunks of 128");

	buffer->lpVtbl->Stop(buffer);
	check_cursors(buffer, 5000, 5000, "after Stop");
	check_status(buffer, 0, "after Stop");
	render(100, 100);
	check_cursors(buffer, 5000, 5000, "100 frames after Stop");

	buffer->lpVtbl->Play(buffer, 0, 0, DSBPLAY_LOOPING);
	render(100, 100);
	check_cursors(buffer, 5200, 5200, "100 frames after resuming");

	buffer->lpVtbl->SetCurrentPosition(buffer, BYTES - 400);
	render(300, 300);
	check_cursors(buffer, 200, 200, "300 frames from 200 frames before the end: wrapped");

	/* Not looping: the voice ends 200 frames before the end of the render. */
	buffer->lpVtbl->Stop(buffer);
	buffer->lpVtbl->SetCurrentPosition(buffer, BYTES - 400);
	buffer->lpVtbl->Play(buffer, 0, 0, 0);
	check_status(buffer, DSBSTATUS_PLAYING, "after Play without looping");
	render(400, 400);
	snprintf(what, sizeof what, "the last frame plays and the next is silent (got %.4f, %.4f)", left_at(199),
			 left_at(200));
	check(close_to(left_at(199), CLIP_LEVEL) && close_to(left_at(200), 0.0f), what);
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

/* --- benchmark ----------------------------------------------------------- */

/* 32 positional voices of a 22,050 Hz noise clip, circling the listener at
 * different distances. The time spent mixing is compared with the duration of
 * the audio. Positions change once per chunk, as they would with per-frame
 * updates from the game. */
static int run_bench(void) {
	enum { VOICES = 32, CLIP_RATE = 22050, CHUNK = 1024, WARMUP_SECONDS = 1, SECONDS = 10 };

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
		/* One batch per chunk, as the game commits its updates. */
		Aeron_Audio3DBeginBatch();
		for (int v = 0; v < VOICES; ++v) {
			angle[v] += 0.02f;
			const float pos[3] = { radius[v] * sinf(angle[v]), 0.0f, -radius[v] * cosf(angle[v]) };
			const AeronAudioSource3D source = make_source(pos, 100.0f, 131072.0f);
			Aeron_AudioVoiceSet3DSource(voices[v], &source);
		}
		Aeron_Audio3DEndBatch();
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
	Aeron_AudioShutdown();

	if (g_failures) {
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
