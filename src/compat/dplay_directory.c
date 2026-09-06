#include "aeron/random.h"
#include "dplay_directory_internal.h"
#include "dplay_directory_json.h"
#include "dplay_http.h"
#include "dplay_ice.h"
#include "dplay_internal.h"
#include <limits.h>

enum {
	DIR_LIST,
	DIR_ROOM,
	DIR_HOST_POLL,
	DIR_JOIN_CREATE,
	DIR_JOIN_OFFER,
	DIR_JOIN_POLL,
	DIR_ANSWER_FIRST,
	DIR_DELETE_FIRST = DIR_ANSWER_FIRST + 8,
	DIR_OPERATIONS   = DIR_DELETE_FIRST + 8
};

typedef struct DirectoryOperation {
	DpHttpRequest             request;
	uint64_t                  id, due, deadline;
	unsigned                  attempts;
	int                       wanted;
	AeronDplayDirectoryStatus status;
} DirectoryOperation;

typedef struct HostEntry {
	int               used;
	DpDirectorySignal signal;
} HostEntry;

typedef struct Directory {
	DpHttp*                     http;
	AeronDplayDirectoryConfig   config;
	AeronDplayDirectorySnapshot cache, next_cache;
	DirectoryOperation          operations[DIR_OPERATIONS];

	struct {
		int                       active, published;
		GUID                      room;
		char                      token[44], cursor[DP_DIRECTORY_CURSOR];
		uint32_t                  revision, sent_revision;
		AeronDplayRoomMetadata    desired, sent;
		AeronDplayDirectoryStatus status, polling;
		uint64_t                  renew_at;
		HostEntry                 entries[8];
		DpDirectoryHostSignals    next_signals;
	} host;

	struct {
		int                     active, have_signal;
		uint64_t                generation;
		char                    token[44];
		AeronDplayJoinStatus    status;
		AeronDplayDirectoryRoom room;
		DpDirectorySignal       signal;
	} join;
} Directory;

static Directory g_directory;
static uint64_t  g_generation;

static int SameGuid(const GUID* a, const GUID* b) { return !memcmp(a, b, sizeof(*a)); }

static int SameIdentity(const AeronDplayConnectionIdentity* a, const AeronDplayConnectionIdentity* b) {
	return SameGuid(&a->application_id, &b->application_id) && SameGuid(&a->room_id, &b->room_id) &&
		   SameGuid(&a->connection_id, &b->connection_id);
}

static AeronDplayDirectoryStatus Status(AeronDplayDirectoryOperationState state,
										AeronDplayDirectoryError          error) {
	AeronDplayDirectoryStatus status = { state, error };
	return status;
}

static int NewToken(char token[44]) {
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	uint8_t           bytes[32];
	if (!Aeron_RandomBytes(bytes, sizeof(bytes)))
		return 0;
	unsigned bits = 0, value = 0, out = 0;
	for (unsigned i = 0; i < sizeof(bytes); ++i) {
		value = (value << 8) | bytes[i];
		bits += 8;
		while (bits >= 6) {
			bits -= 6;
			token[out++] = alphabet[(value >> bits) & 63];
		}
	}
	if (bits)
		token[out++] = alphabet[(value << (6 - bits)) & 63];
	token[out] = 0;
	memset(bytes, 0, sizeof(bytes));
	return 1;
}

static int NewGuid(GUID* id) {
	if (!Aeron_RandomBytes(id, sizeof(*id)))
		return 0;
	id->Data3    = (uint16_t)((id->Data3 & 0x0fff) | 0x4000);
	id->Data4[0] = (uint8_t)((id->Data4[0] & 0x3f) | 0x80);
	return 1;
}

static void Path(char out[DP_HTTP_PATH], const GUID* room, const GUID* connection, const char* suffix) {
	char app[37], r[37], c[37];
	DpDirectory_GuidText(&g_directory.config.application_id, app);
	if (room)
		DpDirectory_GuidText(room, r);
	if (connection)
		DpDirectory_GuidText(connection, c);
	if (connection)
		snprintf(out, DP_HTTP_PATH, "/v1/apps/%s/rooms/%s/connections/%s%s", app, r, c, suffix);
	else if (room)
		snprintf(out, DP_HTTP_PATH, "/v1/apps/%s/rooms/%s%s", app, r, suffix);
	else
		snprintf(out, DP_HTTP_PATH, "/v1/apps/%s/rooms%s", app, suffix);
}

static void CancelOperation(unsigned index) {
	DirectoryOperation* op = &g_directory.operations[index];
	DpHttp_Cancel(g_directory.http, op->id);
	memset(op, 0, sizeof(*op));
}

static AeronDplayDirectoryError QueueOperation(unsigned index, DpHttpMethod method, const GUID* room,
											   const GUID* connection, const char* suffix, const char* token,
											   char* body, uint64_t deadline) {
	DirectoryOperation* op = &g_directory.operations[index];
	if (method == DP_HTTP_PUT && !body)
		return AERON_DPLAY_DIRECTORY_ERROR_NO_MEMORY;
	size_t size = body ? strlen(body) : 0;
	if (size > (index == DIR_ROOM || index == DIR_JOIN_CREATE ? 4096u : DP_HTTP_BODY)) {
		cJSON_free(body);
		return AERON_DPLAY_DIRECTORY_ERROR_BODY_TOO_LARGE;
	}
	CancelOperation(index);
	op->request.method     = method;
	op->request.timeout_ms = index == DIR_HOST_POLL || index == DIR_JOIN_POLL ? 35000 : 10000;
	Path(op->request.path, room, connection, suffix);
	if (token)
		strcpy(op->request.token, token);
	if (body) {
		memcpy(op->request.body, body, size + 1);
		cJSON_free(body);
	}
	op->wanted   = 1;
	op->due      = Aeron_NowUs();
	op->deadline = deadline;
	op->status   = Status(AERON_DPLAY_DIRECTORY_PENDING, AERON_DPLAY_DIRECTORY_ERROR_NONE);
	return AERON_DPLAY_DIRECTORY_ERROR_NONE;
}

static void DeleteResource(const GUID* room, const GUID* connection, const char* token) {
	for (unsigned i = DIR_DELETE_FIRST; i < DIR_OPERATIONS; ++i) {
		if (g_directory.operations[i].wanted)
			continue;
		QueueOperation(i, DP_HTTP_DELETE, room, connection, "", token, NULL, Aeron_NowUs() + 2000000);
		return;
	}
	Aeron_LogWarn("compat.dplay.directory", "cleanup queue full; resource lease will expire");
}

static AeronDplayDirectoryError PublishRoom(void) {
	AeronDplayDirectoryError error =
		QueueOperation(DIR_ROOM, DP_HTTP_PUT, &g_directory.host.room, NULL, "", g_directory.host.token,
					   DpDirectory_RoomJSON(&g_directory.host.desired, g_directory.host.revision,
											g_directory.config.game_version),
					   0);
	if (!error) {
		g_directory.host.sent          = g_directory.host.desired;
		g_directory.host.sent_revision = g_directory.host.revision;
	}
	g_directory.host.status =
		Status(error ? AERON_DPLAY_DIRECTORY_FAILED : AERON_DPLAY_DIRECTORY_PENDING, error);
	return error;
}

static void PollHost(void) {
	QueueOperation(DIR_HOST_POLL, DP_HTTP_GET, &g_directory.host.room, NULL, "/connections",
				   g_directory.host.token, NULL, 0);
	char*  out = g_directory.operations[DIR_HOST_POLL].request.path;
	size_t n   = strlen(out);
	if (g_directory.host.cursor[0]) {
		memcpy(out + n, "?cursor=", 8);
		n += 8;
		/* Encode the opaque cursor, including any query delimiters. */
		for (const unsigned char* p = (const unsigned char*)g_directory.host.cursor; *p; ++p) {
			static const char hex[] = "0123456789ABCDEF";
			out[n++]                = '%';
			out[n++]                = hex[*p >> 4];
			out[n++]                = hex[*p & 15];
		}
		out[n] = 0;
	}
	g_directory.host.polling = Status(AERON_DPLAY_DIRECTORY_PENDING, 0);
}

static void StopJoin(AeronDplayDirectoryOperationState state, AeronDplayDirectoryError error) {
	if (g_directory.join.active) {
		if (state != AERON_DPLAY_DIRECTORY_IDLE)
			DpIce_CancelJoin(g_directory.join.generation);
		for (unsigned i = DIR_JOIN_CREATE; i <= DIR_JOIN_POLL; ++i)
			CancelOperation(i);
		DeleteResource(&g_directory.join.status.identity.room_id,
					   &g_directory.join.status.identity.connection_id, g_directory.join.token);
	}
	g_directory.join.active = g_directory.join.have_signal = 0;
	memset(g_directory.join.token, 0, sizeof(g_directory.join.token));
	memset(&g_directory.join.signal, 0, sizeof(g_directory.join.signal));
	g_directory.join.status.preparation = Status(state, error);
}

AeronDplayDirectoryError AeronDplayDirectory_Configure(const AeronDplayDirectoryConfig* config) {
	char origin[AERON_DPLAY_DIRECTORY_URL_CAPACITY];
	if (!config || !DpDirectory_Version(config->game_version) || !DpHttp_Origin(config->lobby_url, origin))
		return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	if (g_dp.open || g_dp.closing || g_dp.operation.kind || DpIce_Active() || g_directory.host.active ||
		g_directory.join.active || (g_directory.http && DpHttp_Pending(g_directory.http)))
		return AERON_DPLAY_DIRECTORY_ERROR_BUSY;
	for (unsigned i = 0; i < DIR_OPERATIONS; ++i)
		if (g_directory.operations[i].wanted)
			return AERON_DPLAY_DIRECTORY_ERROR_BUSY;
	DpHttp* http = DpHttp_Create(origin);
	if (!http)
		return AERON_DPLAY_DIRECTORY_ERROR_UNAVAILABLE;
	DpHttp_Destroy(g_directory.http);
	memset(&g_directory, 0, sizeof(g_directory));
	g_directory.http = http;
	strcpy(g_directory.config.lobby_url, origin);
	g_directory.config.application_id = config->application_id;
	strcpy(g_directory.config.game_version, config->game_version);
	g_directory.cache.application_id = config->application_id;
	return AERON_DPLAY_DIRECTORY_ERROR_NONE;
}

AeronDplayDirectoryError AeronDplayDirectory_Refresh(void) {
	if (!g_directory.http)
		return AERON_DPLAY_DIRECTORY_ERROR_NOT_CONFIGURED;
	if (!g_directory.operations[DIR_LIST].wanted)
		QueueOperation(DIR_LIST, DP_HTTP_GET, NULL, NULL, "", NULL, NULL, 0);
	g_directory.cache.refresh = g_directory.operations[DIR_LIST].status;
	return AERON_DPLAY_DIRECTORY_ERROR_NONE;
}

void AeronDplayDirectory_GetSnapshot(AeronDplayDirectorySnapshot* out) {
	if (out)
		*out = g_directory.cache;
}

unsigned DpDirectory_GetPage(const GUID*             application,
							 AeronDplayDirectoryRoom rooms[AERON_DPLAY_DIRECTORY_PAGE_SIZE]) {
	if (!g_directory.cache.available || !SameGuid(application, &g_directory.cache.application_id))
		return 0;
	unsigned start = g_directory.cache.page_index * AERON_DPLAY_DIRECTORY_PAGE_SIZE;
	if (start >= g_directory.cache.room_count)
		return 0;
	unsigned count = g_directory.cache.room_count - start;
	if (count > AERON_DPLAY_DIRECTORY_PAGE_SIZE)
		count = AERON_DPLAY_DIRECTORY_PAGE_SIZE;
	memcpy(rooms, g_directory.cache.rooms + start, count * sizeof(*rooms));
	return count;
}

AeronDplayDirectoryError AeronDplayDirectory_SelectPage(uint32_t page) {
	if (page && page >= (g_directory.cache.room_count + 31) / 32)
		return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	g_directory.cache.page_index = page;
	return AERON_DPLAY_DIRECTORY_ERROR_NONE;
}

AeronDplayDirectoryError AeronDplayDirectory_StartHosting(const GUID*                   room,
														  const AeronDplayRoomMetadata* metadata) {
	AeronDplayRoomMetadata clean;
	char                   token[44];
	if (!g_directory.http)
		return AERON_DPLAY_DIRECTORY_ERROR_NOT_CONFIGURED;
	if (!room || !DpDirectory_Metadata(metadata, &clean))
		return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	if (g_directory.join.active || (g_directory.host.active && !SameGuid(room, &g_directory.host.room)))
		return AERON_DPLAY_DIRECTORY_ERROR_BUSY;
	if (!g_dp.open || !g_dp.host || g_dp.closing || g_dp.failed || !SameGuid(room, &g_dp.session) ||
		!SameGuid(&g_directory.config.application_id, &g_dp.app))
		return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	if (g_directory.host.active) {
		if (memcmp(&clean, &g_directory.host.desired, sizeof(clean)))
			return AERON_DPLAY_DIRECTORY_ERROR_CONFLICT;
		if (!g_directory.operations[DIR_ROOM].wanted) {
			AeronDplayDirectoryStatus previous = g_directory.host.status;
			AeronDplayDirectoryError  error    = PublishRoom();
			if (error)
				g_directory.host.status = previous;
			else if (g_directory.host.polling.state == AERON_DPLAY_DIRECTORY_FAILED) {
				CancelOperation(DIR_HOST_POLL);
				g_directory.host.polling   = Status(AERON_DPLAY_DIRECTORY_IDLE, 0);
				g_directory.host.published = 0;
			}
			return error;
		}
		return AERON_DPLAY_DIRECTORY_ERROR_NONE;
	}
	if (!NewToken(token))
		return AERON_DPLAY_DIRECTORY_ERROR_UNAVAILABLE;
	/* Prepare the request before committing the public operation. */
	char*                    json  = DpDirectory_RoomJSON(&clean, 1, g_directory.config.game_version);
	AeronDplayDirectoryError error = QueueOperation(DIR_ROOM, DP_HTTP_PUT, room, NULL, "", token, json, 0);
	if (error)
		return error;
	memset(&g_directory.host, 0, sizeof(g_directory.host));
	g_directory.host.active   = 1;
	g_directory.host.room     = *room;
	g_directory.host.revision = g_directory.host.sent_revision = 1;
	g_directory.host.desired = g_directory.host.sent = clean;
	strcpy(g_directory.host.token, token);
	g_directory.host.status = Status(AERON_DPLAY_DIRECTORY_PENDING, 0);
	return AERON_DPLAY_DIRECTORY_ERROR_NONE;
}

AeronDplayDirectoryError AeronDplayDirectory_UpdateHost(const AeronDplayRoomMetadata* metadata) {
	AeronDplayRoomMetadata clean;
	if (!g_directory.http)
		return AERON_DPLAY_DIRECTORY_ERROR_NOT_CONFIGURED;
	if (!DpDirectory_Metadata(metadata, &clean))
		return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	if (!g_directory.host.active)
		return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	if (!memcmp(&clean, &g_directory.host.desired, sizeof(clean)))
		return AERON_DPLAY_DIRECTORY_ERROR_NONE;
	if (g_directory.host.revision == INT32_MAX)
		return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	g_directory.host.desired = clean;
	++g_directory.host.revision;
	g_directory.host.status   = Status(AERON_DPLAY_DIRECTORY_PENDING, 0);
	g_directory.host.renew_at = 0;
	return AERON_DPLAY_DIRECTORY_ERROR_NONE;
}

void AeronDplayDirectory_GetHostStatus(AeronDplayDirectoryStatus* out) {
	if (out)
		*out = g_directory.host.status;
}

void AeronDplayDirectory_StopHosting(void) {
	if (!g_directory.host.active)
		return;
	CancelOperation(DIR_ROOM);
	CancelOperation(DIR_HOST_POLL);
	for (unsigned i = DIR_ANSWER_FIRST; i < DIR_DELETE_FIRST; ++i)
		CancelOperation(i);
	DeleteResource(&g_directory.host.room, NULL, g_directory.host.token);
	memset(&g_directory.host, 0, sizeof(g_directory.host));
}

AeronDplayDirectoryError AeronDplayDirectory_BeginJoin(const GUID* room) {
	AeronDplayConnectionIdentity identity;
	char                         token[44];
	if (!g_directory.http)
		return AERON_DPLAY_DIRECTORY_ERROR_NOT_CONFIGURED;
	if (!room)
		return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	if (g_directory.host.active || g_directory.join.active || g_dp.open || g_dp.closing ||
		g_dp.operation.kind)
		return AERON_DPLAY_DIRECTORY_ERROR_BUSY;
	if (!g_directory.cache.available)
		return AERON_DPLAY_DIRECTORY_ERROR_UNAVAILABLE;
	const AeronDplayDirectoryRoom* selected = NULL;
	for (unsigned i = 0; i < g_directory.cache.room_count; ++i)
		if (SameGuid(room, &g_directory.cache.rooms[i].room_id)) {
			selected = &g_directory.cache.rooms[i];
			break;
		}
	if (!selected)
		return AERON_DPLAY_DIRECTORY_ERROR_NOT_FOUND;
	if (selected->protocol != 2 || strcmp(selected->game_version, g_directory.config.game_version))
		return AERON_DPLAY_DIRECTORY_ERROR_INCOMPATIBLE;
	if (!selected->metadata.joinable)
		return AERON_DPLAY_DIRECTORY_ERROR_NOT_JOINABLE;
	if (selected->metadata.players >= selected->metadata.max_players)
		return AERON_DPLAY_DIRECTORY_ERROR_FULL;
	identity.application_id = g_directory.config.application_id;
	identity.room_id        = *room;
	if (!NewToken(token) || !NewGuid(&identity.connection_id))
		return AERON_DPLAY_DIRECTORY_ERROR_UNAVAILABLE;
	uint64_t                 deadline = Aeron_NowUs() + 90000000;
	AeronDplayDirectoryError error =
		QueueOperation(DIR_JOIN_CREATE, DP_HTTP_PUT, room, &identity.connection_id, "", token,
					   DpDirectory_ConnectionJSON(g_directory.config.game_version), deadline);
	if (error)
		return error;
	memset(&g_directory.join, 0, sizeof(g_directory.join));
	g_directory.join.active             = 1;
	g_directory.join.generation         = ++g_generation;
	g_directory.join.status.identity    = identity;
	g_directory.join.status.deadline_us = deadline;
	g_directory.join.room               = *selected;
	g_directory.join.status.preparation = Status(AERON_DPLAY_DIRECTORY_PENDING, 0);
	strcpy(g_directory.join.token, token);
	return AERON_DPLAY_DIRECTORY_ERROR_NONE;
}

void AeronDplayDirectory_GetJoinStatus(AeronDplayJoinStatus* out) {
	if (out)
		*out = g_directory.join.status;
}

void AeronDplayDirectory_FinishJoin(void) { StopJoin(AERON_DPLAY_DIRECTORY_IDLE, 0); }

void AeronDplayDirectory_CancelJoin(void) { StopJoin(AERON_DPLAY_DIRECTORY_CANCELLED, 0); }

void DpDirectory_GetHostSignals(DpDirectoryHostSignals* out) {
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->status = g_directory.host.polling;
	for (unsigned i = 0; i < 8; ++i)
		if (g_directory.host.entries[i].used)
			out->connections[out->count++] = g_directory.host.entries[i].signal;
}

int DpDirectory_GetJoinSignal(DpDirectorySignal* out) {
	if (!out || !g_directory.join.active || !g_directory.join.have_signal)
		return 0;
	*out = g_directory.join.signal;
	return 1;
}

int DpDirectory_GetJoinRoom(AeronDplayDirectoryRoom* out) {
	if (!out || !g_directory.join.active)
		return 0;
	*out = g_directory.join.room;
	return 1;
}

AeronDplayDirectoryError DpDirectory_SetDescription(const AeronDplayConnectionIdentity* identity,
													uint64_t generation, const char* sdp,
													AeronDplayDirectoryError rejection) {
	if (!identity || !sdp || !DpDirectory_Text(sdp, DP_DIRECTORY_SDP, 1) ||
		(rejection ? sdp[0] != 0 : sdp[0] == 0) ||
		(rejection && rejection != AERON_DPLAY_DIRECTORY_ERROR_FULL &&
		 rejection != AERON_DPLAY_DIRECTORY_ERROR_NOT_JOINABLE &&
		 rejection != AERON_DPLAY_DIRECTORY_ERROR_CONNECTION_FAILED))
		return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	DpDirectorySignal* signal = NULL;
	unsigned           index  = DIR_JOIN_OFFER;
	if (g_directory.join.active && g_directory.join.have_signal &&
		generation == g_directory.join.generation &&
		SameIdentity(identity, &g_directory.join.status.identity)) {
		if (rejection)
			return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
		signal = &g_directory.join.signal;
	} else {
		for (unsigned i = 0; i < 8; ++i) {
			HostEntry* entry = &g_directory.host.entries[i];
			if (entry->used && entry->signal.generation == generation &&
				SameIdentity(identity, &entry->signal.identity)) {
				signal = &entry->signal;
				index  = DIR_ANSWER_FIRST + i;
				break;
			}
		}
	}
	if (!signal)
		return AERON_DPLAY_DIRECTORY_ERROR_CLOSED;
	if (Aeron_NowUs() >= signal->deadline_us)
		return AERON_DPLAY_DIRECTORY_ERROR_TIMEOUT;
	if (index != DIR_JOIN_OFFER && !rejection && !signal->offer[0])
		return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	char* json = DpDirectory_DescriptionJSON(sdp, rejection);
	if (!json)
		return AERON_DPLAY_DIRECTORY_ERROR_NO_MEMORY;
	DirectoryOperation* op = &g_directory.operations[index];
	if (op->request.body[0]) {
		int same = !strcmp(json, op->request.body);
		cJSON_free(json);
		return same ? AERON_DPLAY_DIRECTORY_ERROR_NONE : AERON_DPLAY_DIRECTORY_ERROR_CONFLICT;
	}
	AeronDplayDirectoryError error = QueueOperation(
		index, DP_HTTP_PUT, &identity->room_id, &identity->connection_id,
		index == DIR_JOIN_OFFER ? "/offer" : "/answer",
		index == DIR_JOIN_OFFER ? g_directory.join.token : g_directory.host.token, json, signal->deadline_us);
	if (!error)
		signal->publication = Status(AERON_DPLAY_DIRECTORY_PENDING, 0);
	return error;
}

void DpDirectory_JoinReady(uint64_t generation, AeronDplayDirectoryError error) {
	if (!g_directory.join.active || !g_directory.join.have_signal ||
		generation != g_directory.join.generation)
		return;
	if (error) {
		StopJoin(AERON_DPLAY_DIRECTORY_FAILED, error);
		return;
	}
	if (Aeron_NowUs() >= g_directory.join.status.deadline_us) {
		StopJoin(AERON_DPLAY_DIRECTORY_FAILED, AERON_DPLAY_DIRECTORY_ERROR_TIMEOUT);
		return;
	}
	if (g_directory.join.signal.state == DP_SIGNAL_ANSWERED)
		g_directory.join.status.preparation = Status(AERON_DPLAY_DIRECTORY_SUCCEEDED, 0);
}

static void InstallHostSignals(void) {
	DpDirectoryHostSignals* snapshot = &g_directory.host.next_signals;
	for (unsigned i = 0; i < 8; ++i) {
		HostEntry* entry = &g_directory.host.entries[i];
		int        found = 0;
		for (unsigned j = 0; entry->used && j < snapshot->count; ++j)
			if (SameGuid(&entry->signal.identity.connection_id,
						 &snapshot->connections[j].identity.connection_id) &&
				!strcmp(entry->signal.ice.username, snapshot->connections[j].ice.username))
				found = 1;
		if (!found) {
			CancelOperation(DIR_ANSWER_FIRST + i);
			memset(entry, 0, sizeof(*entry));
		}
	}
	for (unsigned j = 0; j < snapshot->count; ++j) {
		DpDirectorySignal* fresh = &snapshot->connections[j];
		HostEntry*         entry = NULL;
		for (unsigned i = 0; i < 8; ++i)
			if (g_directory.host.entries[i].used &&
				SameGuid(&g_directory.host.entries[i].signal.identity.connection_id,
						 &fresh->identity.connection_id))
				entry = &g_directory.host.entries[i];
		if (!entry) {
			for (unsigned i = 0; i < 8; ++i)
				if (!g_directory.host.entries[i].used) {
					entry = &g_directory.host.entries[i];
					break;
				}
			entry->used               = 1;
			entry->signal.generation  = ++g_generation;
			entry->signal.deadline_us = Aeron_NowUs() + 90000000;
		}
		fresh->generation              = entry->signal.generation;
		fresh->deadline_us             = entry->signal.deadline_us;
		fresh->publication             = entry->signal.publication;
		fresh->identity.application_id = g_directory.config.application_id;
		fresh->identity.room_id        = g_directory.host.room;
		entry->signal                  = *fresh;
	}
}

static int Success(unsigned index, const cJSON* json, const DpHttpResult* result) {
	if (index >= DIR_ANSWER_FIRST)
		return result->status == 204 && result->size == 0;
	if (result->status != 200 && !(result->status == 201 && (index == DIR_ROOM || index == DIR_JOIN_CREATE)))
		return 0;
	if (index == DIR_LIST) {
		AeronDplayDirectorySnapshot* next = &g_directory.next_cache;
		memset(next, 0, sizeof(*next));
		if (!DpDirectory_ReadRooms(json, next))
			return 0;
		next->application_id = g_directory.config.application_id;
		unsigned pages       = (next->room_count + 31) / 32;
		next->page_index     = g_directory.cache.page_index;
		if (next->page_index >= pages)
			next->page_index = pages ? pages - 1 : 0;
		next->available   = 1;
		next->refresh     = Status(AERON_DPLAY_DIRECTORY_SUCCEEDED, 0);
		g_directory.cache = *next;
	} else if (index == DIR_ROOM) {
		AeronDplayDirectoryRoom room;
		if (!DpDirectory_ReadRoom(json, &room) || !SameGuid(&room.room_id, &g_directory.host.room) ||
			room.revision != g_directory.host.sent_revision || room.protocol != 2 ||
			strcmp(room.game_version, g_directory.config.game_version) ||
			memcmp(&room.metadata, &g_directory.host.sent, sizeof(room.metadata)))
			return 0;
		g_directory.host.published = 1;
		g_directory.host.renew_at = room.revision == g_directory.host.revision ? Aeron_NowUs() + 15000000 : 0;
		g_directory.host.status =
			Status(room.revision == g_directory.host.revision ? AERON_DPLAY_DIRECTORY_SUCCEEDED
															  : AERON_DPLAY_DIRECTORY_PENDING,
				   0);
	} else if (index == DIR_HOST_POLL) {
		char cursor[DP_DIRECTORY_CURSOR];
		if (!DpDirectory_ReadHostSignals(json, &g_directory.host.next_signals, cursor))
			return 0;
		strcpy(g_directory.host.cursor, cursor);
		InstallHostSignals();
		g_directory.host.polling = Status(AERON_DPLAY_DIRECTORY_SUCCEEDED, 0);
	} else {
		DpDirectorySignal signal;
		if (!DpDirectory_ReadSignal(json, 0, &signal) ||
			!SameGuid(&signal.identity.connection_id, &g_directory.join.status.identity.connection_id))
			return 0;
		if (g_directory.join.have_signal &&
			(memcmp(&signal.ice, &g_directory.join.signal.ice, sizeof(signal.ice)) ||
			 signal.expires_at_unix != g_directory.join.signal.expires_at_unix))
			return 0;
		signal.identity              = g_directory.join.status.identity;
		signal.generation            = g_directory.join.generation;
		signal.deadline_us           = g_directory.join.status.deadline_us;
		signal.publication           = g_directory.join.signal.publication;
		g_directory.join.signal      = signal;
		g_directory.join.have_signal = 1;
		if (signal.rejection)
			StopJoin(AERON_DPLAY_DIRECTORY_FAILED, signal.rejection);
		else if (index == DIR_JOIN_CREATE || index == DIR_JOIN_OFFER)
			g_directory.join.status.preparation.error = AERON_DPLAY_DIRECTORY_ERROR_NONE;
	}
	return 1;
}

static void OperationStatus(unsigned index) {
	DirectoryOperation* op = &g_directory.operations[index];
	if (index == DIR_LIST) {
		g_directory.cache.refresh = op->status;
		if (op->status.error)
			g_directory.cache.available = 0;
	} else if (index == DIR_ROOM) {
		if (op->status.state != AERON_DPLAY_DIRECTORY_SUCCEEDED)
			g_directory.host.status = op->status;
	} else if (index == DIR_HOST_POLL)
		g_directory.host.polling = op->status;
	else if (index >= DIR_ANSWER_FIRST && index < DIR_DELETE_FIRST)
		g_directory.host.entries[index - DIR_ANSWER_FIRST].signal.publication = op->status;
	else if (index >= DIR_JOIN_CREATE && index <= DIR_JOIN_POLL && g_directory.join.active) {
		if (index == DIR_JOIN_OFFER)
			g_directory.join.signal.publication = op->status;
		if (op->status.state == AERON_DPLAY_DIRECTORY_FAILED)
			StopJoin(AERON_DPLAY_DIRECTORY_FAILED, op->status.error);
		else
			g_directory.join.status.preparation.error = op->status.error;
	}
}

static void Completed(unsigned index, DpHttpResult* result) {
	DirectoryOperation* op         = &g_directory.operations[index];
	op->id                         = 0;
	cJSON*                   json  = DpDirectory_ParseJSON(result->body, result->size);
	AeronDplayDirectoryError error = result->error;
	if (!error && result->status >= 200 && result->status < 300) {
		if (!Success(index, json, result))
			error = AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
	} else if (!error) {
		error = DpDirectory_ReadError(json);
		if (error == AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST && result->status >= 500)
			error = AERON_DPLAY_DIRECTORY_ERROR_UNAVAILABLE;
		else if (error == AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST && result->status == 429)
			error = AERON_DPLAY_DIRECTORY_ERROR_RATE_LIMITED;
	}
	cJSON_Delete(json);
	/* A terminal rejection may have cancelled/reset this operation in Success. */
	if (index >= DIR_JOIN_CREATE && index <= DIR_JOIN_POLL && !g_directory.join.active)
		return;
	if (!error) {
		op->wanted = 0;
		op->status = Status(AERON_DPLAY_DIRECTORY_SUCCEEDED, 0);
	} else if (index == DIR_HOST_POLL && result->status == 404) {
		op->wanted                 = 0;
		g_directory.host.published = 0;
		g_directory.host.renew_at  = 0;
		g_directory.host.cursor[0] = 0;
		op->status = Status(AERON_DPLAY_DIRECTORY_PENDING, AERON_DPLAY_DIRECTORY_ERROR_NOT_FOUND);
		g_directory.host.status = op->status;
		for (unsigned i = 0; i < 8; ++i) {
			CancelOperation(DIR_ANSWER_FIRST + i);
			memset(&g_directory.host.entries[i], 0, sizeof(g_directory.host.entries[i]));
		}
	} else {
		int retry  = error == AERON_DPLAY_DIRECTORY_ERROR_NETWORK ||
					 error == AERON_DPLAY_DIRECTORY_ERROR_TIMEOUT || result->status == 429 ||
					 result->status >= 500;
		op->wanted = retry;
		op->status = Status(retry ? AERON_DPLAY_DIRECTORY_PENDING : AERON_DPLAY_DIRECTORY_FAILED, error);
		if (retry) {
			unsigned seconds = op->attempts < 4 ? 1u << op->attempts : 15;
			if (op->attempts < 4)
				++op->attempts;
			uint32_t random = 200;
			Aeron_RandomBytes(&random, sizeof(random));
			uint64_t delay     = (uint64_t)seconds * 1000000 * (800 + random % 401) / 1000;
			uint64_t requested = (uint64_t)result->retry_after * 1000000;
			if (requested > delay)
				delay = requested;
			op->due = Aeron_NowUs() + delay;
		}
		if (index < DIR_DELETE_FIRST)
			Aeron_LogWarn("compat.dplay.directory", "operation %u failed (HTTP %ld, error %u)", index,
						  result->status, (unsigned)error);
	}
	OperationStatus(index);
}

static void Schedule(void) {
	uint64_t now = Aeron_NowUs();
	if (g_directory.join.active && now >= g_directory.join.status.deadline_us)
		StopJoin(AERON_DPLAY_DIRECTORY_FAILED, AERON_DPLAY_DIRECTORY_ERROR_TIMEOUT);
	if (g_directory.host.active) {
		if (!g_directory.operations[DIR_ROOM].wanted && now >= g_directory.host.renew_at &&
			g_directory.host.status.state != AERON_DPLAY_DIRECTORY_FAILED)
			PublishRoom();
		if (g_directory.host.published && !g_directory.operations[DIR_HOST_POLL].wanted &&
			g_directory.host.polling.state != AERON_DPLAY_DIRECTORY_FAILED)
			PollHost();
	}
	if (g_directory.join.active && g_directory.join.have_signal &&
		g_directory.join.signal.state < DP_SIGNAL_ANSWERED &&
		g_directory.operations[DIR_JOIN_OFFER].status.state == AERON_DPLAY_DIRECTORY_SUCCEEDED &&
		!g_directory.operations[DIR_JOIN_POLL].wanted) {
		const AeronDplayConnectionIdentity* id = &g_directory.join.status.identity;
		QueueOperation(DIR_JOIN_POLL, DP_HTTP_GET, &id->room_id, &id->connection_id, "",
					   g_directory.join.token, NULL, g_directory.join.status.deadline_us);
	}
	for (unsigned i = 0; i < DIR_OPERATIONS; ++i) {
		DirectoryOperation* op = &g_directory.operations[i];
		if (!op->wanted)
			continue;
		if (op->deadline && now >= op->deadline) {
			DpHttp_Cancel(g_directory.http, op->id);
			op->id     = 0;
			op->wanted = 0;
			op->status = Status(AERON_DPLAY_DIRECTORY_FAILED, AERON_DPLAY_DIRECTORY_ERROR_TIMEOUT);
			OperationStatus(i);
			continue;
		}
		if (!op->id && now >= op->due) {
			DpHttpRequest request = op->request;
			if (op->deadline) {
				uint64_t remaining = (op->deadline - now + 999) / 1000;
				if (remaining < request.timeout_ms)
					request.timeout_ms = (unsigned)remaining;
			}
			op->id = DpHttp_Submit(g_directory.http, &request);
			if (!op->id)
				op->due = now + DP_WAKE_DELAY_US;
		}
	}
}

void DpDirectory_Update(void) {
	if (!g_directory.http)
		return;
	DpHttpResult result;
	/* At most one bounded queue of completions is applied per application tick. */
	for (unsigned n = 0; n < DP_HTTP_SLOTS && DpHttp_Take(g_directory.http, &result); ++n) {
		for (unsigned i = 0; i < DIR_OPERATIONS; ++i)
			if (g_directory.operations[i].id == result.id) {
				Completed(i, &result);
				break;
			}
		free(result.body);
	}
	Schedule();
}

uint64_t DpDirectory_NextWakeDelayUs(void) {
	if (!g_directory.http)
		return UINT64_MAX;
	uint64_t now = Aeron_NowUs(), next = UINT64_MAX;
	if (DpHttp_Pending(g_directory.http))
		next = now + DP_WAKE_DELAY_US;
	if (g_directory.join.active && g_directory.join.status.deadline_us < next)
		next = g_directory.join.status.deadline_us;
	if (g_directory.host.active && !g_directory.operations[DIR_ROOM].wanted &&
		g_directory.host.status.state != AERON_DPLAY_DIRECTORY_FAILED && g_directory.host.renew_at < next)
		next = g_directory.host.renew_at;
	for (unsigned i = 0; i < DIR_OPERATIONS; ++i) {
		DirectoryOperation* op = &g_directory.operations[i];
		if (!op->wanted)
			continue;
		if (!op->id && op->due < next)
			next = op->due;
		if (op->deadline && op->deadline < next)
			next = op->deadline;
	}
	return next == UINT64_MAX ? next : next > now ? next - now : 0;
}

void DpDirectory_Shutdown(void) {
	if (!g_directory.http)
		return;
	AeronDplayDirectory_StopHosting();
	AeronDplayDirectory_CancelJoin();
	CancelOperation(DIR_LIST);
	DpDirectory_Update();
	DpHttp_Destroy(g_directory.http);
	memset(&g_directory, 0, sizeof(g_directory));
}
