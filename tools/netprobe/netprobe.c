#define SDL_MAIN_HANDLED
#include "aeron/aeron.h"
#include "aeron/compat/dplay.h"
#include "aeron/compat/dplay_directory.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PROBE_PINGS = 10, PROBE_PACKET_SIZE = 64, PROBE_RECEIVE_SIZE = 8192 };

typedef enum ProbePacketType { PROBE_PING = 1, PROBE_PONG = 2 } ProbePacketType;

static const char g_version[]   = "aeron-netprobe-1";
static const GUID g_application = {
	0x09438c20, 0xe06a, 0x11ce, { 0x86, 0x81, 0, 0xaa, 0, 0x6c, 0x5d, 0x57 }
};
/* IID_IDirectPlay2A from the DirectPlay SDK. */
static const GUID g_play_iid = {
	0x9d460580, 0xa822, 0x11cf, { 0x96, 0x0c, 0, 0x80, 0xc7, 0x53, 0x4e, 0x82 }
};
static volatile sig_atomic_t       g_stop;
static IDirectPlay2A*              g_play;
static DPID                        g_player;
static char                        g_room_name[AERON_DPLAY_DIRECTORY_NAME_CAPACITY];
static AeronDplayDirectorySnapshot g_snapshot;
static SDL_Mutex*                  g_wake_mutex;
static SDL_Condition*              g_wake_condition;
static int                         g_wake_pending;

static void Wake(void* user) {
	(void)user;
	SDL_LockMutex(g_wake_mutex);
	if (!g_wake_pending) {
		g_wake_pending = 1;
		SDL_SignalCondition(g_wake_condition);
	}
	SDL_UnlockMutex(g_wake_mutex);
}

static void Wait(uint64_t deadline) {
	uint64_t now   = Aeron_NowUs();
	uint64_t delay = AeronDplay_NextWakeDelayUs();
	/* Keep Ctrl+C responsive without calling thread primitives from a signal
	 * handler. Network notifications interrupt the wait immediately. */
	if (delay > 50000)
		delay = 50000;
	uint64_t until = now + delay;
	if (deadline < until)
		until = deadline;
	SDL_LockMutex(g_wake_mutex);
	while (!g_wake_pending && (now = Aeron_NowUs()) < until)
		SDL_WaitConditionTimeout(g_wake_condition, g_wake_mutex, (Sint32)((until - now + 999) / 1000));
	/* Consume before servicing work so arrivals during Update remain pending
	 * for the next wait. The mutex covers the check-to-sleep transition. */
	g_wake_pending = 0;
	SDL_UnlockMutex(g_wake_mutex);
}

static void Stop(int signal_number) {
	(void)signal_number;
	g_stop = 1;
}

static int DplayResult(const char* action, HRESULT result) {
	if (!result)
		return 1;
	fprintf(stderr, "%s failed: DirectPlay 0x%08x\n", action, (unsigned)result);
	return 0;
}

static int DirectoryResult(const char* action, AeronDplayDirectoryError error) {
	static const char* const names[] = {
		[AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST]        = "invalid request or configuration",
		[AERON_DPLAY_DIRECTORY_ERROR_UNAUTHORIZED]           = "unauthorized",
		[AERON_DPLAY_DIRECTORY_ERROR_NOT_FOUND]              = "room or connection not found",
		[AERON_DPLAY_DIRECTORY_ERROR_CONFLICT]               = "conflict",
		[AERON_DPLAY_DIRECTORY_ERROR_INCOMPATIBLE]           = "incompatible version",
		[AERON_DPLAY_DIRECTORY_ERROR_FULL]                   = "room full",
		[AERON_DPLAY_DIRECTORY_ERROR_NOT_JOINABLE]           = "room not joinable",
		[AERON_DPLAY_DIRECTORY_ERROR_CLOSED]                 = "room closed",
		[AERON_DPLAY_DIRECTORY_ERROR_BODY_TOO_LARGE]         = "request too large",
		[AERON_DPLAY_DIRECTORY_ERROR_UNSUPPORTED_MEDIA_TYPE] = "unsupported media type",
		[AERON_DPLAY_DIRECTORY_ERROR_RATE_LIMITED]           = "rate limited",
		[AERON_DPLAY_DIRECTORY_ERROR_CAPACITY]               = "service capacity reached",
		[AERON_DPLAY_DIRECTORY_ERROR_UNAVAILABLE]            = "service unavailable or invalid response",
		[AERON_DPLAY_DIRECTORY_ERROR_METHOD_NOT_ALLOWED]     = "method not allowed",
		[AERON_DPLAY_DIRECTORY_ERROR_CONNECTION_FAILED]      = "ICE connection failed",
		[AERON_DPLAY_DIRECTORY_ERROR_NETWORK]                = "HTTPS request failed (network or TLS)",
		[AERON_DPLAY_DIRECTORY_ERROR_TIMEOUT]                = "timed out",
		[AERON_DPLAY_DIRECTORY_ERROR_NO_MEMORY]              = "out of memory",
		[AERON_DPLAY_DIRECTORY_ERROR_NOT_CONFIGURED]         = "directory not configured",
		[AERON_DPLAY_DIRECTORY_ERROR_BUSY]                   = "operation busy"
	};
	if (!error)
		return 1;
	const char* name = (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : NULL;
	fprintf(stderr, "%s failed: %s (directory error %u)\n", action, name ? name : "unknown error",
			(unsigned)error);
	return 0;
}

/* The shim's worker completions are serviced on the application thread. */
static int Tick(uint64_t deadline) {
	if (g_stop)
		return 0;
	uint64_t now = Aeron_NowUs();
	if (now >= deadline) {
		fprintf(stderr, "Operation timed out\n");
		return 0;
	}
	Wait(deadline);
	AeronDplay_Update();
	return !g_stop;
}

static void PrintGuid(const GUID* id) {
	printf("%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned)id->Data1, (unsigned)id->Data2,
		   (unsigned)id->Data3, id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3], id->Data4[4],
		   id->Data4[5], id->Data4[6], id->Data4[7]);
}

static int Configure(const char* path) {
	char        root[1024] = ".";
	const char* file       = path;
	for (const char* p = path; *p; ++p)
		if (*p == '/' || *p == '\\')
			file = p + 1;
	if (file != path) {
		size_t length = (size_t)(file - path);
		if (length >= sizeof(root)) {
			fprintf(stderr, "Configuration path is too long\n");
			return 0;
		}
		memcpy(root, path, length);
		root[length] = 0;
	}
	AeronVfsConfig paths = {
		.asset_root = root, .resource_root = root, .user_root = root, .temp_root = root
	};
	AeronVfs* vfs = AeronVfs_Create(&paths);
	if (!vfs) {
		fprintf(stderr, "Cannot open configuration directory\n");
		return 0;
	}
	AeronConfigFile* document = NULL;
	AeronConfigError error    = { 0 };
	int              loaded = AeronConfigFile_LoadYamlEx(vfs, AERON_VFS_ROOT_ASSET, file, &document, &error);
	AeronVfs_Destroy(vfs);
	if (!loaded) {
		fprintf(stderr, "%s:%d: %s\n", path, error.line, error.message);
		return 0;
	}
	const char*               origin = AeronConfigFile_GetString(document, "lobby_url", NULL);
	const char*               name   = AeronConfigFile_GetString(document, "room_name", NULL);
	AeronDplayDirectoryConfig config = { 0 };
	int                       valid  = origin && name && *name && strlen(origin) < sizeof(config.lobby_url) &&
									   strlen(name) < sizeof(g_room_name);
	if (valid) {
		strcpy(config.lobby_url, origin);
		strcpy(config.game_version, g_version);
		strcpy(g_room_name, name);
		config.application_id = g_application;
	} else {
		fprintf(stderr,
				"Configuration needs lobby_url and room_name strings (room_name: 1..31 UTF-8 bytes)\n");
	}
	AeronConfigFile_Destroy(document);
	return valid && DirectoryResult("Configure", AeronDplayDirectory_Configure(&config));
}

static int Refresh(void) {
	if (!DirectoryResult("List rooms", AeronDplayDirectory_Refresh()))
		return 0;
	uint64_t deadline = Aeron_NowUs() + 30000000;
	do {
		AeronDplayDirectory_GetSnapshot(&g_snapshot);
		if (g_snapshot.refresh.state == AERON_DPLAY_DIRECTORY_SUCCEEDED)
			return 1;
		if (g_snapshot.refresh.state == AERON_DPLAY_DIRECTORY_FAILED)
			return DirectoryResult("List rooms", g_snapshot.refresh.error);
	} while (Tick(deadline));
	return 0;
}

static int Compatible(const AeronDplayDirectoryRoom* room) {
	return room->protocol == AERON_DPLAY_DIRECTORY_PROTOCOL && !strcmp(room->game_version, g_version);
}

static int List(void) {
	if (!Refresh())
		return 0;
	unsigned count = 0;
	for (unsigned i = 0; i < g_snapshot.room_count; ++i) {
		const AeronDplayDirectoryRoom* room = &g_snapshot.rooms[i];
		if (!Compatible(room))
			continue;
		++count;
		PrintGuid(&room->room_id);
		printf("  %s  %u/%u%s\n", room->metadata.name, room->metadata.players, room->metadata.max_players,
			   room->metadata.joinable ? "" : " (closed)");
	}
	printf("%u probe room(s)\n", count);
	return 1;
}

static int OpenPlayer(DPSESSIONDESC2* desc, int host) {
	uint64_t deadline = Aeron_NowUs() + 30000000;
	HRESULT  result;
	for (;;) {
		result = g_play->lpVtbl->Open(g_play, desc, host ? DPOPEN_CREATE : DPOPEN_JOIN);
		if (result != DPERR_PENDING && result != DPERR_BUSY)
			break;
		if (!Tick(deadline))
			return 0;
	}
	if (!DplayResult("Open session", result))
		return 0;
	DPNAME name = { .dwSize = sizeof(name), .lpszShortNameA = host ? "host" : "client" };
	for (;;) {
		result = g_play->lpVtbl->CreatePlayer(g_play, &g_player, &name, NULL, NULL, 0, 0);
		if (result != DPERR_PENDING && result != DPERR_BUSY)
			return DplayResult("Create player", result);
		if (!Tick(deadline))
			return 0;
	}
}

static int AERON_DXAPI FindPeer(DPID id, uint32_t type, const DPNAME* name, uint32_t flags, void* out) {
	(void)name;
	(void)flags;
	if (type == DPPLAYERTYPE_PLAYER && id != g_player)
		*(DPID*)out = id;
	return 1;
}

static DPID Peer(void) {
	DPID peer = 0;
	g_play->lpVtbl->EnumPlayers(g_play, NULL, FindPeer, &peer, 0);
	return peer;
}

static int PrintPath(DPID peer) {
	AeronDplayConnectionInfo info;
	if (!AeronDplay_GetConnectionInfo(peer, &info))
		return 0;
	printf("ICE: %s; local %s%s -> remote %s%s\n",
		   info.local_relayed || info.remote_relayed ? "TURN relay" : "direct", info.local_address,
		   info.local_relayed ? " (relay)" : "", info.remote_address, info.remote_relayed ? " (relay)" : "");
	return 1;
}

static void Packet(unsigned char* packet, ProbePacketType type, unsigned sequence) {
	memcpy(packet, "ANP1", 4);
	packet[4] = (unsigned char)type;
	packet[5] = (unsigned char)sequence;
	for (unsigned i = 6; i < PROBE_PACKET_SIZE; ++i)
		packet[i] = (unsigned char)(sequence + i);
}

/* Skip DirectPlay system messages and accept only our fixed diagnostic payload. */
static int Receive(unsigned char* packet, DPID peer, ProbePacketType type) {
	for (unsigned n = 0; n < 64; ++n) {
		DPID     from, to;
		uint32_t size   = PROBE_RECEIVE_SIZE;
		HRESULT  result = g_play->lpVtbl->Receive(g_play, &from, &to, DPRECEIVE_ALL, packet, &size);
		if (result == DPERR_NOMESSAGES)
			return 0;
		if (!DplayResult("Receive", result))
			return -1;
		if (from != peer || !peer || to != g_player || size != PROBE_PACKET_SIZE ||
			memcmp(packet, "ANP1", 4) || packet[4] != type || packet[5] >= PROBE_PINGS)
			continue;
		for (unsigned i = 6; i < PROBE_PACKET_SIZE; ++i)
			if (packet[i] != (unsigned char)(packet[5] + i)) {
				fprintf(stderr, "Probe payload mismatch\n");
				return -1;
			}
		return 1;
	}
	return 0;
}

static int Host(void) {
	DPSESSIONDESC2 desc = { .dwSize           = sizeof(desc),
							.guidApplication  = g_application,
							.dwMaxPlayers     = 2,
							.lpszSessionNameA = g_room_name };
	if (!OpenPlayer(&desc, 1))
		return 0;
	AeronDplayRoomMetadata metadata = { .players = 1, .max_players = 2, .joinable = 1 };
	strcpy(metadata.name, g_room_name);
	strcpy(metadata.roster[0].name, "host");
	strcpy(metadata.roster[1].name, "client");
	if (!DirectoryResult("Register room", AeronDplayDirectory_StartHosting(&desc.guidInstance, &metadata)))
		return 0;
	uint64_t deadline   = Aeron_NowUs() + 30000000;
	int      registered = 0, path_printed = 0;
	DPID     previous = 0;
	while (!g_stop) {
		AeronDplayDirectoryStatus status;
		AeronDplayDirectory_GetHostStatus(&status);
		if (status.state == AERON_DPLAY_DIRECTORY_FAILED)
			return DirectoryResult("Host room", status.error);
		if (!registered && status.state == AERON_DPLAY_DIRECTORY_SUCCEEDED) {
			printf("Hosting %s (", g_room_name);
			PrintGuid(&desc.guidInstance);
			printf("). Waiting for a client; Ctrl+C stops the host.\n");
			registered = 1;
			deadline   = UINT64_MAX;
		}
		DPID peer = Peer();
		if (peer != previous) {
			printf(peer ? "Player %u connected\n" : "Player %u disconnected\n", peer ? peer : previous);
			previous          = peer;
			path_printed      = 0;
			metadata.players  = peer ? 2 : 1;
			metadata.joinable = peer ? 0 : 1;
			if (!DirectoryResult("Update room", AeronDplayDirectory_UpdateHost(&metadata)))
				return 0;
		}
		if (peer && !path_printed)
			path_printed = PrintPath(peer);
		unsigned char packet[PROBE_RECEIVE_SIZE];
		int           received = Receive(packet, peer, PROBE_PING);
		if (received < 0)
			return 0;
		if (received) {
			packet[4]      = PROBE_PONG;
			HRESULT result = g_play->lpVtbl->Send(g_play, g_player, peer, 0, packet, PROBE_PACKET_SIZE);
			if (result != DPERR_BUSY && !DplayResult("Echo", result))
				return 0;
			if (!result)
				printf("Echoed ping %u\n", packet[5] + 1);
			AeronDplay_Update();
			continue;
		}
		if (!Tick(deadline))
			return g_stop != 0;
	}
	return 1;
}

static int Ping(DPID peer) {
	uint64_t sent_at[PROBE_PINGS] = { 0 }, sum = 0, maximum = 0;
	unsigned sent = 0, replies = 0, seen = 0;
	uint64_t start = Aeron_NowUs(), finish = start + (PROBE_PINGS + 3) * UINT64_C(1000000);
	int      path_printed = 0;
	while (!g_stop && Aeron_NowUs() < finish && replies < PROBE_PINGS) {
		if (!path_printed)
			path_printed = PrintPath(peer);
		unsigned char packet[PROBE_RECEIVE_SIZE];
		uint64_t      now = Aeron_NowUs();
		if (sent < PROBE_PINGS && now >= start + sent * UINT64_C(1000000)) {
			Packet(packet, PROBE_PING, sent);
			HRESULT result = g_play->lpVtbl->Send(g_play, g_player, peer, 0, packet, PROBE_PACKET_SIZE);
			if (result != DPERR_BUSY && !DplayResult("Ping", result))
				return 0;
			if (!result)
				sent_at[sent++] = now;
		}
		int received = Receive(packet, peer, PROBE_PONG);
		if (received < 0)
			return 0;
		if (received && packet[5] < sent && !(seen & (1u << packet[5]))) {
			uint64_t rtt = Aeron_NowUs() - sent_at[packet[5]];
			seen |= 1u << packet[5];
			++replies;
			sum += rtt;
			if (rtt > maximum)
				maximum = rtt;
			printf("Pong %u: %.2f ms\n", packet[5] + 1, (double)rtt / 1000.0);
			AeronDplay_Update();
			continue;
		}
		uint64_t next_ping = start + sent * UINT64_C(1000000);
		/* A busy Send retries at the transport maintenance interval. */
		Wait(sent < PROBE_PINGS && next_ping > Aeron_NowUs() ? next_ping : finish);
		AeronDplay_Update();
	}
	if (!path_printed && !PrintPath(peer))
		printf("ICE: nominated path unavailable\n");
	printf("%u/%u replies, %u unsent; average %.2f ms, maximum %.2f ms\n", replies, sent, PROBE_PINGS - sent,
		   replies ? (double)sum / replies / 1000.0 : 0.0, (double)maximum / 1000.0);
	return !g_stop && replies > 0;
}

static int Join(void) {
	if (!Refresh())
		return 0;
	const AeronDplayDirectoryRoom* selected = NULL;
	for (unsigned i = 0; i < g_snapshot.room_count; ++i) {
		const AeronDplayDirectoryRoom* room = &g_snapshot.rooms[i];
		if (!Compatible(room) || strcmp(room->metadata.name, g_room_name))
			continue;
		if (selected) {
			fprintf(stderr, "Multiple probe rooms have that name; use a unique room_name\n");
			return 0;
		}
		selected = room;
	}
	if (!selected) {
		fprintf(stderr, "No probe room named '%s'; start the host first\n", g_room_name);
		return 0;
	}
	printf("Joining %s; establishing ICE...\n", g_room_name);
	if (!DirectoryResult("Begin join", AeronDplayDirectory_BeginJoin(&selected->room_id)))
		return 0;
	for (;;) {
		AeronDplayJoinStatus status;
		AeronDplayDirectory_GetJoinStatus(&status);
		if (status.preparation.state == AERON_DPLAY_DIRECTORY_SUCCEEDED)
			break;
		if (status.preparation.state == AERON_DPLAY_DIRECTORY_FAILED)
			return DirectoryResult("ICE preparation", status.preparation.error);
		if (!Tick(status.deadline_us))
			return 0;
	}
	printf("ICE connected; opening DirectPlay session...\n");
	DPSESSIONDESC2 desc = { .dwSize          = sizeof(desc),
							.guidApplication = g_application,
							.guidInstance    = selected->room_id };
	if (!OpenPlayer(&desc, 0))
		return 0;
	AeronDplayDirectory_FinishJoin();
	DPID peer = Peer();
	if (!peer) {
		fprintf(stderr, "Session has no host player\n");
		return 0;
	}
	return Ping(peer);
}

int main(int argc, char** argv) {
	if (argc != 3 || (strcmp(argv[1], "host") && strcmp(argv[1], "join") && strcmp(argv[1], "list"))) {
		fprintf(stderr, "Usage: aeron-netprobe host|list|join config.yaml\n");
		return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
	}
	SDL_SetMainReady();
	if (!SDL_Init(0)) {
		fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
		return 1;
	}
	g_wake_mutex     = SDL_CreateMutex();
	g_wake_condition = SDL_CreateCondition();
	if (!g_wake_mutex || !g_wake_condition) {
		fprintf(stderr, "Cannot create network wait: %s\n", SDL_GetError());
		SDL_DestroyCondition(g_wake_condition);
		SDL_DestroyMutex(g_wake_mutex);
		SDL_Quit();
		return 1;
	}
	AeronDplay_SetWakeCallback(Wake, NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, Stop);
	signal(SIGTERM, Stop);
	int ok = Configure(argv[2]);
	if (ok && !strcmp(argv[1], "list")) {
		ok = List();
	} else if (ok) {
		IDirectPlay* legacy = NULL;
		ok                  = DplayResult("Create DirectPlay", DirectPlayCreate(NULL, &legacy, NULL));
		if (ok) {
			ok = DplayResult("Get DirectPlay2",
							 legacy->lpVtbl->QueryInterface(legacy, &g_play_iid, (void**)&g_play));
			legacy->lpVtbl->Release(legacy);
		}
		if (ok)
			ok = !strcmp(argv[1], "host") ? Host() : Join();
	}
	if (g_play) {
		g_play->lpVtbl->Close(g_play);
		AeronDplayDirectory_CancelJoin();
		uint64_t until = Aeron_NowUs() + 300000;
		while (Aeron_NowUs() < until && AeronDplay_IsActive()) {
			Wait(until);
			AeronDplay_Update();
		}
		g_play->lpVtbl->Release(g_play);
	}
	AeronDplay_Shutdown();
	SDL_DestroyCondition(g_wake_condition);
	SDL_DestroyMutex(g_wake_mutex);
	SDL_Quit();
	return ok ? 0 : 1;
}
