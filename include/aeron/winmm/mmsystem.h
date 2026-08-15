#ifndef AERON_WINMM_MMSYSTEM_H
#define AERON_WINMM_MMSYSTEM_H

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

MMRESULT AERON_WINMMAPI AeronWinmm_MciSendCommandA(MCIDEVICEID device_id, uint32_t message, MciDwordPtr flags,
												   MciDwordPtr params);
uint32_t AERON_WINMMAPI AeronWinmm_AuxGetNumDevs(void);
MMRESULT AERON_WINMMAPI AeronWinmm_AuxGetDevCapsA(uintptr_t device_id, AUXCAPSA* caps, uint32_t size);
MMRESULT AERON_WINMMAPI AeronWinmm_AuxGetVolume(uintptr_t device_id, uint32_t* volume);
MMRESULT AERON_WINMMAPI AeronWinmm_AuxSetVolume(uintptr_t device_id, uint32_t volume);

#ifdef __cplusplus
}
#endif

#endif
