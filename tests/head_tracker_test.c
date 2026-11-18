/*
 * Tests the head tracker's parts that need no tracker (src/head_tracker_*.c):
 * converting what TrackIR and OpenTrack send to a pose, the recentre, and
 * the UDP reader, which gets datagrams that the test sends on the loopback
 * address.
 *
 * Built with the tools as aeron_head_tracker_test. It needs no window and no
 * GPU. It prints each check and exits with a non-zero status when a check
 * fails.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <windows.h>
typedef SOCKET TestSocket;
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int TestSocket;
#endif

#include "aeron/scene/view_rig.h"
#include "head_tracker_internal.h"

#define TOLERANCE 1e-4f
#define PI 3.14159265358979f
/* A full TrackIR value: 180 degrees or 50 cm. */
#define TRACKIR_FULL 16383.0f

static int g_failures;

static void check(int ok, const char* what) {
	printf("%s %s\n", ok ? "ok" : "FAILED", what);
	g_failures += !ok;
}

static int close_to(float value, float expected) { return fabsf(value - expected) <= TOLERANCE; }

static int close3(const float value[3], float x, float y, float z) {
	return close_to(value[0], x) && close_to(value[1], y) && close_to(value[2], z);
}

static float radians(float degrees) { return degrees * PI / 180.0f; }

/* Where one of the head's own axes points in rig space. The head's axes are
 * the rig's: X right, Y up, -Z forward. */
static void head_axis(const AeronViewPose* pose, float x, float y, float z, float out[3]) {
	float m[9];
	AeronViewPose_Rotation(pose, m);
	for (int row = 0; row < 3; ++row)
		out[row] = m[row * 3 + 0] * x + m[row * 3 + 1] * y + m[row * 3 + 2] * z;
}

static void forward_of(const AeronViewPose* pose, float out[3]) { head_axis(pose, 0.0f, 0.0f, -1.0f, out); }

static void up_of(const AeronViewPose* pose, float out[3]) { head_axis(pose, 0.0f, 1.0f, 0.0f, out); }

static int is_identity(const AeronViewPose* pose) {
	float forward[3], up[3];
	forward_of(pose, forward);
	up_of(pose, up);
	return close3(forward, 0.0f, 0.0f, -1.0f) && close3(up, 0.0f, 1.0f, 0.0f) &&
		   close3(pose->position_m, 0.0f, 0.0f, 0.0f);
}

static void test_from_angles(void) {
	AeronViewPose pose;
	float         forward[3], up[3];
	memset(&pose, 0, sizeof pose);

	AeronViewPose_FromAngles(radians(90.0f), 0.0f, 0.0f, pose.orientation);
	forward_of(&pose, forward);
	check(close3(forward, 1.0f, 0.0f, 0.0f), "angles: a positive yaw turns right");

	AeronViewPose_FromAngles(0.0f, radians(90.0f), 0.0f, pose.orientation);
	forward_of(&pose, forward);
	check(close3(forward, 0.0f, 1.0f, 0.0f), "angles: a positive pitch turns up");

	AeronViewPose_FromAngles(0.0f, 0.0f, radians(90.0f), pose.orientation);
	up_of(&pose, up);
	check(close3(up, 1.0f, 0.0f, 0.0f), "angles: a positive roll turns clockwise");

	/* Yaw first, then pitch around the turned head's own axis: the head
	 * looks up whatever the yaw. */
	AeronViewPose_FromAngles(radians(90.0f), radians(90.0f), 0.0f, pose.orientation);
	forward_of(&pose, forward);
	check(close3(forward, 0.0f, 1.0f, 0.0f), "angles: the pitch comes after the yaw");
}

static void opentrack(double x, double y, double z, double yaw, double pitch, double roll,
					  AeronViewPose* out) {
	const double values[6] = { x, y, z, yaw, pitch, roll };
	AeronHeadTracker_FromOpenTrack(values, out);
}

static void test_opentrack(void) {
	AeronViewPose pose;
	float         forward[3], up[3];

	opentrack(0, 0, 0, 0, 0, 0, &pose);
	check(is_identity(&pose), "OpenTrack: zeros give the centre");

	opentrack(0, 0, 0, 90, 0, 0, &pose);
	forward_of(&pose, forward);
	check(close3(forward, 1.0f, 0.0f, 0.0f), "OpenTrack: a positive yaw looks right");

	opentrack(0, 0, 0, 0, 90, 0, &pose);
	forward_of(&pose, forward);
	check(close3(forward, 0.0f, 1.0f, 0.0f), "OpenTrack: a positive pitch looks up");

	opentrack(0, 0, 0, 0, 0, 90, &pose);
	up_of(&pose, up);
	check(close3(up, -1.0f, 0.0f, 0.0f), "OpenTrack: a positive roll tilts left");

	opentrack(10, 0, 0, 0, 0, 0, &pose);
	check(close3(pose.position_m, -0.1f, 0.0f, 0.0f), "OpenTrack: a positive X moves left");
	opentrack(0, 10, 0, 0, 0, 0, &pose);
	check(close3(pose.position_m, 0.0f, 0.1f, 0.0f), "OpenTrack: a positive Y moves up");
	opentrack(0, 0, 10, 0, 0, 0, &pose);
	check(close3(pose.position_m, 0.0f, 0.0f, 0.1f), "OpenTrack: a positive Z moves back");

	/* Yaw, then roll around the turned head's forward axis: the head still
	 * looks right. */
	opentrack(0, 0, 0, 90, 0, 90, &pose);
	forward_of(&pose, forward);
	check(close3(forward, 1.0f, 0.0f, 0.0f), "OpenTrack: the roll comes after the yaw");

	opentrack(1000, -1000, 1000, 400, -400, 400, &pose);
	forward_of(&pose, forward);
	up_of(&pose, up);
	/* Yaw 180, pitch -90: the head looks straight down. */
	check(close3(forward, 0.0f, -1.0f, 0.0f) && close3(pose.position_m, -0.5f, -0.5f, 0.5f),
		  "OpenTrack: values out of range are clamped");
}

static void trackir(float roll, float pitch, float yaw, float x, float y, float z, AeronViewPose* out) {
	const float values[6] = { roll, pitch, yaw, x, y, z };
	AeronHeadTracker_FromTrackIR(values, out);
}

static void test_trackir(void) {
	const float   quarter = TRACKIR_FULL * 0.5f; /* 90 degrees, or 25 cm */
	AeronViewPose pose;
	float         forward[3], up[3];

	trackir(0, 0, 0, 0, 0, 0, &pose);
	check(is_identity(&pose), "TrackIR: zeros give the centre");

	trackir(0, 0, quarter, 0, 0, 0, &pose);
	forward_of(&pose, forward);
	check(close3(forward, -1.0f, 0.0f, 0.0f), "TrackIR: a positive yaw looks left");

	trackir(0, quarter, 0, 0, 0, 0, &pose);
	forward_of(&pose, forward);
	check(close3(forward, 0.0f, -1.0f, 0.0f), "TrackIR: a positive pitch looks down");

	trackir(quarter, 0, 0, 0, 0, 0, &pose);
	up_of(&pose, up);
	check(close3(up, -1.0f, 0.0f, 0.0f), "TrackIR: a positive roll tilts left");

	trackir(0, 0, 0, quarter, 0, 0, &pose);
	check(close3(pose.position_m, -0.25f, 0.0f, 0.0f), "TrackIR: a positive X moves left");
	trackir(0, 0, 0, 0, quarter, 0, &pose);
	check(close3(pose.position_m, 0.0f, 0.25f, 0.0f), "TrackIR: a positive Y moves up");
	trackir(0, 0, 0, 0, 0, quarter, &pose);
	check(close3(pose.position_m, 0.0f, 0.0f, 0.25f), "TrackIR: a positive Z moves back");

	/* Roll first, then yaw around the tilted head's own up axis. A head
	 * that lies on its left side and turns left looks down. */
	trackir(quarter, 0, quarter, 0, 0, 0, &pose);
	forward_of(&pose, forward);
	check(close3(forward, 0.0f, -1.0f, 0.0f), "TrackIR: the yaw comes after the roll");

	trackir(0, 0, 0, 4.0f * TRACKIR_FULL, 0, 0, &pose);
	check(close3(pose.position_m, -0.5f, 0.0f, 0.0f), "TrackIR: values out of range are clamped");
}

static void test_recenter(void) {
	AeronViewPose reference, pose, relative;
	float         forward[3];

	opentrack(3, -4, 5, 30, -10, 20, &reference);
	AeronHeadTracker_Relative(&reference, &reference, &relative);
	check(is_identity(&relative), "recentre: the pose that became the centre is the centre");

	/* The centre looks 30 degrees to the right. A head that then looks 50
	 * degrees to the right, 10 cm in front of the centre's place, looks 20
	 * degrees to the right of the centre, from a point ahead of it and to
	 * its left. */
	memset(&reference, 0, sizeof reference);
	memset(&pose, 0, sizeof pose);
	AeronViewPose_FromAngles(radians(30.0f), 0.0f, 0.0f, reference.orientation);
	reference.position_m[0] = 0.1f;
	AeronViewPose_FromAngles(radians(50.0f), 0.0f, 0.0f, pose.orientation);
	pose.position_m[0] = 0.1f;
	pose.position_m[2] = -0.1f;
	AeronHeadTracker_Relative(&reference, &pose, &relative);
	forward_of(&relative, forward);
	check(close3(forward, sinf(radians(20.0f)), 0.0f, -cosf(radians(20.0f))),
		  "recentre: the orientation is seen from the centre");
	check(close3(relative.position_m, -0.1f * sinf(radians(30.0f)), 0.0f, -0.1f * cosf(radians(30.0f))),
		  "recentre: the position is seen from the centre");
}

static void test_decode(void) {
	const double pose[6] = { 1.5, -2.5, 3.5, 10.0, -20.0, 30.0 };
	double       values[6];
	char         bytes[64];

	memset(bytes, 0, sizeof bytes);
	memcpy(bytes, pose, sizeof pose);
	check(AeronHeadUdp_Decode(bytes, 48, values) && memcmp(values, pose, sizeof pose) == 0,
		  "datagram: 48 bytes are read as six numbers");
	check(!AeronHeadUdp_Decode(bytes, 47, values) && !AeronHeadUdp_Decode(bytes, 49, values) &&
			  !AeronHeadUdp_Decode(bytes, 0, values),
		  "datagram: another size is refused");

	double bad[6];
	memcpy(bad, pose, sizeof bad);
	bad[4] = NAN;
	check(!AeronHeadUdp_Decode(bad, sizeof bad, values), "datagram: a NaN is refused");
	bad[4] = INFINITY;
	check(!AeronHeadUdp_Decode(bad, sizeof bad, values), "datagram: an infinite value is refused");
}

static void test_sleep_ms(int ms) {
#ifdef _WIN32
	Sleep((DWORD)ms);
#else
	const struct timespec time = { 0, ms * 1000000L };
	nanosleep(&time, NULL);
#endif
}

/* Sends `size` bytes to `port` of this computer. */
static int test_send(int port, const void* data, size_t size) {
	const TestSocket   sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in address;
	memset(&address, 0, sizeof address);
	address.sin_family = AF_INET;
	address.sin_port   = htons((unsigned short)port);
	/* 127.0.0.1. POSIX has no name for it. */
	address.sin_addr.s_addr = htonl(0x7f000001u);
#ifdef _WIN32
	const int sent =
		sendto(sender, (const char*)data, (int)size, 0, (const struct sockaddr*)&address, sizeof address);
	closesocket(sender);
#else
	const long sent = (long)sendto(sender, data, size, 0, (const struct sockaddr*)&address, sizeof address);
	close(sender);
#endif
	return sent == (long)size;
}

/* Reads until a pose arrives, for one second at most. */
static int test_wait_for_pose(AeronHeadUdp* udp, double values[6]) {
	for (int i = 0; i < 200; ++i) {
		if (AeronHeadUdp_Read(udp, values))
			return 1;
		test_sleep_ms(5);
	}
	return 0;
}

static void test_socket(void) {
	AeronHeadUdp udp, second;
	char         error[96] = "";
	int          port      = 0;
	/* Another program may use a port, so try a few. */
	for (int candidate = 47110; candidate < 47130 && !port; ++candidate) {
		if (AeronHeadUdp_Open(&udp, candidate, error, sizeof error))
			port = candidate;
	}
	check(port != 0, "socket: a port opens");
	if (!port) {
		printf("   %s\n", error);
		return;
	}

	error[0] = '\0';
	check(!AeronHeadUdp_Open(&second, port, error, sizeof error) && strstr(error, "used by another program"),
		  "socket: a port that is in use gives that reason");
	check(!AeronHeadUdp_Open(&second, 70000, error, sizeof error), "socket: a port past 65535 is refused");

	double values[6] = { 0 };
	check(!AeronHeadUdp_Read(&udp, values), "socket: nothing is read before anything is sent");

	const double first[6]    = { 1, 2, 3, 4, 5, 6 };
	const double last[6]     = { -1, -2, -3, -4, -5, -6 };
	const char   short_one[] = "not a pose";
	char         long_one[200];
	memset(long_one, 0, sizeof long_one);
	check(test_send(port, first, sizeof first) && test_wait_for_pose(&udp, values) &&
			  memcmp(values, first, sizeof first) == 0,
		  "socket: a pose that is sent is read");

	/* Several datagrams between two reads: the last pose wins, and the
	 * ones that aren't poses are skipped. */
	int sent = test_send(port, first, sizeof first) && test_send(port, short_one, sizeof short_one) &&
			   test_send(port, last, sizeof last) && test_send(port, long_one, sizeof long_one);
	test_sleep_ms(100);
	check(sent && AeronHeadUdp_Read(&udp, values) && memcmp(values, last, sizeof last) == 0,
		  "socket: the last pose is kept, and other datagrams are skipped");
	check(!AeronHeadUdp_Read(&udp, values), "socket: a pose is read once");

	AeronHeadUdp_Close(&udp);
	check(!AeronHeadUdp_Read(&udp, values), "socket: a closed socket reads nothing");
	check(AeronHeadUdp_Open(&udp, port, error, sizeof error), "socket: the port opens again after a close");
	AeronHeadUdp_Close(&udp);
}

int main(void) {
	test_from_angles();
	test_opentrack();
	test_trackir();
	test_recenter();
	test_decode();
	test_socket();
	printf("%s\n", g_failures ? "FAILED" : "all checks passed");
	return g_failures ? 1 : 0;
}
