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
/* Sleeps until an absolute monotonic deadline. Zero or past deadlines return immediately. */
void Aeron_Wait(uint64_t app_deadline_us);
/* Sleeps until both the application and configured presentation deadlines are satisfied. */
void Aeron_WaitForNextFrame(uint64_t app_deadline_us);

#ifdef __cplusplus
}
#endif

#endif
