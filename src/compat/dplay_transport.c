#include "dplay_directory_internal.h"
#include "dplay_internal.h"

DpCore g_dp;

static void (*g_wake_callback)(void*);
static void* g_wake_user;

void AeronDplay_SetWakeCallback(void (*callback)(void*), void* user) {
	g_wake_callback = callback;
	g_wake_user     = user;
}

void DpWake(void) {
	if (g_wake_callback)
		g_wake_callback(g_wake_user);
}

uint32_t DpRead32(const uint8_t* data) {
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
		   ((uint32_t)data[3] << 24);
}

void DpWrite32(uint8_t* data, uint32_t value) {
	for (unsigned i = 0; i < sizeof(DpWireU32); ++i)
		data[i] = (uint8_t)(value >> (i * 8));
}

uint16_t DpRead16(const uint8_t* data) { return (uint16_t)(data[0] | ((uint16_t)data[1] << 8)); }

void DpWrite16(uint8_t* data, uint16_t value) {
	data[0] = (uint8_t)value;
	data[1] = (uint8_t)(value >> 8);
}

void DpReadGuid(const uint8_t* data, GUID* value) {
	value->Data1 = DpRead32(data);
	value->Data2 = DpRead16(data + 4);
	value->Data3 = DpRead16(data + 6);
	memcpy(value->Data4, data + 8, 8);
}

void DpWriteGuid(uint8_t* data, const GUID* value) {
	DpWrite32(data, value->Data1);
	DpWrite16(data + 4, value->Data2);
	DpWrite16(data + 6, value->Data3);
	memcpy(data + 8, value->Data4, 8);
}

uint64_t DpNow(void) { return Aeron_NowUs() / 1000; }

int DpFindPeer(DPID id) {
	for (int i = 0; i < DP_PEERS; ++i)
		if (id && g_dp.peers[i].id == id)
			return i;
	return -1;
}

int DpSend(DpLink link, unsigned kind, uint32_t request, DPID from, DPID to, const void* data,
		   unsigned size) {
	uint8_t       packet[DP_MAX_DATAGRAM] = { 0 };
	DpWireHeader* header                  = (DpWireHeader*)packet;
	if (size > sizeof(packet) - DP_HEADER || !DpIce_Matches(link, &g_dp.app, &g_dp.session))
		return -1;
	memcpy(header->magic, "ADP2", sizeof(header->magic));
	DpWrite32(header->version, DP_PROTOCOL);
	DpWriteGuid(header->application, &g_dp.app);
	DpWriteGuid(header->session, &g_dp.session);
	DpWrite32(header->sender, from);
	DpWrite32(header->recipient, to);
	header->kind = (uint8_t)kind;
	DpWrite16(header->payload_size, (uint16_t)size);
	DpWrite32(header->request, request);
	if (size)
		memcpy(packet + DP_HEADER, data, size);
	return DpIce_Send(link, packet, DP_HEADER + size);
}

int DpQueueEvent(uint32_t kind, DPID id) {
	DpPacket* packet;
	if (!kind)
		return 1;
	if (kind == DPSYS_SETPLAYERORGROUPNAME) {
		int peer = DpFindPeer(id);
		return peer >= 0 && DpQueueNameEvent(id, g_dp.peers[peer].short_name, g_dp.peers[peer].long_name);
	}
	if (g_dp.control_count >= DP_NORMAL_CONTROL_LIMIT &&
		!(kind == DPSYS_DESTROYPLAYERORGROUP && id == g_dp.host_id && g_dp.failed))
		return 0;
	packet = &g_dp.control[(g_dp.control_read + g_dp.control_count++) % DP_CONTROL_QUEUE];
	memset(packet, 0, sizeof(*packet));
	packet->order = ++g_dp.delivery_order;
	packet->to    = g_dp.local_id;
	/* Both recovered dispatchers consume this common DX5 system-message prefix. */
	const uint32_t message[] = { kind, DPPLAYERTYPE_PLAYER, id };
	packet->size             = sizeof(message);
	memcpy(packet->bytes, message, sizeof(message));
	return 1;
}

int DpQueueNameEvent(DPID id, const char* short_name, const char* long_name) {
	DPMSG_SETPLAYERORGROUPNAME message = { 0 };
	DpPacket*                  packet;
	size_t                     short_size = strlen(short_name) + 1, long_size = strlen(long_name) + 1;
	if (g_dp.control_count >= DP_NORMAL_CONTROL_LIMIT ||
		sizeof(message) + short_size + long_size > DP_PAYLOAD)
		return 0;
	packet = &g_dp.control[(g_dp.control_read + g_dp.control_count++) % DP_CONTROL_QUEUE];
	memset(packet, 0, sizeof(*packet));
	packet->order          = ++g_dp.delivery_order;
	packet->to             = g_dp.local_id;
	message.dwType         = DPSYS_SETPLAYERORGROUPNAME;
	message.dwPlayerType   = DPPLAYERTYPE_PLAYER;
	message.dpId           = id;
	message.dpnName.dwSize = sizeof(DPNAME);
	memcpy(packet->bytes, &message, sizeof(message));
	memcpy(packet->bytes + sizeof(message), short_name, short_size);
	memcpy(packet->bytes + sizeof(message) + short_size, long_name, long_size);
	packet->size = (uint32_t)(sizeof(message) + short_size + long_size);
	return 1;
}

void DpLoseSession(void) {
	if (g_dp.failed)
		return;
	g_dp.failed           = 1;
	g_dp.operation.result = DPERR_SESSIONLOST;
	/* Preserve queued departure ordering; reserve the final slot for host loss. */
	DpQueueEvent(DPSYS_DESTROYPLAYERORGROUP, g_dp.host_id);
	Aeron_LogError("compat.dplay", "session lost");
}

int DpLinkLost(DpLink link) {
	if (!g_dp.host && link == g_dp.host_link) {
		if (!g_dp.closing && (g_dp.open || g_dp.operation.kind))
			DpLoseSession();
		return 1;
	}
	if (g_dp.host) {
		for (int i = 0; i < DP_PEERS; ++i) {
			if (!g_dp.peers[i].id || g_dp.peers[i].link != link)
				continue;
			for (unsigned j = 0; j < g_dp.transaction_count; ++j)
				g_dp.transactions[(g_dp.transaction_read + j) % DP_CONTROL_QUEUE].pending &= ~(1u << i);
			return DpLocalControl(DP_DESTROY_PLAYER, g_dp.peers[i].id, 0, NULL, NULL) != DPERR_BUSY;
		}
	}
	return 1;
}

void DpResetSession(void) {
	g_dp.open = g_dp.host = g_dp.closing = g_dp.failed = 0;
	g_dp.maximum = g_dp.local_id = g_dp.host_id = g_dp.version = 0;
	g_dp.next_id                                               = 1;
	g_dp.game_read = g_dp.game_count = g_dp.control_read = g_dp.control_count = 0;
	g_dp.transaction_read = g_dp.transaction_count = 0;
	g_dp.assembly_mask = g_dp.assembly_version = 0;
	g_dp.cancel_open_request                   = 0;
	g_dp.host_link                             = 0;
	memset(g_dp.cancelled, 0, sizeof(g_dp.cancelled));
	memset(g_dp.peers, 0, sizeof(g_dp.peers));
	memset(g_dp.groups, 0, sizeof(g_dp.groups));
	memset(&g_dp.operation, 0, sizeof(g_dp.operation));
	memset(&g_dp.session, 0, sizeof(g_dp.session));
}

static int DpPayloadValid(unsigned kind, const uint8_t* data, unsigned size) {
	switch (kind) {
		case DP_OPEN:
		case DP_KEEPALIVE:
		case DP_LEAVE:
			return size == 0;
		case DP_ACCEPT:
			return size == sizeof(DpWireAccept);
		case DP_CONTROL:
			return size == sizeof(DpWireControl);
		case DP_RESULT:
			return size == sizeof(DpWireResult);
		case DP_ACK:
			return size == sizeof(DpWireAck);
		case DP_GAME:
			return size <= DP_PAYLOAD;
		case DP_SNAPSHOT: {
			unsigned              index, bytes;
			const DpWireFragment* fragment = (const DpWireFragment*)data;
			if (size < DP_FRAGMENT_HEADER)
				return 0;
			index = DpRead32(fragment->index);
			if (index >= DP_FRAGMENT_COUNT)
				return 0;
			bytes = DP_SNAPSHOT_SIZE - index * DP_FRAGMENT;
			if (bytes > DP_FRAGMENT)
				bytes = DP_FRAGMENT;
			return size == DP_FRAGMENT_HEADER + bytes;
		}
		default:
			return 0;
	}
}

static void DpIngress(DpLink link, const uint8_t* packet, unsigned received) {
	const DpWireHeader* header = (const DpWireHeader*)packet;
	GUID                app, session;
	if (received < DP_HEADER || memcmp(header->magic, "ADP2", 4) ||
		DpRead32(header->version) != DP_PROTOCOL || header->reserved)
		return;
	DpReadGuid(header->application, &app);
	DpReadGuid(header->session, &session);
	if (!DpIce_Matches(link, &app, &session) || memcmp(&app, &g_dp.app, sizeof(app)) ||
		memcmp(&session, &g_dp.session, sizeof(session)))
		return;
	unsigned size = DpRead16(header->payload_size), kind = header->kind;
	if (size != received - DP_HEADER || !DpPayloadValid(kind, packet + DP_HEADER, size))
		return;
	uint32_t request = DpRead32(header->request);
	DPID     from = DpRead32(header->sender), to = DpRead32(header->recipient);
	if (kind == DP_OPEN || kind == DP_ACCEPT) {
		if ((kind == DP_OPEN && (!g_dp.host || from || to)) ||
			(kind == DP_ACCEPT && (g_dp.host || link != g_dp.host_link || !from)))
			return;
		DpHandleOpen(link, kind, request, from, to, packet + DP_HEADER, size);
		return;
	}
	if (kind == DP_LEAVE && !from && g_dp.host) {
		if (to && to != g_dp.local_id)
			return;
		int cancellation = -1;
		for (int i = 0; i < DP_CONTROL_QUEUE; ++i) {
			if (g_dp.cancelled[i].expires <= DpNow() && cancellation < 0)
				cancellation = i;
			if (g_dp.cancelled[i].request == request && g_dp.cancelled[i].link == link) {
				cancellation = i;
				break;
			}
		}
		if (cancellation < 0)
			return;
		g_dp.cancelled[cancellation].link    = link;
		g_dp.cancelled[cancellation].request = request;
		g_dp.cancelled[cancellation].expires = DpNow() + DP_CANCEL_RETENTION_MS;
		for (int i = 0; i < DP_PEERS; ++i)
			if (g_dp.peers[i].id && g_dp.peers[i].open_request == request && g_dp.peers[i].link == link)
				DpLocalControl(DP_DESTROY_PLAYER, g_dp.peers[i].id, 0, NULL, NULL);
		return;
	}
	int peer = DpFindPeer(from);
	if (peer < 0 || (g_dp.host ? g_dp.peers[peer].link != link : link != g_dp.host_link))
		return;
	if (kind == DP_GAME) {
		int recipient = DpFindPeer(to);
		if (g_dp.failed || g_dp.closing || !g_dp.open || !g_dp.peers[peer].active || recipient < 0 ||
			!g_dp.peers[recipient].active)
			return;
		if (g_dp.host && to != g_dp.local_id) {
			g_dp.peers[peer].last_seen = DpNow();
			DpSend(g_dp.peers[recipient].link, DP_GAME, request, from, to, packet + DP_HEADER, size);
			return;
		}
		if (to != g_dp.local_id || g_dp.game_count == DP_GAME_QUEUE)
			return;
		/* On clients only the physical host link determines peer liveness. */
		int physical = g_dp.host ? peer : DpFindPeer(g_dp.host_id);
		if (physical >= 0)
			g_dp.peers[physical].last_seen = DpNow();
		DpPacket* queued = &g_dp.gameplay[(g_dp.game_read + g_dp.game_count++) % DP_GAME_QUEUE];
		queued->order    = ++g_dp.delivery_order;
		queued->from     = from;
		queued->to       = to;
		queued->size     = size;
		memcpy(queued->bytes, packet + DP_HEADER, size);
		return;
	}
	if (to != g_dp.local_id || (!g_dp.host && from != g_dp.host_id))
		return;
	if ((kind == DP_CONTROL || kind == DP_ACK) && !g_dp.host)
		return;
	if ((kind == DP_RESULT || kind == DP_SNAPSHOT) && g_dp.host)
		return;
	g_dp.peers[peer].last_seen = DpNow();
	DpHandleControl(link, kind, request, from, to, packet + DP_HEADER, size);
}

/* Shared by the periodic update and Receive; session state stays on the
 * application thread, and each call retains the ingress work budget. */
void DpPumpIncoming(void) {
	for (int i = 0; i < DP_RECEIVE_BUDGET; ++i) {
		uint8_t  packet[DP_MAX_DATAGRAM];
		unsigned size;
		DpLink   link;
		if (!DpIce_Receive(&link, packet, &size))
			break;
		DpIngress(link, packet, size);
	}
}

void AeronDplay_Update(void) {
	DpDirectory_Update();
	DpIce_Update();
	uint64_t now = DpNow();
	DpPumpIncoming();
	DpServiceControl();
	if (g_dp.open && !g_dp.failed && !g_dp.closing && now >= g_dp.keepalive) {
		for (int i = 0; i < DP_PEERS; ++i) {
			DpPeer* peer = &g_dp.peers[i];
			if (!peer->id || peer->id == g_dp.local_id || (!g_dp.host && peer->id != g_dp.host_id))
				continue;
			DpSend(peer->link, DP_KEEPALIVE, 0, g_dp.local_id, peer->id, NULL, 0);
			if (now - peer->last_seen >= DP_PEER_TIMEOUT_MS) {
				if (!g_dp.host)
					DpLoseSession();
				else
					DpLocalControl(DP_DESTROY_PLAYER, peer->id, 0, NULL, NULL);
			}
		}
		g_dp.keepalive = now + DP_KEEPALIVE_INTERVAL_MS;
	}
	if (g_dp.closing && now >= g_dp.close_until) {
		AeronDplayJoinStatus join;
		AeronDplayDirectory_GetJoinStatus(&join);
		if (join.preparation.state == AERON_DPLAY_DIRECTORY_PENDING ||
			join.preparation.state == AERON_DPLAY_DIRECTORY_SUCCEEDED)
			AeronDplayDirectory_CancelJoin();
		DpIce_Reset();
		DpResetSession();
	}
}

uint64_t AeronDplay_NextWakeDelayUs(void) {
	uint64_t directory = DpDirectory_NextWakeDelayUs();
	uint64_t transport =
		g_dp.open || g_dp.closing || g_dp.operation.kind || DpIce_Active() ? DP_WAKE_DELAY_US : UINT64_MAX;
	return directory < transport ? directory : transport;
}

int AeronDplay_IsActive(void) {
	return g_dp.open || g_dp.closing || g_dp.operation.kind != 0 || DpIce_Active();
}

void AeronDplay_Shutdown(void) {
	DpDirectory_Shutdown();
	DpIce_Reset();
	DpResetSession();
	AeronDplay_SetWakeCallback(NULL, NULL);
}
