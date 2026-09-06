#include "dplay_ice.h"
#include "dplay_directory_internal.h"
#include "dplay_directory_json.h"
#include "dplay_internal.h"
#include <juice/juice.h>

enum { ICE_LINKS = DP_PEERS, ICE_DATAGRAMS = 64, ICE_EVENTS = 16 };

enum { ICE_GATHER_DONE = 100, ICE_RELAY_READY };

typedef struct IceDatagram {
	unsigned size;
	uint8_t  data[DP_MAX_DATAGRAM];
} IceDatagram;

typedef struct IceLink {
	DpLink                       id;
	AeronDplayConnectionIdentity identity;
	uint64_t                     generation, deadline, started, gather_until;
	juice_agent_t*               agent;
	AeronMutex*                  mutex;
	/* Callbacks touch only these queues and the overflow flag, under mutex. */
	IceDatagram datagrams[ICE_DATAGRAMS];
	unsigned    read, count, event_read, event_count;
	int         events[ICE_EVENTS], overflow;
	/* Remaining state belongs exclusively to the application thread. */
	int host, seen, remote, published, gathered, relay_ready, connected, failed, reported, retiring, owned,
		admitted;
	char local[DP_DIRECTORY_SDP];
} IceLink;

static IceLink*               g_links[ICE_LINKS];
static uint64_t               g_next_link;
static unsigned               g_receive_cursor;
static DpDirectoryHostSignals g_host_signals;

static IceLink* IceFind(DpLink id) {
	for (unsigned i = 0; id && i < ICE_LINKS; ++i)
		if (g_links[i] && g_links[i]->id == id)
			return g_links[i];
	return NULL;
}

static int IdentityEqual(const AeronDplayConnectionIdentity* a, const AeronDplayConnectionIdentity* b) {
	return !memcmp(a, b, sizeof(*a));
}

static void IceEvent(IceLink* link, int event) {
	Aeron_MutexLock(link->mutex);
	if (link->event_count == ICE_EVENTS)
		link->overflow = 1;
	else
		link->events[(link->event_read + link->event_count++) % ICE_EVENTS] = event;
	Aeron_MutexUnlock(link->mutex);
	DpWake();
}

static void IceState(juice_agent_t* agent, juice_state_t state, void* user) {
	(void)agent;
	IceEvent(user, (int)state);
}

static void IceGathered(juice_agent_t* agent, void* user) {
	(void)agent;
	IceEvent(user, ICE_GATHER_DONE);
}

static void IceCandidate(juice_agent_t* agent, const char* candidate, void* user) {
	(void)agent;
	/* Inspect only the type in libjuice's generated candidate; never retain SDP
	 * or call back into the agent while its callback holds the worker lock. */
	const char* type = strstr(candidate, " typ ");
	if (!type)
		return;
	type += sizeof(" typ ") - 1;
	size_t length = strcspn(type, " \t\r\n");
	if (length == sizeof("relay") - 1 && !memcmp(type, "relay", length))
		IceEvent(user, ICE_RELAY_READY);
}

static void IceReceive(juice_agent_t* agent, const char* data, size_t size, void* user) {
	IceLink* link = user;
	(void)agent;
	if (size < DP_HEADER || size > DP_MAX_DATAGRAM)
		return;
	Aeron_MutexLock(link->mutex);
	if (link->count == ICE_DATAGRAMS) {
		if ((unsigned char)data[offsetof(DpWireHeader, kind)] != DP_GAME)
			link->overflow = 1;
	} else {
		IceDatagram* packet = &link->datagrams[(link->read + link->count++) % ICE_DATAGRAMS];
		packet->size        = (unsigned)size;
		memcpy(packet->data, data, size);
	}
	Aeron_MutexUnlock(link->mutex);
	DpWake();
}

static void IceDestroy(IceLink* link) {
	/* juice_destroy joins callbacks; the context and mutex must outlive it. */
	juice_destroy(link->agent);
	Aeron_MutexDestroy(link->mutex);
	memset(link, 0, sizeof(*link));
	free(link);
}

static IceLink* IceCreate(const DpDirectorySignal* signal, int host) {
	unsigned slot;
	for (slot = 0; slot < ICE_LINKS && g_links[slot]; ++slot) {
	}
	if (slot == ICE_LINKS)
		return NULL;
	IceLink* link = calloc(1, sizeof(*link));
	if (!link)
		return NULL;
	link->mutex = Aeron_MutexCreate();
	if (!link->mutex) {
		free(link);
		return NULL;
	}
	link->id                   = ++g_next_link;
	link->identity             = signal->identity;
	link->generation           = signal->generation;
	link->deadline             = signal->deadline_us;
	link->started              = Aeron_NowUs();
	link->gather_until         = link->started + 15000000;
	link->host                 = host;
	juice_turn_server_t turn   = { signal->ice.turn.host, signal->ice.username, signal->ice.password,
								   signal->ice.turn.port };
	juice_config_t      config = { 0 };
	config.concurrency_mode    = JUICE_CONCURRENCY_MODE_POLL;
	config.stun_server_host    = signal->ice.stun.host;
	config.stun_server_port    = signal->ice.stun.port;
	config.turn_servers        = &turn;
	config.turn_servers_count  = 1;
	config.cb_state_changed    = IceState;
	config.cb_gathering_done   = IceGathered;
	config.cb_candidate        = IceCandidate;
	config.cb_recv             = IceReceive;
	config.user_ptr            = link;
	/* Aeron reports failures without libjuice's diagnostic SDP/credentials. */
	juice_set_log_level(JUICE_LOG_LEVEL_NONE);
	link->agent   = juice_create(&config);
	g_links[slot] = link;
	if (!link->agent) {
		link->failed = 1;
		return link;
	}
	/* Installing the offer first makes this agent the controlled ICE side. */
	if (host) {
		if (juice_set_remote_description(link->agent, signal->offer) ||
			juice_set_remote_gathering_done(link->agent)) {
			link->failed = 1;
			return link;
		}
		link->remote = 1;
	}
	if (juice_gather_candidates(link->agent))
		link->failed = 1;
	return link;
}

static void IceApplyEvents(IceLink* link) {
	int      events[ICE_EVENTS];
	unsigned count;
	Aeron_MutexLock(link->mutex);
	if (link->overflow)
		link->failed = 1;
	count = link->event_count;
	for (unsigned i = 0; i < count; ++i)
		events[i] = link->events[(link->event_read + i) % ICE_EVENTS];
	link->event_read  = (link->event_read + count) % ICE_EVENTS;
	link->event_count = 0;
	Aeron_MutexUnlock(link->mutex);
	for (unsigned i = 0; i < count; ++i) {
		if (events[i] == ICE_GATHER_DONE)
			link->gathered = 1;
		else if (events[i] == ICE_RELAY_READY)
			link->relay_ready = 1;
		else if (events[i] == JUICE_STATE_CONNECTED || events[i] == JUICE_STATE_COMPLETED)
			link->connected = 1;
		else if (events[i] == JUICE_STATE_FAILED ||
				 (events[i] == JUICE_STATE_DISCONNECTED && link->connected))
			link->failed = 1;
	}
}

static IceLink* IceSignal(const DpDirectorySignal* signal, int host) {
	for (unsigned i = 0; i < ICE_LINKS; ++i) {
		IceLink* link = g_links[i];
		if (link && link->host == host && IdentityEqual(&link->identity, &signal->identity)) {
			/* A live admitted link survives directory recreation with a new cursor. */
			if (link->generation == signal->generation || link->admitted)
				return link;
			link->retiring = 1;
		}
	}
	if (signal->publication.state == AERON_DPLAY_DIRECTORY_FAILED || signal->state == DP_SIGNAL_REJECTED ||
		Aeron_NowUs() >= signal->deadline_us || (host && !signal->offer[0]))
		return NULL;
	IceLink* link = IceCreate(signal, host);
	if (!link) {
		if (host)
			DpDirectory_SetDescription(&signal->identity, signal->generation, "",
									   AERON_DPLAY_DIRECTORY_ERROR_CONNECTION_FAILED);
		else
			DpDirectory_JoinReady(signal->generation, AERON_DPLAY_DIRECTORY_ERROR_NO_MEMORY);
	}
	return link;
}

static void IceAdvance(IceLink* link, const DpDirectorySignal* signal) {
	link->seen = 1;
	if (link->failed || link->retiring || link->admitted)
		return;
	if (signal->rejection || signal->publication.state == AERON_DPLAY_DIRECTORY_FAILED) {
		link->failed = 1;
		return;
	}
	if (!link->host && !link->remote && signal->answer[0]) {
		if (juice_set_remote_description(link->agent, signal->answer) ||
			juice_set_remote_gathering_done(link->agent)) {
			link->failed = 1;
			return;
		}
		link->remote = 1;
	}
	/* TURN supplies both mapped and relay candidates. Publish them without
	 * waiting for unrelated STUN requests to finish. */
	if (!link->published && (link->gathered || link->relay_ready || (link->host && link->connected) ||
							 Aeron_NowUs() >= link->gather_until)) {
		if (!link->local[0] && juice_get_local_description(link->agent, link->local, sizeof(link->local))) {
			link->failed = 1;
			return;
		}
		AeronDplayDirectoryError error =
			DpDirectory_SetDescription(&link->identity, link->generation, link->local, 0);
		if (error) {
			link->failed = 1;
			return;
		}
		link->published    = 1;
		const char* reason = link->gathered                    ? "gathering complete"
							 : link->relay_ready               ? "relay ready"
							 : (link->host && link->connected) ? "connected"
															   : "gathering deadline";
		char        connection[37];
		DpDirectory_GuidText(&link->identity.connection_id, connection);
		Aeron_LogInfo("compat.dplay.ice", "%s connection %s: queued %s after %.3f s (%s)",
					  link->host ? "host" : "client", connection, link->host ? "answer" : "offer",
					  (double)(Aeron_NowUs() - link->started) / 1000000.0, reason);
	}
	if (!link->host && link->connected && link->remote && link->published)
		DpDirectory_JoinReady(link->generation, AERON_DPLAY_DIRECTORY_ERROR_NONE);
}

void DpIce_Update(void) {
	DpDirectorySignal join;
	int               joining = DpDirectory_GetJoinSignal(&join);
	DpDirectory_GetHostSignals(&g_host_signals);
	for (unsigned i = 0; i < ICE_LINKS; ++i)
		if (g_links[i]) {
			g_links[i]->seen = 0;
			IceApplyEvents(g_links[i]);
		}
	if (g_dp.host && g_dp.open && !g_dp.closing && !g_dp.failed) {
		for (unsigned i = 0; i < g_host_signals.count; ++i) {
			DpDirectorySignal* signal = &g_host_signals.connections[i];
			if (memcmp(&signal->identity.application_id, &g_dp.app, sizeof(GUID)) ||
				memcmp(&signal->identity.room_id, &g_dp.session, sizeof(GUID)))
				continue;
			IceLink* link = IceSignal(signal, 1);
			if (link)
				IceAdvance(link, signal);
		}
	} else if (joining && !g_dp.closing && !g_dp.failed) {
		IceLink* link = IceSignal(&join, 0);
		if (link)
			IceAdvance(link, &join);
	}
	for (unsigned i = 0; i < ICE_LINKS; ++i) {
		IceLink* link = g_links[i];
		if (!link)
			continue;
		if (!link->admitted && Aeron_NowUs() >= link->deadline)
			link->failed = 1;
		/* SDP exchange is sufficient to outlive signaling; admission preserves
		 * the link beyond its setup deadline. Cancellation is explicit. */
		if (!link->seen && !link->owned && !(link->host && link->published && link->remote))
			link->retiring = 1;
		if (link->failed && !link->reported) {
			link->reported = 1;
			if (!link->host)
				DpDirectory_JoinReady(link->generation, AERON_DPLAY_DIRECTORY_ERROR_CONNECTION_FAILED);
			else if (!link->published)
				DpDirectory_SetDescription(&link->identity, link->generation, "",
										   AERON_DPLAY_DIRECTORY_ERROR_CONNECTION_FAILED);
			Aeron_LogWarn("compat.dplay.ice", "ICE link failed");
		}
		if (link->retiring || link->failed) {
			if (!DpLinkLost(link->id))
				continue;
			/* Retain the terminal generation until signaling forgets it, so a
			 * stale offer cannot recreate a failed or departed player's agent. */
			if (link->seen && link->host) {
				juice_destroy(link->agent);
				link->agent = NULL;
				continue;
			}
			g_links[i] = NULL;
			IceDestroy(link);
		}
	}
}

int DpIce_Receive(DpLink* id, void* data, unsigned* size) {
	for (unsigned n = 0; n < ICE_LINKS; ++n) {
		unsigned i    = g_receive_cursor++ % ICE_LINKS;
		IceLink* link = g_links[i];
		if (!link || link->failed || link->retiring)
			continue;
		Aeron_MutexLock(link->mutex);
		if (!link->count) {
			Aeron_MutexUnlock(link->mutex);
			continue;
		}
		IceDatagram* packet = &link->datagrams[link->read];
		*id                 = link->id;
		*size               = packet->size;
		memcpy(data, packet->data, packet->size);
		link->read = (link->read + 1) % ICE_DATAGRAMS;
		--link->count;
		Aeron_MutexUnlock(link->mutex);
		return 1;
	}
	return 0;
}

int DpIce_Send(DpLink id, const void* data, unsigned size) {
	IceLink* link = IceFind(id);
	if (!link || link->failed || !link->agent)
		return -1;
	if (!link->connected)
		return 0;
	int result = juice_send(link->agent, data, size);
	if (!result)
		return 1;
	if (result == JUICE_ERR_AGAIN)
		return 0;
	link->failed = 1;
	return -1;
}

int DpIce_Matches(DpLink id, const GUID* app, const GUID* room) {
	IceLink* link = IceFind(id);
	return link && !link->failed && !memcmp(&link->identity.application_id, app, sizeof(*app)) &&
		   !memcmp(&link->identity.room_id, room, sizeof(*room));
}

DpLink DpIce_Prepared(const GUID* app, const GUID* room) {
	for (unsigned i = 0; i < ICE_LINKS; ++i) {
		IceLink* link = g_links[i];
		if (link && !link->retiring && !link->host && link->connected && link->remote && link->published &&
			DpIce_Matches(link->id, app, room))
			return link->id;
	}
	return 0;
}

void DpIce_Claim(DpLink id) {
	IceLink* link = IceFind(id);
	if (link)
		link->owned = 1;
}

void DpIce_Admit(DpLink id) {
	IceLink* link = IceFind(id);
	if (link)
		link->admitted = link->owned = 1;
}

void DpIce_Retire(DpLink id) {
	IceLink* link = IceFind(id);
	if (link)
		link->retiring = 1;
}

void DpIce_CancelJoin(uint64_t generation) {
	for (unsigned i = 0; i < ICE_LINKS; ++i)
		if (g_links[i] && !g_links[i]->host && g_links[i]->generation == generation)
			g_links[i]->retiring = 1;
}

int AeronDplay_GetConnectionInfo(DPID player, AeronDplayConnectionInfo* info) {
	if (!info)
		return 0;
	memset(info, 0, sizeof(*info));
	int peer = DpFindPeer(player);
	if (peer < 0 || player == g_dp.local_id || !g_dp.peers[peer].active || g_dp.failed || g_dp.closing)
		return 0;
	IceLink* link = IceFind(g_dp.host ? g_dp.peers[peer].link : g_dp.host_link);
	if (!link || !link->agent || link->failed || link->retiring || !link->connected)
		return 0;
	if (juice_get_state(link->agent) != JUICE_STATE_COMPLETED)
		return 0;
	char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN], remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
	AeronDplayConnectionInfo selected = { 0 };
	if (juice_get_selected_candidates(link->agent, local, sizeof(local), remote, sizeof(remote)) ||
		juice_get_selected_addresses(link->agent, selected.local_address, sizeof(selected.local_address),
									 selected.remote_address, sizeof(selected.remote_address)))
		return 0;
	selected.local_relayed  = strstr(local, " typ relay") != NULL;
	selected.remote_relayed = strstr(remote, " typ relay") != NULL;
	*info                   = selected;
	return 1;
}

int DpIce_Active(void) {
	for (unsigned i = 0; i < ICE_LINKS; ++i)
		if (g_links[i])
			return 1;
	return 0;
}

void DpIce_Reset(void) {
	for (unsigned i = 0; i < ICE_LINKS; ++i) {
		if (g_links[i]) {
			IceDestroy(g_links[i]);
			g_links[i] = NULL;
		}
	}
	memset(&g_host_signals, 0, sizeof(g_host_signals));
}
