#ifndef AERON_HEAD_TRACKER_H
#define AERON_HEAD_TRACKER_H

#include "aeron/scene/view_rig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Head tracking for a player in front of a screen. Aeron reads the head's
 * pose from the tracker's software on each frame. The application decides
 * what to do with it.
 *
 * These functions exist in every build. A VR headset has its own head pose
 * (aeron/xr.h): while a VR session runs, the tracker gives no pose. */

typedef enum AeronHeadTrackerKind {
	AERON_HEAD_TRACKER_NONE,
	/* The TrackIR interface: the NPClient64.dll that the TrackIR software
	 * installs, or the one that OpenTrack's "freetrack 2.0 Enhanced" output
	 * installs. Windows only. */
	AERON_HEAD_TRACKER_TRACKIR,
	/* OpenTrack's "UDP over network" output, and the phone apps that send
	 * the same datagrams. */
	AERON_HEAD_TRACKER_OPENTRACK,
} AeronHeadTrackerKind;

typedef enum AeronHeadTrackerStatus {
	AERON_HEAD_TRACKER_OFF,
	/* The tracker is chosen and gives no head pose: its software isn't
	 * running yet, or it has stopped sending. */
	AERON_HEAD_TRACKER_WAITING,
	AERON_HEAD_TRACKER_TRACKING,
	/* The tracker can't be read. Aeron_HeadTrackerError gives the reason. */
	AERON_HEAD_TRACKER_FAILED,
} AeronHeadTrackerStatus;

/* Nonzero when this build can read this kind of tracker. */
int Aeron_HeadTrackerAvailable(AeronHeadTrackerKind kind);
/* Chooses the tracker, and stops reading the one chosen before. It can be
 * called at any time. `udp_port` is the port that OpenTrack sends to; the
 * other kinds don't use it. A kind that isn't available is treated as none. */
void                   Aeron_SetHeadTracker(AeronHeadTrackerKind kind, int udp_port);
AeronHeadTrackerStatus Aeron_HeadTrackerStatus(void);
/* Why the tracker can't be read, as a sentence to show the user. NULL when
 * the status isn't AERON_HEAD_TRACKER_FAILED. */
const char* Aeron_HeadTrackerError(void);
/* Gives the head's pose as of this frame, in the space of a view rig: X
 * right, Y up, -Z forward, in metres, relative to the centre. Returns 0 when
 * the status isn't AERON_HEAD_TRACKER_TRACKING. */
int Aeron_HeadTrackerPose(AeronViewPose* out);
/* Makes the current pose the centre. It does nothing while the tracker gives
 * no pose. */
void Aeron_HeadTrackerRecenter(void);

#ifdef __cplusplus
}
#endif

#endif
