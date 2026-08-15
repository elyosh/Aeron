#include "ffmpeg_vfs_io.h"
#include "video_internal.h"

#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <limits.h>
#include <math.h>
#include <string.h>

typedef struct AeronVideoDecodeContext {
	AeronVideoPlayer*  player;
	AVFormatContext*   format;
	AVIOContext*       avio;
	AVCodecContext*    video_codec;
	AVCodecContext*    audio_codec;
	AVStream*          video_stream;
	AVStream*          audio_stream;
	int                video_stream_index;
	int                audio_stream_index;
	AVPacket*          packet;
	AVFrame*           frame;
	struct SwsContext* scaler;
	SwrContext*        resampler;
	uint8_t*           rgba_temp;
	size_t             rgba_temp_size;
	uint8_t*           audio_temp;
	unsigned int       audio_temp_size;
	int64_t            timeline_origin_us;
	int64_t            discard_audio_before_us;
	int64_t            next_video_pts_us;
	int64_t            fallback_frame_duration_us;
	uint64_t           next_video_index;
	uint64_t           reported_audio_frames_trimmed;
	int                rotation_degrees;
	int                source_width;
	int                source_height;
	int                output_width;
	int                output_height;
} AeronVideoDecodeContext;

static void AeronVideo_FfmpegError(char* dst, size_t dst_size, int error) {
	av_make_error_string(dst, dst_size, error);
}

static int AeronVideo_ShouldInterrupt(void* opaque) {
	AeronVideoPlayer* player = (AeronVideoPlayer*)opaque;
	int               interrupted;
	SDL_LockMutex(player->lock);
	interrupted = player->stop_requested;
	SDL_UnlockMutex(player->lock);
	return interrupted;
}

static AVCodecContext* AeronVideo_OpenCodec(AVFormatContext* format, int stream_index) {
	AVStream*       stream = format->streams[stream_index];
	const AVCodec*  codec  = avcodec_find_decoder(stream->codecpar->codec_id);
	AVCodecContext* context;
	if (!codec) {
		return NULL;
	}
	context = avcodec_alloc_context3(codec);
	if (!context) {
		return NULL;
	}
	if (avcodec_parameters_to_context(context, stream->codecpar) < 0 ||
		avcodec_open2(context, codec, NULL) < 0) {
		avcodec_free_context(&context);
		return NULL;
	}
	return context;
}

static int AeronVideo_NormalizeRotation(const AVStream* stream) {
	const AVPacketSideData* side_data;
	double                  rotation;
	int                     degrees;

	side_data = av_packet_side_data_get(stream->codecpar->coded_side_data,
										stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
	if (!side_data || side_data->size < 9u * sizeof(int32_t)) {
		return 0;
	}
	rotation = -av_display_rotation_get((const int32_t*)side_data->data);
	if (!isfinite(rotation)) {
		return 0;
	}
	degrees = (int)lround(rotation);
	degrees %= 360;
	if (degrees < 0) {
		degrees += 360;
	}
	if (degrees < 45 || degrees >= 315) {
		return 0;
	}
	if (degrees < 135) {
		return 90;
	}
	if (degrees < 225) {
		return 180;
	}
	return 270;
}

static void AeronVideo_RotateRgba(uint8_t* dst, const uint8_t* src, int width, int height, int rotation) {
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int            dx;
			int            dy;
			const uint8_t* source = src + ((size_t)y * (size_t)width + (size_t)x) * 4u;
			if (rotation == 90) {
				dx = height - 1 - y;
				dy = x;
			} else if (rotation == 180) {
				dx = width - 1 - x;
				dy = height - 1 - y;
			} else {
				dx = y;
				dy = width - 1 - x;
			}
			memcpy(dst + ((size_t)dy * (size_t)(rotation == 180 ? width : height) + (size_t)dx) * 4u, source,
				   4u);
		}
	}
}

static int64_t AeronVideo_FrameTimestampUs(const AeronVideoDecodeContext* decode, const AVFrame* frame,
										   const AVStream* stream) {
	int64_t timestamp = frame->best_effort_timestamp;
	if (timestamp == AV_NOPTS_VALUE) {
		timestamp = frame->pts;
	}
	if (timestamp == AV_NOPTS_VALUE) {
		return AV_NOPTS_VALUE;
	}
	return av_rescale_q(timestamp, stream->time_base, AV_TIME_BASE_Q) - decode->timeline_origin_us;
}

static int AeronVideo_ConfigureColorConversion(AeronVideoDecodeContext* decode, const AVFrame* frame) {
	int        color_space = frame->colorspace;
	const int* coefficients;
	if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
		return AVERROR(ENOTSUP);
	}
	if (color_space == AVCOL_SPC_UNSPECIFIED) {
		color_space = frame->height <= 576 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
	}
	coefficients = sws_getCoefficients(color_space);
	if (!coefficients) {
		coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
	}
	return sws_setColorspaceDetails(decode->scaler, coefficients, frame->color_range == AVCOL_RANGE_JPEG,
									coefficients, 1, 0, 1 << 16, 1 << 16) >= 0
			   ? 0
			   : AVERROR(EINVAL);
}

static int AeronVideo_ReserveFrame(AeronVideoDecodeContext* decode, uint32_t* out_index,
								   uint32_t* out_serial) {
	AeronVideoPlayer* player = decode->player;
	SDL_LockMutex(player->lock);
	while (player->frame_count >= player->queue_capacity && !player->stop_requested &&
		   !player->seek_pending && !player->stopped) {
		/* Demux and audio decoding share this worker. Before playback starts,
		 * retain the earliest prebuffered video frames and discard later ones
		 * instead of preventing the worker from reaching the audio watermark. */
		if (player->state == AERON_VIDEO_BUFFERING && player->desired_playing && player->info.has_audio &&
			!player->audio_prebuffer_ready) {
			++player->video_frames_decoded;
			++player->video_frames_dropped;
			SDL_UnlockMutex(player->lock);
			return -1;
		}
		SDL_WaitCondition(player->condition, player->lock);
	}
	if (player->stop_requested || player->seek_pending || player->stopped) {
		SDL_UnlockMutex(player->lock);
		return 0;
	}
	*out_index  = (player->frame_head + player->frame_count) % player->queue_capacity;
	*out_serial = player->serial;
	SDL_UnlockMutex(player->lock);
	return 1;
}

static int AeronVideo_QueueVideoFrame(AeronVideoDecodeContext* decode, AVFrame* source) {
	AeronVideoPlayer*       player = decode->player;
	AeronDecodedVideoFrame* queued;
	uint32_t                index;
	uint32_t                serial;
	uint8_t*                conversion_target;
	uint8_t*                planes[4]  = { NULL, NULL, NULL, NULL };
	int                     strides[4] = { 0, 0, 0, 0 };
	int                     result;
	int64_t                 pts_us;
	int64_t                 duration_us;

	{
		const int reserve_result = AeronVideo_ReserveFrame(decode, &index, &serial);
		if (reserve_result < 0) {
			return 0;
		}
		if (reserve_result == 0) {
			return AVERROR_EXIT;
		}
	}
	queued            = &player->frames[index];
	conversion_target = decode->rotation_degrees ? decode->rgba_temp : queued->pixels;

	decode->scaler = sws_getCachedContext(
		decode->scaler, source->width, source->height, (enum AVPixelFormat)source->format,
		decode->source_width, decode->source_height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
	if (!decode->scaler) {
		return AVERROR(EINVAL);
	}
	result = AeronVideo_ConfigureColorConversion(decode, source);
	if (result < 0) {
		return result;
	}
	planes[0]  = conversion_target;
	strides[0] = decode->source_width * 4;
	if (sws_scale(decode->scaler, (const uint8_t* const*)source->data, source->linesize, 0, source->height,
				  planes, strides) != decode->source_height) {
		return AVERROR(EIO);
	}
	if (decode->rotation_degrees) {
		AeronVideo_RotateRgba(queued->pixels, decode->rgba_temp, decode->source_width, decode->source_height,
							  decode->rotation_degrees);
	}

	pts_us = AeronVideo_FrameTimestampUs(decode, source, decode->video_stream);
	if (pts_us == AV_NOPTS_VALUE) {
		pts_us = decode->next_video_pts_us;
	}
	if (pts_us < 0) {
		pts_us = 0;
	}
	duration_us = source->duration > 0
					  ? av_rescale_q(source->duration, decode->video_stream->time_base, AV_TIME_BASE_Q)
					  : decode->fallback_frame_duration_us;
	if (duration_us <= 0) {
		duration_us = 33333;
	}
	decode->next_video_pts_us = pts_us + duration_us;

	SDL_LockMutex(player->lock);
	if (player->stop_requested || player->seek_pending || player->stopped || player->serial != serial) {
		SDL_UnlockMutex(player->lock);
		return AVERROR_EXIT;
	}
	queued->pts_us      = pts_us;
	queued->duration_us = duration_us;
	queued->index       = ++decode->next_video_index;
	queued->serial      = serial;
	++player->frame_count;
	++player->video_frames_decoded;
	SDL_BroadcastCondition(player->condition);
	SDL_UnlockMutex(player->lock);
	return 0;
}

static int AeronVideo_WriteAudio(AeronVideoDecodeContext* decode, const uint8_t* pcm, size_t frame_count) {
	AeronVideoPlayer* player = decode->player;
	size_t            offset = 0;
	while (offset < frame_count) {
		const size_t written = Aeron_AudioStreamWrite(
			player->audio_stream, pcm + offset * AERON_VIDEO_AUDIO_CHANNELS * sizeof(int16_t),
			frame_count - offset);
		offset += written;
		if (offset == frame_count) {
			return 0;
		}
		if (Aeron_AudioStreamWaitWritable(player->audio_stream, 1)) {
			continue;
		}

		SDL_LockMutex(player->lock);
		if (player->stop_requested || player->seek_pending || player->stopped) {
			SDL_UnlockMutex(player->lock);
			return AVERROR_EXIT;
		}
		SDL_WaitCondition(player->condition, player->lock);
		SDL_UnlockMutex(player->lock);
	}
	return 0;
}

static size_t AeronVideo_LimitAudioFrames(AeronVideoDecodeContext* decode, int64_t pts_us,
										  size_t frame_count) {
	AeronVideoPlayer* player   = decode->player;
	size_t            retained = frame_count;

	SDL_LockMutex(player->lock);
	if (player->total_audio_frames == 0) {
		player->audio_start_pts_us = pts_us == AV_NOPTS_VALUE || pts_us < 0 ? 0 : pts_us;
	}
	if (player->info.duration_us >= 0) {
		int64_t remaining_us = player->info.duration_us - player->audio_start_pts_us;
		int64_t maximum_total_frames =
			remaining_us > 0 ? av_rescale_rnd(remaining_us, AERON_VIDEO_AUDIO_RATE, 1000000, AV_ROUND_DOWN)
							 : 0;
		uint64_t remaining_total_frames =
			maximum_total_frames > 0 && player->total_audio_frames < (uint64_t)maximum_total_frames
				? (uint64_t)maximum_total_frames - player->total_audio_frames
				: 0;

		if (remaining_total_frames < retained) {
			retained = (size_t)remaining_total_frames;
		}
		if (pts_us != AV_NOPTS_VALUE) {
			remaining_us = player->info.duration_us - pts_us;
			{
				const int64_t timestamp_frames =
					remaining_us > 0
						? av_rescale_rnd(remaining_us, AERON_VIDEO_AUDIO_RATE, 1000000, AV_ROUND_DOWN)
						: 0;
				if (timestamp_frames < (int64_t)retained) {
					retained = (size_t)timestamp_frames;
				}
			}
		}
	}
	player->total_audio_frames += retained;
	player->audio_frames_trimmed += frame_count - retained;
	SDL_UnlockMutex(player->lock);
	return retained;
}

static int AeronVideo_QueueConvertedAudio(AeronVideoDecodeContext* decode, const uint8_t* pcm,
										  size_t frame_count, int64_t pts_us) {
	AeronVideoPlayer* player = decode->player;
	int               startup_buffering;
	int               video_prebuffer_missing;
	size_t            queued_frames;
	size_t            required_frames;
	size_t            capacity_frames;
	int               result;
	const size_t maximum_frames = (size_t)AERON_VIDEO_AUDIO_RATE * AERON_VIDEO_MAX_AUDIO_BUFFER_MS / 1000u;

	frame_count = AeronVideo_LimitAudioFrames(decode, pts_us, frame_count);
	if (frame_count == 0) {
		return 0;
	}

	SDL_LockMutex(player->lock);
	startup_buffering =
		!player->playback_started && player->desired_playing && player->state == AERON_VIDEO_BUFFERING;
	video_prebuffer_missing = player->info.has_video && player->frame_count == 0;
	SDL_UnlockMutex(player->lock);

	queued_frames = Aeron_AudioStreamQueuedFrames(player->audio_stream);
	if (frame_count > SIZE_MAX - queued_frames) {
		return AVERROR(EOVERFLOW);
	}
	required_frames = queued_frames + frame_count;
	capacity_frames = Aeron_AudioStreamCapacityFrames(player->audio_stream);
	/* Grow for an oversized decoded frame, or until the worker reaches the
	 * first video packet. Once video is ready, ordinary backpressure must
	 * start playback instead of expanding toward the entire movie. */
	if (startup_buffering && required_frames > capacity_frames &&
		(frame_count > capacity_frames || video_prebuffer_missing)) {
		size_t grown_capacity = capacity_frames;
		while (grown_capacity < required_frames && grown_capacity < maximum_frames) {
			grown_capacity = grown_capacity > maximum_frames / 2u ? maximum_frames : grown_capacity * 2u;
		}
		if (grown_capacity < required_frames ||
			!Aeron_AudioStreamEnsureCapacity(player->audio_stream, grown_capacity)) {
			SDL_LockMutex(player->lock);
			startup_buffering = !player->playback_started && player->desired_playing &&
								player->state == AERON_VIDEO_BUFFERING;
			SDL_UnlockMutex(player->lock);
			if (startup_buffering) {
				return AVERROR(ENOBUFS);
			}
		}
	}

	result = AeronVideo_WriteAudio(decode, pcm, frame_count);
	if (result == 0) {
		const uint64_t prebuffer_frames =
			((uint64_t)AERON_VIDEO_AUDIO_RATE * player->audio_buffer_ms) / 4000u;
		queued_frames = Aeron_AudioStreamQueuedFrames(player->audio_stream);
		if (queued_frames >= prebuffer_frames) {
			SDL_LockMutex(player->lock);
			player->audio_prebuffer_ready = 1;
			SDL_BroadcastCondition(player->condition);
			SDL_UnlockMutex(player->lock);
		}
	}
	return result;
}

static int AeronVideo_QueueAudioFrame(AeronVideoDecodeContext* decode, AVFrame* source) {
	const int out_samples = (int)av_rescale_rnd(
		swr_get_delay(decode->resampler, decode->audio_codec->sample_rate) + source->nb_samples,
		AERON_VIDEO_AUDIO_RATE, decode->audio_codec->sample_rate, AV_ROUND_UP);
	const size_t bytes = (size_t)out_samples * AERON_VIDEO_AUDIO_CHANNELS * sizeof(int16_t);
	uint8_t*     output;
	int          converted;
	int          skipped;
	int64_t      pts_us;

	if (bytes > UINT_MAX) {
		return AVERROR(EOVERFLOW);
	}
	av_fast_malloc(&decode->audio_temp, &decode->audio_temp_size, bytes);
	if (!decode->audio_temp) {
		return AVERROR(ENOMEM);
	}
	output    = decode->audio_temp;
	converted = swr_convert(decode->resampler, &output, out_samples, (const uint8_t**)source->extended_data,
							source->nb_samples);
	if (converted < 0) {
		return converted;
	}

	pts_us  = AeronVideo_FrameTimestampUs(decode, source, decode->audio_stream);
	skipped = 0;
	if (decode->discard_audio_before_us >= 0) {
		if (pts_us == AV_NOPTS_VALUE) {
			pts_us                          = decode->discard_audio_before_us;
			decode->discard_audio_before_us = -1;
		} else if (pts_us < decode->discard_audio_before_us) {
			const int64_t delta_us = decode->discard_audio_before_us - pts_us;
			const int64_t discard_frames =
				av_rescale_rnd(delta_us, AERON_VIDEO_AUDIO_RATE, 1000000, AV_ROUND_UP);
			if (discard_frames >= converted) {
				return 0;
			}
			skipped = (int)discard_frames;
			pts_us += (int64_t)((uint64_t)skipped * 1000000u / AERON_VIDEO_AUDIO_RATE);
			decode->discard_audio_before_us = -1;
		} else {
			decode->discard_audio_before_us = -1;
		}
	}
	output += (size_t)skipped * AERON_VIDEO_AUDIO_CHANNELS * sizeof(int16_t);
	converted -= skipped;
	return converted > 0 ? AeronVideo_QueueConvertedAudio(decode, output, (size_t)converted, pts_us) : 0;
}

static int AeronVideo_DrainVideo(AeronVideoDecodeContext* decode) {
	for (;;) {
		int result = avcodec_receive_frame(decode->video_codec, decode->frame);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
			return 0;
		}
		if (result < 0) {
			return result;
		}
		result = AeronVideo_QueueVideoFrame(decode, decode->frame);
		av_frame_unref(decode->frame);
		if (result < 0) {
			return result;
		}
	}
}

static int AeronVideo_DrainAudio(AeronVideoDecodeContext* decode) {
	for (;;) {
		int result = avcodec_receive_frame(decode->audio_codec, decode->frame);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
			return 0;
		}
		if (result < 0) {
			return result;
		}
		if (decode->player->audio_stream) {
			result = AeronVideo_QueueAudioFrame(decode, decode->frame);
		}
		av_frame_unref(decode->frame);
		if (result < 0) {
			return result;
		}
	}
}

static int AeronVideo_SendPacket(AeronVideoDecodeContext* decode, AVCodecContext* codec,
								 const AVPacket* packet, int video) {
	int result = avcodec_send_packet(codec, packet);
	if (result == AVERROR(EAGAIN)) {
		result = video ? AeronVideo_DrainVideo(decode) : AeronVideo_DrainAudio(decode);
		if (result < 0) {
			return result;
		}
		result = avcodec_send_packet(codec, packet);
	}
	if (result < 0 && result != AVERROR_EOF) {
		return result;
	}
	return video ? AeronVideo_DrainVideo(decode) : AeronVideo_DrainAudio(decode);
}

static int AeronVideo_AllocateFrames(AeronVideoDecodeContext* decode) {
	AeronVideoPlayer* player      = decode->player;
	const size_t      frame_bytes = (size_t)decode->output_width * (size_t)decode->output_height * 4u;

	player->frames = (AeronDecodedVideoFrame*)av_calloc(player->queue_capacity, sizeof(*player->frames));
	if (!player->frames) {
		return 0;
	}
	for (uint32_t i = 0; i < player->queue_capacity; ++i) {
		player->frames[i].pixels = (uint8_t*)av_malloc(frame_bytes);
		if (!player->frames[i].pixels) {
			return 0;
		}
	}
	player->current_pixels = (uint8_t*)av_malloc(frame_bytes);
	if (!player->current_pixels) {
		return 0;
	}
	if (decode->rotation_degrees) {
		decode->rgba_temp_size = (size_t)decode->source_width * (size_t)decode->source_height * 4u;
		decode->rgba_temp      = (uint8_t*)av_malloc(decode->rgba_temp_size);
		if (!decode->rgba_temp) {
			return 0;
		}
	}
	return 1;
}

static int AeronVideo_InitStreams(AeronVideoDecodeContext* decode) {
	AeronVideoPlayer* player = decode->player;
	AVRational        sar;
	int64_t           frame_bytes;

	decode->video_stream_index = av_find_best_stream(decode->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	decode->audio_stream_index = av_find_best_stream(decode->format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (decode->video_stream_index < 0 && decode->audio_stream_index < 0) {
		return 0;
	}

	if (decode->video_stream_index >= 0) {
		decode->video_stream = decode->format->streams[decode->video_stream_index];
		decode->video_codec  = AeronVideo_OpenCodec(decode->format, decode->video_stream_index);
		if (!decode->video_codec || decode->video_codec->width <= 0 || decode->video_codec->height <= 0 ||
			decode->video_codec->width > player->max_width ||
			decode->video_codec->height > player->max_height) {
			return 0;
		}
		decode->source_width     = decode->video_codec->width;
		decode->source_height    = decode->video_codec->height;
		decode->rotation_degrees = AeronVideo_NormalizeRotation(decode->video_stream);
		decode->output_width     = decode->rotation_degrees == 90 || decode->rotation_degrees == 270
									   ? decode->source_height
									   : decode->source_width;
		decode->output_height    = decode->rotation_degrees == 90 || decode->rotation_degrees == 270
									   ? decode->source_width
									   : decode->source_height;
		frame_bytes              = (int64_t)decode->output_width * decode->output_height * 4;
		if (frame_bytes <= 0 || (uint64_t)frame_bytes > player->max_frame_bytes ||
			(uint64_t)frame_bytes * (player->queue_capacity + 1u) > player->max_frame_bytes * 2u) {
			return 0;
		}
		if (!AeronVideo_AllocateFrames(decode)) {
			return 0;
		}
		sar = av_guess_sample_aspect_ratio(decode->format, decode->video_stream, NULL);
		if (sar.num <= 0 || sar.den <= 0) {
			sar = (AVRational) { 1, 1 };
		}
		player->info.width  = decode->output_width;
		player->info.height = decode->output_height;
		if (decode->rotation_degrees == 90 || decode->rotation_degrees == 270) {
			player->info.display_width  = decode->source_height;
			player->info.display_height = (int)av_rescale(decode->source_width, sar.den, sar.num);
		} else {
			player->info.display_width  = (int)av_rescale(decode->source_width, sar.num, sar.den);
			player->info.display_height = decode->source_height;
		}
		player->info.rotation_degrees = decode->rotation_degrees;
		player->info.has_video        = 1;
		player->info.frame_rate_num   = decode->video_stream->avg_frame_rate.num;
		player->info.frame_rate_den   = decode->video_stream->avg_frame_rate.den;
		SDL_snprintf(player->info.video_codec, sizeof(player->info.video_codec), "%s",
					 decode->video_codec->codec->name);
		if (decode->video_stream->avg_frame_rate.num > 0 && decode->video_stream->avg_frame_rate.den > 0) {
			decode->fallback_frame_duration_us =
				av_rescale_q(1, av_inv_q(decode->video_stream->avg_frame_rate), AV_TIME_BASE_Q);
		} else {
			decode->fallback_frame_duration_us = 33333;
		}
	}

	if (decode->audio_stream_index >= 0) {
		AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
		size_t          capacity_frames;
		decode->audio_stream = decode->format->streams[decode->audio_stream_index];
		decode->audio_codec  = AeronVideo_OpenCodec(decode->format, decode->audio_stream_index);
		if (!decode->audio_codec || decode->audio_codec->sample_rate <= 0 ||
			decode->audio_codec->ch_layout.nb_channels <= 0) {
			return 0;
		}
		player->info.has_audio         = 1;
		player->info.audio_channels    = decode->audio_codec->ch_layout.nb_channels;
		player->info.audio_sample_rate = decode->audio_codec->sample_rate;
		SDL_snprintf(player->info.audio_codec, sizeof(player->info.audio_codec), "%s",
					 decode->audio_codec->codec->name);
		capacity_frames = (size_t)AERON_VIDEO_AUDIO_RATE * player->audio_buffer_ms / 1000u;
		if (capacity_frames < 4096u) {
			capacity_frames = 4096u;
		}
		{
			const AeronAudioStream audio_stream =
				Aeron_AudioStreamOpen(AERON_VIDEO_AUDIO_RATE, AERON_VIDEO_AUDIO_CHANNELS, AERON_PCM_S16,
									  capacity_frames, player->gain);
			SDL_LockMutex(player->lock);
			player->audio_stream = audio_stream;
			SDL_UnlockMutex(player->lock);
		}
		if (player->audio_stream) {
			if (swr_alloc_set_opts2(&decode->resampler, &output_layout, AV_SAMPLE_FMT_S16,
									AERON_VIDEO_AUDIO_RATE, &decode->audio_codec->ch_layout,
									decode->audio_codec->sample_fmt, decode->audio_codec->sample_rate, 0,
									NULL) < 0 ||
				swr_init(decode->resampler) < 0) {
				return 0;
			}
		}
	}
	return 1;
}

static int AeronVideo_InitDecoder(AeronVideoDecodeContext* decode) {
	AeronVideoPlayer* player = decode->player;
	int               result;

	decode->avio = AeronFfmpegVfs_CreateIo(player->file);
	if (!decode->avio)
		return AVERROR(ENOMEM);
	decode->format = avformat_alloc_context();
	if (!decode->format) {
		return AVERROR(ENOMEM);
	}
	decode->format->pb = decode->avio;
	decode->format->flags |= AVFMT_FLAG_CUSTOM_IO;
	decode->format->interrupt_callback.callback = AeronVideo_ShouldInterrupt;
	decode->format->interrupt_callback.opaque   = player;
	result                                      = avformat_open_input(&decode->format, NULL, NULL, NULL);
	if (result < 0) {
		return result;
	}
	result = avformat_find_stream_info(decode->format, NULL);
	if (result < 0) {
		return result;
	}
	decode->timeline_origin_us =
		decode->format->start_time == AV_NOPTS_VALUE ? 0 : decode->format->start_time;
	decode->packet = av_packet_alloc();
	decode->frame  = av_frame_alloc();
	if (!decode->packet || !decode->frame || !AeronVideo_InitStreams(decode)) {
		return AVERROR(EINVAL);
	}

	SDL_LockMutex(player->lock);
	player->info.duration_us = decode->format->duration == AV_NOPTS_VALUE ? -1 : decode->format->duration;
	SDL_snprintf(player->info.container, sizeof(player->info.container), "%s",
				 decode->format->iformat && decode->format->iformat->name ? decode->format->iformat->name
																		  : "unknown");
	player->worker_ready = 1;
	player->state        = player->desired_playing ? AERON_VIDEO_BUFFERING : AERON_VIDEO_PAUSED;
	SDL_BroadcastCondition(player->condition);
	SDL_UnlockMutex(player->lock);

	Aeron_LogInfo("aeron.video", "%s: %s, video=%s %dx%d, audio=%s %d Hz/%d ch", player->path,
				  player->info.container, player->info.has_video ? player->info.video_codec : "none",
				  player->info.width, player->info.height,
				  player->info.has_audio ? player->info.audio_codec : "none", player->info.audio_sample_rate,
				  player->info.audio_channels);
	return 0;
}

static int AeronVideo_HandleControl(AeronVideoDecodeContext* decode) {
	AeronVideoPlayer* player    = decode->player;
	int               seek      = 0;
	int64_t           target_us = 0;

	SDL_LockMutex(player->lock);
	while (player->stopped && !player->stop_requested && !player->seek_pending) {
		SDL_WaitCondition(player->condition, player->lock);
	}
	if (player->stop_requested) {
		SDL_UnlockMutex(player->lock);
		return -1;
	}
	if (player->seek_pending) {
		seek                          = 1;
		target_us                     = player->seek_target_us;
		player->seek_pending          = 0;
		player->worker_eof            = 0;
		player->total_audio_frames    = 0;
		player->audio_start_pts_us    = target_us;
		player->audio_prebuffer_ready = 0;
		player->audio_clock_exhausted = 0;
		SDL_BroadcastCondition(player->condition);
	}
	SDL_UnlockMutex(player->lock);

	if (seek) {
		const int result =
			av_seek_frame(decode->format, -1, target_us + decode->timeline_origin_us, AVSEEK_FLAG_BACKWARD);
		if (result < 0) {
			return result;
		}
		if (decode->video_codec) {
			avcodec_flush_buffers(decode->video_codec);
		}
		if (decode->audio_codec) {
			avcodec_flush_buffers(decode->audio_codec);
		}
		if (decode->resampler) {
			swr_close(decode->resampler);
			if (swr_init(decode->resampler) < 0) {
				return AVERROR(EINVAL);
			}
		}
		decode->next_video_pts_us       = target_us;
		decode->discard_audio_before_us = target_us;
		decode->next_video_index        = decode->fallback_frame_duration_us > 0
											  ? (uint64_t)(target_us / decode->fallback_frame_duration_us)
											  : 0;
	}
	return seek;
}

static int AeronVideo_DrainResampler(AeronVideoDecodeContext* decode) {
	if (!decode->resampler || !decode->player->audio_stream) {
		return 0;
	}
	for (;;) {
		const int64_t delay = swr_get_delay(decode->resampler, AERON_VIDEO_AUDIO_RATE);
		size_t        bytes;
		uint8_t*      output;
		int           converted;
		int           result;

		if (delay <= 0 || delay > INT_MAX) {
			return delay > INT_MAX ? AVERROR(EOVERFLOW) : 0;
		}
		bytes = (size_t)delay * AERON_VIDEO_AUDIO_CHANNELS * sizeof(int16_t);
		if (bytes > UINT_MAX) {
			return AVERROR(EOVERFLOW);
		}
		av_fast_malloc(&decode->audio_temp, &decode->audio_temp_size, bytes);
		if (!decode->audio_temp) {
			return AVERROR(ENOMEM);
		}
		output    = decode->audio_temp;
		converted = swr_convert(decode->resampler, &output, (int)delay, NULL, 0);
		if (converted < 0) {
			return converted;
		}
		if (converted == 0) {
			return 0;
		}
		result = AeronVideo_QueueConvertedAudio(decode, output, (size_t)converted, AV_NOPTS_VALUE);
		if (result < 0) {
			return result;
		}
	}
}

static int AeronVideo_FlushDecoders(AeronVideoDecodeContext* decode) {
	int result;
	if (decode->video_codec) {
		result = AeronVideo_SendPacket(decode, decode->video_codec, NULL, 1);
		if (result < 0) {
			return result;
		}
	}
	if (decode->audio_codec) {
		result = AeronVideo_SendPacket(decode, decode->audio_codec, NULL, 0);
		if (result < 0) {
			return result;
		}
	}
	return AeronVideo_DrainResampler(decode);
}

static void AeronVideo_CleanupDecoder(AeronVideoDecodeContext* decode) {
	AeronVideoPlayer* player = decode->player;
	av_packet_free(&decode->packet);
	av_frame_free(&decode->frame);
	sws_freeContext(decode->scaler);
	swr_free(&decode->resampler);
	avcodec_free_context(&decode->video_codec);
	avcodec_free_context(&decode->audio_codec);
	avformat_close_input(&decode->format);
	AeronFfmpegVfs_DestroyIo(&decode->avio);
	av_free(decode->rgba_temp);
	av_free(decode->audio_temp);
	if (player->file) {
		AeronVfs_Close(player->file);
		SDL_LockMutex(player->lock);
		player->file = NULL;
		SDL_UnlockMutex(player->lock);
	}
}

int AeronVideo_WorkerMain(void* userdata) {
	AeronVideoPlayer*       player = (AeronVideoPlayer*)userdata;
	AeronVideoDecodeContext decode;
	char                    error_text[AV_ERROR_MAX_STRING_SIZE];
	int                     result;

	memset(&decode, 0, sizeof(decode));
	decode.player                  = player;
	decode.video_stream_index      = -1;
	decode.audio_stream_index      = -1;
	decode.discard_audio_before_us = -1;
	result                         = AeronVideo_InitDecoder(&decode);
	if (result < 0) {
		int stopping;
		SDL_LockMutex(player->lock);
		stopping = player->stop_requested;
		SDL_UnlockMutex(player->lock);
		if (!stopping) {
			AeronVideo_FfmpegError(error_text, sizeof(error_text), result);
			AeronVideo_SetError(player, "could not initialize decoder: %s", error_text);
		}
		AeronVideo_CleanupDecoder(&decode);
		return 0;
	}

	for (;;) {
		result = AeronVideo_HandleControl(&decode);
		if (result < 0) {
			break;
		}
		result = av_read_frame(decode.format, decode.packet);
		if (result == AVERROR_EOF) {
			result = AeronVideo_FlushDecoders(&decode);
			if (result < 0) {
				int stopping;
				SDL_LockMutex(player->lock);
				stopping = player->stop_requested || player->seek_pending || player->stopped;
				SDL_UnlockMutex(player->lock);
				if (stopping || result == AVERROR_EXIT) {
					continue;
				}
				AeronVideo_FfmpegError(error_text, sizeof(error_text), result);
				AeronVideo_SetError(player, "decoder flush failed: %s", error_text);
				break;
			}
			SDL_LockMutex(player->lock);
			{
				const uint64_t trimmed = player->audio_frames_trimmed;
				SDL_UnlockMutex(player->lock);
				if (trimmed > decode.reported_audio_frames_trimmed) {
					Aeron_LogWarn("aeron.video", "%s: trimmed %llu audio frames beyond the media duration",
								  player->path,
								  (unsigned long long)(trimmed - decode.reported_audio_frames_trimmed));
					decode.reported_audio_frames_trimmed = trimmed;
				}
			}
			SDL_LockMutex(player->lock);
			player->worker_eof = 1;
			SDL_BroadcastCondition(player->condition);
			while (!player->stop_requested && !player->seek_pending && !player->stopped) {
				SDL_WaitCondition(player->condition, player->lock);
			}
			SDL_UnlockMutex(player->lock);
			continue;
		}
		if (result < 0) {
			int stopping;
			SDL_LockMutex(player->lock);
			stopping = player->stop_requested || player->seek_pending || player->stopped;
			SDL_UnlockMutex(player->lock);
			if (stopping || result == AVERROR_EXIT) {
				continue;
			}
			AeronVideo_FfmpegError(error_text, sizeof(error_text), result);
			AeronVideo_SetError(player, "demux failed: %s", error_text);
			break;
		}

		if (decode.packet->stream_index == decode.video_stream_index) {
			result = AeronVideo_SendPacket(&decode, decode.video_codec, decode.packet, 1);
		} else if (decode.packet->stream_index == decode.audio_stream_index) {
			result = AeronVideo_SendPacket(&decode, decode.audio_codec, decode.packet, 0);
		} else {
			result = 0;
		}
		av_packet_unref(decode.packet);
		if (result < 0 && result != AVERROR_EXIT) {
			if (result == AVERROR(ENOTSUP)) {
				AeronVideo_SetError(player, "unsupported HDR transfer function");
			} else {
				AeronVideo_FfmpegError(error_text, sizeof(error_text), result);
				AeronVideo_SetError(player, "decode failed: %s", error_text);
			}
			break;
		}
	}

	AeronVideo_CleanupDecoder(&decode);
	return 0;
}
