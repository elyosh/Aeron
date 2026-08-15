#include "aeron/audio_decode.h"

#include "aeron/log.h"
#include "ffmpeg_vfs_io.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

#include <limits.h>
#include <string.h>

enum { AERON_AUDIO_DECODER_PATH_SIZE = 1024 };

struct AeronAudioDecoder {
	AeronFile*       file;
	char             path[AERON_AUDIO_DECODER_PATH_SIZE];
	AVFormatContext* format;
	AVIOContext*     avio;
	AVCodecContext*  codec;
	AVStream*        stream;
	AVPacket*        packet;
	AVFrame*         frame;
	SwrContext*      resampler;
	uint8_t*         converted;
	unsigned int     converted_capacity;
	size_t           converted_frames;
	size_t           converted_offset;
	int              stream_index;
	int              sample_rate;
	int64_t          duration_us;
	int64_t          discard_until_us;
	int              sent_eof;
	int              resampler_drained;
	int              failed;
};

static void AeronAudioDecoder_LogError(AeronAudioDecoder* decoder, const char* operation, int error) {
	char message[AV_ERROR_MAX_STRING_SIZE];
	av_make_error_string(message, sizeof message, error);
	Aeron_LogError("aeron.audio_decode", "%s: %s failed: %s", decoder->path, operation, message);
}

static void AeronAudioDecoder_Destroy(AeronAudioDecoder* decoder) {
	if (!decoder)
		return;
	av_packet_free(&decoder->packet);
	av_frame_free(&decoder->frame);
	swr_free(&decoder->resampler);
	avcodec_free_context(&decoder->codec);
	avformat_close_input(&decoder->format);
	AeronFfmpegVfs_DestroyIo(&decoder->avio);
	av_free(decoder->converted);
	if (decoder->file)
		AeronVfs_Close(decoder->file);
	av_free(decoder);
}

static int AeronAudioDecoder_Configure(AeronAudioDecoder* decoder) {
	const AVCodec*  codec_impl = NULL;
	AVChannelLayout output_layout;
	int             result;

	decoder->avio = AeronFfmpegVfs_CreateIo(decoder->file);
	if (!decoder->avio)
		return AVERROR(ENOMEM);
	decoder->format = avformat_alloc_context();
	if (!decoder->format)
		return AVERROR(ENOMEM);
	decoder->format->pb = decoder->avio;
	decoder->format->flags |= AVFMT_FLAG_CUSTOM_IO;
	result = avformat_open_input(&decoder->format, NULL, NULL, NULL);
	if (result < 0)
		return result;
	result = avformat_find_stream_info(decoder->format, NULL);
	if (result < 0)
		return result;
	decoder->stream_index = av_find_best_stream(decoder->format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec_impl, 0);
	if (decoder->stream_index < 0 || !codec_impl)
		return decoder->stream_index < 0 ? decoder->stream_index : AVERROR_DECODER_NOT_FOUND;
	decoder->stream = decoder->format->streams[decoder->stream_index];
	decoder->codec  = avcodec_alloc_context3(codec_impl);
	if (!decoder->codec)
		return AVERROR(ENOMEM);
	result = avcodec_parameters_to_context(decoder->codec, decoder->stream->codecpar);
	if (result < 0)
		return result;
	result = avcodec_open2(decoder->codec, codec_impl, NULL);
	if (result < 0)
		return result;
	if (decoder->codec->sample_rate <= 0 || decoder->codec->ch_layout.nb_channels <= 0)
		return AVERROR_INVALIDDATA;

	decoder->sample_rate = decoder->codec->sample_rate;
	av_channel_layout_default(&output_layout, 2);
	result = swr_alloc_set_opts2(&decoder->resampler, &output_layout, AV_SAMPLE_FMT_S16, decoder->sample_rate,
								 &decoder->codec->ch_layout, decoder->codec->sample_fmt,
								 decoder->codec->sample_rate, 0, NULL);
	av_channel_layout_uninit(&output_layout);
	if (result < 0 || !decoder->resampler)
		return result < 0 ? result : AVERROR(ENOMEM);
	result = swr_init(decoder->resampler);
	if (result < 0)
		return result;

	decoder->packet = av_packet_alloc();
	decoder->frame  = av_frame_alloc();
	if (!decoder->packet || !decoder->frame)
		return AVERROR(ENOMEM);
	if (decoder->format->duration != AV_NOPTS_VALUE) {
		decoder->duration_us = decoder->format->duration;
	} else if (decoder->stream->duration != AV_NOPTS_VALUE) {
		decoder->duration_us =
			av_rescale_q(decoder->stream->duration, decoder->stream->time_base, AV_TIME_BASE_Q);
	} else {
		decoder->duration_us = -1;
	}
	decoder->discard_until_us = -1;
	return 0;
}

AeronAudioDecoder* Aeron_AudioDecoderOpenFile(AeronFile* file, const char* debug_name) {
	AeronAudioDecoder* decoder;
	int                result;

	if (!file)
		return NULL;
	decoder = (AeronAudioDecoder*)av_mallocz(sizeof(*decoder));
	if (!decoder) {
		AeronVfs_Close(file);
		return NULL;
	}
	decoder->file             = file;
	decoder->stream_index     = -1;
	decoder->duration_us      = -1;
	decoder->discard_until_us = -1;
	if (debug_name && debug_name[0])
		av_strlcpy(decoder->path, debug_name, sizeof decoder->path);
	else
		av_strlcpy(decoder->path, "<audio>", sizeof decoder->path);
	result = AeronAudioDecoder_Configure(decoder);
	if (result < 0) {
		AeronAudioDecoder_LogError(decoder, "open", result);
		AeronAudioDecoder_Destroy(decoder);
		return NULL;
	}
	return decoder;
}

AeronAudioDecoder* Aeron_AudioDecoderOpen(AeronVfs* vfs, AeronVfsRoot root, const char* path) {
	AeronFile* file = NULL;
	if (!vfs || !path || !AeronVfs_Open(vfs, root, path, AERON_VFS_READ, &file))
		return NULL;
	return Aeron_AudioDecoderOpenFile(file, path);
}

void Aeron_AudioDecoderGetInfo(const AeronAudioDecoder* decoder, AeronAudioDecoderInfo* out) {
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->duration_us = -1;
	if (!decoder)
		return;
	out->sample_rate = decoder->sample_rate;
	out->channels    = 2;
	out->duration_us = decoder->duration_us;
}

static int64_t AeronAudioDecoder_FrameTimeUs(const AeronAudioDecoder* decoder, const AVFrame* frame) {
	int64_t timestamp = frame->best_effort_timestamp;
	int64_t start;
	if (timestamp == AV_NOPTS_VALUE)
		timestamp = frame->pts;
	if (timestamp == AV_NOPTS_VALUE)
		return AV_NOPTS_VALUE;
	start = decoder->stream->start_time == AV_NOPTS_VALUE ? 0 : decoder->stream->start_time;
	return av_rescale_q(timestamp - start, decoder->stream->time_base, AV_TIME_BASE_Q);
}

static int AeronAudioDecoder_StoreConverted(AeronAudioDecoder* decoder, AVFrame* frame) {
	uint8_t* output;
	int      output_capacity;
	int      converted;
	size_t   skip = 0;

	output_capacity = swr_get_out_samples(decoder->resampler, frame ? frame->nb_samples : 0);
	if (output_capacity < 0)
		return output_capacity;
	if (output_capacity == 0)
		return 0;
	av_fast_malloc(&decoder->converted, &decoder->converted_capacity,
				   (size_t)output_capacity * 2u * sizeof(int16_t));
	if (!decoder->converted)
		return AVERROR(ENOMEM);
	output    = decoder->converted;
	converted = swr_convert(decoder->resampler, &output, output_capacity,
							frame ? (const uint8_t* const*)frame->extended_data : NULL,
							frame ? frame->nb_samples : 0);
	if (converted < 0)
		return converted;

	if (frame && decoder->discard_until_us >= 0 && converted > 0) {
		const int64_t frame_us = AeronAudioDecoder_FrameTimeUs(decoder, frame);
		if (frame_us != AV_NOPTS_VALUE && frame_us < decoder->discard_until_us) {
			const int64_t delta_us = decoder->discard_until_us - frame_us;
			skip = (size_t)av_rescale_rnd(delta_us, decoder->sample_rate, AV_TIME_BASE, AV_ROUND_UP);
			if (skip > (size_t)converted)
				skip = (size_t)converted;
		}
		if (skip < (size_t)converted)
			decoder->discard_until_us = -1;
	}
	decoder->converted_offset = skip;
	decoder->converted_frames = (size_t)converted;
	return converted > 0 ? 1 : 0;
}

static int AeronAudioDecoder_SendNextPacket(AeronAudioDecoder* decoder) {
	int result;
	for (;;) {
		result = av_read_frame(decoder->format, decoder->packet);
		if (result == AVERROR_EOF) {
			if (decoder->sent_eof)
				return 0;
			decoder->sent_eof = 1;
			result            = avcodec_send_packet(decoder->codec, NULL);
			return result == AVERROR_EOF ? 0 : result;
		}
		if (result < 0)
			return result;
		if (decoder->packet->stream_index != decoder->stream_index) {
			av_packet_unref(decoder->packet);
			continue;
		}
		result = avcodec_send_packet(decoder->codec, decoder->packet);
		av_packet_unref(decoder->packet);
		return result;
	}
}

static int AeronAudioDecoder_FillPending(AeronAudioDecoder* decoder) {
	int result;

	decoder->converted_frames = 0;
	decoder->converted_offset = 0;
	for (;;) {
		result = avcodec_receive_frame(decoder->codec, decoder->frame);
		if (result == 0) {
			result = AeronAudioDecoder_StoreConverted(decoder, decoder->frame);
			av_frame_unref(decoder->frame);
			if (result < 0)
				return result;
			if (decoder->converted_offset < decoder->converted_frames)
				return 1;
			continue;
		}
		if (result == AVERROR(EAGAIN)) {
			if (decoder->sent_eof)
				return AVERROR_INVALIDDATA;
			result = AeronAudioDecoder_SendNextPacket(decoder);
			if (result == AVERROR(EAGAIN))
				continue;
			if (result < 0)
				return result;
			continue;
		}
		if (result == AVERROR_EOF) {
			if (decoder->resampler_drained)
				return 0;
			result = AeronAudioDecoder_StoreConverted(decoder, NULL);
			if (result < 0)
				return result;
			if (decoder->converted_frames > 0)
				return 1;
			decoder->resampler_drained = 1;
			return 0;
		}
		return result;
	}
}

size_t Aeron_AudioDecoderRead(AeronAudioDecoder* decoder, int16_t* pcm, size_t max_frames) {
	size_t written = 0;

	if (!decoder || !pcm || max_frames == 0 || decoder->failed)
		return 0;
	while (written < max_frames) {
		size_t available = decoder->converted_frames - decoder->converted_offset;
		if (available == 0) {
			const int result = AeronAudioDecoder_FillPending(decoder);
			if (result < 0) {
				AeronAudioDecoder_LogError(decoder, "decode", result);
				decoder->failed = 1;
				break;
			}
			if (result == 0)
				break;
			available = decoder->converted_frames - decoder->converted_offset;
		}
		if (available > max_frames - written)
			available = max_frames - written;
		memcpy(pcm + written * 2u, (const int16_t*)decoder->converted + decoder->converted_offset * 2u,
			   available * 2u * sizeof(int16_t));
		decoder->converted_offset += available;
		written += available;
	}
	return written;
}

int Aeron_AudioDecoderSeekUs(AeronAudioDecoder* decoder, int64_t position_us) {
	int64_t start;
	int64_t timestamp;
	int     result;

	if (!decoder || position_us < 0 || (decoder->duration_us >= 0 && position_us > decoder->duration_us))
		return 0;
	start     = decoder->stream->start_time == AV_NOPTS_VALUE ? 0 : decoder->stream->start_time;
	timestamp = start + av_rescale_q(position_us, AV_TIME_BASE_Q, decoder->stream->time_base);
	result    = av_seek_frame(decoder->format, decoder->stream_index, timestamp, AVSEEK_FLAG_BACKWARD);
	if (result < 0) {
		AeronAudioDecoder_LogError(decoder, "seek", result);
		return 0;
	}
	avcodec_flush_buffers(decoder->codec);
	swr_close(decoder->resampler);
	if (swr_init(decoder->resampler) < 0)
		return 0;
	decoder->converted_frames  = 0;
	decoder->converted_offset  = 0;
	decoder->sent_eof          = 0;
	decoder->resampler_drained = 0;
	decoder->failed            = 0;
	decoder->discard_until_us  = position_us;
	return 1;
}

void Aeron_AudioDecoderClose(AeronAudioDecoder* decoder) { AeronAudioDecoder_Destroy(decoder); }
