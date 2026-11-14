#ifndef AERON_AUDIO_DEBUG_H
#define AERON_AUDIO_DEBUG_H

/* The mixer's side of the audio debug tool (audio_debug.cpp). */

#ifdef __cplusplus
extern "C" {
#endif

/* What the audio debug tool shows (audio_debug.cpp). The mix load is the
 * time spent in the device callback divided by the duration of the audio it
 * produced, over the last full second: on average, and in the worst
 * callback. */
typedef struct AeronAudioDebugInfo {
	int   out_channels;
	int   device_channels;
	int   headphones;
	int   hrtf_taps;
	int   hrtf_responses;
	int   voices;
	int   positional_voices;
	float mix_load_average;
	float mix_load_peak;
} AeronAudioDebugInfo;

void Aeron_AudioGetDebugInfo(AeronAudioDebugInfo* info);
/* Adds the Audio tool to the debug UI. Built only with AERON_DEBUG_UI. */
void Aeron_AudioRegisterDebugTool(void);

#ifdef __cplusplus
}
#endif

#endif
