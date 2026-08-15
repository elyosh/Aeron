#ifndef AERON_FFMPEG_VFS_IO_H
#define AERON_FFMPEG_VFS_IO_H

#include "aeron/vfs.h"

struct AVIOContext;

/* Creates an FFmpeg read/seek adapter for an already-open Aeron file. The file
 * remains owned by the caller and must outlive the returned context. */
struct AVIOContext* AeronFfmpegVfs_CreateIo(AeronFile* file);
void                AeronFfmpegVfs_DestroyIo(struct AVIOContext** context);

#endif
