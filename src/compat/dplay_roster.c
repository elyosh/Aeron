#include "dplay_internal.h"

static void DpEncodeRoster(DpWireSnapshot* data) {
	memset(data, 0, sizeof(*data));
	DpWrite32(data->maximum, g_dp.maximum);
	DpWrite32(data->host, g_dp.host_id);
	for (int i = 0; i < DP_GROUPS; ++i)
		DpWrite32(data->groups[i], g_dp.groups[i]);
	for (int i = 0; i < DP_PEERS; ++i) {
		const DpPeer* peer = &g_dp.peers[i];
		DpWirePeer*   row  = &data->peers[i];
		DpWrite32(row->player, peer->id);
		row->active = (uint8_t)peer->active;
		row->groups = (uint8_t)peer->groups;
		memcpy(row->short_name, peer->short_name, sizeof(row->short_name));
		memcpy(row->long_name, peer->long_name, sizeof(row->long_name));
	}
}

int DpPublish(uint32_t event, DPID id) {
	DpTransaction* tx;
	if (g_dp.transaction_count == DP_CONTROL_QUEUE)
		return 0;
	tx = &g_dp.transactions[(g_dp.transaction_read + g_dp.transaction_count++) % DP_CONTROL_QUEUE];
	memset(tx, 0, sizeof(*tx));
	tx->version  = ++g_dp.version;
	tx->event    = event;
	tx->event_id = id;
	if (event == DPSYS_SETPLAYERORGROUPNAME) {
		int peer = DpFindPeer(id);
		if (peer >= 0) {
			memcpy(tx->short_name, g_dp.peers[peer].short_name, sizeof(tx->short_name));
			memcpy(tx->long_name, g_dp.peers[peer].long_name, sizeof(tx->long_name));
		}
	}
	for (int i = 0; i < DP_PEERS; ++i)
		if (g_dp.peers[i].id && g_dp.peers[i].id != g_dp.local_id)
			tx->pending |= (uint32_t)1 << i;
	DpEncodeRoster(&tx->snapshot);
	return 1;
}

static int DpInstallRoster(const DpWireSnapshot* data) {
	DpPeer   peers[DP_PEERS] = { 0 };
	DPID     groups[DP_GROUPS];
	unsigned group_mask = 0, occupied = 0;
	int      has_host = 0, has_local = 0;
	unsigned events = 0;
	uint64_t now    = DpNow();
	if (DpRead32(data->maximum) != g_dp.maximum || DpRead32(data->host) != g_dp.host_id)
		return 0;
	for (int i = 0; i < DP_GROUPS; ++i) {
		groups[i] = DpRead32(data->groups[i]);
		if (!groups[i])
			continue;
		for (int j = 0; j < i; ++j)
			if (groups[j] == groups[i])
				return 0;
		group_mask |= 1u << i;
	}
	for (int i = 0; i < DP_PEERS; ++i) {
		const DpWirePeer* row  = &data->peers[i];
		DpPeer*           peer = &peers[i];
		int               old;
		peer->id     = DpRead32(row->player);
		peer->active = row->active;
		peer->groups = row->groups;
		if (DpRead32(row->reserved) || peer->active > 1 || (peer->groups & ~group_mask) ||
			!memchr(row->short_name, 0, sizeof(row->short_name)) ||
			!memchr(row->long_name, 0, sizeof(row->long_name)))
			return 0;
		if (peer->id) {
			if (++occupied > g_dp.maximum)
				return 0;
			has_host |= peer->id == g_dp.host_id;
			has_local |= peer->id == g_dp.local_id;
			for (int j = 0; j < i; ++j)
				if (peers[j].id == peer->id)
					return 0;
			for (int j = 0; j < DP_GROUPS; ++j)
				if (groups[j] == peer->id)
					return 0;
		} else if (peer->active || peer->groups)
			return 0;
		memcpy(peer->short_name, row->short_name, sizeof(peer->short_name));
		memcpy(peer->long_name, row->long_name, sizeof(peer->long_name));
		old             = DpFindPeer(peer->id);
		peer->last_seen = old >= 0 ? g_dp.peers[old].last_seen : now;
		if (peer->id == g_dp.host_id)
			peer->link = g_dp.host_link;
		if (peer->active &&
			(old < 0 || !g_dp.peers[old].active || strcmp(peer->short_name, g_dp.peers[old].short_name) ||
			 strcmp(peer->long_name, g_dp.peers[old].long_name)))
			++events;
	}
	if (!has_host || !has_local)
		return 0;
	for (int i = 0; i < DP_PEERS; ++i) {
		int exists = 0;
		if (!g_dp.peers[i].active)
			continue;
		for (int j = 0; j < DP_PEERS; ++j)
			if (peers[j].id == g_dp.peers[i].id && peers[j].active)
				exists = 1;
		if (!exists)
			++events;
	}
	if (events + g_dp.control_count >= DP_CONTROL_QUEUE)
		return 0;
	/* Capture event identities, publish the entire roster, then enqueue events. */
	uint32_t types[DP_PEERS * 2], ids[DP_PEERS * 2];
	unsigned count = 0;
	for (int i = 0; i < DP_PEERS; ++i) {
		int old = DpFindPeer(peers[i].id);
		if (peers[i].active) {
			uint32_t type = old < 0 || !g_dp.peers[old].active ? DPSYS_CREATEPLAYERORGROUP
							: (strcmp(peers[i].short_name, g_dp.peers[old].short_name) ||
							   strcmp(peers[i].long_name, g_dp.peers[old].long_name))
								? DPSYS_SETPLAYERORGROUPNAME
								: 0;
			if (type) {
				types[count] = type;
				ids[count++] = peers[i].id;
			}
		}
		if (g_dp.peers[i].active) {
			int exists = 0;
			for (int j = 0; j < DP_PEERS; ++j)
				if (peers[j].id == g_dp.peers[i].id && peers[j].active)
					exists = 1;
			if (!exists) {
				types[count] = DPSYS_DESTROYPLAYERORGROUP;
				ids[count++] = g_dp.peers[i].id;
			}
		}
	}
	memcpy(g_dp.peers, peers, sizeof(peers));
	memcpy(g_dp.groups, groups, sizeof(groups));
	int local = DpFindPeer(g_dp.local_id);
	if (local >= 0 && g_dp.peers[local].active)
		DpIce_Admit(g_dp.host_link);
	for (unsigned i = 0; i < count; ++i)
		DpQueueEvent(types[i], ids[i]);
	return 1;
}

static void DpReceiveSnapshot(DpLink link, uint32_t version, const uint8_t* data, unsigned size) {
	unsigned              index, offset, expected;
	DpWireAck             ack;
	const DpWireFragment* fragment = (const DpWireFragment*)data;
	if (size < DP_FRAGMENT_HEADER || link != g_dp.host_link || g_dp.host)
		return;
	index  = DpRead32(fragment->index);
	offset = index * DP_FRAGMENT;
	if (index >= DP_FRAGMENT_COUNT)
		return;
	expected = DP_SNAPSHOT_SIZE - offset;
	if (expected > DP_FRAGMENT)
		expected = DP_FRAGMENT;
	if (size != DP_FRAGMENT_HEADER + expected)
		return;
	if (version <= g_dp.version) {
		DpWrite32(ack.revision, version);
		DpSend(link, DP_ACK, version, g_dp.local_id, g_dp.host_id, &ack, sizeof(ack));
		return;
	}
	if (version != g_dp.version + 1)
		return;
	if (g_dp.assembly_version != version) {
		g_dp.assembly_mask    = 0;
		g_dp.assembly_version = version;
	}
	memcpy((uint8_t*)&g_dp.assembly + offset, fragment->data, expected);
	g_dp.assembly_mask |= 1u << index;
	if (g_dp.assembly_mask == DP_FRAGMENT_COMPLETE_MASK && DpInstallRoster(&g_dp.assembly)) {
		g_dp.version = version;
		DpWrite32(ack.revision, version);
		DpSend(link, DP_ACK, version, g_dp.local_id, g_dp.host_id, &ack, sizeof(ack));
	}
}

HRESULT DpLocalControl(unsigned kind, DPID id, DPID group, const DPNAME* name, DPID* created) {
	int      peer = DpFindPeer(id), group_slot = -1;
	uint32_t event = 0;
	if (g_dp.failed)
		return DPERR_SESSIONLOST;
	if (!g_dp.host)
		return DX_E_NOTIMPL;
	if (g_dp.transaction_count >= DP_NORMAL_CONTROL_LIMIT || g_dp.control_count >= DP_NORMAL_CONTROL_LIMIT)
		return DPERR_BUSY;
	for (int i = 0; i < DP_GROUPS; ++i)
		if (g_dp.groups[i] == group && group)
			group_slot = i;
	if (kind == DP_CREATE_GROUP) {
		for (int i = 0; i < DP_GROUPS; ++i)
			if (!g_dp.groups[i]) {
				g_dp.groups[i] = g_dp.next_id++;
				if (created)
					*created = g_dp.groups[i];
				DpPublish(0, 0);
				return 0;
			}
		return DPERR_BUSY;
	}
	if (kind == DP_DESTROY_GROUP) {
		if (group_slot < 0)
			return DX_E_INVALIDARG;
		g_dp.groups[group_slot] = 0;
		for (int i = 0; i < DP_PEERS; ++i)
			g_dp.peers[i].groups &= ~(1u << group_slot);
	} else {
		if (peer < 0)
			return DPERR_INVALIDPLAYER;
		if (kind == DP_CREATE_PLAYER || kind == DP_RENAME_PLAYER) {
			const char* short_name = name && name->lpszShortNameA ? name->lpszShortNameA : "";
			const char* long_name  = name && name->lpszLongNameA ? name->lpszLongNameA : "";
			if (strlen(short_name) >= DP_SHORT_NAME_CAPACITY || strlen(long_name) >= DP_LONG_NAME_CAPACITY)
				return DX_E_INVALIDARG;
			if (kind == DP_CREATE_PLAYER && g_dp.peers[peer].active)
				return DPERR_CANTCREATEPLAYER;
			strcpy(g_dp.peers[peer].short_name, short_name);
			strcpy(g_dp.peers[peer].long_name, long_name);
			g_dp.peers[peer].active = 1;
			if (g_dp.peers[peer].link)
				DpIce_Admit(g_dp.peers[peer].link);
			event = kind == DP_CREATE_PLAYER ? DPSYS_CREATEPLAYERORGROUP : DPSYS_SETPLAYERORGROUPNAME;
			if (created)
				*created = id;
		} else if (kind == DP_DESTROY_PLAYER) {
			event = g_dp.peers[peer].active ? DPSYS_DESTROYPLAYERORGROUP : 0;
			/* Retire pending deliveries before reusing this player's roster slot. */
			for (unsigned i = 0; i < g_dp.transaction_count; ++i)
				g_dp.transactions[(g_dp.transaction_read + i) % DP_CONTROL_QUEUE].pending &= ~(1u << peer);
			DpIce_Retire(g_dp.peers[peer].link);
			memset(&g_dp.peers[peer], 0, sizeof(g_dp.peers[peer]));
		} else if (kind == DP_ADD_GROUP || kind == DP_DELETE_GROUP) {
			if (group_slot < 0 || !g_dp.peers[peer].active)
				return DX_E_INVALIDARG;
			if (kind == DP_ADD_GROUP)
				g_dp.peers[peer].groups |= 1u << group_slot;
			else
				g_dp.peers[peer].groups &= ~(1u << group_slot);
		} else
			return DX_E_INVALIDARG;
	}
	DpPublish(event, id);
	return 0;
}

static void DpReply(DpPeer* peer, uint32_t request) {
	DpWireResult data;
	DpWrite32(data.result, (uint32_t)peer->result);
	DpWrite32(data.player, peer->result_id);
	DpSend(peer->link, DP_RESULT, request, g_dp.local_id, peer->id, &data, sizeof(data));
}

static void DpRemoteControl(DpLink link, uint32_t request, DPID from, const uint8_t* data, unsigned size) {
	int                  peer = DpFindPeer(from);
	HRESULT              result;
	DPID                 created = 0;
	unsigned             kind;
	DPNAME               name    = { 0 };
	const DpWireControl* control = (const DpWireControl*)data;
	if (!g_dp.host || g_dp.closing || peer < 0 || size != sizeof(*control) || link != g_dp.peers[peer].link ||
		DpRead32(control->reserved))
		return;
	if (request == g_dp.peers[peer].request) {
		DpReply(&g_dp.peers[peer], request);
		return;
	}
	if (request < g_dp.peers[peer].request)
		return;
	kind = DpRead32(control->operation);
	/* Only the introducer changes groups or another participant's identity. */
	if ((kind != DP_CREATE_PLAYER && kind != DP_RENAME_PLAYER) || DpRead32(control->player) != from ||
		!memchr(control->short_name, 0, sizeof(control->short_name)) ||
		!memchr(control->long_name, 0, sizeof(control->long_name)))
		return;
	name.dwSize         = sizeof(name);
	name.lpszShortNameA = (char*)control->short_name;
	name.lpszLongNameA  = (char*)control->long_name;
	result              = DpLocalControl(kind, from, 0, &name, &created);
	if (result == DPERR_BUSY)
		return;
	g_dp.peers[peer].request   = request;
	g_dp.peers[peer].result    = result;
	g_dp.peers[peer].result_id = created;
	DpReply(&g_dp.peers[peer], request);
}

void DpHandleControl(DpLink link, unsigned kind, uint32_t request, DPID from, DPID to, const uint8_t* data,
					 unsigned size) {
	if (kind == DP_SNAPSHOT) {
		if (from == g_dp.host_id)
			DpReceiveSnapshot(link, request, data, size);
	} else if (kind == DP_CONTROL)
		DpRemoteControl(link, request, from, data, size);
	else if (kind == DP_ACK && g_dp.host && size == sizeof(DpWireAck) &&
			 DpRead32(((const DpWireAck*)data)->revision) == request) {
		int peer = DpFindPeer(from);
		if (peer < 0)
			return;
		for (unsigned i = 0; i < g_dp.transaction_count; ++i) {
			DpTransaction* tx = &g_dp.transactions[(g_dp.transaction_read + i) % DP_CONTROL_QUEUE];
			if (tx->version == request) {
				tx->pending &= ~(1u << peer);
				break;
			}
		}
	} else if (kind == DP_RESULT && !g_dp.host && from == g_dp.host_id && size == sizeof(DpWireResult) &&
			   request == g_dp.operation.request && g_dp.operation.kind) {
		const DpWireResult* reply = (const DpWireResult*)data;
		g_dp.operation.result     = (HRESULT)DpRead32(reply->result);
		g_dp.operation.id         = DpRead32(reply->player);
	} else if (kind == DP_LEAVE && !size) {
		if (g_dp.host) {
			DpWireResult reply = { 0 };
			if (DpLocalControl(DP_DESTROY_PLAYER, from, 0, NULL, NULL) == DPERR_BUSY)
				return;
			DpSend(link, DP_RESULT, request, g_dp.local_id, from, &reply, sizeof(reply));
		} else if (from == g_dp.host_id)
			DpLoseSession();
	} else if (kind == DP_OPEN || kind == DP_ACCEPT) {
		DpHandleOpen(link, kind, request, from, to, data, size);
	}
}

void DpServiceControl(void) {
	uint64_t now     = DpNow();
	uint32_t visited = 0;
	unsigned budget  = DP_FRAGMENT_SEND_BUDGET;
	for (unsigned i = 0; i < g_dp.transaction_count; ++i) {
		DpTransaction* tx = &g_dp.transactions[(g_dp.transaction_read + i) % DP_CONTROL_QUEUE];
		for (unsigned peer = 0; peer < DP_PEERS && budget; ++peer) {
			uint32_t bit = 1u << peer;
			if (!(tx->pending & bit) || (visited & bit))
				continue;
			visited |= bit;
			/* Queueing behind an older revision does not consume this peer's retry window. */
			if (!tx->deadline[peer])
				tx->deadline[peer] = now + DP_ROSTER_TIMEOUT_MS;
			if (now >= tx->deadline[peer]) {
				Aeron_LogError("compat.dplay", "roster acknowledgement timed out for player %u",
							   g_dp.peers[peer].id);
				if (DpLocalControl(DP_DESTROY_PLAYER, g_dp.peers[peer].id, 0, NULL, NULL) == DPERR_BUSY)
					DpLoseSession();
				continue;
			}
			if (now < tx->next_send[peer])
				continue;
			while (budget && tx->cursor[peer] * DP_FRAGMENT < DP_SNAPSHOT_SIZE) {
				DpWireFragment fragment;
				unsigned       offset = tx->cursor[peer] * DP_FRAGMENT, size = DP_SNAPSHOT_SIZE - offset;
				if (size > DP_FRAGMENT)
					size = DP_FRAGMENT;
				DpWrite32(fragment.index, tx->cursor[peer]);
				memcpy(fragment.data, (const uint8_t*)&tx->snapshot + offset, size);
				--budget;
				if (DpSend(g_dp.peers[peer].link, DP_SNAPSHOT, tx->version, g_dp.local_id,
						   g_dp.peers[peer].id, &fragment, size + DP_FRAGMENT_HEADER) <= 0)
					break;
				++tx->cursor[peer];
			}
			if (tx->cursor[peer] * DP_FRAGMENT >= DP_SNAPSHOT_SIZE) {
				tx->cursor[peer]    = 0;
				tx->next_send[peer] = now + DP_CONTROL_RETRY_MS;
			}
		}
	}
	while (g_dp.transaction_count) {
		DpTransaction* tx = &g_dp.transactions[g_dp.transaction_read];
		if (tx->pending)
			break;
		if (!(tx->event == DPSYS_SETPLAYERORGROUPNAME
				  ? DpQueueNameEvent(tx->event_id, tx->short_name, tx->long_name)
				  : DpQueueEvent(tx->event, tx->event_id)))
			break;
		--g_dp.transaction_count;
		g_dp.transaction_read = (g_dp.transaction_read + 1) % DP_CONTROL_QUEUE;
	}
	DpOperation* op = &g_dp.operation;
	if (DpOperationWaiting()) {
		if (now - op->start >= DP_OPERATION_TIMEOUT_MS) {
			DpLoseSession();
		} else if (op->result == DPERR_PENDING && now >= op->next_send) {
			DpSend(g_dp.host_link, op->kind == DP_OPEN ? DP_OPEN : DP_CONTROL, op->request, g_dp.local_id,
				   g_dp.host_id, op->bytes, op->size);
			op->next_send = now + DP_CONTROL_RETRY_MS;
		}
	}
	if (g_dp.closing && now >= g_dp.keepalive) {
		if (g_dp.cancel_open_request)
			DpSend(g_dp.host_link, DP_LEAVE, g_dp.cancel_open_request, 0, g_dp.host_id, NULL, 0);
		for (int i = 0; i < DP_PEERS; ++i) {
			DpPeer* peer = &g_dp.peers[i];
			if (peer->id && peer->id != g_dp.local_id && (g_dp.host || peer->id == g_dp.host_id))
				DpSend(peer->link, DP_LEAVE, g_dp.operation.request, g_dp.local_id, peer->id, NULL, 0);
		}
		g_dp.keepalive = now + DP_CLOSE_RETRY_MS;
	}
}
