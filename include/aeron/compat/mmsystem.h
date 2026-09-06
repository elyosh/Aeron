#ifndef AERON_COMPAT_MMSYSTEM_H
#define AERON_COMPAT_MMSYSTEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t  MMRESULT;
typedef uint32_t  MCIDEVICEID;
typedef uintptr_t MciDwordPtr;

#if defined(_WIN32) && defined(_M_IX86)
#define AERON_WINMMAPI __stdcall
#else
#define AERON_WINMMAPI
#endif

/* On 32-bit Windows matching builds the entry points stay dllimport so
 * recovered call sites go through the import thunk. */
#if defined(_WIN32) && defined(_M_IX86) && !defined(AERON_WINMM_COMPAT_IMPLEMENTATION)
#define AERON_WINMMIMPORT __declspec(dllimport)
#else
#define AERON_WINMMIMPORT
#endif

enum {
	MMSYSERR_NOERROR            = 0,
	MMSYSERR_BADDEVICEID        = 2,
	MCIERR_INVALID_DEVICE_ID    = 257,
	MCIERR_UNRECOGNIZED_COMMAND = 261,
	MCIERR_UNSUPPORTED_FUNCTION = 274,
	MCIERR_OUTOFRANGE           = 282,
};

enum {
	MCI_OPEN   = 0x0803,
	MCI_CLOSE  = 0x0804,
	MCI_PLAY   = 0x0806,
	MCI_STOP   = 0x0808,
	MCI_SET    = 0x080D,
	MCI_STATUS = 0x0814,
};

enum {
	MCI_NOTIFY          = 0x00000001,
	MCI_WAIT            = 0x00000002,
	MCI_FROM            = 0x00000004,
	MCI_TO              = 0x00000008,
	MCI_TRACK           = 0x00000010,
	MCI_STATUS_ITEM     = 0x00000100,
	MCI_SET_TIME_FORMAT = 0x00000400,
	MCI_OPEN_TYPE       = 0x00002000,
};

enum {
	MCI_STATUS_LENGTH           = 0x00000001,
	MCI_STATUS_NUMBER_OF_TRACKS = 0x00000003,
	MCI_FORMAT_TMSF             = 10,
};

#define MCI_MAKE_TMSF(track, minute, second, frame)                                                          \
	((uint32_t)(uint8_t)(track) | ((uint32_t)(uint8_t)(minute) << 8) | ((uint32_t)(uint8_t)(second) << 16) | \
	 ((uint32_t)(uint8_t)(frame) << 24))
#define MCI_TMSF_TRACK(value) ((uint8_t)((uint32_t)(value) & 0xFFu))
#define MCI_TMSF_MINUTE(value) ((uint8_t)(((uint32_t)(value) >> 8) & 0xFFu))
#define MCI_TMSF_SECOND(value) ((uint8_t)(((uint32_t)(value) >> 16) & 0xFFu))
#define MCI_TMSF_FRAME(value) ((uint8_t)(((uint32_t)(value) >> 24) & 0xFFu))
#define MCI_MSF_MINUTE(value) ((uint8_t)((uint32_t)(value) & 0xFFu))
#define MCI_MSF_SECOND(value) ((uint8_t)(((uint32_t)(value) >> 8) & 0xFFu))
#define MCI_MSF_FRAME(value) ((uint8_t)(((uint32_t)(value) >> 16) & 0xFFu))

typedef struct MCI_OPEN_PARMSA {
	MciDwordPtr dwCallback;
	MCIDEVICEID wDeviceID;
	const char* lpstrDeviceType;
	const char* lpstrElementName;
	const char* lpstrAlias;
} MCI_OPEN_PARMSA;

typedef struct MCI_SET_PARMS {
	MciDwordPtr dwCallback;
	uint32_t    dwTimeFormat;
	uint32_t    dwAudio;
} MCI_SET_PARMS;

typedef struct MCI_STATUS_PARMS {
	MciDwordPtr dwCallback;
	MciDwordPtr dwReturn;
	uint32_t    dwItem;
	uint32_t    dwTrack;
} MCI_STATUS_PARMS;

typedef struct MCI_PLAY_PARMS {
	MciDwordPtr dwCallback;
	uint32_t    dwFrom;
	uint32_t    dwTo;
} MCI_PLAY_PARMS;

typedef struct MCI_GENERIC_PARMS {
	MciDwordPtr dwCallback;
} MCI_GENERIC_PARMS;

enum {
	AUXCAPS_CDAUDIO  = 1,
	AUXCAPS_VOLUME   = 0x0001,
	AUXCAPS_LRVOLUME = 0x0002,
};

typedef struct AUXCAPSA {
	uint16_t wMid;
	uint16_t wPid;
	uint32_t vDriverVersion;
	char     szPname[32];
	uint16_t wTechnology;
	uint16_t wReserved1;
	uint32_t dwSupport;
} AUXCAPSA;

typedef char AeronWinmmAuxCapsSizeCheck[(sizeof(AUXCAPSA) == 48) ? 1 : -1];

AERON_WINMMIMPORT MMRESULT AERON_WINMMAPI mciSendCommandA(MCIDEVICEID device_id, uint32_t message,
														  MciDwordPtr flags, void* params);
AERON_WINMMIMPORT uint32_t AERON_WINMMAPI auxGetNumDevs(void);
AERON_WINMMIMPORT MMRESULT AERON_WINMMAPI auxGetDevCapsA(uintptr_t device_id, AUXCAPSA* caps, uint32_t size);
AERON_WINMMIMPORT MMRESULT AERON_WINMMAPI auxGetVolume(uintptr_t device_id, uint32_t* volume);
AERON_WINMMIMPORT MMRESULT AERON_WINMMAPI auxSetVolume(uintptr_t device_id, uint32_t volume);

/* --- WinMM joystick API ---------------------------------------------------
 *
 * Original joyGetNumDevs / joyGetDevCapsA / joyGetPosEx entry points, filled
 * from the logical controller state registered through
 * AeronCompat_SetJoystickSource (aeron/compat/host.h). Recovered game code
 * keeps its original call sites; on 32-bit Windows matching builds the
 * declarations stay dllimport so calls go through the import thunk. */

#define JOYERR_NOERROR 0u
#define JOYERR_PARMS 165u
#define JOYERR_UNPLUGGED 167u

/* JOYCAPS.wCaps bits used by the recovered code. */
#define JOYCAPS_HASZ 0x0001u
#define JOYCAPS_HASR 0x0002u
#define JOYCAPS_HASU 0x0004u
#define JOYCAPS_HASV 0x0008u
#define JOYCAPS_HASPOV 0x0010u

/* JOYINFOEX.dwFlags bits (which fields joyGetPosEx returns). The shim fills
 * every axis regardless, so these are accepted and not required. */
#define JOY_RETURNX 0x00000001u
#define JOY_RETURNY 0x00000002u
#define JOY_RETURNZ 0x00000004u
#define JOY_RETURNR 0x00000008u
#define JOY_RETURNPOV 0x00000040u
#define JOY_RETURNBUTTONS 0x00000080u
#define JOY_RETURNCENTERED 0x00000400u
#define JOY_RETURNALL 0x000000FFu

/* Centered POV value. */
#define JOY_POVCENTERED 0xFFFFu

/* joyGetDevCapsA capabilities structure (ANSI). Full Win32 layout so the
 * recovered code's sizeof/0x194 argument and field offsets match. */
typedef struct tagJOYCAPSA {
	uint16_t wMid;
	uint16_t wPid;
	char     szPname[32];
	uint32_t wXmin;
	uint32_t wXmax;
	uint32_t wYmin;
	uint32_t wYmax;
	uint32_t wZmin;
	uint32_t wZmax;
	uint32_t wNumButtons;
	uint32_t wPeriodMin;
	uint32_t wPeriodMax;
	uint32_t wRmin;
	uint32_t wRmax;
	uint32_t wUmin;
	uint32_t wUmax;
	uint32_t wVmin;
	uint32_t wVmax;
	uint32_t wCaps;
	uint32_t wMaxAxes;
	uint32_t wNumAxes;
	uint32_t wMaxButtons;
	char     szRegKey[32];
	char     szOEMVxD[260];
} JOYCAPSA;

/* joyGetPosEx extended position structure. */
typedef struct joyinfoex_tag {
	uint32_t dwSize;
	uint32_t dwFlags;
	uint32_t dwXpos;
	uint32_t dwYpos;
	uint32_t dwZpos;
	uint32_t dwRpos;
	uint32_t dwUpos;
	uint32_t dwVpos;
	uint32_t dwButtons;
	uint32_t dwButtonNumber;
	uint32_t dwPOV;
	uint32_t dwReserved1;
	uint32_t dwReserved2;
} JOYINFOEX;

/* Number of joystick devices (recovered code treats >0 as "devices exist"). */
AERON_WINMMIMPORT uint32_t AERON_WINMMAPI joyGetNumDevs(void);

/* Fills caps for device uJoyID; JOYERR_NOERROR if present, else an error. */
AERON_WINMMIMPORT MMRESULT AERON_WINMMAPI joyGetDevCapsA(uint32_t uJoyID, JOYCAPSA* pjc, uint32_t cbjc);

/* Fills the extended position for device uJoyID from the registered source. */
AERON_WINMMIMPORT MMRESULT AERON_WINMMAPI joyGetPosEx(uint32_t uJoyID, JOYINFOEX* pji);

#ifdef __cplusplus
}
#endif

#endif
