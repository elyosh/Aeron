#include "aeron/compat/dsound.h"

#include "aeron/audio.h"
#include "aeron/sync.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* DirectSound -> Aeron compatibility shim. See aeron/compat/dsound.h.
 *
 * Objects begin with an lpVtbl pointer so recovered code can call methods
 * through the real DirectSound ABI indices (both the typed vtbls in sound.h /
 * frontend_sound.c and the numeric-index calls in imsound.c resolve here).
 *
 * Secondary buffers share mutable PCM storage, with one independent voice per
 * buffer. Staging writes are committed on Unlock under the mixer lock. */

#define DS_OK 0
#define DS_FAIL (-1)

enum { DS_DSBPLAY_LOOPING = DSBPLAY_LOOPING };

typedef struct DSDevice DSDevice;

/* Serializes buffer state shared by game and audio-producer threads. */
typedef struct DSBufferStorage {
	AeronMutex*      guard;
	int              refcount;
	AeronClip        clip;
	struct DSBuffer* locked_by;
	uint32_t         lock_offset;
	uint32_t         lock_first;
	uint32_t         lock_second;
	uint8_t          staging[];
} DSBufferStorage;

typedef struct DSBuffer {
	IDirectSoundBuffer iface;
	int                refcount;
	DSDevice*          device;
	struct DSBuffer*   prev;
	struct DSBuffer*   next;

	int      rate;
	int      channels;
	int      bits;
	uint32_t capacity; /* dwBufferBytes */
	uint32_t flags;
	int      is_primary;

	DSBufferStorage* storage;

	int      volume_mb; /* 0 == full volume */
	int      pan_mb;    /* 0 == centered */
	uint32_t frequency; /* 0 == native rate */

	AeronVoice voice;
	int        looping;
	uint32_t   cursor; /* saved byte position while stopped */

	int   is3d; /* play positionally (DirectSound3D, mode != DISABLE) */
	float pos3[3];
	float vel3[3];
	float min_dist;
	float max_dist;
} DSBuffer;

struct DSDevice {
	const IDirectSoundVtbl* lpVtbl;
	int                     refcount;
	DSBuffer*               buffers;
};

static void DSoundBuffer_LockState(DSBuffer* buffer) {
	if (buffer->storage && buffer->storage->guard) {
		Aeron_MutexLock(buffer->storage->guard);
	}
}

static void DSoundBuffer_UnlockState(DSBuffer* buffer) {
	if (buffer->storage && buffer->storage->guard) {
		Aeron_MutexUnlock(buffer->storage->guard);
	}
}

/* --- unit conversions ---------------------------------------------------- */

static float DSoundCompat_GainFromMillibels(int millibels) {
	if (millibels <= -10000) {
		return 0.0f;
	}
	if (millibels >= 0) {
		return 1.0f;
	}
	return powf(10.0f, (float)millibels / 2000.0f);
}

/* DirectSound leaves one channel at full volume and attenuates the opposite
 * channel by the signed hundredths-of-a-decibel pan value. */
static float DSoundCompat_BalanceFromMillibels(int millibels) {
	if (millibels <= -10000) {
		return -1.0f;
	}
	if (millibels >= 10000) {
		return 1.0f;
	}
	if (millibels < 0) {
		return powf(10.0f, (float)millibels / 2000.0f) - 1.0f;
	}
	if (millibels > 0) {
		return 1.0f - powf(10.0f, (float)-millibels / 2000.0f);
	}
	return 0.0f;
}

static float DSoundCompat_Pitch(const DSBuffer* buffer) {
	if (buffer->frequency == 0 || buffer->rate <= 0) {
		return 1.0f;
	}
	return (float)buffer->frequency / (float)buffer->rate;
}

/* --- DirectSound3D sub-interfaces ----------------------------------------- */

const DSCompatGuid IID_IDirectSound3DBuffer = {
	0x279AFA86, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};
const DSCompatGuid IID_IDirectSound3DListener = {
	0x279AFA84, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};

#define DS3DMODE_DISABLE 2u

static int DSoundCompat_GuidEqual(const void* a, const DSCompatGuid* b) {
	return a != NULL && memcmp(a, b, sizeof(DSCompatGuid)) == 0;
}

/* IDirectSound3DBuffer: positional parameters for one secondary buffer. A thin
 * wrapper that forwards to its owning DSBuffer and that buffer's live Aeron
 * voice. Vtable indices match the DirectSound3D ABI (only the methods the
 * recovered flight code calls are populated). */
typedef struct DS3DBuffer DS3DBuffer;

typedef struct DS3DBufferVtbl {
	int (*QueryInterface)(void*, const void*, void**);               /* 0 */
	int (*AddRef)(void*);                                            /* 1 */
	int (*Release)(void*);                                           /* 2 */
	int (*GetAllParameters)(void*, void*);                           /* 3 */
	int (*GetConeAngles)(void*, void*, void*);                       /* 4 */
	int (*GetConeOrientation)(void*, void*);                         /* 5 */
	int (*GetConeOutsideVolume)(void*, void*);                       /* 6 */
	int (*GetMaxDistance)(void*, float*);                            /* 7 */
	int (*GetMinDistance)(void*, float*);                            /* 8 */
	int (*GetMode)(void*, uint32_t*);                                /* 9 */
	int (*GetPosition)(void*, void*);                                /* 10 */
	int (*GetVelocity)(void*, void*);                                /* 11 */
	int (*SetAllParameters)(void*, const void*, uint32_t);           /* 12 */
	int (*SetConeAngles)(void*, uint32_t, uint32_t, uint32_t);       /* 13 */
	int (*SetConeOrientation)(void*, float, float, float, uint32_t); /* 14 */
	int (*SetConeOutsideVolume)(void*, int32_t, uint32_t);           /* 15 */
	int (*SetMaxDistance)(void*, float, uint32_t);                   /* 16 */
	int (*SetMinDistance)(void*, float, uint32_t);                   /* 17 */
	int (*SetMode)(void*, uint32_t, uint32_t);                       /* 18 */
	int (*SetPosition)(void*, float, float, float, uint32_t);        /* 19 */
	int (*SetVelocity)(void*, float, float, float, uint32_t);        /* 20 */
} DS3DBufferVtbl;

struct DS3DBuffer {
	const DS3DBufferVtbl* lpVtbl;
	int                   refcount;
	DSBuffer*             owner;
};

static int DS3DBuffer_AddRef(void* self) { return ++((DS3DBuffer*)self)->refcount; }

static int DS3DBuffer_Release(void* self) {
	DS3DBuffer* b = (DS3DBuffer*)self;
	if (--b->refcount > 0) {
		return b->refcount;
	}
	free(b);
	return 0;
}

static int DS3DBuffer_SetMaxDistance(void* self, float dist, uint32_t apply) {
	(void)apply;
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	DSoundBuffer_LockState(owner);
	owner->max_dist = dist;
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

static int DS3DBuffer_SetMinDistance(void* self, float dist, uint32_t apply) {
	(void)apply;
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	DSoundBuffer_LockState(owner);
	owner->min_dist = dist;
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

static int DS3DBuffer_SetMode(void* self, uint32_t mode, uint32_t apply) {
	(void)apply;
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	DSoundBuffer_LockState(owner);
	owner->is3d = (mode != DS3DMODE_DISABLE);
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

static int DS3DBuffer_SetPosition(void* self, float x, float y, float z, uint32_t apply) {
	(void)apply;
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	DSoundBuffer_LockState(owner);
	owner->pos3[0] = x;
	owner->pos3[1] = y;
	owner->pos3[2] = z;
	owner->is3d    = 1;
	if (owner->voice) {
		float pos[3] = { x, y, z };
		Aeron_AudioVoiceSet3DPosition(owner->voice, pos);
	}
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

static int DS3DBuffer_SetVelocity(void* self, float x, float y, float z, uint32_t apply) {
	(void)apply;
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	DSoundBuffer_LockState(owner);
	owner->vel3[0] = x;
	owner->vel3[1] = y;
	owner->vel3[2] = z;
	if (owner->voice) {
		float vel[3] = { x, y, z };
		Aeron_AudioVoiceSet3DVelocity(owner->voice, vel);
	}
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

static const DS3DBufferVtbl g_ds3d_buffer_vtbl = {
	0,
	DS3DBuffer_AddRef,
	DS3DBuffer_Release,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	DS3DBuffer_SetMaxDistance,
	DS3DBuffer_SetMinDistance,
	DS3DBuffer_SetMode,
	DS3DBuffer_SetPosition,
	DS3DBuffer_SetVelocity,
};

static int DSoundCompat_QueryInterface3DBuffer(DSBuffer* owner, void** out) {
	DS3DBuffer* wrapper = (DS3DBuffer*)calloc(1, sizeof(DS3DBuffer));
	if (!wrapper) {
		*out = NULL;
		return DS_FAIL;
	}
	wrapper->lpVtbl   = &g_ds3d_buffer_vtbl;
	wrapper->refcount = 1;
	wrapper->owner    = owner;
	*out              = wrapper;
	return DS_OK;
}

/* IDirectSound3DListener: a single shared listener obtained from the primary
 * buffer. Accumulates position/orientation/velocity and the distance model,
 * pushing them to the Aeron mixer on CommitDeferredSettings. */
typedef struct DS3DListener {
	const void** lpVtbl;
	int          refcount;
	float        pos[3];
	float        front[3];
	float        top[3];
	float        vel[3];
	float        distance_factor;
	float        rolloff_factor;
	float        doppler_factor;
} DS3DListener;

static DS3DListener g_ds3d_listener;

static int DS3DListener_AddRef(void* self) { return ++((DS3DListener*)self)->refcount; }

static int DS3DListener_Release(void* self) { return --((DS3DListener*)self)->refcount; }

static void DS3DListener_PushDistanceModel(DS3DListener* l) {
	Aeron_AudioSetDistanceModel(l->distance_factor, l->rolloff_factor, l->doppler_factor);
}

static int DS3DListener_SetDistanceFactor(void* self, float value, uint32_t apply) {
	(void)apply;
	DS3DListener* l    = (DS3DListener*)self;
	l->distance_factor = value;
	DS3DListener_PushDistanceModel(l);
	return DS_OK;
}

static int DS3DListener_SetDopplerFactor(void* self, float value, uint32_t apply) {
	(void)apply;
	DS3DListener* l   = (DS3DListener*)self;
	l->doppler_factor = value;
	DS3DListener_PushDistanceModel(l);
	return DS_OK;
}

static int DS3DListener_SetOrientation(void* self, float fx, float fy, float fz, float tx, float ty, float tz,
									   uint32_t apply) {
	(void)apply;
	DS3DListener* l = (DS3DListener*)self;
	l->front[0]     = fx;
	l->front[1]     = fy;
	l->front[2]     = fz;
	l->top[0]       = tx;
	l->top[1]       = ty;
	l->top[2]       = tz;
	return DS_OK;
}

static int DS3DListener_SetPosition(void* self, float x, float y, float z, uint32_t apply) {
	(void)apply;
	DS3DListener* l = (DS3DListener*)self;
	l->pos[0]       = x;
	l->pos[1]       = y;
	l->pos[2]       = z;
	return DS_OK;
}

static int DS3DListener_SetVelocity(void* self, float x, float y, float z, uint32_t apply) {
	(void)apply;
	DS3DListener* l = (DS3DListener*)self;
	l->vel[0]       = x;
	l->vel[1]       = y;
	l->vel[2]       = z;
	return DS_OK;
}

static int DS3DListener_CommitDeferredSettings(void* self) {
	DS3DListener*      l = (DS3DListener*)self;
	AeronAudioListener listener;
	memcpy(listener.pos, l->pos, sizeof(listener.pos));
	memcpy(listener.front, l->front, sizeof(listener.front));
	memcpy(listener.top, l->top, sizeof(listener.top));
	memcpy(listener.vel, l->vel, sizeof(listener.vel));
	Aeron_AudioSetListener(&listener);
	return DS_OK;
}

static const void* g_ds3d_listener_vtbl[] = {
	NULL,                              /* 0 QueryInterface */
	(const void*)DS3DListener_AddRef,  /* 1 */
	(const void*)DS3DListener_Release, /* 2 */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,                                             /* 3..9 getters */
	NULL,                                             /* 10 SetAllParameters */
	(const void*)DS3DListener_SetDistanceFactor,      /* 11 */
	(const void*)DS3DListener_SetDopplerFactor,       /* 12 */
	(const void*)DS3DListener_SetOrientation,         /* 13 */
	(const void*)DS3DListener_SetPosition,            /* 14 */
	NULL,                                             /* 15 SetRolloffFactor */
	(const void*)DS3DListener_SetVelocity,            /* 16 */
	(const void*)DS3DListener_CommitDeferredSettings, /* 17 */
};

static int DSoundCompat_QueryInterface3DListener(void** out) {
	g_ds3d_listener.lpVtbl          = g_ds3d_listener_vtbl;
	g_ds3d_listener.refcount        = 1;
	g_ds3d_listener.distance_factor = 1.0f;
	g_ds3d_listener.rolloff_factor  = 1.0f;
	g_ds3d_listener.doppler_factor  = 1.0f;
	*out                            = &g_ds3d_listener;
	return DS_OK;
}

/* --- buffer methods ------------------------------------------------------ */

static int AERON_DXAPI DSoundBuffer_QueryInterface(void* self, const void* iid, void** out) {
	if (!out) {
		return DS_FAIL;
	}
	if (DSoundCompat_GuidEqual(iid, &IID_IDirectSound3DBuffer)) {
		return DSoundCompat_QueryInterface3DBuffer((DSBuffer*)self, out);
	}
	if (DSoundCompat_GuidEqual(iid, &IID_IDirectSound3DListener)) {
		/* The recovered code obtains the single listener from the primary buffer. */
		return DSoundCompat_QueryInterface3DListener(out);
	}
	*out = NULL;
	return DS_FAIL;
}

static int AERON_DXAPI DSoundBuffer_AddRef(void* self) {
	DSBuffer* b = (DSBuffer*)self;
	return Aeron_AtomicAdd(&b->refcount, 1) + 1;
}

static void DSoundBuffer_Destroy(DSBuffer* b) {
	if (b->prev) {
		b->prev->next = b->next;
	} else {
		b->device->buffers = b->next;
	}
	if (b->next) {
		b->next->prev = b->prev;
	}
	DSBufferStorage* storage = b->storage;
	DSoundBuffer_LockState(b);
	if (b->voice) {
		Aeron_AudioVoiceStop(b->voice);
	}
	int last_owner = 0;
	if (storage) {
		if (storage->locked_by == b)
			storage->locked_by = NULL;
		last_owner = --storage->refcount == 0;
		if (last_owner)
			Aeron_AudioClipDestroy(storage->clip);
	}
	DSoundBuffer_UnlockState(b);
	if (last_owner) {
		if (storage->guard)
			Aeron_MutexDestroy(storage->guard);
		free(storage);
	}
	free(b);
}

static int AERON_DXAPI DSoundBuffer_Release(void* self) {
	DSBuffer* b          = (DSBuffer*)self;
	int       references = Aeron_AtomicAdd(&b->refcount, -1) - 1;
	if (references > 0) {
		return references;
	}
	DSoundBuffer_Destroy(b);
	return 0;
}

static int AERON_DXAPI DSoundBuffer_GetCaps(void* self, void* caps) {
	(void)self;
	(void)caps;
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_GetCurrentPosition(void* self, uint32_t* play, uint32_t* write) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	uint32_t play_cursor  = b->cursor;
	uint32_t write_cursor = b->cursor;
	if (b->voice) {
		size_t played, mixed;
		if (Aeron_AudioVoiceGetCursors(b->voice, &played, &mixed)) {
			uint32_t align = (uint32_t)(b->channels * (b->bits / 8));
			play_cursor    = (uint32_t)played * align;
			write_cursor   = (uint32_t)mixed * align;
		} else {
			b->voice  = 0;
			b->cursor = play_cursor = write_cursor = 0;
		}
	}
	if (play)
		*play = play_cursor;
	if (write)
		*write = write_cursor;
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_GetFormat(void* self, void* fmt, uint32_t size, uint32_t* written) {
	DSBuffer*    b = (DSBuffer*)self;
	DSWaveFormat wf;

	if (fmt && size >= sizeof(DSWaveFormat)) {
		wf.wFormatTag      = 1; /* WAVE_FORMAT_PCM */
		wf.nChannels       = (uint16_t)b->channels;
		wf.nSamplesPerSec  = (uint32_t)b->rate;
		wf.wBitsPerSample  = (uint16_t)b->bits;
		wf.nBlockAlign     = (uint16_t)(b->channels * (b->bits / 8));
		wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
		wf.cbSize          = 0;
		memcpy(fmt, &wf, sizeof(wf));
	}
	if (written) {
		*written = sizeof(DSWaveFormat);
	}
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_GetVolume(void* self, int32_t* volume) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	if (volume) {
		*volume = b->volume_mb;
	}
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_GetPan(void* self, int32_t* pan) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	if (pan) {
		*pan = b->pan_mb;
	}
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_GetFrequency(void* self, uint32_t* frequency) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	if (frequency) {
		*frequency = b->frequency ? b->frequency : (uint32_t)b->rate;
	}
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_GetStatus(void* self, uint32_t* status) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	uint32_t s       = 0;
	int      playing = b->voice && Aeron_AudioVoiceIsPlaying(b->voice);
	if (playing) {
		s = DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0u);
	}
	if (status) {
		*status = s;
	}
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_Initialize(void* self, void* device, const void* desc) {
	(void)self;
	(void)device;
	(void)desc;
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_Lock(void* self, uint32_t offset, uint32_t bytes, void** p1, uint32_t* b1,
										 void** p2, uint32_t* b2, uint32_t flags) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	DSBufferStorage* storage = b->storage;
	if (!storage || storage->locked_by || !p1 || !b1 ||
		(flags & ~(DSBLOCK_ENTIREBUFFER | DSBLOCK_FROMWRITECURSOR))) {
		DSoundBuffer_UnlockState(b);
		return DS_FAIL;
	}
	if (flags & DSBLOCK_FROMWRITECURSOR) {
		DSoundBuffer_GetCurrentPosition(self, NULL, &offset);
	}
	if (flags & DSBLOCK_ENTIREBUFFER) {
		bytes = b->capacity;
	}
	if (offset >= b->capacity || !bytes || bytes > b->capacity) {
		DSoundBuffer_UnlockState(b);
		return DS_FAIL;
	}
	uint32_t first = b->capacity - offset;
	if (first > bytes)
		first = bytes;
	uint32_t second = bytes - first;
	if (second && (!p2 || !b2)) {
		DSoundBuffer_UnlockState(b);
		return DS_FAIL;
	}

	*p1 = storage->staging + offset;
	*b1 = first;
	if (p2)
		*p2 = second ? storage->staging : NULL;
	if (b2)
		*b2 = second;
	storage->locked_by   = b;
	storage->lock_offset = offset;
	storage->lock_first  = first;
	storage->lock_second = second;
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_Unlock(void* self, void* p1, uint32_t b1, void* p2, uint32_t b2) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	DSBufferStorage* storage = b->storage;
	if (!storage || storage->locked_by != b || p1 != storage->staging + storage->lock_offset ||
		b1 > storage->lock_first || b2 > storage->lock_second ||
		p2 != (storage->lock_second ? storage->staging : NULL)) {
		DSoundBuffer_UnlockState(b);
		return DS_FAIL;
	}
	AeronPcmWrite writes[2] = { { storage->lock_offset, p1, b1 }, { 0, p2, b2 } };
	int           ok        = Aeron_AudioClipWrite(storage->clip, writes, 2);
	storage->locked_by      = NULL;
	DSoundBuffer_UnlockState(b);
	return ok ? DS_OK : DS_FAIL;
}

static int AERON_DXAPI DSoundBuffer_Play(void* self, uint32_t reserved1, uint32_t priority, uint32_t flags) {
	(void)reserved1;
	(void)priority;
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	if (b->is_primary) {
		DSoundBuffer_UnlockState(b);
		return DS_OK;
	}
	if (!b->storage) {
		DSoundBuffer_UnlockState(b);
		return DS_FAIL;
	}

	b->looping = (flags & DS_DSBPLAY_LOOPING) != 0;
	if (b->voice) {
		if (Aeron_AudioVoiceSetLooping(b->voice, b->looping)) {
			DSoundBuffer_UnlockState(b);
			return DS_OK;
		}
		b->voice  = 0;
		b->cursor = 0;
	}
	float  gain  = DSoundCompat_GainFromMillibels(b->volume_mb);
	float  pitch = DSoundCompat_Pitch(b);
	size_t frame = b->cursor / (uint32_t)(b->channels * (b->bits / 8));
	if (b->is3d) {
		b->voice = Aeron_AudioVoicePlay3DFrom(b->storage->clip, gain, pitch, b->looping, b->pos3, b->min_dist,
											  b->max_dist, frame);
		Aeron_AudioVoiceSet3DVelocity(b->voice, b->vel3);
	} else {
		b->voice = Aeron_AudioVoicePlayFrom(
			b->storage->clip, gain, DSoundCompat_BalanceFromMillibels(b->pan_mb), pitch, b->looping, frame);
	}
	int result = b->voice ? DS_OK : DS_FAIL;
	DSoundBuffer_UnlockState(b);
	return result;
}

static int AERON_DXAPI DSoundBuffer_SetCurrentPosition(void* self, uint32_t position) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	if (!b->storage || position >= b->capacity) {
		DSoundBuffer_UnlockState(b);
		return DS_FAIL;
	}
	uint32_t align = (uint32_t)(b->channels * (b->bits / 8));
	position -= position % align;
	if (b->voice && !Aeron_AudioVoiceSetPosition(b->voice, position / align)) {
		b->voice = 0;
	}
	b->cursor = position;
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_SetFormat(void* self, const void* fmt) {
	(void)self;
	(void)fmt;
	/* Primary-buffer format is fixed by the Aeron device. */
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_SetVolume(void* self, int32_t volume) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	b->volume_mb = volume;
	if (b->voice) {
		Aeron_AudioVoiceSetGain(b->voice, DSoundCompat_GainFromMillibels(volume));
	}
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_SetPan(void* self, int32_t pan) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	b->pan_mb = pan;
	if (b->voice) {
		Aeron_AudioVoiceSetPan(b->voice, DSoundCompat_BalanceFromMillibels(pan));
	}
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_SetFrequency(void* self, uint32_t frequency) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	b->frequency = frequency;
	if (b->voice) {
		Aeron_AudioVoiceSetPitch(b->voice, DSoundCompat_Pitch(b));
	}
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_Stop(void* self) {
	DSBuffer* b = (DSBuffer*)self;
	DSoundBuffer_LockState(b);
	if (b->voice) {
		b->cursor =
			(uint32_t)Aeron_AudioVoiceStopAndGetPosition(b->voice) * (uint32_t)(b->channels * (b->bits / 8));
		b->voice = 0;
	}
	DSoundBuffer_UnlockState(b);
	return DS_OK;
}

static int AERON_DXAPI DSoundBuffer_Restore(void* self) {
	(void)self;
	return DS_OK;
}

static const IDirectSoundBufferVtbl g_ds_buffer_vtbl = {
	DSoundBuffer_QueryInterface,
	DSoundBuffer_AddRef,
	DSoundBuffer_Release,
	DSoundBuffer_GetCaps,
	DSoundBuffer_GetCurrentPosition,
	DSoundBuffer_GetFormat,
	DSoundBuffer_GetVolume,
	DSoundBuffer_GetPan,
	DSoundBuffer_GetFrequency,
	DSoundBuffer_GetStatus,
	DSoundBuffer_Initialize,
	DSoundBuffer_Lock,
	DSoundBuffer_Play,
	DSoundBuffer_SetCurrentPosition,
	DSoundBuffer_SetFormat,
	DSoundBuffer_SetVolume,
	DSoundBuffer_SetPan,
	DSoundBuffer_SetFrequency,
	DSoundBuffer_Stop,
	DSoundBuffer_Unlock,
	DSoundBuffer_Restore,
};

static DSBuffer* DSoundCompat_AllocBuffer(DSDevice* device) {
	DSBuffer* b = (DSBuffer*)calloc(1, sizeof(DSBuffer));
	if (b) {
		b->iface.lpVtbl = &g_ds_buffer_vtbl;
		b->refcount     = 1;
		b->device       = device;
		b->next         = device->buffers;
		if (b->next) {
			b->next->prev = b;
		}
		device->buffers = b;
	}
	return b;
}

/* --- device methods ------------------------------------------------------ */

static int AERON_DXAPI DSoundDevice_QueryInterface(void* self, const void* iid, void** out) {
	(void)self;
	(void)iid;
	/* TODO(flight 3D): return an IDirectSound3DListener shim. */
	if (out) {
		*out = NULL;
	}
	return DS_FAIL;
}

static int AERON_DXAPI DSoundDevice_AddRef(void* self) {
	DSDevice* d = (DSDevice*)self;
	return ++d->refcount;
}

static int AERON_DXAPI DSoundDevice_Release(void* self) {
	DSDevice* d = (DSDevice*)self;
	if (--d->refcount > 0) {
		return d->refcount;
	}
	/* Native DirectSound invalidates every child buffer with its device. */
	while (d->buffers) {
		DSoundBuffer_Destroy(d->buffers);
	}
	free(d);
	return 0;
}

static int AERON_DXAPI DSoundDevice_CreateSoundBuffer(void* self, const DSBufferDesc* desc,
													  IDirectSoundBuffer** buffer, void* outer) {
	(void)outer;
	if (!buffer || !desc) {
		return DS_FAIL;
	}
	*buffer = NULL;

	DSBuffer* b = DSoundCompat_AllocBuffer((DSDevice*)self);
	if (!b) {
		return DS_FAIL;
	}
	b->flags    = desc->dwFlags;
	b->capacity = desc->dwBufferBytes;

	if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) {
		b->is_primary = 1;
	} else if (desc->lpwfxFormat) {
		b->rate     = (int)desc->lpwfxFormat->nSamplesPerSec;
		b->channels = desc->lpwfxFormat->nChannels;
		b->bits     = desc->lpwfxFormat->wBitsPerSample;
	}

	if (!b->is_primary) {
		if (!desc->lpwfxFormat || desc->lpwfxFormat->wFormatTag != 1 || b->rate <= 0 ||
			(b->channels != 1 && b->channels != 2) || (b->bits != 8 && b->bits != 16)) {
			DSoundBuffer_Destroy(b);
			return DS_FAIL;
		}
		uint32_t align = (uint32_t)(b->channels * (b->bits / 8));
		if (!b->capacity || b->capacity % align || desc->lpwfxFormat->nBlockAlign != align ||
			(size_t)b->capacity > SIZE_MAX - sizeof(DSBufferStorage)) {
			DSoundBuffer_Destroy(b);
			return DS_FAIL;
		}
		b->storage = (DSBufferStorage*)calloc(1, sizeof(DSBufferStorage) + (size_t)b->capacity);
		if (!b->storage) {
			DSoundBuffer_Destroy(b);
			return DS_FAIL;
		}
		b->storage->refcount = 1;
		b->storage->guard    = Aeron_MutexCreate();
		if (!b->storage->guard) {
			DSoundBuffer_Destroy(b);
			return DS_FAIL;
		}
		memset(b->storage->staging, b->bits == 8 ? 128 : 0, b->capacity);
		b->storage->clip = Aeron_AudioClipCreateMutable(b->capacity / align, b->rate, b->channels,
														b->bits == 8 ? AERON_PCM_U8 : AERON_PCM_S16);
		if (!b->storage->clip) {
			DSoundBuffer_Destroy(b);
			return DS_FAIL;
		}
		b->is3d     = (desc->dwFlags & DSBCAPS_CTRL3D) != 0;
		b->min_dist = 1.0f;
		b->max_dist = 1000000000.0f;
	}

	*buffer = &b->iface;
	return DS_OK;
}

static int AERON_DXAPI DSoundDevice_GetCaps(void* self, void* caps) {
	(void)self;
	(void)caps;
	/* Caller pre-zeroes the DSCAPS struct; leaving it lets the recovered code
	 * pick its non-hardware primary-buffer path. */
	return DS_OK;
}

static int AERON_DXAPI DSoundDevice_DuplicateSoundBuffer(void* self, IDirectSoundBuffer* source,
														 IDirectSoundBuffer** duplicate) {
	if (!duplicate || !source) {
		return DS_FAIL;
	}
	*duplicate = NULL;

	DSBuffer* src = (DSBuffer*)source;
	if (!src->storage || src->device != (DSDevice*)self)
		return DS_FAIL;
	DSBuffer* dup = DSoundCompat_AllocBuffer((DSDevice*)self);
	if (!dup) {
		return DS_FAIL;
	}
	DSoundBuffer_LockState(src);
	dup->rate      = src->rate;
	dup->channels  = src->channels;
	dup->bits      = src->bits;
	dup->capacity  = src->capacity;
	dup->flags     = src->flags;
	dup->volume_mb = src->volume_mb;
	dup->pan_mb    = src->pan_mb;
	dup->frequency = src->frequency;
	dup->storage   = src->storage;
	++dup->storage->refcount;
	dup->is3d = src->is3d;
	memcpy(dup->pos3, src->pos3, sizeof dup->pos3);
	memcpy(dup->vel3, src->vel3, sizeof dup->vel3);
	dup->min_dist = src->min_dist; /* DirectSound copies 3D params into the duplicate */
	dup->max_dist = src->max_dist;

	DSoundBuffer_UnlockState(src);
	*duplicate = &dup->iface;
	return DS_OK;
}

static int AERON_DXAPI DSoundDevice_SetCooperativeLevel(void* self, void* hwnd, uint32_t level) {
	(void)self;
	(void)hwnd;
	(void)level;
	return DS_OK;
}

static int AERON_DXAPI DSoundDevice_Compact(void* self) {
	(void)self;
	return DS_OK;
}

static int AERON_DXAPI DSoundDevice_GetSpeakerConfig(void* self, uint32_t* config) {
	(void)self;
	if (config) {
		*config = 0;
	}
	return DS_OK;
}

static int AERON_DXAPI DSoundDevice_SetSpeakerConfig(void* self, uint32_t config) {
	(void)self;
	(void)config;
	return DS_OK;
}

static int AERON_DXAPI DSoundDevice_Initialize(void* self, const void* guid) {
	(void)self;
	(void)guid;
	return DS_OK;
}

static const IDirectSoundVtbl g_ds_device_vtbl = {
	DSoundDevice_QueryInterface,      DSoundDevice_AddRef,     DSoundDevice_Release,
	DSoundDevice_CreateSoundBuffer,   DSoundDevice_GetCaps,    DSoundDevice_DuplicateSoundBuffer,
	DSoundDevice_SetCooperativeLevel, DSoundDevice_Compact,    DSoundDevice_GetSpeakerConfig,
	DSoundDevice_SetSpeakerConfig,    DSoundDevice_Initialize,
};

int AERON_DXAPI DirectSoundCreate(const DSCompatGuid* device_guid, void** outDevice, void* outerUnknown) {
	(void)device_guid;
	(void)outerUnknown;
	if (!outDevice) {
		return DS_FAIL;
	}
	*outDevice = NULL;

	DSDevice* device = (DSDevice*)calloc(1, sizeof(DSDevice));
	if (!device) {
		return DS_FAIL;
	}
	device->lpVtbl   = &g_ds_device_vtbl;
	device->refcount = 1;
	*outDevice       = device;
	return DS_OK;
}
