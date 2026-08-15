#ifndef AERON_WINMM_COMPAT_H
#define AERON_WINMM_COMPAT_H

#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronWinmmCdAudioDesc {
	AeronVfs*    vfs;
	AeronVfsRoot root;
	const char*  directory;
} AeronWinmmCdAudioDesc;

int  AeronWinmm_ConfigureCdAudio(const AeronWinmmCdAudioDesc* desc);
void AeronWinmm_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
