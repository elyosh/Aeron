#include "ffmpeg_vfs_io.h"

#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include <errno.h>
#include <stdio.h>

enum { AERON_FFMPEG_AVIO_BUFFER_SIZE = 32768 };

static int AeronFfmpegVfs_Read(void* opaque, uint8_t* buffer, int buffer_size) {
	AeronFile* file      = (AeronFile*)opaque;
	size_t     read_size = 0;
	int        read_ok;

	if (buffer_size <= 0)
		return 0;
	read_ok = AeronVfs_Read(file, buffer, (size_t)buffer_size, &read_size);
	if (read_size > 0)
		return (int)read_size;
	if (!read_ok && AeronVfs_Tell(file) < AeronVfs_GetSize(file))
		return AVERROR(EIO);
	return AVERROR_EOF;
}

static int64_t AeronFfmpegVfs_Seek(void* opaque, int64_t offset, int whence) {
	AeronFile* file = (AeronFile*)opaque;
	int        origin;

	if (whence == AVSEEK_SIZE)
		return AeronVfs_GetSize(file);
	switch (whence & ~AVSEEK_FORCE) {
		case SEEK_SET:
			origin = 0;
			break;
		case SEEK_CUR:
			origin = 1;
			break;
		case SEEK_END:
			origin = 2;
			break;
		default:
			return AVERROR(EINVAL);
	}
	if (!AeronVfs_Seek(file, offset, origin))
		return AVERROR(EIO);
	return AeronVfs_Tell(file);
}

AVIOContext* AeronFfmpegVfs_CreateIo(AeronFile* file) {
	uint8_t*     buffer;
	AVIOContext* context;

	if (!file)
		return NULL;
	buffer = (uint8_t*)av_malloc(AERON_FFMPEG_AVIO_BUFFER_SIZE);
	if (!buffer)
		return NULL;
	context = avio_alloc_context(buffer, AERON_FFMPEG_AVIO_BUFFER_SIZE, 0, file, AeronFfmpegVfs_Read, NULL,
								 AeronFfmpegVfs_Seek);
	if (!context)
		av_free(buffer);
	return context;
}

void AeronFfmpegVfs_DestroyIo(AVIOContext** context) {
	if (!context || !*context)
		return;
	av_freep(&(*context)->buffer);
	avio_context_free(context);
}
