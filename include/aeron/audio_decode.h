#ifndef AERON_AUDIO_DECODE_H
#define AERON_AUDIO_DECODE_H

#include "aeron/vfs.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronAudioDecoder AeronAudioDecoder;

typedef struct AeronAudioDecoderInfo {
	int     sample_rate;
	int     channels;
	int64_t duration_us;
} AeronAudioDecoderInfo;

/* Opens through an Aeron VFS on the calling thread. The returned decoder owns
 * the resulting file handle. */
AeronAudioDecoder* Aeron_AudioDecoderOpen(AeronVfs* vfs, AeronVfsRoot root, const char* path);
/* Takes ownership of an already-open file. This is the worker-safe entry point
 * when a VFS-confined owner transfers a file handle to a decoder thread. */
AeronAudioDecoder* Aeron_AudioDecoderOpenFile(AeronFile* file, const char* debug_name);
void               Aeron_AudioDecoderGetInfo(const AeronAudioDecoder* decoder, AeronAudioDecoderInfo* out);
/* Produces interleaved stereo signed-16 PCM at the source sample rate. */
size_t Aeron_AudioDecoderRead(AeronAudioDecoder* decoder, int16_t* pcm, size_t max_frames);
int    Aeron_AudioDecoderSeekUs(AeronAudioDecoder* decoder, int64_t position_us);
void   Aeron_AudioDecoderClose(AeronAudioDecoder* decoder);

#ifdef __cplusplus
}
#endif

#endif
