#ifndef AERON_VFS_ISO9660_H
#define AERON_VFS_ISO9660_H

#include "aeron/vfs.h"

#include <SDL3/SDL_iostream.h>

typedef struct AeronIso9660 AeronIso9660;

AeronIso9660* AeronIso9660_Open(const char* source_path);
void          AeronIso9660_Destroy(AeronIso9660* iso);
SDL_IOStream* AeronIso9660_OpenFile(const AeronIso9660* iso, const char* path, int case_insensitive);
int           AeronIso9660_Stat(const AeronIso9660* iso, const char* path, int case_insensitive,
								AeronFileInfo* out_info);
int AeronIso9660_Glob(const AeronIso9660* iso, const char* directory, const char* pattern, uint32_t flags,
					  int root_case_insensitive, AeronVfsGlobCallback callback, void* userdata);

#endif
