#include "aeron/random.h"
#include "dplay_directory_internal.h"
#include "dplay_internal.h"

int DpOperationWaiting(void) {
	const DpOperation*   op      = &g_dp.operation;
	const DpWireControl* control = (const DpWireControl*)op->bytes;
	int                  peer;
	if (!op->kind)
		return 0;
	if (op->result == DPERR_PENDING)
		return 1;
	if (op->result || op->kind != DP_CONTROL)
		return 0;
	if (DpRead32(control->operation) == DP_CREATE_PLAYER) {
		peer = DpFindPeer(op->id);
		return peer < 0 || !g_dp.peers[peer].active;
	}
	if (DpRead32(control->operation) == DP_RENAME_PLAYER) {
		peer = DpFindPeer(DpRead32(control->player));
		return peer < 0 || strcmp(g_dp.peers[peer].short_name, control->short_name) ||
			   strcmp(g_dp.peers[peer].long_name, control->long_name);
	}
	return 0;
}

HRESULT DpBeginOperation(unsigned kind, const void* data, unsigned size, DPID* id) {
	DpOperation* op = &g_dp.operation;
	HRESULT      result;
	if (g_dp.failed)
		return DPERR_SESSIONLOST;
	if (size > sizeof(op->bytes))
		return DX_E_INVALIDARG;
	if (!op->kind) {
		memset(op, 0, sizeof(*op));
		op->kind    = kind;
		op->request = ++g_dp.next_request;
		op->size    = size;
		if (size)
			memcpy(op->bytes, data, size);
		op->start  = DpNow();
		op->result = DPERR_PENDING;
		return DPERR_PENDING;
	}
	if (op->kind != kind || op->size != size || (size && memcmp(op->bytes, data, size)))
		return DPERR_BUSY;
	if (DpOperationWaiting())
		return DPERR_PENDING;
	result = op->result;
	if (id)
		*id = op->id;
	memset(op, 0, sizeof(*op));
	return result;
}

static void DpAccept(DpLink link, uint32_t request, DPID id, HRESULT result) {
	DpWireAccept data;
	int          peer = DpFindPeer(id);
	DpWrite32(data.result, (uint32_t)result);
	DpWrite32(data.player, id);
	DpWrite32(data.revision, peer >= 0 ? g_dp.peers[peer].open_version : g_dp.version);
	DpWrite32(data.maximum, g_dp.maximum);
	DpSend(link, DP_ACCEPT, request, g_dp.host_id, id, &data, sizeof(data));
}

void DpHandleOpen(DpLink link, unsigned kind, uint32_t request, DPID from, DPID to, const uint8_t* data,
				  unsigned size) {
	if (kind == DP_OPEN && !size && g_dp.host && g_dp.open && !g_dp.closing && !g_dp.failed) {
		int slot = -1, count = 0;
		for (int i = 0; i < DP_CONTROL_QUEUE; ++i)
			if (g_dp.cancelled[i].expires > DpNow() && g_dp.cancelled[i].request == request &&
				g_dp.cancelled[i].link == link)
				return;
		for (int i = 0; i < DP_PEERS; ++i) {
			DpPeer* peer = &g_dp.peers[i];
			if (peer->id) {
				++count;
				if (peer->link == link) {
					/* A repeated Open must not reserve a second participant. */
					if (peer->open_request == request)
						DpAccept(link, request, peer->id, 0);
					return;
				}
			} else if (slot < 0)
				slot = i;
		}
		if (count >= (int)g_dp.maximum || slot < 0) {
			DpAccept(link, request, 0, DPERR_NONEWPLAYERS);
			return;
		}
		if (g_dp.transaction_count >= DP_NORMAL_CONTROL_LIMIT)
			return;
		DpPeer* peer = &g_dp.peers[slot];
		memset(peer, 0, sizeof(*peer));
		peer->id           = g_dp.next_id++;
		peer->link         = link;
		peer->last_seen    = DpNow();
		peer->request      = request;
		peer->open_request = request;
		peer->open_version = g_dp.version;
		DpIce_Claim(link);
		DpAccept(link, request, peer->id, 0);
	} else if (kind == DP_ACCEPT && size == sizeof(DpWireAccept) && !g_dp.host &&
			   g_dp.operation.kind == DP_OPEN && g_dp.operation.request == request &&
			   g_dp.operation.result == DPERR_PENDING && link == g_dp.host_link) {
		const DpWireAccept* reply  = (const DpWireAccept*)data;
		HRESULT             result = (HRESULT)DpRead32(reply->result);
		DPID                id     = DpRead32(reply->player);
		if (!result) {
			if (!id || id != to || id == from || !from || !DpRead32(reply->maximum) ||
				DpRead32(reply->maximum) > DP_PEERS)
				return;
			g_dp.local_id           = id;
			g_dp.host_id            = from;
			g_dp.maximum            = DpRead32(reply->maximum);
			g_dp.version            = DpRead32(reply->revision);
			g_dp.peers[0].id        = from;
			g_dp.peers[0].link      = link;
			g_dp.peers[0].last_seen = DpNow();
			g_dp.open               = 1;
		}
		g_dp.operation.result = result;
		g_dp.operation.id     = id;
	}
}

HRESULT DpOpen(DPSESSIONDESC2* desc, uint32_t flags) {
	uint64_t now = DpNow();
	if (!desc || desc->dwSize != sizeof(*desc) || (flags != DPOPEN_JOIN && flags != DPOPEN_CREATE))
		return DX_E_INVALIDARG;
	if (g_dp.closing)
		return DPERR_PENDING;
	if (g_dp.operation.kind == DP_OPEN) {
		if (flags != DPOPEN_JOIN || memcmp(&desc->guidApplication, &g_dp.app, sizeof(GUID)) ||
			memcmp(&desc->guidInstance, &g_dp.session, sizeof(GUID)))
			return DPERR_BUSY;
		return DpBeginOperation(DP_OPEN, NULL, 0, NULL);
	}
	if (g_dp.open)
		return DX_E_FAIL;
	if (flags == DPOPEN_CREATE) {
		enum {
			UUID_VERSION_MASK = 0x0fff,
			UUID_VERSION_4    = 0x4000,
			UUID_VARIANT_MASK = 0x3f,
			UUID_VARIANT_RFC  = 0x80
		};

		GUID instance;
		if (!desc->dwMaxPlayers || desc->dwMaxPlayers > DP_PEERS ||
			(desc->lpszSessionNameA && strlen(desc->lpszSessionNameA) >= sizeof(g_dp.name)))
			return DX_E_INVALIDARG;
		if (!Aeron_RandomBytes(&instance, sizeof(instance))) {
			Aeron_LogError("compat.dplay", "cannot obtain session-ID entropy");
			return DX_E_FAIL;
		}
		instance.Data3    = (uint16_t)((instance.Data3 & UUID_VERSION_MASK) | UUID_VERSION_4);
		instance.Data4[0] = (uint8_t)((instance.Data4[0] & UUID_VARIANT_MASK) | UUID_VARIANT_RFC);
		AeronDplayDirectoryRoom prepared;
		if (DpDirectory_GetJoinRoom(&prepared))
			return DPERR_BUSY;
		DpIce_Reset();
		DpResetSession();
		g_dp.app     = desc->guidApplication;
		g_dp.session = instance;
		g_dp.maximum = desc->dwMaxPlayers;
		snprintf(g_dp.name, sizeof(g_dp.name), "%s", desc->lpszSessionNameA ? desc->lpszSessionNameA : "");
		g_dp.local_id = g_dp.host_id = g_dp.next_id++;
		g_dp.peers[0].id             = g_dp.local_id;
		g_dp.peers[0].last_seen      = now;
		g_dp.host = g_dp.open = 1;
		desc->guidInstance    = g_dp.session;
		return 0;
	}
	AeronDplayJoinStatus    join;
	AeronDplayDirectoryRoom selected;
	AeronDplayDirectory_GetJoinStatus(&join);
	if (!DpDirectory_GetJoinRoom(&selected) ||
		memcmp(&desc->guidApplication, &join.identity.application_id, sizeof(GUID)) ||
		memcmp(&desc->guidInstance, &selected.room_id, sizeof(GUID)))
		return DX_E_INVALIDARG;
	if (join.preparation.state == AERON_DPLAY_DIRECTORY_PENDING)
		return DPERR_PENDING;
	if (join.preparation.state != AERON_DPLAY_DIRECTORY_SUCCEEDED)
		return DPERR_SESSIONLOST;
	DpLink link = DpIce_Prepared(&desc->guidApplication, &desc->guidInstance);
	if (!link)
		return DPERR_SESSIONLOST;
	DpResetSession();
	g_dp.host_link = link;
	g_dp.app       = desc->guidApplication;
	g_dp.session   = selected.room_id;
	g_dp.maximum   = selected.metadata.max_players;
	snprintf(g_dp.name, sizeof(g_dp.name), "%s", selected.metadata.name);
	DpIce_Claim(link);
	return DpBeginOperation(DP_OPEN, NULL, 0, NULL);
}

HRESULT DpClose(void) {
	if (g_dp.closing)
		return DPERR_PENDING;
	if (!g_dp.open && !g_dp.operation.kind)
		return 0;
	if (g_dp.host)
		AeronDplayDirectory_StopHosting();
	g_dp.closing             = 1;
	g_dp.close_until         = DpNow() + DP_CLOSE_DRAIN_MS;
	g_dp.cancel_open_request = g_dp.operation.kind == DP_OPEN ? g_dp.operation.request : 0;
	memset(&g_dp.operation, 0, sizeof(g_dp.operation));
	g_dp.operation.request = ++g_dp.next_request;
	g_dp.keepalive         = 0;
	return DPERR_PENDING;
}
