/* The head tracker's state (aeron/head_tracker.h): which tracker is read,
 * its last pose, the centre, and whether the pose is still fresh. */

#include "aeron/head_tracker.h"

#include "head_tracker_internal.h"
#include "internal.h"

#include <string.h>

/* A tracker that sent nothing for this long has stopped. The slowest
 * trackers send 30 poses per second. */
#define HEAD_TIMEOUT_US 500000u

static struct {
	AeronHeadTrackerKind   kind;
	int                    udp_port;
	AeronHeadTrackerStatus status;
	char                   error[96];
	AeronHeadUdp           udp;
	/* The last pose that the tracker gave, and when. */
	int           has_pose;
	AeronViewPose pose;
	uint64_t      pose_time_us;
	/* The pose that Aeron_HeadTrackerRecenter made the centre. */
	int           has_center;
	AeronViewPose center;
} g_head;

int Aeron_HeadTrackerAvailable(AeronHeadTrackerKind kind) {
	switch (kind) {
		case AERON_HEAD_TRACKER_TRACKIR:
#ifdef _WIN32
			return 1;
#else
			return 0;
#endif
		case AERON_HEAD_TRACKER_OPENTRACK:
			return 1;
		default:
			return 0;
	}
}

static void head_close(void) {
	AeronHeadUdp_Close(&g_head.udp);
#ifdef _WIN32
	AeronHeadTrackIR_Close();
#endif
	memset(&g_head, 0, sizeof g_head);
}

void Aeron_SetHeadTracker(AeronHeadTrackerKind kind, int udp_port) {
	if (!Aeron_HeadTrackerAvailable(kind))
		kind = AERON_HEAD_TRACKER_NONE;
	if (kind != AERON_HEAD_TRACKER_OPENTRACK)
		udp_port = 0;
	if (kind == g_head.kind && udp_port == g_head.udp_port)
		return;
	head_close();
	g_head.kind     = kind;
	g_head.udp_port = udp_port;
	if (kind == AERON_HEAD_TRACKER_NONE)
		return;
	g_head.status = AERON_HEAD_TRACKER_WAITING;
	if (kind == AERON_HEAD_TRACKER_OPENTRACK) {
		if (AeronHeadUdp_Open(&g_head.udp, udp_port, g_head.error, sizeof g_head.error)) {
			Aeron_LogInfo("aeron.head", "OpenTrack: listening on UDP port %d", udp_port);
		} else {
			g_head.status = AERON_HEAD_TRACKER_FAILED;
			Aeron_LogWarn("aeron.head", "OpenTrack: %s", g_head.error);
		}
	}
}

AeronHeadTrackerStatus Aeron_HeadTrackerStatus(void) { return g_head.status; }

const char* Aeron_HeadTrackerError(void) {
	return g_head.status == AERON_HEAD_TRACKER_FAILED ? g_head.error : NULL;
}

/* Reads the chosen tracker. Returns 1 when it gave a new pose. */
static int head_read(uint64_t now_us, AeronViewPose* out) {
	if (g_head.kind == AERON_HEAD_TRACKER_OPENTRACK) {
		double values[6];
		if (!AeronHeadUdp_Read(&g_head.udp, values))
			return 0;
		AeronHeadTracker_FromOpenTrack(values, out);
		return 1;
	}
#ifdef _WIN32
	if (g_head.kind == AERON_HEAD_TRACKER_TRACKIR && g_aeron.window) {
		float values[6];
		void* window = SDL_GetPointerProperty(SDL_GetWindowProperties(g_aeron.window),
											  SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
		if (!AeronHeadTrackIR_Read(window, now_us, values))
			return 0;
		AeronHeadTracker_FromTrackIR(values, out);
		return 1;
	}
#endif
	(void)now_us;
	return 0;
}

void AeronHeadTracker_Update(void) {
	if (g_head.status != AERON_HEAD_TRACKER_WAITING && g_head.status != AERON_HEAD_TRACKER_TRACKING)
		return;
	const uint64_t now_us = Aeron_NowUs();
	if (Aeron_XrSessionRunning()) {
		/* A headset has its own head pose. The datagrams that arrive
		 * during the session are read and dropped, so that they aren't
		 * mistaken for new ones when it ends. */
		double dropped[6];
		AeronHeadUdp_Read(&g_head.udp, dropped);
		g_head.has_pose = 0;
	} else {
		AeronViewPose pose;
		if (head_read(now_us, &pose)) {
			g_head.pose         = pose;
			g_head.pose_time_us = now_us;
			g_head.has_pose     = 1;
		}
	}
	const int tracking = g_head.has_pose && now_us - g_head.pose_time_us < HEAD_TIMEOUT_US;
	if (tracking != (g_head.status == AERON_HEAD_TRACKER_TRACKING)) {
		Aeron_LogInfo("aeron.head", "%s",
					  tracking ? "the tracker started sending" : "the tracker stopped sending");
	}
	g_head.status = tracking ? AERON_HEAD_TRACKER_TRACKING : AERON_HEAD_TRACKER_WAITING;
}

void AeronHeadTracker_Shutdown(void) { head_close(); }

int Aeron_HeadTrackerPose(AeronViewPose* out) {
	if (g_head.status != AERON_HEAD_TRACKER_TRACKING || !out)
		return 0;
	if (g_head.has_center)
		AeronHeadTracker_Relative(&g_head.center, &g_head.pose, out);
	else
		*out = g_head.pose;
	return 1;
}

void Aeron_HeadTrackerRecenter(void) {
	if (g_head.status != AERON_HEAD_TRACKER_TRACKING)
		return;
	g_head.center     = g_head.pose;
	g_head.has_center = 1;
}
