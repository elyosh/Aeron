#include "dplay_directory_internal.h"
#include "dplay_internal.h"

static IDirectPlay2AVtbl g_play_vtable;
static IDirectPlayVtbl   g_legacy_vtable;
static IDirectPlay2A     g_play   = { &g_play_vtable };
static IDirectPlay       g_legacy = { &g_legacy_vtable };
static uint32_t          g_references;
static const GUID        g_play_iid = {
	0x9d460580, 0xa822, 0x11cf, { 0x96, 0x0c, 0, 0x80, 0xc7, 0x53, 0x4e, 0x82 }
};
static const GUID g_legacy_iid = { 0x5454e9a0, 0xdb65, 0x11ce, { 0x92, 0x1c, 0, 0xaa, 0, 0x6c, 0x49, 0x72 } };
static const GUID g_unknown_iid = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static HRESULT DpQuery(const GUID* iid, void** out) {
	if (!out || !iid)
		return DX_E_INVALIDARG;
	*out = NULL;
	if (!memcmp(iid, &g_play_iid, sizeof(*iid)))
		*out = &g_play;
	else if (!memcmp(iid, &g_legacy_iid, sizeof(*iid)) || !memcmp(iid, &g_unknown_iid, sizeof(*iid)))
		*out = &g_legacy;
	else
		return DPERR_NOINTERFACE;
	++g_references;
	return 0;
}

static HRESULT AERON_DXAPI DpQuery2(IDirectPlay2A* self, const GUID* iid, void** out) {
	(void)self;
	return DpQuery(iid, out);
}

static HRESULT AERON_DXAPI DpQuery1(IDirectPlay* self, const GUID* iid, void** out) {
	(void)self;
	return DpQuery(iid, out);
}

static uint32_t AERON_DXAPI DpAddRef2(IDirectPlay2A* self) {
	(void)self;
	return ++g_references;
}

static uint32_t AERON_DXAPI DpAddRef1(IDirectPlay* self) {
	(void)self;
	return ++g_references;
}

static uint32_t DpRelease(void) {
	if (g_references && !--g_references)
		DpClose();
	return g_references;
}

static uint32_t AERON_DXAPI DpRelease2(IDirectPlay2A* self) {
	(void)self;
	return DpRelease();
}

static uint32_t AERON_DXAPI DpRelease1(IDirectPlay* self) {
	(void)self;
	return DpRelease();
}

static HRESULT AERON_DXAPI DpClose2(IDirectPlay2A* self) {
	(void)self;
	return DpClose();
}

static HRESULT AERON_DXAPI DpOpen2(IDirectPlay2A* self, DPSESSIONDESC2* desc, uint32_t flags) {
	(void)self;
	return DpOpen(desc, flags);
}

static HRESULT AERON_DXAPI DpCreatePlayer(IDirectPlay2A* self, DPID* out, DPNAME* name, void* event,
										  void* data, uint32_t size, uint32_t flags) {
	DpWireControl request    = { 0 };
	const char*   short_name = name && name->lpszShortNameA ? name->lpszShortNameA : "";
	const char*   long_name  = name && name->lpszLongNameA ? name->lpszLongNameA : "";
	(void)self;
	if (!out || !g_dp.open || event || data || size || flags ||
		strlen(short_name) >= sizeof(request.short_name) || strlen(long_name) >= sizeof(request.long_name))
		return DX_E_INVALIDARG;
	if (g_dp.host)
		return DpLocalControl(DP_CREATE_PLAYER, g_dp.local_id, 0, name, out);
	DpWrite32(request.operation, DP_CREATE_PLAYER);
	DpWrite32(request.player, g_dp.local_id);
	strcpy(request.short_name, short_name);
	strcpy(request.long_name, long_name);
	return DpBeginOperation(DP_CONTROL, &request, sizeof(request), out);
}

static HRESULT AERON_DXAPI DpCreateGroup(IDirectPlay2A* self, DPID* out, DPNAME* name, void* data,
										 uint32_t size, uint32_t flags) {
	(void)self;
	(void)name;
	if (!out || data || size || flags)
		return DX_E_INVALIDARG;
	return DpLocalControl(DP_CREATE_GROUP, 0, 0, NULL, out);
}

static HRESULT AERON_DXAPI DpDestroyGroup(IDirectPlay2A* self, DPID group) {
	(void)self;
	return DpLocalControl(DP_DESTROY_GROUP, 0, group, NULL, NULL);
}

static HRESULT AERON_DXAPI DpAddGroup(IDirectPlay2A* self, DPID group, DPID player) {
	(void)self;
	return DpLocalControl(DP_ADD_GROUP, player, group, NULL, NULL);
}

static HRESULT AERON_DXAPI DpDeleteGroup(IDirectPlay2A* self, DPID group, DPID player) {
	(void)self;
	return DpLocalControl(DP_DELETE_GROUP, player, group, NULL, NULL);
}

static HRESULT AERON_DXAPI DpDestroyPlayer(IDirectPlay2A* self, DPID player) {
	(void)self;
	if (player == g_dp.local_id)
		return DpClose();
	return DpLocalControl(DP_DESTROY_PLAYER, player, 0, NULL, NULL);
}

static HRESULT AERON_DXAPI DpSetName(IDirectPlay2A* self, DPID player, DPNAME* name, uint32_t flags) {
	DpWireControl request = { 0 };
	(void)self;
	if (!name || flags || !name->lpszShortNameA || !name->lpszLongNameA ||
		strlen(name->lpszShortNameA) >= sizeof(request.short_name) ||
		strlen(name->lpszLongNameA) >= sizeof(request.long_name))
		return DX_E_INVALIDARG;
	if (g_dp.host)
		return DpLocalControl(DP_RENAME_PLAYER, player, 0, name, NULL);
	if (player != g_dp.local_id)
		return DPERR_INVALIDPLAYER;
	DpWrite32(request.operation, DP_RENAME_PLAYER);
	DpWrite32(request.player, player);
	strcpy(request.short_name, name->lpszShortNameA);
	strcpy(request.long_name, name->lpszLongNameA);
	return DpBeginOperation(DP_CONTROL, &request, sizeof(request), NULL);
}

static HRESULT AERON_DXAPI DpEnumPlayers(IDirectPlay2A* self, GUID* instance, DPEnumPlayersCallback2 callback,
										 void* context, uint32_t flags) {
	(void)self;
	if (!callback || flags || (instance && memcmp(instance, &g_dp.session, sizeof(*instance))))
		return DX_E_INVALIDARG;
	for (int i = 0; i < DP_PEERS; ++i) {
		DpPeer* peer = &g_dp.peers[i];
		DPNAME  name = { sizeof(name), 0, peer->short_name, peer->long_name };
		if (peer->active && !callback(peer->id, DPPLAYERTYPE_PLAYER, &name, 0, context))
			break;
	}
	return 0;
}

static HRESULT AERON_DXAPI DpEnumSessions(IDirectPlay2A* self, DPSESSIONDESC2* desc, uint32_t timeout,
										  DPEnumSessionsCallback2 callback, void* context, uint32_t flags) {
	uint32_t                callback_timeout = 0;
	AeronDplayDirectoryRoom rooms[AERON_DPLAY_DIRECTORY_PAGE_SIZE];
	(void)self;
	(void)timeout;
	if (!desc || !callback || flags != DPENUMSESSIONS_AVAILABLE)
		return DX_E_INVALIDARG;
	unsigned count = DpDirectory_GetPage(&desc->guidApplication, rooms);
	for (unsigned i = 0; i < count; ++i) {
		DPSESSIONDESC2 session   = { 0 };
		session.dwSize           = sizeof(session);
		session.guidApplication  = desc->guidApplication;
		session.guidInstance     = rooms[i].room_id;
		session.dwCurrentPlayers = rooms[i].metadata.players;
		session.dwMaxPlayers     = rooms[i].metadata.max_players;
		session.lpszSessionNameA = rooms[i].metadata.name;
		if (!callback(&session, &callback_timeout, 0, context))
			return 0;
	}
	callback(NULL, &callback_timeout, DPESC_TIMEDOUT, context);
	return 0;
}

static HRESULT AERON_DXAPI DpGetCaps(IDirectPlay2A* self, DPCAPS* caps, uint32_t flags) {
	(void)self;
	if (!caps || caps->dwSize != sizeof(*caps) || flags)
		return DX_E_INVALIDARG;
	memset(caps, 0, sizeof(*caps));
	caps->dwSize            = sizeof(*caps);
	caps->dwMaxBufferSize   = DP_PAYLOAD;
	caps->dwMaxQueueSize    = DP_GAME_QUEUE;
	caps->dwMaxPlayers      = DP_PEERS;
	caps->dwMaxLocalPlayers = 1;
	caps->dwTimeout         = DP_OPERATION_TIMEOUT_MS;
	return 0;
}

static HRESULT AERON_DXAPI DpReceive(IDirectPlay2A* self, DPID* from, DPID* to, uint32_t flags, void* data,
									 uint32_t* size) {
	DpPacket* packet;
	int       control;
	(void)self;
	if (!from || !to || !size || flags != DPRECEIVE_ALL)
		return DX_E_INVALIDARG;
	/* Drain already delivered messages first, then service new ICE arrivals
	 * before reporting an empty queue, even between application updates. */
	if (!g_dp.game_count && !g_dp.control_count)
		DpPumpIncoming();
	if (!g_dp.game_count && !g_dp.control_count)
		return DPERR_NOMESSAGES;
	control = !g_dp.game_count || (g_dp.control_count && g_dp.control[g_dp.control_read].order <
															 g_dp.gameplay[g_dp.game_read].order);
	packet  = control ? &g_dp.control[g_dp.control_read] : &g_dp.gameplay[g_dp.game_read];
	if (!data || *size < packet->size) {
		*size = packet->size;
		return DPERR_BUFFERTOOSMALL;
	}
	*from = packet->from;
	*to   = packet->to;
	*size = packet->size;
	memcpy(data, packet->bytes, packet->size);
	if (control && DpRead32(packet->bytes) == DPSYS_SETPLAYERORGROUPNAME) {
		DPMSG_SETPLAYERORGROUPNAME message;
		memcpy(&message, data, sizeof(message));
		message.dpnName.lpszShortNameA = (char*)data + sizeof(message);
		message.dpnName.lpszLongNameA =
			message.dpnName.lpszShortNameA + strlen(message.dpnName.lpszShortNameA) + 1;
		memcpy(data, &message, sizeof(message));
	}
	if (control) {
		g_dp.control_read = (g_dp.control_read + 1) % DP_CONTROL_QUEUE;
		--g_dp.control_count;
	} else {
		g_dp.game_read = (g_dp.game_read + 1) % DP_GAME_QUEUE;
		--g_dp.game_count;
	}
	return 0;
}

static HRESULT AERON_DXAPI DpSendGame(IDirectPlay2A* self, DPID from, DPID to, uint32_t flags, void* data,
									  uint32_t size) {
	int     local = DpFindPeer(from), group = -1, found = 0;
	HRESULT result = 0;
	(void)self;
	if (!g_dp.open || g_dp.failed || g_dp.closing)
		return DPERR_SESSIONLOST;
	if (from != g_dp.local_id || local < 0 || !g_dp.peers[local].active)
		return DPERR_INVALIDPLAYER;
	if (flags || !data || size > DP_PAYLOAD)
		return DX_E_INVALIDARG;
	for (int i = 0; i < DP_GROUPS; ++i)
		if (to && g_dp.groups[i] == to)
			group = i;
	for (int i = 0; i < DP_PEERS; ++i) {
		DpPeer* peer = &g_dp.peers[i];
		if (!peer->active || (to && peer->id != to && (group < 0 || !(peer->groups & (1u << group)))))
			continue;
		found = 1;
		/* BoP implements its own broadcast/group loopback above DirectPlay. */
		if (peer->id == from)
			continue;
		int sent = DpSend(g_dp.host ? peer->link : g_dp.host_link, DP_GAME, 0, from, peer->id, data, size);
		if (sent <= 0)
			result = sent == 0 ? DPERR_BUSY : DX_E_FAIL;
	}
	return found || !to ? result : DPERR_INVALIDPLAYER;
}

static IDirectPlay2AVtbl g_play_vtable   = { .QueryInterface        = DpQuery2,
											 .AddRef                = DpAddRef2,
											 .Release               = DpRelease2,
											 .AddPlayerToGroup      = DpAddGroup,
											 .Close                 = DpClose2,
											 .CreateGroup           = DpCreateGroup,
											 .CreatePlayer          = DpCreatePlayer,
											 .DeletePlayerFromGroup = DpDeleteGroup,
											 .DestroyGroup          = DpDestroyGroup,
											 .DestroyPlayer         = DpDestroyPlayer,
											 .EnumPlayers           = DpEnumPlayers,
											 .EnumSessions          = DpEnumSessions,
											 .GetCaps               = DpGetCaps,
											 .Open                  = DpOpen2,
											 .Receive               = DpReceive,
											 .Send                  = DpSendGame,
											 .SetPlayerName         = DpSetName };
static IDirectPlayVtbl   g_legacy_vtable = { .QueryInterface = DpQuery1,
											 .AddRef         = DpAddRef1,
											 .Release        = DpRelease1 };

HRESULT AERON_DXAPI DirectPlayCreate(const GUID* provider, IDirectPlay** out, void* outer) {
	if (!out)
		return DX_E_INVALIDARG;
	*out = NULL;
	if (outer ||
		(provider && provider->Data1 != DP_PROVIDER_TCPIP_DATA1 && provider->Data1 != DP_PROVIDER_IPX_DATA1))
		return DX_E_NOTIMPL;
	/* The old IPX menu path selects modern LAN discovery over the same UDP provider. */
	++g_references;
	*out = &g_legacy;
	return 0;
}
