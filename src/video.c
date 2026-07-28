#include "video_internal.h"

#include <libavutil/mem.h>

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int64_t AeronVideo_AudioFramesToUs(uint64_t frames) {
	const uint64_t seconds   = frames / AERON_VIDEO_AUDIO_RATE;
	const uint64_t remainder = frames % AERON_VIDEO_AUDIO_RATE;
	const uint64_t fractional_us =
		(remainder * 1000000u + AERON_VIDEO_AUDIO_RATE / 2u) / AERON_VIDEO_AUDIO_RATE;

	if (seconds > ((uint64_t)INT64_MAX - fractional_us) / 1000000u) {
		return INT64_MAX;
	}
	return (int64_t)(seconds * 1000000u + fractional_us);
}

static int64_t AeronVideo_ClampPosition(const AeronVideoPlayer* player, int64_t position_us) {
	if (position_us < 0) {
		return 0;
	}
	if (player->info.duration_us >= 0 && position_us > player->info.duration_us) {
		return player->info.duration_us;
	}
	return position_us;
}

static int64_t AeronVideo_CurrentFrameEnd(const AeronVideoPlayer* player) {
	int64_t frame_end;

	if (!player->current_valid) {
		return 0;
	}
	if (player->current_duration_us > INT64_MAX - player->current_pts_us) {
		frame_end = INT64_MAX;
	} else {
		frame_end = player->current_pts_us + player->current_duration_us;
	}
	/* Container duration and frame timestamps can differ by a microsecond when
	 * their rational time bases are converted independently. */
	if (player->info.duration_us >= 0 && frame_end > player->info.duration_us) {
		frame_end = player->info.duration_us;
	}
	return frame_end;
}

static AeronAudioStream AeronVideo_GetAudioStream(const AeronVideoPlayer* player) {
	AeronAudioStream stream;
	SDL_LockMutex(player->lock);
	stream = player->audio_stream;
	SDL_UnlockMutex(player->lock);
	return stream;
}

void AeronVideo_SetError(AeronVideoPlayer* player, const char* fmt, ...) {
	char    message[AERON_VIDEO_ERROR_SIZE];
	va_list args;

	if (!player) {
		return;
	}
	va_start(args, fmt);
	SDL_vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	SDL_LockMutex(player->lock);
	SDL_snprintf(player->error, sizeof(player->error), "%s", message);
	player->worker_failed = 1;
	player->state         = AERON_VIDEO_ERROR;
	SDL_BroadcastCondition(player->condition);
	SDL_UnlockMutex(player->lock);
	Aeron_LogError("aeron.video", "%s: %s", player->path, message);
}

void AeronVideo_ClearFrameQueueLocked(AeronVideoPlayer* player) {
	player->frame_head  = 0;
	player->frame_count = 0;
	SDL_BroadcastCondition(player->condition);
}

static void AeronVideo_FreeFrames(AeronVideoPlayer* player) {
	if (player->frames) {
		for (uint32_t i = 0; i < player->queue_capacity; ++i) {
			av_free(player->frames[i].pixels);
		}
		av_free(player->frames);
	}
	av_free(player->current_pixels);
	player->frames         = NULL;
	player->current_pixels = NULL;
}

AeronVideoPlayer* Aeron_VideoOpen(const AeronVideoOpenDesc* desc) {
	AeronVideoPlayer* player;

	if (!desc || !desc->vfs || !desc->path || !desc->path[0]) {
		return NULL;
	}

	player = (AeronVideoPlayer*)SDL_calloc(1, sizeof(*player));
	if (!player) {
		return NULL;
	}
	player->state            = AERON_VIDEO_OPENING;
	player->info.duration_us = -1;
	player->autoplay         = desc->autoplay != 0;
	player->desired_playing  = player->autoplay;
	player->loop             = desc->loop != 0;
	player->gain             = desc->gain < 0.0f ? 0.0f : desc->gain;
	player->queue_capacity =
		desc->video_queue_frames ? desc->video_queue_frames : AERON_VIDEO_DEFAULT_QUEUE_FRAMES;
	player->audio_buffer_ms =
		desc->audio_buffer_ms ? desc->audio_buffer_ms : AERON_VIDEO_DEFAULT_AUDIO_BUFFER_MS;
	player->max_width  = desc->max_width > 0 ? desc->max_width : AERON_VIDEO_DEFAULT_MAX_WIDTH;
	player->max_height = desc->max_height > 0 ? desc->max_height : AERON_VIDEO_DEFAULT_MAX_HEIGHT;
	player->max_frame_bytes =
		desc->max_frame_bytes ? desc->max_frame_bytes : AERON_VIDEO_DEFAULT_MAX_FRAME_BYTES;
	player->serial = 1;
	SDL_snprintf(player->path, sizeof(player->path), "%s", desc->path);

	if (player->queue_capacity < 2 || player->queue_capacity > 64 ||
		player->audio_buffer_ms > AERON_VIDEO_MAX_AUDIO_BUFFER_MS ||
		!AeronVfs_Open(desc->vfs, desc->root, desc->path, AERON_VFS_READ, &player->file)) {
		SDL_free(player);
		return NULL;
	}

	player->lock      = SDL_CreateMutex();
	player->condition = SDL_CreateCondition();
	if (!player->lock || !player->condition) {
		if (player->condition) {
			SDL_DestroyCondition(player->condition);
		}
		if (player->lock) {
			SDL_DestroyMutex(player->lock);
		}
		AeronVfs_Close(player->file);
		SDL_free(player);
		return NULL;
	}

	player->worker = SDL_CreateThread(AeronVideo_WorkerMain, "aeron-video-decode", player);
	if (!player->worker) {
		Aeron_LogError("aeron.video", "%s: SDL_CreateThread failed: %s", player->path, SDL_GetError());
		SDL_DestroyCondition(player->condition);
		SDL_DestroyMutex(player->lock);
		AeronVfs_Close(player->file);
		SDL_free(player);
		return NULL;
	}
	return player;
}

void Aeron_VideoClose(AeronVideoPlayer* player) {
	AeronAudioStream audio_stream;

	if (!player) {
		return;
	}

	if (player->render_submission) {
		Aeron_CancelRenderSubmission(player->render_submission);
		player->render_submission = 0;
	}

	SDL_LockMutex(player->lock);
	player->stop_requested = 1;
	audio_stream           = player->audio_stream;
	SDL_BroadcastCondition(player->condition);
	SDL_UnlockMutex(player->lock);
	if (audio_stream) {
		Aeron_AudioStreamPause(audio_stream);
	}
	if (player->worker) {
		SDL_WaitThread(player->worker, NULL);
		player->worker = NULL;
	}
	if (player->audio_stream) {
		Aeron_AudioStreamClose(player->audio_stream);
		player->audio_stream = 0;
	}
	if (player->file) {
		AeronVfs_Close(player->file);
		player->file = NULL;
	}
	AeronVideo_FreeFrames(player);
	SDL_DestroyCondition(player->condition);
	SDL_DestroyMutex(player->lock);
	SDL_free(player);
}

void Aeron_VideoPlay(AeronVideoPlayer* player) {
	int restart;
	if (!player) {
		return;
	}
	SDL_LockMutex(player->lock);
	restart = player->stopped || (player->state == AERON_VIDEO_ENDED && player->worker_eof);
	SDL_UnlockMutex(player->lock);
	if (restart) {
		(void)Aeron_VideoSeek(player, 0);
	}

	SDL_LockMutex(player->lock);
	player->desired_playing = 1;
	player->stopped         = 0;
	if (player->state == AERON_VIDEO_PAUSED) {
		player->state = AERON_VIDEO_BUFFERING;
	}
	SDL_BroadcastCondition(player->condition);
	SDL_UnlockMutex(player->lock);
}

void Aeron_VideoPause(AeronVideoPlayer* player) {
	AeronAudioStream audio_stream;
	if (!player) {
		return;
	}
	audio_stream = AeronVideo_GetAudioStream(player);
	if (audio_stream) {
		Aeron_AudioStreamPause(audio_stream);
	}
	SDL_LockMutex(player->lock);
	player->desired_playing  = 0;
	player->playback_started = 0;
	if (player->state != AERON_VIDEO_ENDED && player->state != AERON_VIDEO_ERROR) {
		player->state = AERON_VIDEO_PAUSED;
	}
	SDL_BroadcastCondition(player->condition);
	SDL_UnlockMutex(player->lock);
}

void Aeron_VideoStop(AeronVideoPlayer* player) {
	AeronAudioStream audio_stream;
	if (!player) {
		return;
	}
	if (player->render_submission) {
		Aeron_CancelRenderSubmission(player->render_submission);
		player->render_submission = 0;
	}
	audio_stream = AeronVideo_GetAudioStream(player);
	if (audio_stream) {
		Aeron_AudioStreamPause(audio_stream);
		Aeron_AudioStreamFlush(audio_stream);
	}
	SDL_LockMutex(player->lock);
	player->desired_playing  = 0;
	player->stopped          = 1;
	player->playback_started = 0;
	player->position_us      = 0;
	player->current_valid    = 0;
	AeronVideo_ClearFrameQueueLocked(player);
	player->state = AERON_VIDEO_ENDED;
	SDL_BroadcastCondition(player->condition);
	SDL_UnlockMutex(player->lock);
}

int Aeron_VideoSeek(AeronVideoPlayer* player, int64_t position_us) {
	AeronAudioStream audio_stream;
	if (!player) {
		return 0;
	}
	position_us = AeronVideo_ClampPosition(player, position_us);
	if (player->render_submission) {
		Aeron_CancelRenderSubmission(player->render_submission);
		player->render_submission = 0;
	}
	audio_stream = AeronVideo_GetAudioStream(player);
	if (audio_stream) {
		Aeron_AudioStreamPause(audio_stream);
		Aeron_AudioStreamFlush(audio_stream);
	}

	SDL_LockMutex(player->lock);
	if (player->worker_failed || player->stop_requested) {
		SDL_UnlockMutex(player->lock);
		return 0;
	}
	++player->serial;
	if (player->serial == 0) {
		player->serial = 1;
	}
	player->seek_target_us         = position_us;
	player->seek_pending           = 1;
	player->stopped                = 0;
	player->worker_eof             = 0;
	player->playback_started       = 0;
	player->position_us            = position_us;
	player->wall_start_position_us = position_us;
	player->current_valid          = 0;
	player->audio_draining         = 0;
	player->audio_clock_exhausted  = 0;
	player->last_audio_underruns   = 0;
	player->audio_prebuffer_ready  = 0;
	AeronVideo_ClearFrameQueueLocked(player);
	player->state = player->desired_playing ? AERON_VIDEO_BUFFERING : AERON_VIDEO_PAUSED;
	SDL_BroadcastCondition(player->condition);
	SDL_UnlockMutex(player->lock);
	return 1;
}

void Aeron_VideoSetGain(AeronVideoPlayer* player, float gain) {
	AeronAudioStream audio_stream;
	if (!player) {
		return;
	}
	gain = gain < 0.0f ? 0.0f : gain;
	SDL_LockMutex(player->lock);
	player->gain = gain;
	audio_stream = player->audio_stream;
	SDL_UnlockMutex(player->lock);
	if (audio_stream) {
		Aeron_AudioStreamSetGain(audio_stream, gain);
	}
}

static int64_t AeronVideo_MediaPosition(AeronVideoPlayer* player, uint64_t now_us) {
	if (!player->playback_started) {
		return player->position_us;
	}
	if (player->audio_stream && player->info.has_audio) {
		const uint64_t audible    = Aeron_AudioStreamAudibleFrames(player->audio_stream);
		const int64_t  elapsed_us = AeronVideo_AudioFramesToUs(audible);
		int64_t        audio_position;
		if (elapsed_us > INT64_MAX - player->audio_start_pts_us) {
			return INT64_MAX;
		}
		audio_position = player->audio_start_pts_us + elapsed_us;
		if (!player->audio_clock_exhausted && player->worker_eof &&
			audible >= player->total_audio_frames) {
			/* Audio is the master clock while samples remain. Continue from the
			 * same position on wall time once EOF audio is exhausted so a video
			 * tail, including sub-sample timestamp rounding, can complete. */
			player->audio_clock_exhausted  = 1;
			player->wall_start_us          = now_us;
			player->wall_start_position_us = audio_position;
		}
		if (!player->audio_clock_exhausted) {
			return audio_position;
		}
	}
	return player->wall_start_position_us + (int64_t)(now_us - player->wall_start_us);
}

static void AeronVideo_PromoteDueFramesLocked(AeronVideoPlayer* player, int64_t position_us) {
	int promoted = 0;

	while (player->frame_count > 0) {
		AeronDecodedVideoFrame* frame = &player->frames[player->frame_head];
		if (player->current_valid && frame->pts_us > position_us) {
			break;
		}

		{
			uint8_t* old_current   = player->current_pixels;
			player->current_pixels = frame->pixels;
			frame->pixels          = old_current;
		}
		player->current_pts_us      = frame->pts_us;
		player->current_duration_us = frame->duration_us;
		player->current_index       = frame->index;
		player->current_valid       = 1;
		++player->current_generation;
		if (player->current_generation == 0) {
			player->current_generation = 1;
		}
		player->frame_head = (player->frame_head + 1u) % player->queue_capacity;
		--player->frame_count;
		if (promoted) {
			++player->video_frames_dropped;
		}
		promoted = 1;
	}
	if (promoted) {
		SDL_BroadcastCondition(player->condition);
	}
}

void Aeron_VideoUpdate(AeronVideoPlayer* player, uint64_t now_us) {
	size_t           queued_audio = 0;
	uint64_t         underruns    = 0;
	AeronAudioStream audio_stream;
	int              start_audio = 0;
	int              pause_audio = 0;

	if (!player) {
		return;
	}
	audio_stream = AeronVideo_GetAudioStream(player);
	if (audio_stream) {
		queued_audio = Aeron_AudioStreamQueuedFrames(audio_stream);
		underruns    = Aeron_AudioStreamUnderrunCount(audio_stream);
	}

	SDL_LockMutex(player->lock);
	if (player->worker_failed || player->state == AERON_VIDEO_ERROR || player->stopped) {
		SDL_UnlockMutex(player->lock);
		return;
	}

	if (player->worker_ready && !player->playback_started && player->desired_playing) {
		const uint64_t prebuffer_frames =
			((uint64_t)AERON_VIDEO_AUDIO_RATE * player->audio_buffer_ms) / 4000u;
		const int video_ready         = !player->info.has_video || player->frame_count > 0;
		const int audio_ready         = !player->info.has_audio || !player->audio_stream ||
										queued_audio >= prebuffer_frames || player->worker_eof;
		player->audio_prebuffer_ready = audio_ready;
		if (video_ready && audio_ready) {
			player->playback_started       = 1;
			player->wall_start_us          = now_us;
			player->wall_start_position_us = player->position_us;
			player->state                  = AERON_VIDEO_PLAYING;
			start_audio                    = player->audio_stream != 0;
			SDL_BroadcastCondition(player->condition);
		}
	}

	if (player->state == AERON_VIDEO_PLAYING && player->audio_stream && player->info.has_audio &&
		queued_audio == 0 && underruns > player->last_audio_underruns && !player->worker_eof) {
		player->state                 = AERON_VIDEO_BUFFERING;
		player->playback_started      = 0;
		player->audio_prebuffer_ready = 0;
		pause_audio                   = 1;
	}
	player->last_audio_underruns = underruns;

	player->position_us = AeronVideo_ClampPosition(player, AeronVideo_MediaPosition(player, now_us));
	AeronVideo_PromoteDueFramesLocked(player, player->position_us);

	if (player->worker_eof && player->frame_count == 0 &&
		(!player->current_valid || player->position_us >= AeronVideo_CurrentFrameEnd(player))) {
		const int audio_empty = !player->audio_stream || queued_audio == 0;
		if (audio_empty) {
			if (!player->audio_stream) {
				if (player->loop && player->desired_playing) {
					SDL_UnlockMutex(player->lock);
					(void)Aeron_VideoSeek(player, 0);
					return;
				}
				player->state            = AERON_VIDEO_ENDED;
				player->desired_playing  = 0;
				player->playback_started = 0;
			} else if (!player->audio_draining) {
				player->audio_draining         = 1;
				player->audio_drain_started_us = now_us;
			} else if (now_us - player->audio_drain_started_us >= 200000u) {
				if (player->loop && player->desired_playing) {
					SDL_UnlockMutex(player->lock);
					(void)Aeron_VideoSeek(player, 0);
					return;
				}
				player->state            = AERON_VIDEO_ENDED;
				player->desired_playing  = 0;
				player->playback_started = 0;
				pause_audio              = player->audio_stream != 0;
			}
		}
	}

	if (player->state == AERON_VIDEO_PLAYING && player->frame_count > 0) {
		const AeronDecodedVideoFrame* next  = &player->frames[player->frame_head];
		const int64_t                 delay = next->pts_us - player->position_us;
		player->next_wake_us                = delay > 0 ? now_us + (uint64_t)delay : now_us;
	} else if (player->state == AERON_VIDEO_BUFFERING) {
		player->next_wake_us = now_us + 10000u;
	} else {
		player->next_wake_us = 0;
	}
	SDL_UnlockMutex(player->lock);

	if (pause_audio) {
		Aeron_AudioStreamPause(audio_stream);
	}
	if (start_audio) {
		Aeron_AudioStreamPlay(audio_stream);
		/* The decoder may have observed a full paused stream after the state
		 * transition but before AudioStreamPlay. Wake that fallback wait now
		 * that audio consumption can release FIFO capacity. */
		SDL_LockMutex(player->lock);
		SDL_BroadcastCondition(player->condition);
		SDL_UnlockMutex(player->lock);
	}
}

static AeronRectI AeronVideo_ComputeRect(const AeronVideoPresentDesc* desc, int width, int height) {
	AeronRectI rect = desc->bounds;
	if (desc->scale_mode == AERON_VIDEO_SCALE_STRETCH || width <= 0 || height <= 0 ||
		desc->bounds.width <= 0 || desc->bounds.height <= 0) {
		return rect;
	}

	{
		const int64_t fit_width        = (int64_t)desc->bounds.height * width / height;
		const int     contain_by_width = fit_width > desc->bounds.width;
		if ((desc->scale_mode == AERON_VIDEO_SCALE_CONTAIN && contain_by_width) ||
			(desc->scale_mode == AERON_VIDEO_SCALE_COVER && !contain_by_width)) {
			rect.width  = desc->bounds.width;
			rect.height = (int)((int64_t)desc->bounds.width * height / width);
		} else {
			rect.height = desc->bounds.height;
			rect.width  = (int)fit_width;
		}
		rect.x = desc->bounds.x + (desc->bounds.width - rect.width) / 2;
		rect.y = desc->bounds.y + (desc->bounds.height - rect.height) / 2;
	}
	return rect;
}

int Aeron_VideoSubmit(AeronVideoPlayer* player, const AeronVideoPresentDesc* desc) {
	AeronPixelLayerDesc layer;
	int                 width;
	int                 height;

	if (!player || !desc || desc->bounds.width <= 0 || desc->bounds.height <= 0) {
		return 0;
	}
	if (player->render_submission) {
		Aeron_CancelRenderSubmission(player->render_submission);
		player->render_submission = 0;
	}

	SDL_LockMutex(player->lock);
	if (!player->current_valid || !player->current_pixels) {
		SDL_UnlockMutex(player->lock);
		return 0;
	}
	width  = player->info.width;
	height = player->info.height;
	memset(&layer, 0, sizeof(layer));
	layer.frame.pixels      = player->current_pixels;
	layer.frame.width       = width;
	layer.frame.height      = height;
	layer.frame.pitch       = width * 4;
	layer.frame.format      = AERON_PIXEL_FORMAT_RGBA8888;
	layer.frame.color_space = AERON_COLOR_SPACE_SRGB;
	layer.frame.generation  = player->current_generation;
	layer.logical_rect =
		AeronVideo_ComputeRect(desc, player->info.display_width, player->info.display_height);
	layer.blend_mode   = desc->blend_mode;
	layer.sampling     = desc->filter == AERON_VIDEO_FILTER_NEAREST ? AERON_PIXEL_SAMPLING_NEAREST
																	: AERON_PIXEL_SAMPLING_LINEAR;
	layer.tint_enabled = desc->tint_enabled;
	memcpy(layer.tint_rgba, desc->tint_rgba, sizeof(layer.tint_rgba));
	layer.scissor = desc->scissor;
	if (desc->scale_mode == AERON_VIDEO_SCALE_COVER && layer.scissor.width <= 0) {
		layer.scissor = desc->bounds;
	}
	SDL_UnlockMutex(player->lock);

	player->render_submission = Aeron_SubmitPixelLayer(&layer);
	if (player->render_submission) {
		SDL_LockMutex(player->lock);
		if (player->submitted_generation != player->current_generation) {
			player->submitted_generation = player->current_generation;
			++player->video_frames_presented;
		}
		SDL_UnlockMutex(player->lock);
	}
	return player->render_submission != 0;
}

AeronVideoState Aeron_VideoGetState(const AeronVideoPlayer* player) {
	AeronVideoState state;
	if (!player) {
		return AERON_VIDEO_ERROR;
	}
	SDL_LockMutex(player->lock);
	state = player->state;
	SDL_UnlockMutex(player->lock);
	return state;
}

int64_t Aeron_VideoGetPositionUs(const AeronVideoPlayer* player) {
	int64_t position;
	if (!player) {
		return 0;
	}
	SDL_LockMutex(player->lock);
	position = player->position_us;
	SDL_UnlockMutex(player->lock);
	return position;
}

uint64_t Aeron_VideoGetPresentedFrameIndex(const AeronVideoPlayer* player) {
	uint64_t index;
	if (!player) {
		return 0;
	}
	SDL_LockMutex(player->lock);
	index = player->current_valid ? player->current_index : 0;
	SDL_UnlockMutex(player->lock);
	return index;
}

uint64_t Aeron_VideoNextWakeDeadlineUs(const AeronVideoPlayer* player) {
	uint64_t deadline;
	if (!player) {
		return 0;
	}
	SDL_LockMutex(player->lock);
	deadline = player->next_wake_us;
	SDL_UnlockMutex(player->lock);
	return deadline;
}

int Aeron_VideoGetInfo(const AeronVideoPlayer* player, AeronVideoInfo* out_info) {
	if (!player || !out_info) {
		return 0;
	}
	SDL_LockMutex(player->lock);
	*out_info       = player->info;
	const int ready = player->worker_ready;
	SDL_UnlockMutex(player->lock);
	return ready;
}

int Aeron_VideoGetStats(const AeronVideoPlayer* player, AeronVideoStats* out_stats) {
	if (!player || !out_stats) {
		return 0;
	}
	SDL_LockMutex(player->lock);
	memset(out_stats, 0, sizeof(*out_stats));
	out_stats->video_frames_decoded     = player->video_frames_decoded;
	out_stats->video_frames_presented   = player->video_frames_presented;
	out_stats->video_frames_dropped     = player->video_frames_dropped;
	out_stats->audio_frames_trimmed     = player->audio_frames_trimmed;
	out_stats->queued_video_frames      = player->frame_count;
	out_stats->position_us              = player->position_us;
	out_stats->duration_us              = player->info.duration_us;
	const AeronAudioStream audio_stream = player->audio_stream;
	SDL_UnlockMutex(player->lock);
	if (audio_stream) {
		out_stats->queued_audio_frames = Aeron_AudioStreamQueuedFrames(audio_stream);
		out_stats->audio_underruns     = Aeron_AudioStreamUnderrunCount(audio_stream);
	}
	return 1;
}

const char* Aeron_VideoGetError(const AeronVideoPlayer* player) {
	return player ? player->error : "invalid video player";
}
