#include "internal.h"
#include "time_internal.h"

uint64_t Aeron_NowUs(void) { return SDL_GetTicksNS() / 1000u; }

uint64_t Aeron_TimeFrameStartUs(void) { return g_aeron.last_frame_us; }

static double Aeron_DisplayModeRefreshRate(const SDL_DisplayMode* mode) {
	if (!mode) {
		return 0.0;
	}
	if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0) {
		return (double)mode->refresh_rate_numerator / (double)mode->refresh_rate_denominator;
	}
	return (double)mode->refresh_rate;
}

static uint64_t Aeron_PresentationIntervalUs(void) {
	const double interval_us =
		1000000.0 * (double)Aeron_PresentationVsyncDivisor() / g_aeron.display_refresh_hz;
	return (uint64_t)(interval_us + 0.5);
}

void Aeron_RefreshPresentationTiming(void) {
	SDL_DisplayID          display_id;
	const SDL_DisplayMode* mode;
	double                 refresh_hz;

	if (!g_aeron.window) {
		return;
	}
	display_id = SDL_GetDisplayForWindow(g_aeron.window);
	mode       = display_id ? SDL_GetCurrentDisplayMode(display_id) : NULL;
	refresh_hz = Aeron_DisplayModeRefreshRate(mode);
	if ((refresh_hz < 1.0 || refresh_hz > 1000.0) && display_id) {
		refresh_hz = Aeron_DisplayModeRefreshRate(SDL_GetDesktopDisplayMode(display_id));
	}
	if (refresh_hz < 1.0 || refresh_hz > 1000.0) {
		refresh_hz = 60.0;
		Aeron_LogWarn("aeron", "display refresh unavailable; using 60 Hz for presentation pacing");
	}
	g_aeron.presentation_display_id = display_id;
	g_aeron.display_refresh_hz      = refresh_hz;
	g_aeron.presentation_next_frame_us =
		Aeron_PresentationVsyncDivisor() == 2 ? Aeron_NowUs() + Aeron_PresentationIntervalUs() : 0;
	Aeron_ResetPacing();
}

int Aeron_SetPresentationVsyncDivisor(int divisor) {
	if (!g_aeron.window || (divisor != 1 && divisor != 2)) {
		return 0;
	}
	g_aeron.presentation_vsync_divisor = divisor;
	Aeron_RefreshPresentationTiming();
	return 1;
}

int Aeron_PresentationVsyncDivisor(void) { return g_aeron.presentation_vsync_divisor == 2 ? 2 : 1; }

double Aeron_DisplayRefreshRate(void) {
	if (g_aeron.display_refresh_hz <= 0.0) {
		Aeron_RefreshPresentationTiming();
	}
	return g_aeron.display_refresh_hz;
}

double Aeron_PresentationRate(void) {
	return Aeron_DisplayRefreshRate() / (double)Aeron_PresentationVsyncDivisor();
}

void Aeron_WaitForNextFrame(uint64_t app_wake_delay_us) {
	uint64_t deadline_us;
	uint64_t now_us;

	if (!g_aeron.initialized) {
		return;
	}
	deadline_us = app_wake_delay_us > UINT64_MAX - g_aeron.last_frame_us
					  ? UINT64_MAX
					  : g_aeron.last_frame_us + app_wake_delay_us;
	if (Aeron_PresentationVsyncDivisor() == 2) {
		const uint64_t frame_interval_us = Aeron_PresentationIntervalUs();
		now_us                           = Aeron_NowUs();
		if (!g_aeron.presentation_next_frame_us) {
			g_aeron.presentation_next_frame_us = now_us + frame_interval_us;
		} else if (g_aeron.presentation_next_frame_us <= now_us) {
			const uint64_t expired_intervals =
				(now_us - g_aeron.presentation_next_frame_us) / frame_interval_us + 1u;
			g_aeron.presentation_next_frame_us += expired_intervals * frame_interval_us;
		}
		if (g_aeron.presentation_next_frame_us > deadline_us) {
			deadline_us = g_aeron.presentation_next_frame_us;
		}
	}
	now_us = Aeron_NowUs();
	/* A frame the app holds back is late by choice, which pacing ignores. */
	g_aeron.pacing.app_delayed = deadline_us > now_us;
	if (Aeron_PacingActive() && g_aeron.pacing.delay_us && g_aeron.pacing.last_present_us) {
		const uint64_t paced_us = g_aeron.pacing.last_present_us + g_aeron.pacing.delay_us;
		if (paced_us > deadline_us) {
			deadline_us = paced_us;
		}
	}
	if (deadline_us > now_us) {
		SDL_DelayNS((deadline_us - now_us) * 1000u);
	}
}

/* Latency pacing. The delay grows by a small step while the GPU work is done
 * when the swapchain image arrives and the present still waited for it. It
 * falls by a larger step when the GPU work isn't done by then, so about one
 * frame in ten finishes after the vsync, which is still before the frame's
 * real deadline. A late frame halves it and holds it down for a while. */
#define AERON_PACING_STEP_US 100u
#define AERON_PACING_DECAY_US 250u
#define AERON_PACING_BUSY_US 1000u
#define AERON_PACING_DROP_US 500u
#define AERON_PACING_MIN_WAIT_US 500u
#define AERON_PACING_HOLD_FRAMES 60u

void Aeron_ResetPacing(void) {
	g_aeron.pacing.delay_us    = 0;
	g_aeron.pacing.hold_frames = AERON_PACING_HOLD_FRAMES;
}

int Aeron_PacingActive(void) {
	return g_aeron.pacing.enabled && g_aeron.fence_query_reliable && g_aeron.gpu_device &&
		   Aeron_PresentationVsyncDivisor() == 1;
}

void Aeron_NotePacingPresent(uint64_t acquire_wait_us, int gpu_done, uint64_t present_us) {
	AeronPacingState* p           = &g_aeron.pacing;
	const uint64_t    interval_us = Aeron_PresentationIntervalUs();
	const int         late =
		p->last_present_us && present_us - p->last_present_us > interval_us * 3u / 2u && !p->app_delayed;
	const uint32_t i = p->history_next;

	p->present_us[i]      = present_us;
	p->acquire_wait_us[i] = acquire_wait_us > UINT32_MAX ? UINT32_MAX : (uint32_t)acquire_wait_us;
	p->late[i]            = (uint8_t)late;
	p->busy[i]            = (uint8_t)!gpu_done;
	p->history_next       = (i + 1u) % AERON_PACING_HISTORY;
	if (p->history_count < AERON_PACING_HISTORY) {
		p->history_count++;
	}
	p->last_present_us = present_us;

	if (!Aeron_PacingActive()) {
		p->delay_us    = 0;
		p->hold_frames = 0;
		return;
	}
	if (p->app_delayed) {
		return;
	}
	if (late) {
		p->delay_us = p->delay_us / 2u > AERON_PACING_DROP_US ? p->delay_us / 2u - AERON_PACING_DROP_US : 0;
		p->hold_frames = AERON_PACING_HOLD_FRAMES;
	} else if (!gpu_done) {
		p->delay_us = p->delay_us > AERON_PACING_BUSY_US ? p->delay_us - AERON_PACING_BUSY_US : 0;
	} else if (acquire_wait_us < AERON_PACING_MIN_WAIT_US) {
		p->delay_us = p->delay_us > AERON_PACING_DECAY_US ? p->delay_us - AERON_PACING_DECAY_US : 0;
	} else if (p->hold_frames) {
		p->hold_frames--;
	} else {
		const uint64_t max_us = interval_us / 2u;
		p->delay_us =
			(uint32_t)(p->delay_us + AERON_PACING_STEP_US < max_us ? p->delay_us + AERON_PACING_STEP_US
																   : max_us);
	}
}

void Aeron_SetLatencyPacing(int enabled) {
	g_aeron.pacing.enabled = enabled != 0;
	Aeron_ResetPacing();
}

int Aeron_LatencyPacing(void) { return g_aeron.pacing.enabled; }

static int Aeron_CompareU32(const void* a, const void* b) {
	const uint32_t x = *(const uint32_t*)a;
	const uint32_t y = *(const uint32_t*)b;
	return x < y ? -1 : x > y;
}

void Aeron_GetPacingStats(AeronPacingStats* out) {
	const AeronPacingState* p = &g_aeron.pacing;
	uint32_t                waits[AERON_PACING_HISTORY];
	const uint64_t          now_us = Aeron_NowUs();

	if (!out) {
		return;
	}
	SDL_zerop(out);
	out->active   = Aeron_PacingActive();
	out->delay_us = p->delay_us;
	for (uint32_t i = 0; i < p->history_count; ++i) {
		waits[i] = p->acquire_wait_us[i];
		if (now_us - p->present_us[i] <= 1000000u) {
			out->late_frames += p->late[i];
			out->busy_frames += p->busy[i];
		}
	}
	if (p->history_count) {
		SDL_qsort(waits, p->history_count, sizeof waits[0], Aeron_CompareU32);
		out->acquire_wait_us = waits[p->history_count / 2u];
	}
}
