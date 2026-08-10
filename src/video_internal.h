#ifndef AERON_VIDEO_INTERNAL_H
#define AERON_VIDEO_INTERNAL_H

#include "aeron/audio.h"
#include "aeron/log.h"
#include "aeron/time.h"
#include "aeron/video.h"
#include "time_internal.h"

#include <SDL3/SDL.h>

#include <stdint.h>

enum {
	AERON_VIDEO_DEFAULT_QUEUE_FRAMES    = 8,
	AERON_VIDEO_DEFAULT_AUDIO_BUFFER_MS = 1000,
	AERON_VIDEO_MAX_AUDIO_BUFFER_MS     = 60000,
	AERON_VIDEO_DEFAULT_MAX_WIDTH       = 8192,
	AERON_VIDEO_DEFAULT_MAX_HEIGHT      = 8192,
	AERON_VIDEO_ERROR_SIZE              = 512,
	AERON_VIDEO_PATH_SIZE               = 1024,
	AERON_VIDEO_AUDIO_RATE              = 48000,
	AERON_VIDEO_AUDIO_CHANNELS          = 2,
};

#define AERON_VIDEO_DEFAULT_MAX_FRAME_BYTES ((size_t)256 * 1024u * 1024u)

typedef struct AeronDecodedVideoFrame {
	uint8_t* pixels;
	int64_t  pts_us;
	int64_t  duration_us;
	uint64_t index;
	uint32_t serial;
} AeronDecodedVideoFrame;

struct AeronVideoPlayer {
	SDL_Mutex*     lock;
	SDL_Condition* condition;
	SDL_Thread*    worker;
	AeronFile*     file;
	char           path[AERON_VIDEO_PATH_SIZE];

	AeronVideoState state;
	AeronVideoInfo  info;
	char            error[AERON_VIDEO_ERROR_SIZE];

	int      autoplay;
	int      desired_playing;
	int      loop;
	float    gain;
	uint32_t queue_capacity;
	uint32_t audio_buffer_ms;
	int      max_width;
	int      max_height;
	size_t   max_frame_bytes;

	int      stop_requested;
	int      stopped;
	int      seek_pending;
	int64_t  seek_target_us;
	uint32_t serial;
	int      worker_ready;
	int      worker_eof;
	int      worker_failed;

	AeronDecodedVideoFrame* frames;
	uint32_t                frame_head;
	uint32_t                frame_count;
	uint8_t*                current_pixels;
	int64_t                 current_pts_us;
	int64_t                 current_duration_us;
	uint64_t                current_index;
	uint32_t                current_generation;
	int                     current_valid;

	AeronAudioStream audio_stream;
	int64_t          audio_start_pts_us;
	uint64_t         total_audio_frames;
	uint64_t         audio_frames_trimmed;
	uint64_t         last_audio_underruns;
	int              audio_prebuffer_ready;
	int              audio_draining;
	int              audio_clock_exhausted;
	uint64_t         audio_drain_started_us;

	int      playback_started;
	uint64_t wall_start_us;
	int64_t  wall_start_position_us;
	int64_t  position_us;
	uint64_t next_wake_us;

	AeronRenderSubmission render_submission;
	uint32_t              submitted_generation;

	uint64_t video_frames_decoded;
	uint64_t video_frames_presented;
	uint64_t video_frames_dropped;
};

void AeronVideo_SetError(AeronVideoPlayer* player, const char* fmt, ...);
int  AeronVideo_WorkerMain(void* userdata);
void AeronVideo_ClearFrameQueueLocked(AeronVideoPlayer* player);

#endif
