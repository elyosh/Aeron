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
	SDL_DisplayID         display_id;
	const SDL_DisplayMode* mode;
	double                refresh_hz;

	if (!g_aeron.window) {
		return;
	}
	display_id = SDL_GetDisplayForWindow(g_aeron.window);
	mode = display_id ? SDL_GetCurrentDisplayMode(display_id) : NULL;
	refresh_hz = Aeron_DisplayModeRefreshRate(mode);
	if ((refresh_hz < 1.0 || refresh_hz > 1000.0) && display_id) {
		refresh_hz = Aeron_DisplayModeRefreshRate(SDL_GetDesktopDisplayMode(display_id));
	}
	if (refresh_hz < 1.0 || refresh_hz > 1000.0) {
		refresh_hz = 60.0;
		Aeron_LogWarn("aeron", "display refresh unavailable; using 60 Hz for presentation pacing");
	}
	g_aeron.presentation_display_id = display_id;
	g_aeron.display_refresh_hz = refresh_hz;
	g_aeron.presentation_next_frame_us =
		Aeron_PresentationVsyncDivisor() == 2 ? Aeron_NowUs() + Aeron_PresentationIntervalUs() : 0;
}

int Aeron_SetPresentationVsyncDivisor(int divisor) {
	if (!g_aeron.window || (divisor != 1 && divisor != 2)) {
		return 0;
	}
	g_aeron.presentation_vsync_divisor = divisor;
	Aeron_RefreshPresentationTiming();
	return 1;
}

int Aeron_PresentationVsyncDivisor(void) {
	return g_aeron.presentation_vsync_divisor == 2 ? 2 : 1;
}

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
		now_us = Aeron_NowUs();
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
	if (deadline_us > now_us) {
		SDL_DelayNS((deadline_us - now_us) * 1000u);
	}
}
