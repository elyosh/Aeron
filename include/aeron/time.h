#ifndef AERON_TIME_H
#define AERON_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns SDL monotonic ticks converted to microseconds. */
uint64_t Aeron_NowUs(void);
/* Selects full-refresh (1) or half-refresh (2) VSync pacing. */
int Aeron_SetPresentationVsyncDivisor(int divisor);
int Aeron_PresentationVsyncDivisor(void);
/* Current display refresh and configured presentation rate in Hz. */
double Aeron_DisplayRefreshRate(void);
double Aeron_PresentationRate(void);
/* Sleeps until the delay from the current frame start and configured presentation pacing are satisfied. */
void Aeron_WaitForNextFrame(uint64_t app_wake_delay_us);
/* How many frames the GPU may work on at once: 1 or 2. With 2, the next
 * frame starts while the GPU still draws the last one. Changing it waits for
 * the GPU to finish its work. */
int Aeron_SetFramesInFlight(int count);
int Aeron_FramesInFlight(void);
/* Latency pacing: with full-refresh VSync, the next frame starts later by
 * the time the last frames waited for the vsync with their GPU work already
 * done. It then reads input later for the same display time. */
void Aeron_SetLatencyPacing(int enabled);
int  Aeron_LatencyPacing(void);

typedef struct AeronPacingStats {
	int      active;          /* the delay is applied */
	uint32_t delay_us;        /* the delay before the next frame reads input */
	uint32_t acquire_wait_us; /* median wait for the swapchain image, last 120 frames */
	uint32_t late_frames;     /* frames shown more than 1.5 intervals after the last, in the last second */
	uint32_t busy_frames;     /* frames whose GPU work wasn't done at the vsync, in the last second */
} AeronPacingStats;

void Aeron_GetPacingStats(AeronPacingStats* out);

#ifdef __cplusplus
}
#endif

#endif
