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

#ifdef __cplusplus
}
#endif

#endif
