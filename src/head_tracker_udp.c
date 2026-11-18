/* Receives OpenTrack's "UDP over network" output. A datagram is 48 bytes:
 * six doubles in the sender's byte order, with no header. They are X, Y and
 * Z in centimetres, then yaw, pitch and roll in degrees. This file doesn't
 * use SDL, so tests/head_tracker_test.c can build it on its own. */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif

#include "head_tracker_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
typedef SOCKET HeadSocket;
#define HEAD_SOCKET_INVALID INVALID_SOCKET
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int HeadSocket;
#define HEAD_SOCKET_INVALID (-1)
#endif

/* The most datagrams read in one call, so that a flood can't hold up a frame.
 * OpenTrack sends a few hundred per second at most. */
#define HEAD_UDP_MAX_READS 64

static void head_socket_close(HeadSocket handle) {
#ifdef _WIN32
	closesocket(handle);
#else
	close(handle);
#endif
}

static int head_socket_set_nonblocking(HeadSocket handle) {
#ifdef _WIN32
	u_long enabled = 1;
	return ioctlsocket(handle, FIONBIO, &enabled) == 0;
#else
	const int flags = fcntl(handle, F_GETFL, 0);
	return flags >= 0 && fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static int head_socket_address_in_use(void) {
#ifdef _WIN32
	return WSAGetLastError() == WSAEADDRINUSE;
#else
	return errno == EADDRINUSE;
#endif
}

int AeronHeadUdp_Open(AeronHeadUdp* udp, int port, char* error, size_t error_size) {
	memset(udp, 0, sizeof *udp);
	if (port < 1 || port > 65535) {
		snprintf(error, error_size, "Port %d is not a valid port.", port);
		return 0;
	}
#ifdef _WIN32
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
		snprintf(error, error_size, "The network could not be started.");
		return 0;
	}
#endif
	const HeadSocket handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	int              ok     = handle != HEAD_SOCKET_INVALID && head_socket_set_nonblocking(handle);
	if (!ok)
		snprintf(error, error_size, "Port %d could not be opened.", port);
	if (ok) {
		/* Every address, so that a phone on the network can send too. */
		struct sockaddr_in address;
		memset(&address, 0, sizeof address);
		address.sin_family      = AF_INET;
		address.sin_port        = htons((uint16_t)port);
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		ok                      = bind(handle, (const struct sockaddr*)&address, sizeof address) == 0;
		if (!ok && head_socket_address_in_use())
			snprintf(error, error_size, "Port %d is used by another program.", port);
		else if (!ok)
			snprintf(error, error_size, "Port %d could not be opened.", port);
	}
	if (!ok) {
		if (handle != HEAD_SOCKET_INVALID)
			head_socket_close(handle);
#ifdef _WIN32
		WSACleanup();
#endif
		return 0;
	}
	udp->socket = (uintptr_t)handle;
	udp->open   = 1;
	return 1;
}

void AeronHeadUdp_Close(AeronHeadUdp* udp) {
	if (!udp->open)
		return;
	head_socket_close((HeadSocket)udp->socket);
#ifdef _WIN32
	WSACleanup();
#endif
	memset(udp, 0, sizeof *udp);
}

int AeronHeadUdp_Decode(const void* data, size_t size, double values[6]) {
	double decoded[6];
	if (size != AERON_HEAD_UDP_DATAGRAM_SIZE)
		return 0;
	/* Every platform that Aeron runs on is little-endian, as are the
	 * computers and phones that send. */
	memcpy(decoded, data, sizeof decoded);
	for (int i = 0; i < 6; ++i) {
		if (!isfinite(decoded[i]))
			return 0;
	}
	memcpy(values, decoded, sizeof decoded);
	return 1;
}

int AeronHeadUdp_Read(AeronHeadUdp* udp, double values[6]) {
	int received = 0;
	if (!udp->open)
		return 0;
	for (int i = 0; i < HEAD_UDP_MAX_READS; ++i) {
		/* One byte more than a pose, so that a longer datagram isn't read
		 * as one. */
		char buffer[AERON_HEAD_UDP_DATAGRAM_SIZE + 1];
#ifdef _WIN32
		const int size = recv((HeadSocket)udp->socket, buffer, (int)sizeof buffer, 0);
		if (size == SOCKET_ERROR) {
			/* Windows gives an error for a datagram that is longer than
			 * the buffer, and drops it. The next one may be good. */
			if (WSAGetLastError() == WSAEMSGSIZE)
				continue;
			break;
		}
#else
		const long size = (long)recv((HeadSocket)udp->socket, buffer, sizeof buffer, 0);
		if (size < 0)
			break;
#endif
		if (AeronHeadUdp_Decode(buffer, (size_t)size, values))
			received = 1;
	}
	return received;
}
