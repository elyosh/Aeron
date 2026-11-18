#ifndef AERON_HEAD_TRACKER_INTERNAL_H
#define AERON_HEAD_TRACKER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "aeron/scene/view_rig.h"

/* The parts of the head tracker (aeron/head_tracker.h). They don't use SDL
 * or Aeron's state, so tests/head_tracker_test.c can build the math and the
 * UDP reader on their own. */

/* --- The math (head_tracker_math.c) --- */

/* Converts the six values of a TrackIR frame, in the order of its data:
 * roll, pitch, yaw, then X, Y, Z. Each is from -16383 to 16383, which is 180
 * degrees for an angle and 50 cm for a movement. */
void AeronHeadTracker_FromTrackIR(const float values[6], AeronViewPose* out);
/* Converts the six values of an OpenTrack datagram: X, Y, Z in centimetres,
 * then yaw, pitch, roll in degrees. */
void AeronHeadTracker_FromOpenTrack(const double values[6], AeronViewPose* out);
/* Gives `pose` as seen from `reference`: the identity when both are the
 * same. */
void AeronHeadTracker_Relative(const AeronViewPose* reference, const AeronViewPose* pose, AeronViewPose* out);

/* --- OpenTrack's UDP output (head_tracker_udp.c) --- */

#define AERON_HEAD_UDP_DATAGRAM_SIZE 48

typedef struct AeronHeadUdp {
	int       open;
	uintptr_t socket;
} AeronHeadUdp;

/* Opens a socket that receives the datagrams sent to `port`, on every
 * address of this computer. Returns 0 and writes a sentence for the user
 * when it can't. */
int  AeronHeadUdp_Open(AeronHeadUdp* udp, int port, char* error, size_t error_size);
void AeronHeadUdp_Close(AeronHeadUdp* udp);
/* Reads the datagrams that arrived, without waiting, and gives the values
 * of the last good one. Returns 0 when none arrived. */
int AeronHeadUdp_Read(AeronHeadUdp* udp, double values[6]);
/* Returns whether `data` is a head pose: 48 bytes that hold six finite
 * numbers. Writes them to `values`. */
int AeronHeadUdp_Decode(const void* data, size_t size, double values[6]);

/* --- The TrackIR interface (head_tracker_trackir_win32.c) --- */

/* Reads a frame, and connects first when needed: finds NPClient64.dll,
 * loads it and registers `window`, the HWND of the application's window. A
 * failed connection is tried again 2 seconds later. `now_us` is the current
 * time. Returns 1 when the tracker gave a new frame, and writes its six
 * values, as AeronHeadTracker_FromTrackIR reads them. */
int  AeronHeadTrackIR_Read(void* window, uint64_t now_us, float values[6]);
void AeronHeadTrackIR_Close(void);

#endif
