#ifndef AERON_COMPAT_DSOUND_H
#define AERON_COMPAT_DSOUND_H

#include "aeron/compat/win_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Port-owned DirectSound compatibility shim.
 *
 * The recovered game audio code (flight SFX, frontend UI sound, and the iMUSE
 * music engine) was ported to call DirectSound COM objects through their
 * vtables. This shim provides those objects with real C vtables laid out at the
 * exact DirectSound ABI indices, backed by the generic Aeron mixer. It is the
 * single replacement for dsound.dll / A3D: recovered code keeps issuing the
 * same vtable calls, and this layer translates them into Aeron clips, voices
 * and streams.
 *
 * HRESULT convention: methods return 0 (DS_OK) on success and a negative value
 * on failure, matching the original `result >= 0` success tests. */

enum { DSSCL_PRIORITY = 2 };

/* WAVEFORMATEX-compatible PCM format descriptor (matches the on-disk WAV
 * `fmt ` chunk layout). */
#pragma pack(push, 1)

typedef struct DSWaveFormat {
	uint16_t wFormatTag;
	uint16_t nChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
	uint16_t cbSize;
} DSWaveFormat;

/* DSBUFFERDESC-compatible. Mirrors imsound.c's ImDSBufferDescCompat field for
 * field, so both consumers can share the shim's CreateSoundBuffer. */
typedef struct DSBufferDesc {
	uint32_t      dwSize;
	uint32_t      dwFlags;
	uint32_t      dwBufferBytes;
	uint32_t      dwReserved;
	DSWaveFormat* lpwfxFormat;
} DSBufferDesc;

typedef struct DSBufferCaps {
	uint32_t dwSize;
	uint32_t dwFlags;
	uint32_t dwBufferBytes;
	uint32_t dwUnlockTransferRate;
	uint32_t dwPlayCpuOverhead;
} DSBufferCaps;

/* DSCAPS-compatible device capability descriptor used by the flight sound
 * engine to determine the number of hardware 3D buffers. */
typedef struct DSoundDeviceCaps {
	uint32_t dwSize;
	uint32_t dwFlags;
	uint32_t dwMinSecondarySampleRate;
	uint32_t dwMaxSecondarySampleRate;
	uint32_t dwPrimaryBuffers;
	uint32_t dwMaxHwMixingAllBuffers;
	uint32_t dwFreeHwMixingAllBuffers;
	uint32_t dwMaxHwMixingStaticBuffers;
	uint32_t dwFreeHwMixingStaticBuffers;
	uint32_t dwMaxHwMixingStreamingBuffers;
	uint32_t dwFreeHwMixingStreamingBuffers;
	uint32_t dwMaxHw3DAllBuffers;
	uint32_t dwFreeHw3DAllBuffers;
	uint32_t dwMaxHw3DStaticBuffers;
	uint32_t dwFreeHw3DStaticBuffers;
	uint32_t dwMaxHw3DStreamingBuffers;
	uint32_t dwFreeHw3DStreamingBuffers;
	uint32_t dwTotalHwMemBytes;
	uint32_t dwFreeHwMemBytes;
	uint32_t dwMaxContigFreeHwMemBytes;
	uint32_t dwUnlockTransferRateHwBuffers;
	uint32_t dwPlayCpuOverheadSwBuffers;
	uint32_t dwReserved1;
	uint32_t dwReserved2;
} DSoundDeviceCaps;

#pragma pack(pop)

/* DirectSound buffer capability/playback flags actually used by recovered code. */
enum {
	DSBCAPS_PRIMARYBUFFER       = 0x00000001,
	DSBCAPS_STATIC              = 0x00000002,
	DSBCAPS_LOCHARDWARE         = 0x00000004,
	DSBCAPS_LOCSOFTWARE         = 0x00000008,
	DSBCAPS_CTRL3D              = 0x00000010,
	DSBCAPS_CTRLFREQUENCY       = 0x00000020,
	DSBCAPS_CTRLPAN             = 0x00000040,
	DSBCAPS_CTRLVOLUME          = 0x00000080,
	DSBCAPS_MUTE3DATMAXDISTANCE = 0x00020000,
	DSBLOCK_ENTIREBUFFER        = 0x00000002,
	DSBPLAY_LOOPING             = 0x00000001,
	DSBSTATUS_PLAYING           = 0x00000001,
	DSBSTATUS_LOOPING           = 0x00000004,
	/* DirectSound DSBCAPS_GETCURRENTPOSITION2: the buffer's owner polls the play
	 * cursor, i.e. it is used as a streaming buffer. iMUSE already sets this on
	 * its music buffer; the shim uses it to back a buffer with an Aeron ring
	 * instead of a static clip. */
	DSBCAPS_GETCURRENTPOSITION2 = 0x00010000
};

/* COM-style interface id (GUID layout). The recovered flight code passes these
 * to QueryInterface to obtain the DirectSound3D sub-interfaces. */
typedef struct DSCompatGuid {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t  data4[8];
} DSCompatGuid;

extern const DSCompatGuid IID_IDirectSound3DBuffer;
extern const DSCompatGuid IID_IDirectSound3DListener;

/* DirectSound device ABI. The shim and recovered callers share these slots. */
typedef struct IDirectSoundVtbl {
	int(AERON_DXAPI* QueryInterface)(void* self, const void* iid, void** out);
	int(AERON_DXAPI* AddRef)(void* self);
	int(AERON_DXAPI* Release)(void* self);
	int(AERON_DXAPI* CreateSoundBuffer)(void* self, const DSBufferDesc* desc, void** buffer, void* outer);
	int(AERON_DXAPI* GetCaps)(void* self, void* caps);
	int(AERON_DXAPI* DuplicateSoundBuffer)(void* self, void* source, void** duplicate);
	int(AERON_DXAPI* SetCooperativeLevel)(void* self, void* hwnd, uint32_t level);
	int(AERON_DXAPI* Compact)(void* self);
	int(AERON_DXAPI* GetSpeakerConfig)(void* self, uint32_t* config);
	int(AERON_DXAPI* SetSpeakerConfig)(void* self, uint32_t config);
	int(AERON_DXAPI* Initialize)(void* self, const void* guid);
} IDirectSoundVtbl;

typedef struct IDirectSound {
	const IDirectSoundVtbl* lpVtbl;
} IDirectSound;

/* Shared buffer ABI. Keep all 21 original slots, including Stop at slot 18.
 * The opaque self arguments also accept the shim's private buffer objects. */
typedef struct IDirectSoundBufferVtbl {
	int(AERON_DXAPI* QueryInterface)(void* self, const void* iid, void** out);            /* 0 */
	int(AERON_DXAPI* AddRef)(void* self);                                                 /* 1 */
	int(AERON_DXAPI* Release)(void* self);                                                /* 2 */
	int(AERON_DXAPI* GetCaps)(void* self, void* caps);                                    /* 3 */
	int(AERON_DXAPI* GetCurrentPosition)(void* self, uint32_t* play, uint32_t* write);    /* 4 */
	int(AERON_DXAPI* GetFormat)(void* self, void* fmt, uint32_t size, uint32_t* written); /* 5 */
	int(AERON_DXAPI* GetVolume)(void* self, int32_t* volume);                             /* 6 */
	int(AERON_DXAPI* GetPan)(void* self, int32_t* pan);                                   /* 7 */
	int(AERON_DXAPI* GetFrequency)(void* self, uint32_t* frequency);                      /* 8 */
	int(AERON_DXAPI* GetStatus)(void* self, uint32_t* status);                            /* 9 */
	int(AERON_DXAPI* Initialize)(void* self, void* device, const void* desc);             /* 10 */
	int(AERON_DXAPI* Lock)(void* self, uint32_t offset, uint32_t bytes, void** p1, uint32_t* b1, void** p2,
						   uint32_t* b2, uint32_t flags);                                      /* 11 */
	int(AERON_DXAPI* Play)(void* self, uint32_t reserved1, uint32_t priority, uint32_t flags); /* 12 */
	int(AERON_DXAPI* SetCurrentPosition)(void* self, uint32_t position);                       /* 13 */
	int(AERON_DXAPI* SetFormat)(void* self, const void* fmt);                                  /* 14 */
	int(AERON_DXAPI* SetVolume)(void* self, int32_t volume);                                   /* 15 */
	int(AERON_DXAPI* SetPan)(void* self, int32_t pan);                                         /* 16 */
	int(AERON_DXAPI* SetFrequency)(void* self, uint32_t frequency);                            /* 17 */
	int(AERON_DXAPI* Stop)(void* self);                                                        /* 18 */
	int(AERON_DXAPI* Unlock)(void* self, void* p1, uint32_t b1, void* p2, uint32_t b2);        /* 19 */
	int(AERON_DXAPI* Restore)(void* self);                                                     /* 20 */
} IDirectSoundBufferVtbl;

typedef struct IDirectSoundBuffer {
	const IDirectSoundBufferVtbl* lpVtbl;
} IDirectSoundBuffer;

/* Creates the IDirectSound shim device backed by the Aeron mixer, replacing
 * dsound.dll's DirectSoundCreate / A3D_CreateDirectSound with the original
 * entry-point signature. The device GUID and aggregation pointer are accepted
 * and ignored. Returns >= 0 on success and stores the opaque device pointer
 * (callable through its vtable, e.g. lpVtbl->CreateSoundBuffer) in
 * *outDevice. */
int AERON_DXAPI DirectSoundCreate(const DSCompatGuid* device_guid, void** outDevice, void* outerUnknown);

#ifdef __cplusplus
}
#endif

#endif
