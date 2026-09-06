#ifndef AERON_COMPAT_DINPUT_H
#define AERON_COMPAT_DINPUT_H

/* DirectInput (5/3) compatibility shim: IDirectInput / IDirectInputDevice for the
 * system keyboard and mouse, backed by the Aeron input snapshot. Lets the recovered
 * input code (dinput.c) run its original DirectInput COM calls unchanged.
 *
 * Only the surface the recovered keyboard/mouse code uses is defined here; the
 * vtable method order matches the real DirectInput A interfaces so the call
 * ordinals line up with the original binary. Joystick / force feedback (tier 2/3)
 * are not covered. */

#include "aeron/compat/win_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IDirectInputA       IDirectInputA;
typedef struct IDirectInputDeviceA IDirectInputDeviceA;

/* Buffered device event (keyboard): dwOfs is the DIK scancode, dwData bit 0x80 is
 * the make/break state. */
typedef struct DIDEVICEOBJECTDATA {
	uint32_t dwOfs;
	uint32_t dwData;
	uint32_t dwTimeStamp;
	uint32_t dwSequence;
} DIDEVICEOBJECTDATA;

/* Immediate mouse state (c_dfDIMouse): relative lX/lY, wheel lZ, four buttons. */
typedef struct DIMOUSESTATE {
	int32_t lX;
	int32_t lY;
	int32_t lZ;
	uint8_t rgbButtons[4];
} DIMOUSESTATE;

/* Property header + DWORD property (used for DIPROP_BUFFERSIZE). */
typedef struct DIPROPHEADER {
	uint32_t dwSize;
	uint32_t dwHeaderSize;
	uint32_t dwObj;
	uint32_t dwHow;
} DIPROPHEADER;

typedef struct DIPROPDWORD {
	DIPROPHEADER diph;
	uint32_t     dwData;
} DIPROPDWORD;

/* Device data format. The shim keys device behavior off the CreateDevice GUID, so it
 * does not parse this; the recovered code only passes c_dfDIKeyboard / c_dfDIMouse. */
typedef struct DIDATAFORMAT {
	uint32_t dwSize;
	uint32_t dwObjSize;
	uint32_t dwFlags;
	uint32_t dwDataSize;
	uint32_t dwNumObjs;
	void*    rgodf;
} DIDATAFORMAT;

/* Cooperative-level flags (SetCooperativeLevel); the original uses FOREGROUND|NONEXCLUSIVE. */
#define DISCL_EXCLUSIVE 0x00000001u
#define DISCL_NONEXCLUSIVE 0x00000002u
#define DISCL_FOREGROUND 0x00000004u
#define DISCL_BACKGROUND 0x00000008u

/* GetDeviceData flag: peek without dequeuing. */
#define DIGDD_PEEK 0x00000001u

/* DIPROP_BUFFERSIZE is a MAKEDIPROP(1) pseudo-GUID pointer. */
#define DINPUT_DIPROP_BUFFERSIZE ((const DxGuid*)1)

/* Result codes the recovered retry loops test. */
#define DI_OK ((HRESULT)0)
#define DI_NOEFFECT ((HRESULT)1) /* S_FALSE: Acquire when already acquired */
#define DIERR_INPUTLOST ((HRESULT)0x8007001E)

typedef struct IDirectInputDeviceAVtbl {
	HRESULT(AERON_DXAPI* QueryInterface)(IDirectInputDeviceA*, DxRefIid, void**);
	uint32_t(AERON_DXAPI* AddRef)(IDirectInputDeviceA*);
	uint32_t(AERON_DXAPI* Release)(IDirectInputDeviceA*);
	HRESULT(AERON_DXAPI* GetCapabilities)(IDirectInputDeviceA*, void*);
	HRESULT(AERON_DXAPI* EnumObjects)(IDirectInputDeviceA*, void*, void*, uint32_t);
	HRESULT(AERON_DXAPI* GetProperty)(IDirectInputDeviceA*, const DxGuid*, DIPROPHEADER*);
	HRESULT(AERON_DXAPI* SetProperty)(IDirectInputDeviceA*, const DxGuid*, const DIPROPHEADER*);
	HRESULT(AERON_DXAPI* Acquire)(IDirectInputDeviceA*);
	HRESULT(AERON_DXAPI* Unacquire)(IDirectInputDeviceA*);
	HRESULT(AERON_DXAPI* GetDeviceState)(IDirectInputDeviceA*, uint32_t, void*);
	HRESULT(AERON_DXAPI* GetDeviceData)(IDirectInputDeviceA*, uint32_t, DIDEVICEOBJECTDATA*, uint32_t*,
										uint32_t);
	HRESULT(AERON_DXAPI* SetDataFormat)(IDirectInputDeviceA*, const DIDATAFORMAT*);
	HRESULT(AERON_DXAPI* SetEventNotification)(IDirectInputDeviceA*, void*);
	HRESULT(AERON_DXAPI* SetCooperativeLevel)(IDirectInputDeviceA*, void*, uint32_t);
	HRESULT(AERON_DXAPI* GetObjectInfo)(IDirectInputDeviceA*, void*, uint32_t, uint32_t);
	HRESULT(AERON_DXAPI* GetDeviceInfo)(IDirectInputDeviceA*, void*);
	HRESULT(AERON_DXAPI* RunControlPanel)(IDirectInputDeviceA*, void*, uint32_t);
	HRESULT(AERON_DXAPI* Initialize)(IDirectInputDeviceA*, void*, uint32_t, DxRefIid);
} IDirectInputDeviceAVtbl;

struct IDirectInputDeviceA {
	const IDirectInputDeviceAVtbl* lpVtbl;
};

/* Forward-declared so the EnumDevices vtbl slot below can take a typed callback
 * (the force-feedback code passes a real callback; the full struct is defined later). */
struct DIDEVICEINSTANCEA;
typedef int(AERON_DXAPI* LPDIENUMDEVICESCALLBACKA)(const struct DIDEVICEINSTANCEA*, void*);

typedef struct IDirectInputAVtbl {
	HRESULT(AERON_DXAPI* QueryInterface)(IDirectInputA*, DxRefIid, void**);
	uint32_t(AERON_DXAPI* AddRef)(IDirectInputA*);
	uint32_t(AERON_DXAPI* Release)(IDirectInputA*);
	HRESULT(AERON_DXAPI* CreateDevice)(IDirectInputA*, const DxGuid*, IDirectInputDeviceA**, void*);
	HRESULT(AERON_DXAPI* EnumDevices)(IDirectInputA*, uint32_t, LPDIENUMDEVICESCALLBACKA, void*, uint32_t);
	HRESULT(AERON_DXAPI* GetDeviceStatus)(IDirectInputA*, const DxGuid*);
	HRESULT(AERON_DXAPI* RunControlPanel)(IDirectInputA*, void*, uint32_t);
	HRESULT(AERON_DXAPI* Initialize)(IDirectInputA*, void*, uint32_t);
} IDirectInputAVtbl;

struct IDirectInputA {
	const IDirectInputAVtbl* lpVtbl;
};

/* System device instance GUIDs (CreateDevice). */
extern const DxGuid GUID_SysKeyboard;
extern const DxGuid GUID_SysMouse;

/* Standard device data formats; the shim ignores their contents. */
extern const DIDATAFORMAT c_dfDIKeyboard;
extern const DIDATAFORMAT c_dfDIMouse;
extern const DIDATAFORMAT c_dfDIJoystick;

/* DirectInput entry point (matches the imported DirectInputCreateA ABI). */
HRESULT AERON_DXAPI DirectInputCreateA(void* hinst, uint32_t version, IDirectInputA** out, void* outer);

/* --- Force feedback (IDirectInputDevice2 / IDirectInputEffect) ------------- *
 * The recovered force-feedback code (forcefeedback.c) drives DirectInput 5 effect
 * objects: it enumerates a force-feedback device, creates constant-force / periodic /
 * condition effects, and starts/stops them. This shim implements that COM surface on
 * top of Aeron two-motor gamepad rumble: constant and periodic effects become rumble
 * pulses scaled by magnitude x effect gain x device gain; condition (spring) effects
 * have no gamepad analog and are accepted as no-ops. */

typedef struct IDirectInputDevice2A IDirectInputDevice2A;
typedef struct IDirectInputEffect   IDirectInputEffect;

/* DIEFFECT.dwFlags */
#define DIEFF_OBJECTOFFSETS 0x00000002u
#define DIEFF_POLAR 0x00000020u

/* DIEFFECT.dwTriggerButton "no trigger". */
#define DIEB_NOTRIGGER 0xFFFFFFFFu

/* DIEFFECT.dwDuration "play until stopped". */
#define DI_INFINITE 0xFFFFFFFFu

/* SetParameters flags (DIEP_*). */
#define DIEP_DURATION 0x00000001u
#define DIEP_SAMPLEPERIOD 0x00000002u
#define DIEP_GAIN 0x00000004u
#define DIEP_TRIGGERBUTTON 0x00000008u
#define DIEP_TRIGGERREPEATINTERVAL 0x00000010u
#define DIEP_AXES 0x00000020u
#define DIEP_DIRECTION 0x00000040u
#define DIEP_ENVELOPE 0x00000080u
#define DIEP_TYPESPECIFICPARAMS 0x00000100u
#define DIEP_START 0x20000000u
#define DIEP_NORESTART 0x40000000u
#define DIEP_NODOWNLOAD 0x80000000u

/* Start flags. */
#define DIES_SOLO 0x00000001u
#define DIES_NODOWNLOAD 0x80000000u

/* EnumDevices device type and flags. */
#define DIDEVTYPE_JOYSTICK 4u
#define DIEDFL_ATTACHEDONLY 0x00000001u
#define DIEDFL_FORCEFEEDBACK 0x00000100u

/* Enumeration callback return values. */
#define DIENUM_STOP 0
#define DIENUM_CONTINUE 1

/* MAKEDIPROP force-feedback device properties (pseudo-GUID pointers). */
#define DINPUT_DIPROP_FFGAIN ((const DxGuid*)7)
#define DINPUT_DIPROP_AUTOCENTER ((const DxGuid*)9)

/* Standard DirectInput force-feedback effect GUIDs used by the recovered code. */
extern const DxGuid GUID_ConstantForce;
extern const DxGuid GUID_Sine;
extern const DxGuid GUID_Spring;
/* Interface id the recovered code QueryInterface's the enumerated device to. */
extern const DxGuid IID_IDirectInputDevice2A;

/* Force-feedback envelope (attack/fade), attached via DIEFFECT.lpEnvelope. */
typedef struct DIENVELOPE {
	uint32_t dwSize;
	uint32_t dwAttackLevel;
	uint32_t dwAttackTime;
	uint32_t dwFadeLevel;
	uint32_t dwFadeTime;
} DIENVELOPE;

/* Type-specific parameter blocks. */
typedef struct DICONSTANTFORCE {
	int32_t lMagnitude;
} DICONSTANTFORCE;

typedef struct DIPERIODIC {
	uint32_t dwMagnitude;
	int32_t  lOffset;
	uint32_t dwPhase;
	uint32_t dwPeriod;
} DIPERIODIC;

typedef struct DICONDITION {
	int32_t  lOffset;
	int32_t  lPositiveCoefficient;
	int32_t  lNegativeCoefficient;
	uint32_t dwPositiveSaturation;
	uint32_t dwNegativeSaturation;
	int32_t  lDeadBand;
} DICONDITION;

/* DirectInput 5 effect descriptor (original dwSize == 52 on 32-bit). */
typedef struct DIEFFECT {
	uint32_t    dwSize;
	uint32_t    dwFlags;
	uint32_t    dwDuration;
	uint32_t    dwSamplePeriod;
	uint32_t    dwGain;
	uint32_t    dwTriggerButton;
	uint32_t    dwTriggerRepeatInterval;
	uint32_t    cAxes;
	uint32_t*   rgdwAxes;
	int32_t*    rglDirection;
	DIENVELOPE* lpEnvelope;
	uint32_t    cbTypeSpecificParams;
	void*       lpvTypeSpecificParams;
} DIEFFECT;

/* Effect capability record returned by GetEffectInfo. */
typedef struct DIEFFECTINFOA {
	uint32_t dwSize;
	DxGuid   guid;
	uint32_t dwEffType;
	uint32_t dwStaticParams;
	uint32_t dwDynamicParams;
	char     tszName[260];
} DIEFFECTINFOA;

/* Device instance record passed to the EnumDevices callback. */
typedef struct DIDEVICEINSTANCEA {
	uint32_t dwSize;
	DxGuid   guidInstance;
	DxGuid   guidProduct;
	uint32_t dwDevType;
	char     tszInstanceName[260];
	char     tszProductName[260];
	DxGuid   guidFFDriver;
	uint16_t wUsagePage;
	uint16_t wUsage;
} DIDEVICEINSTANCEA;

/* DirectInput enumeration callbacks return DIENUM_STOP / DIENUM_CONTINUE (a Win32
 * BOOL; typed int here since the shim's dx types do not define BOOL). */
typedef int(AERON_DXAPI* LPDIENUMEFFECTSCALLBACKA)(const DIEFFECTINFOA*, void*);

typedef struct IDirectInputEffectVtbl {
	HRESULT(AERON_DXAPI* QueryInterface)(IDirectInputEffect*, DxRefIid, void**);
	uint32_t(AERON_DXAPI* AddRef)(IDirectInputEffect*);
	uint32_t(AERON_DXAPI* Release)(IDirectInputEffect*);
	HRESULT(AERON_DXAPI* Initialize)(IDirectInputEffect*, void*, uint32_t, DxRefIid);
	HRESULT(AERON_DXAPI* GetEffectGuid)(IDirectInputEffect*, DxGuid*);
	HRESULT(AERON_DXAPI* GetParameters)(IDirectInputEffect*, DIEFFECT*, uint32_t);
	HRESULT(AERON_DXAPI* SetParameters)(IDirectInputEffect*, const DIEFFECT*, uint32_t);
	HRESULT(AERON_DXAPI* Start)(IDirectInputEffect*, uint32_t, uint32_t);
	HRESULT(AERON_DXAPI* Stop)(IDirectInputEffect*);
	HRESULT(AERON_DXAPI* GetEffectStatus)(IDirectInputEffect*, uint32_t*);
	HRESULT(AERON_DXAPI* Download)(IDirectInputEffect*);
	HRESULT(AERON_DXAPI* Unload)(IDirectInputEffect*);
	HRESULT(AERON_DXAPI* Escape)(IDirectInputEffect*, void*);
} IDirectInputEffectVtbl;

struct IDirectInputEffect {
	const IDirectInputEffectVtbl* lpVtbl;
};

/* IDirectInputDevice2A: the base IDirectInputDeviceA methods followed by the
 * force-feedback methods (CreateEffect / EnumEffects / GetEffectInfo / ...). */
typedef struct IDirectInputDevice2AVtbl {
	HRESULT(AERON_DXAPI* QueryInterface)(IDirectInputDevice2A*, DxRefIid, void**);
	uint32_t(AERON_DXAPI* AddRef)(IDirectInputDevice2A*);
	uint32_t(AERON_DXAPI* Release)(IDirectInputDevice2A*);
	HRESULT(AERON_DXAPI* GetCapabilities)(IDirectInputDevice2A*, void*);
	HRESULT(AERON_DXAPI* EnumObjects)(IDirectInputDevice2A*, void*, void*, uint32_t);
	HRESULT(AERON_DXAPI* GetProperty)(IDirectInputDevice2A*, const DxGuid*, DIPROPHEADER*);
	HRESULT(AERON_DXAPI* SetProperty)(IDirectInputDevice2A*, const DxGuid*, const DIPROPHEADER*);
	HRESULT(AERON_DXAPI* Acquire)(IDirectInputDevice2A*);
	HRESULT(AERON_DXAPI* Unacquire)(IDirectInputDevice2A*);
	HRESULT(AERON_DXAPI* GetDeviceState)(IDirectInputDevice2A*, uint32_t, void*);
	HRESULT(AERON_DXAPI* GetDeviceData)(IDirectInputDevice2A*, uint32_t, DIDEVICEOBJECTDATA*, uint32_t*,
										uint32_t);
	HRESULT(AERON_DXAPI* SetDataFormat)(IDirectInputDevice2A*, const DIDATAFORMAT*);
	HRESULT(AERON_DXAPI* SetEventNotification)(IDirectInputDevice2A*, void*);
	HRESULT(AERON_DXAPI* SetCooperativeLevel)(IDirectInputDevice2A*, void*, uint32_t);
	HRESULT(AERON_DXAPI* GetObjectInfo)(IDirectInputDevice2A*, void*, uint32_t, uint32_t);
	HRESULT(AERON_DXAPI* GetDeviceInfo)(IDirectInputDevice2A*, void*);
	HRESULT(AERON_DXAPI* RunControlPanel)(IDirectInputDevice2A*, void*, uint32_t);
	HRESULT(AERON_DXAPI* Initialize)(IDirectInputDevice2A*, void*, uint32_t, DxRefIid);
	HRESULT(AERON_DXAPI* CreateEffect)(IDirectInputDevice2A*, DxRefIid, const DIEFFECT*, IDirectInputEffect**,
									   void*);
	HRESULT(AERON_DXAPI* EnumEffects)(IDirectInputDevice2A*, LPDIENUMEFFECTSCALLBACKA, void*, uint32_t);
	HRESULT(AERON_DXAPI* GetEffectInfo)(IDirectInputDevice2A*, DIEFFECTINFOA*, DxRefIid);
	HRESULT(AERON_DXAPI* GetForceFeedbackState)(IDirectInputDevice2A*, uint32_t*);
	HRESULT(AERON_DXAPI* SendForceFeedbackCommand)(IDirectInputDevice2A*, uint32_t);
	HRESULT(AERON_DXAPI* EnumCreatedEffectObjects)(IDirectInputDevice2A*, void*, void*, uint32_t);
	HRESULT(AERON_DXAPI* Escape)(IDirectInputDevice2A*, void*);
	HRESULT(AERON_DXAPI* Poll)(IDirectInputDevice2A*);
	HRESULT(AERON_DXAPI* SendDeviceData)(IDirectInputDevice2A*, uint32_t, const void*, uint32_t*, uint32_t);
} IDirectInputDevice2AVtbl;

struct IDirectInputDevice2A {
	const IDirectInputDevice2AVtbl* lpVtbl;
};

#ifdef __cplusplus
}
#endif

#endif
