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

typedef struct DS3DParams {
	int   is3d; /* play positionally (DirectSound3D, mode != DISABLE) */
	float pos[3];
	float vel[3];
	float min_dist;
	float max_dist;
} DS3DParams;

/* The DirectSound3D parameters waiting for CommitDeferredSettings. */
enum {
	DS3D_PARAM_MODE     = 1u << 0,
	DS3D_PARAM_POSITION = 1u << 1,
	DS3D_PARAM_VELOCITY = 1u << 2,
	DS3D_PARAM_MIN      = 1u << 3,
	DS3D_PARAM_MAX      = 1u << 4,
};

/* The apply argument of the DirectSound3D setters. */
enum { DS3D_IMMEDIATE = 0, DS3D_DEFERRED = 1 };

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

	/* The DirectSound3D parameters in use, and the ones set with
	   DS3D_DEFERRED that wait for CommitDeferredSettings. */
	DS3DParams       params3d;
	DS3DParams       deferred3d;
	uint32_t         deferred3d_mask; /* DS3D_PARAM_* */
	struct DSBuffer* deferred_next;   /* in the device's deferred list */
} DSBuffer;

/* Buffers are created and finally released on the game thread (see
 * aeron/compat/dsound.h). The DirectSound3D setters and CommitDeferredSettings
 * run there too. So neither list needs a lock. Producer threads such as the
 * iMUSE timer only use their own buffer, under its storage guard. */
struct DSDevice {
	const IDirectSoundVtbl* lpVtbl;
	int                     refcount;
	DSBuffer*               buffers;
	/* The buffers with DS3D_DEFERRED settings. The commit only walks these:
	   the game has thousands of buffers, but only a few playing sounds. */
	DSBuffer* deferred;
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

#define DS3DMODE_NORMAL 0u
#define DS3DMODE_DISABLE 2u

/* DirectSound uses left-handed coordinates, and the Aeron mixer uses
 * right-handed ones. Negating z converts a position, velocity or direction. */
static void DSoundCompat_ToMixerCoords(float x, float y, float z, float out[3]) {
	out[0] = x;
	out[1] = y;
	out[2] = -z;
}

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

/* Where the fade of a DSBCAPS_MUTE3DATMAXDISTANCE buffer starts, as a part of
 * its max distance. */
#define DSOUND_MUTE_FADE_START 0.75f

/* The source the mixer gets for a buffer. A buffer created with
 * DSBCAPS_MUTE3DATMAXDISTANCE is silent past its max distance, as in
 * DirectSound. DirectSound mutes it at once. Here its gain fades out over the
 * last quarter of the max distance instead, so it doesn't stop with an
 * audible step. Caller holds the storage guard. */
static void DSoundBuffer_Source3D(const DSBuffer* b, AeronAudioSource3D* out) {
	memcpy(out->pos, b->params3d.pos, sizeof out->pos);
	memcpy(out->vel, b->params3d.vel, sizeof out->vel);
	out->min_dist        = b->params3d.min_dist;
	out->max_dist        = b->params3d.max_dist;
	out->mute_beyond_max = (b->flags & DSBCAPS_MUTE3DATMAXDISTANCE) != 0;
	out->fade_start      = out->mute_beyond_max ? DSOUND_MUTE_FADE_START * out->max_dist : 0.0f;
}

/* Sends the parameters in use to the playing voice. Caller holds the storage
 * guard. */
static void DSoundBuffer_Push3D(DSBuffer* b) {
	if (b->voice && b->params3d.is3d) {
		AeronAudioSource3D source;
		DSoundBuffer_Source3D(b, &source);
		Aeron_AudioVoiceSet3DSource(b->voice, &source);
	}
}

/* Records a DS3D_DEFERRED setting, and adds the buffer to its device's
 * deferred list the first time. */
static void DSoundBuffer_MarkDeferred3D(DSBuffer* b, uint32_t param) {
	if (!b->deferred3d_mask) {
		b->deferred_next    = b->device->deferred;
		b->device->deferred = b;
	}
	b->deferred3d_mask |= param;
}

/* Applies the parameters set with DS3D_DEFERRED. The caller holds the storage
 * guard, and removes the buffer from the deferred list. */
static void DSoundBuffer_ApplyDeferred3D(DSBuffer* b) {
	const uint32_t mask = b->deferred3d_mask;
	if (!mask) {
		return;
	}
	if (mask & DS3D_PARAM_MODE) {
		b->params3d.is3d = b->deferred3d.is3d;
	}
	if (mask & DS3D_PARAM_POSITION) {
		memcpy(b->params3d.pos, b->deferred3d.pos, sizeof b->params3d.pos);
		b->params3d.is3d = 1;
	}
	if (mask & DS3D_PARAM_VELOCITY) {
		memcpy(b->params3d.vel, b->deferred3d.vel, sizeof b->params3d.vel);
	}
	if (mask & DS3D_PARAM_MIN) {
		b->params3d.min_dist = b->deferred3d.min_dist;
	}
	if (mask & DS3D_PARAM_MAX) {
		b->params3d.max_dist = b->deferred3d.max_dist;
	}
	b->deferred3d_mask = 0;
	DSoundBuffer_Push3D(b);
}

/* With DS3D_IMMEDIATE, a setter changes the parameters in use and the playing
 * voice. With DS3D_DEFERRED, the change waits for CommitDeferredSettings. */
static int DS3DBuffer_SetMaxDistance(void* self, float dist, uint32_t apply) {
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	DSoundBuffer_LockState(owner);
	if (apply & DS3D_DEFERRED) {
		owner->deferred3d.max_dist = dist;
		DSoundBuffer_MarkDeferred3D(owner, DS3D_PARAM_MAX);
	} else {
		owner->params3d.max_dist = dist;
		DSoundBuffer_Push3D(owner);
	}
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

static int DS3DBuffer_SetMinDistance(void* self, float dist, uint32_t apply) {
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	DSoundBuffer_LockState(owner);
	if (apply & DS3D_DEFERRED) {
		owner->deferred3d.min_dist = dist;
		DSoundBuffer_MarkDeferred3D(owner, DS3D_PARAM_MIN);
	} else {
		owner->params3d.min_dist = dist;
		DSoundBuffer_Push3D(owner);
	}
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

/* The mode only chooses how the next Play starts the voice. */
static int DS3DBuffer_SetMode(void* self, uint32_t mode, uint32_t apply) {
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	DSoundBuffer_LockState(owner);
	if (apply & DS3D_DEFERRED) {
		owner->deferred3d.is3d = (mode != DS3DMODE_DISABLE);
		DSoundBuffer_MarkDeferred3D(owner, DS3D_PARAM_MODE);
	} else {
		owner->params3d.is3d = (mode != DS3DMODE_DISABLE);
	}
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

/* Setting a position also turns 3D on again. */
static int DS3DBuffer_SetPosition(void* self, float x, float y, float z, uint32_t apply) {
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	DSoundBuffer_LockState(owner);
	if (apply & DS3D_DEFERRED) {
		DSoundCompat_ToMixerCoords(x, y, z, owner->deferred3d.pos);
		DSoundBuffer_MarkDeferred3D(owner, DS3D_PARAM_POSITION);
	} else {
		DSoundCompat_ToMixerCoords(x, y, z, owner->params3d.pos);
		owner->params3d.is3d = 1;
		DSoundBuffer_Push3D(owner);
	}
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

static int DS3DBuffer_SetVelocity(void* self, float x, float y, float z, uint32_t apply) {
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	DSoundBuffer_LockState(owner);
	if (apply & DS3D_DEFERRED) {
		DSoundCompat_ToMixerCoords(x, y, z, owner->deferred3d.vel);
		DSoundBuffer_MarkDeferred3D(owner, DS3D_PARAM_VELOCITY);
	} else {
		DSoundCompat_ToMixerCoords(x, y, z, owner->params3d.vel);
		DSoundBuffer_Push3D(owner);
	}
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

/* The getters return the parameters in use. Settings made with DS3D_DEFERRED
 * only show after CommitDeferredSettings. */
static int DS3DBuffer_GetMaxDistance(void* self, float* dist) {
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	if (!dist) {
		return DS_FAIL;
	}
	DSoundBuffer_LockState(owner);
	*dist = owner->params3d.max_dist;
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

static int DS3DBuffer_GetMinDistance(void* self, float* dist) {
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	if (!dist) {
		return DS_FAIL;
	}
	DSoundBuffer_LockState(owner);
	*dist = owner->params3d.min_dist;
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

static int DS3DBuffer_GetMode(void* self, uint32_t* mode) {
	DSBuffer* owner = ((DS3DBuffer*)self)->owner;
	if (!mode) {
		return DS_FAIL;
	}
	DSoundBuffer_LockState(owner);
	*mode = owner->params3d.is3d ? DS3DMODE_NORMAL : DS3DMODE_DISABLE;
	DSoundBuffer_UnlockState(owner);
	return DS_OK;
}

static int DS3DBuffer_GetPosition(void* self, void* out) {
	DSBuffer*   owner  = ((DS3DBuffer*)self)->owner;
	DS3DVector* vector = (DS3DVector*)out;
	float       pos[3];
	if (!vector) {
		return DS_FAIL;
	}
	DSoundBuffer_LockState(owner);
	memcpy(pos, owner->params3d.pos, sizeof pos);
	DSoundBuffer_UnlockState(owner);
	/* Back to DirectSound coordinates. */
	vector->x = pos[0];
	vector->y = pos[1];
	vector->z = -pos[2];
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
	DS3DBuffer_GetMaxDistance,
	DS3DBuffer_GetMinDistance,
	DS3DBuffer_GetMode,
	DS3DBuffer_GetPosition,
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
 * buffer. It keeps the position, orientation and velocity in mixer
 * coordinates. Settings made with DS3D_IMMEDIATE reach the mixer at once.
 * Settings made with DS3D_DEFERRED wait for CommitDeferredSettings, which
 * sends them in one batch with the deferred settings of every buffer of the
 * device. */
enum {
	DS3D_LISTENER_POSITION    = 1u << 0,
	DS3D_LISTENER_ORIENTATION = 1u << 1,
	DS3D_LISTENER_VELOCITY    = 1u << 2,
	DS3D_LISTENER_DISTANCE    = 1u << 3,
	DS3D_LISTENER_DOPPLER     = 1u << 4,
};

typedef struct DS3DListener {
	const void**       lpVtbl;
	int                refcount;
	DSDevice*          device; /* the device of the primary buffer */
	AeronAudioListener current;
	AeronAudioListener deferred;
	uint32_t           deferred_mask; /* DS3D_LISTENER_* */
	float              distance_factor;
	float              rolloff_factor;
	float              doppler_factor;
	float              deferred_distance_factor;
	float              deferred_doppler_factor;
} DS3DListener;

static DS3DListener g_ds3d_listener;

static int DS3DListener_AddRef(void* self) { return ++((DS3DListener*)self)->refcount; }

static int DS3DListener_Release(void* self) { return --((DS3DListener*)self)->refcount; }

static void DS3DListener_PushDistanceModel(DS3DListener* l) {
	Aeron_AudioSetDistanceModel(l->distance_factor, l->rolloff_factor, l->doppler_factor);
}

static int DS3DListener_SetDistanceFactor(void* self, float value, uint32_t apply) {
	DS3DListener* l = (DS3DListener*)self;
	if (apply & DS3D_DEFERRED) {
		l->deferred_distance_factor = value;
		l->deferred_mask |= DS3D_LISTENER_DISTANCE;
	} else {
		l->distance_factor = value;
		DS3DListener_PushDistanceModel(l);
	}
	return DS_OK;
}

static int DS3DListener_SetDopplerFactor(void* self, float value, uint32_t apply) {
	DS3DListener* l = (DS3DListener*)self;
	if (apply & DS3D_DEFERRED) {
		l->deferred_doppler_factor = value;
		l->deferred_mask |= DS3D_LISTENER_DOPPLER;
	} else {
		l->doppler_factor = value;
		DS3DListener_PushDistanceModel(l);
	}
	return DS_OK;
}

static int DS3DListener_SetOrientation(void* self, float fx, float fy, float fz, float tx, float ty, float tz,
									   uint32_t apply) {
	DS3DListener* l = (DS3DListener*)self;
	if (apply & DS3D_DEFERRED) {
		DSoundCompat_ToMixerCoords(fx, fy, fz, l->deferred.front);
		DSoundCompat_ToMixerCoords(tx, ty, tz, l->deferred.top);
		l->deferred_mask |= DS3D_LISTENER_ORIENTATION;
	} else {
		DSoundCompat_ToMixerCoords(fx, fy, fz, l->current.front);
		DSoundCompat_ToMixerCoords(tx, ty, tz, l->current.top);
		Aeron_AudioSetListener(&l->current);
	}
	return DS_OK;
}

/* The position in use. One set with DS3D_DEFERRED only shows after
 * CommitDeferredSettings. */
static int DS3DListener_GetPosition(void* self, void* out) {
	const DS3DListener* l      = (const DS3DListener*)self;
	DS3DVector*         vector = (DS3DVector*)out;
	if (!vector) {
		return DS_FAIL;
	}
	vector->x = l->current.pos[0];
	vector->y = l->current.pos[1];
	vector->z = -l->current.pos[2];
	return DS_OK;
}

static int DS3DListener_SetPosition(void* self, float x, float y, float z, uint32_t apply) {
	DS3DListener* l = (DS3DListener*)self;
	if (apply & DS3D_DEFERRED) {
		DSoundCompat_ToMixerCoords(x, y, z, l->deferred.pos);
		l->deferred_mask |= DS3D_LISTENER_POSITION;
	} else {
		DSoundCompat_ToMixerCoords(x, y, z, l->current.pos);
		Aeron_AudioSetListener(&l->current);
	}
	return DS_OK;
}

static int DS3DListener_SetVelocity(void* self, float x, float y, float z, uint32_t apply) {
	DS3DListener* l = (DS3DListener*)self;
	if (apply & DS3D_DEFERRED) {
		DSoundCompat_ToMixerCoords(x, y, z, l->deferred.vel);
		l->deferred_mask |= DS3D_LISTENER_VELOCITY;
	} else {
		DSoundCompat_ToMixerCoords(x, y, z, l->current.vel);
		Aeron_AudioSetListener(&l->current);
	}
	return DS_OK;
}

/* Sends the listener and the deferred settings of every buffer in one batch,
 * so no mixed chunk uses some of them without the others. The listener is
 * sent even if nothing changed. */
static int DS3DListener_CommitDeferredSettings(void* self) {
	DS3DListener*  l      = (DS3DListener*)self;
	DSDevice*      device = l->device;
	const uint32_t mask   = l->deferred_mask;

	Aeron_Audio3DBeginBatch();
	if (device) {
		DSBuffer* next;
		for (DSBuffer* b = device->deferred; b; b = next) {
			next             = b->deferred_next;
			b->deferred_next = NULL;
			DSoundBuffer_LockState(b);
			DSoundBuffer_ApplyDeferred3D(b);
			DSoundBuffer_UnlockState(b);
		}
		device->deferred = NULL;
	}
	if (mask & DS3D_LISTENER_POSITION) {
		memcpy(l->current.pos, l->deferred.pos, sizeof l->current.pos);
	}
	if (mask & DS3D_LISTENER_ORIENTATION) {
		memcpy(l->current.front, l->deferred.front, sizeof l->current.front);
		memcpy(l->current.top, l->deferred.top, sizeof l->current.top);
	}
	if (mask & DS3D_LISTENER_VELOCITY) {
		memcpy(l->current.vel, l->deferred.vel, sizeof l->current.vel);
	}
	if (mask & DS3D_LISTENER_DISTANCE) {
		l->distance_factor = l->deferred_distance_factor;
	}
	if (mask & DS3D_LISTENER_DOPPLER) {
		l->doppler_factor = l->deferred_doppler_factor;
	}
	if (mask & (DS3D_LISTENER_DISTANCE | DS3D_LISTENER_DOPPLER)) {
		DS3DListener_PushDistanceModel(l);
	}
	l->deferred_mask = 0;
	Aeron_AudioSetListener(&l->current);
	Aeron_Audio3DEndBatch();
	return DS_OK;
}

static const void* g_ds3d_listener_vtbl[] = {
	NULL,                              /* 0 QueryInterface */
	(const void*)DS3DListener_AddRef,  /* 1 */
	(const void*)DS3DListener_Release, /* 2 */
	NULL,
	NULL,
	NULL,
	NULL,                                  /* 3..6 */
	(const void*)DS3DListener_GetPosition, /* 7 */
	NULL,
	NULL,                                             /* 8..9 */
	NULL,                                             /* 10 SetAllParameters */
	(const void*)DS3DListener_SetDistanceFactor,      /* 11 */
	(const void*)DS3DListener_SetDopplerFactor,       /* 12 */
	(const void*)DS3DListener_SetOrientation,         /* 13 */
	(const void*)DS3DListener_SetPosition,            /* 14 */
	NULL,                                             /* 15 SetRolloffFactor */
	(const void*)DS3DListener_SetVelocity,            /* 16 */
	(const void*)DS3DListener_CommitDeferredSettings, /* 17 */
};

static int DSoundCompat_QueryInterface3DListener(DSDevice* device, void** out) {
	g_ds3d_listener.lpVtbl          = g_ds3d_listener_vtbl;
	g_ds3d_listener.refcount        = 1;
	g_ds3d_listener.device          = device;
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
		return DSoundCompat_QueryInterface3DListener(((DSBuffer*)self)->device, out);
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
	if (b->deferred3d_mask) {
		DSBuffer** link = &b->device->deferred;
		while (*link && *link != b) {
			link = &(*link)->deferred_next;
		}
		if (*link) {
			*link = b->deferred_next;
		}
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

/* The flags are the ones the buffer was created with. */
static int AERON_DXAPI DSoundBuffer_GetCaps(void* self, void* caps) {
	const DSBuffer* b   = (const DSBuffer*)self;
	DSBufferCaps*   out = (DSBufferCaps*)caps;
	if (!out || out->dwSize < sizeof(DSBufferCaps)) {
		return DS_FAIL;
	}
	out->dwFlags              = b->flags;
	out->dwBufferBytes        = b->capacity;
	out->dwUnlockTransferRate = 0;
	out->dwPlayCpuOverhead    = 0;
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
	if (b->params3d.is3d) {
		AeronAudioSource3D source;
		DSoundBuffer_Source3D(b, &source);
		b->voice = Aeron_AudioVoicePlay3DSource(b->storage->clip, gain, pitch, b->looping, &source, frame);
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
	if (g_ds3d_listener.device == d) {
		g_ds3d_listener.device = NULL;
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
		b->params3d.is3d     = (desc->dwFlags & DSBCAPS_CTRL3D) != 0;
		b->params3d.min_dist = 1.0f;
		b->params3d.max_dist = 1000000000.0f;
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
	/* DirectSound copies the 3D parameters in use into the duplicate. Deferred
	   ones stay with the source. */
	dup->params3d = src->params3d;

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
