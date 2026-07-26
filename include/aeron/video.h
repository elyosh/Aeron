#ifndef AERON_VIDEO_H
#define AERON_VIDEO_H

#include "aeron/render.h"
#include "aeron/vfs.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronVideoPlayer AeronVideoPlayer;

typedef enum AeronVideoState {
	AERON_VIDEO_OPENING,
	AERON_VIDEO_BUFFERING,
	AERON_VIDEO_PLAYING,
	AERON_VIDEO_PAUSED,
	AERON_VIDEO_ENDED,
	AERON_VIDEO_ERROR
} AeronVideoState;

typedef enum AeronVideoScaleMode {
	AERON_VIDEO_SCALE_CONTAIN,
	AERON_VIDEO_SCALE_COVER,
	AERON_VIDEO_SCALE_STRETCH
} AeronVideoScaleMode;

typedef enum AeronVideoFilter { AERON_VIDEO_FILTER_LINEAR, AERON_VIDEO_FILTER_NEAREST } AeronVideoFilter;

typedef struct AeronVideoOpenDesc {
	AeronVfs*    vfs;
	AeronVfsRoot root;
	const char*  path;
	int          autoplay;
	int          loop;
	float        gain;
	uint32_t     video_queue_frames; /* zero selects the engine default */
	uint32_t     audio_buffer_ms;    /* zero selects the engine default */
	int          max_width;          /* zero selects the engine default */
	int          max_height;         /* zero selects the engine default */
	size_t       max_frame_bytes;    /* zero selects the engine default */
} AeronVideoOpenDesc;

typedef struct AeronVideoPresentDesc {
	AeronRectI          bounds;
	AeronVideoScaleMode scale_mode;
	AeronVideoFilter    filter;
	AeronLayerBlendMode blend_mode;
	int                 tint_enabled;
	float               tint_rgba[4];
	AeronRectI          scissor;
} AeronVideoPresentDesc;

typedef struct AeronVideoInfo {
	int     width;
	int     height;
	int     display_width;
	int     display_height;
	int     rotation_degrees;
	int     has_video;
	int     has_audio;
	int     audio_channels;
	int     audio_sample_rate;
	int64_t duration_us; /* -1 when unknown */
	int     frame_rate_num;
	int     frame_rate_den;
	char    container[64];
	char    video_codec[64];
	char    audio_codec[64];
} AeronVideoInfo;

typedef struct AeronVideoStats {
	uint64_t video_frames_decoded;
	uint64_t video_frames_presented;
	uint64_t video_frames_dropped;
	uint64_t audio_underruns;
	uint64_t audio_frames_trimmed;
	uint32_t queued_video_frames;
	uint64_t queued_audio_frames;
	int64_t  position_us;
	int64_t  duration_us;
} AeronVideoStats;

/* Opens a VFS file and starts asynchronous stream discovery. The returned
 * player is main-thread confined; its private decoder worker owns the moved
 * AeronFile and all FFmpeg objects. */
AeronVideoPlayer* Aeron_VideoOpen(const AeronVideoOpenDesc* desc);
void              Aeron_VideoClose(AeronVideoPlayer* player);

void Aeron_VideoPlay(AeronVideoPlayer* player);
void Aeron_VideoPause(AeronVideoPlayer* player);
void Aeron_VideoStop(AeronVideoPlayer* player);
int  Aeron_VideoSeek(AeronVideoPlayer* player, int64_t position_us);
void Aeron_VideoSetGain(AeronVideoPlayer* player, float gain);

/* Advances state and promotes all due decoded frames without blocking. */
void Aeron_VideoUpdate(AeronVideoPlayer* player, uint64_t now_us);
/* Queues the current frame as an Aeron layer. Returns zero when no frame is ready. */
int Aeron_VideoSubmit(AeronVideoPlayer* player, const AeronVideoPresentDesc* desc);

AeronVideoState Aeron_VideoGetState(const AeronVideoPlayer* player);
int64_t         Aeron_VideoGetPositionUs(const AeronVideoPlayer* player);
uint64_t        Aeron_VideoGetPresentedFrameIndex(const AeronVideoPlayer* player);
uint64_t        Aeron_VideoNextWakeDeadlineUs(const AeronVideoPlayer* player);
int             Aeron_VideoGetInfo(const AeronVideoPlayer* player, AeronVideoInfo* out_info);
int             Aeron_VideoGetStats(const AeronVideoPlayer* player, AeronVideoStats* out_stats);
const char*     Aeron_VideoGetError(const AeronVideoPlayer* player);

#ifdef __cplusplus
}
#endif

#endif
