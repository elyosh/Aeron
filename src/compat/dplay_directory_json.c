#include "dplay_directory_json.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void DpDirectory_GuidText(const GUID* id, char text[37]) {
	snprintf(text, 37, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned)id->Data1,
			 (unsigned)id->Data2, (unsigned)id->Data3, id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
			 id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7]);
}

int DpDirectory_ParseGuid(const char* text, GUID* id) {
	uint8_t bytes[16] = { 0 };
	if (!text || strlen(text) != 36)
		return 0;
	unsigned n = 0;
	for (unsigned i = 0; i < 36; ++i) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (text[i] != '-')
				return 0;
			continue;
		}
		unsigned c = (unsigned char)text[i];
		if (c >= '0' && c <= '9')
			c -= '0';
		else if (c >= 'a' && c <= 'f')
			c = c - 'a' + 10;
		else
			return 0;
		bytes[n / 2] = (uint8_t)(bytes[n / 2] * 16 + c);
		++n;
	}
	id->Data1 = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
	id->Data2 = (uint16_t)(bytes[4] << 8 | bytes[5]);
	id->Data3 = (uint16_t)(bytes[6] << 8 | bytes[7]);
	memcpy(id->Data4, bytes + 8, 8);
	return 1;
}

/* Reject malformed UTF-8 and, for display text, Unicode C0/C1 controls. */
int DpDirectory_Text(const char* text, size_t capacity, int controls) {
	if (!text || !capacity)
		return 0;
	size_t n = 0;
	while (n < capacity && text[n])
		++n;
	if (n == capacity)
		return 0;
	for (size_t i = 0; i < n;) {
		uint32_t c = (unsigned char)text[i++], minimum = 0;
		unsigned continuation = 0;
		if (c >= 0xc2 && c <= 0xdf) {
			c &= 31;
			continuation = 1;
			minimum      = 0x80;
		} else if (c >= 0xe0 && c <= 0xef) {
			c &= 15;
			continuation = 2;
			minimum      = 0x800;
		} else if (c >= 0xf0 && c <= 0xf4) {
			c &= 7;
			continuation = 3;
			minimum      = 0x10000;
		} else if (c >= 0x80)
			return 0;
		if (continuation > n - i)
			return 0;
		while (continuation--) {
			unsigned next = (unsigned char)text[i++];
			if ((next & 0xc0) != 0x80)
				return 0;
			c = (c << 6) | (next & 63);
		}
		if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
			return 0;
		if (!controls && (c < 32 || (c >= 127 && c <= 159)))
			return 0;
	}
	return 1;
}

int DpDirectory_Version(const char* text) {
	if (!DpDirectory_Text(text, AERON_DPLAY_DIRECTORY_VERSION_CAPACITY, 0) || !*text)
		return 0;
	for (; *text; ++text)
		if (!(*text >= 'a' && *text <= 'z') && !(*text >= 'A' && *text <= 'Z') &&
			!(*text >= '0' && *text <= '9') && !strchr("._-", *text))
			return 0;
	return 1;
}

int DpDirectory_Metadata(const AeronDplayRoomMetadata* in, AeronDplayRoomMetadata* out) {
	if (!in || !DpDirectory_Text(in->name, sizeof(in->name), 0) || !in->name[0] || in->players < 1 ||
		in->players > in->max_players || in->max_players > 8 || in->password_required > 1 ||
		in->joinable > 1 || (in->state != AERON_DPLAY_ROOM_LOBBY && in->state != AERON_DPLAY_ROOM_FLIGHT) ||
		in->mission.present > 1)
		return 0;
	memset(out, 0, sizeof(*out));
	strcpy(out->name, in->name);
	out->players           = in->players;
	out->max_players       = in->max_players;
	out->password_required = in->password_required;
	out->joinable          = in->joinable;
	out->state             = in->state;
	if (in->mission.present) {
		if (in->mission.directory > 5 || in->mission.id < 0 ||
			!DpDirectory_Text(in->mission.name, sizeof(in->mission.name), 0))
			return 0;
		out->mission.present   = 1;
		out->mission.directory = in->mission.directory;
		out->mission.id        = in->mission.id;
		strcpy(out->mission.name, in->mission.name);
	}
	for (unsigned i = 0; i < in->players; ++i) {
		if (!DpDirectory_Text(in->roster[i].name, sizeof(in->roster[i].name), 0) || !in->roster[i].name[0])
			return 0;
		strcpy(out->roster[i].name, in->roster[i].name);
		out->roster[i].rating = in->roster[i].rating;
	}
	return 1;
}

cJSON* DpDirectory_ParseJSON(const char* data, size_t size) {
	if (!data || memchr(data, 0, size) || !DpDirectory_Text(data, size + 1, 1))
		return NULL;
	/* cJSON strings are NUL-terminated: reject embedded NUL escapes before they
	 * can truncate a credential, identifier or display field during conversion. */
	for (size_t i = 0; i + 1 < size; ++i) {
		if (data[i] != '\\')
			continue;
		++i;
		if (size - i >= 5 && !memcmp(data + i, "u0000", 5))
			return NULL;
	}
	return cJSON_ParseWithLengthOpts(data, size + 1, NULL, 1);
}

static const cJSON* Field(const cJSON* object, const char* name) {
	const cJSON* found = NULL;
	if (!cJSON_IsObject(object))
		return NULL;
	for (const cJSON* p = object->child; p; p = p->next)
		if (!strcmp(p->string, name)) {
			if (found)
				return NULL;
			found = p;
		}
	return found;
}

static int Fields(const cJSON* object, int count) {
	return cJSON_IsObject(object) && cJSON_GetArraySize(object) == count;
}

static int Number(const cJSON* value, int minimum, int maximum, int* out) {
	if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < minimum ||
		value->valuedouble > maximum || floor(value->valuedouble) != value->valuedouble)
		return 0;
	*out = (int)value->valuedouble;
	return 1;
}

static int Text(const cJSON* value, char* out, size_t capacity, int controls, int empty) {
	if (!cJSON_IsString(value) || !DpDirectory_Text(value->valuestring, capacity, controls) ||
		(!empty && !value->valuestring[0]))
		return 0;
	strcpy(out, value->valuestring);
	return 1;
}

static int Guid(const cJSON* value, GUID* out) {
	return cJSON_IsString(value) && DpDirectory_ParseGuid(value->valuestring, out);
}

static int Timestamp(const cJSON* value, int64_t* out) {
	if (!cJSON_IsString(value) || strlen(value->valuestring) != 20)
		return 0;
	const char* s       = value->valuestring;
	const char* pattern = "0000-00-00T00:00:00Z";
	for (unsigned i = 0; i < 20; ++i)
		if (pattern[i] == '0' ? s[i] < '0' || s[i] > '9' : s[i] != pattern[i])
			return 0;
	int y, m, d, h, min, sec;
	if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &m, &d, &h, &min, &sec) != 6 || y < 1970 || m < 1 ||
		m > 12 || h > 23 || min > 59 || sec > 59)
		return 0;
	const int month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int       leap    = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
	if (d < 1 || d > month[m - 1] + (m == 2 && leap))
		return 0;
	int64_t days =
		365LL * (y - 1970) + (y - 1) / 4 - 1969 / 4 - (y - 1) / 100 + 1969 / 100 + (y - 1) / 400 - 1969 / 400;
	for (int i = 1; i < m; ++i)
		days += month[i - 1] + (i == 2 && leap);
	*out = ((days + d - 1) * 24 + h) * 3600 + min * 60 + sec;
	return 1;
}

int DpDirectory_ReadRoom(const cJSON* json, AeronDplayDirectoryRoom* room) {
	AeronDplayRoomMetadata metadata = { 0 };
	int                    revision, protocol, players, maximum, n;
	const cJSON *          mission = Field(json, "mission"), *roster = Field(json, "roster");
	const cJSON *          password = Field(json, "password_required"), *joinable = Field(json, "joinable"),
				*state = Field(json, "state");
	memset(room, 0, sizeof(*room));
	if (!Fields(json, 13) || !Guid(Field(json, "room_id"), &room->room_id) ||
		!Timestamp(Field(json, "expires_at"), &room->expires_at_unix) ||
		!Number(Field(json, "revision"), 1, INT32_MAX, &revision) ||
		!Number(Field(json, "protocol"), 1, INT32_MAX, &protocol) ||
		!Text(Field(json, "game_version"), room->game_version, sizeof(room->game_version), 0, 0) ||
		!DpDirectory_Version(room->game_version) ||
		!Text(Field(json, "name"), metadata.name, sizeof(metadata.name), 0, 0) ||
		!Number(Field(json, "players"), 1, 8, &players) ||
		!Number(Field(json, "max_players"), 1, 8, &maximum) || !cJSON_IsBool(password) ||
		!cJSON_IsBool(joinable) || !cJSON_IsString(state) || !mission || !cJSON_IsArray(roster))
		return 0;
	room->revision             = (uint32_t)revision;
	room->protocol             = (uint32_t)protocol;
	metadata.players           = (uint8_t)players;
	metadata.max_players       = (uint8_t)maximum;
	metadata.password_required = (uint8_t)cJSON_IsTrue(password);
	metadata.joinable          = (uint8_t)cJSON_IsTrue(joinable);
	if (!strcmp(state->valuestring, "flight"))
		metadata.state = AERON_DPLAY_ROOM_FLIGHT;
	else if (strcmp(state->valuestring, "lobby"))
		return 0;
	if (!cJSON_IsNull(mission)) {
		const cJSON* name = Field(mission, "name");
		if (!Fields(mission, 3) || !Number(Field(mission, "directory"), 0, 5, &n))
			return 0;
		metadata.mission.directory = (uint8_t)n;
		if (!Number(Field(mission, "id"), 0, INT32_MAX, &n) || !name)
			return 0;
		metadata.mission.id      = n;
		metadata.mission.present = 1;
		if (!cJSON_IsNull(name) && !Text(name, metadata.mission.name, sizeof(metadata.mission.name), 0, 0))
			return 0;
	}
	if (cJSON_GetArraySize(roster) != players)
		return 0;
	unsigned i = 0;
	for (const cJSON* p = roster->child; p; p = p->next, ++i) {
		if (!Fields(p, 2) ||
			!Text(Field(p, "name"), metadata.roster[i].name, sizeof(metadata.roster[i].name), 0, 0) ||
			!Number(Field(p, "rating"), 0, 255, &n))
			return 0;
		metadata.roster[i].rating = (uint8_t)n;
	}
	return DpDirectory_Metadata(&metadata, &room->metadata);
}

static int GuidOrder(const GUID* a, const GUID* b) {
	char left[37], right[37];
	DpDirectory_GuidText(a, left);
	DpDirectory_GuidText(b, right);
	return strcmp(left, right);
}

int DpDirectory_ReadRooms(const cJSON* json, AeronDplayDirectorySnapshot* snapshot) {
	const cJSON* rooms = Field(json, "rooms");
	if (!Fields(json, 1) || !cJSON_IsArray(rooms) ||
		cJSON_GetArraySize(rooms) > AERON_DPLAY_DIRECTORY_MAX_ROOMS)
		return 0;
	snapshot->room_count = 0;
	for (const cJSON* p = rooms->child; p; p = p->next) {
		unsigned i = snapshot->room_count++;
		if (!DpDirectory_ReadRoom(p, &snapshot->rooms[i]) ||
			(i && GuidOrder(&snapshot->rooms[i - 1].room_id, &snapshot->rooms[i].room_id) >= 0))
			return 0;
	}
	return 1;
}

static int Endpoint(const cJSON* json, DpDirectoryEndpoint* out) {
	int port;
	if (!Text(Field(json, "host"), out->host, sizeof(out->host), 0, 0) ||
		!Number(Field(json, "port"), 1, 65535, &port))
		return 0;
	/* Only DNS names are part of IceConfig, never URLs or embedded credentials. */
	for (const char* p = out->host; *p; ++p)
		if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') && !(*p >= '0' && *p <= '9') &&
			*p != '-' && *p != '.')
			return 0;
	out->port = (uint16_t)port;
	return 1;
}

static AeronDplayDirectoryError Rejection(const char* name) {
	if (!strcmp(name, "full"))
		return AERON_DPLAY_DIRECTORY_ERROR_FULL;
	if (!strcmp(name, "not_joinable"))
		return AERON_DPLAY_DIRECTORY_ERROR_NOT_JOINABLE;
	if (!strcmp(name, "connection_failed"))
		return AERON_DPLAY_DIRECTORY_ERROR_CONNECTION_FAILED;
	return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
}

int DpDirectory_ReadSignal(const cJSON* json, int host, DpDirectorySignal* signal) {
	const cJSON *ice = Field(json, "ice"), *stun = Field(ice, "stun"), *turn = Field(ice, "turn");
	const cJSON *state = Field(json, "state"), *answer = Field(json, "answer"), *offer = Field(json, "offer");
	memset(signal, 0, sizeof(*signal));
	if (!Fields(json, host ? 6 : 5) || !Guid(Field(json, "connection_id"), &signal->identity.connection_id) ||
		!Timestamp(Field(json, "expires_at"), &signal->expires_at_unix) || !Fields(ice, 2) ||
		!Fields(stun, 2) || !Fields(turn, 5) || !Endpoint(stun, &signal->ice.stun) ||
		!Endpoint(turn, &signal->ice.turn) ||
		!Text(Field(turn, "username"), signal->ice.username, sizeof(signal->ice.username), 0, 0) ||
		!Text(Field(turn, "password"), signal->ice.password, sizeof(signal->ice.password), 0, 0) ||
		!Timestamp(Field(turn, "expires_at"), &signal->ice.expires_at_unix) || !cJSON_IsString(state) ||
		!answer)
		return 0;
	if (!strcmp(state->valuestring, "waiting_offer"))
		signal->state = DP_SIGNAL_WAITING_OFFER;
	else if (!strcmp(state->valuestring, "waiting_answer"))
		signal->state = DP_SIGNAL_WAITING_ANSWER;
	else if (!strcmp(state->valuestring, "answered"))
		signal->state = DP_SIGNAL_ANSWERED;
	else if (!strcmp(state->valuestring, "rejected"))
		signal->state = DP_SIGNAL_REJECTED;
	else
		return 0;
	if (!cJSON_IsNull(answer)) {
		const cJSON* rejection = Field(answer, "error");
		if (!Fields(answer, 1))
			return 0;
		if (cJSON_IsString(rejection)) {
			signal->rejection = Rejection(rejection->valuestring);
			if (signal->rejection == AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST)
				return 0;
		} else if (!Text(Field(answer, "sdp"), signal->answer, sizeof(signal->answer), 1, 0))
			return 0;
	}
	if ((signal->state == DP_SIGNAL_ANSWERED) != (signal->answer[0] != 0) ||
		(signal->state == DP_SIGNAL_REJECTED) != (signal->rejection != 0))
		return 0;
	if (host) {
		if (!offer || (!cJSON_IsNull(offer) && (!Fields(offer, 1) || !Text(Field(offer, "sdp"), signal->offer,
																		   sizeof(signal->offer), 1, 0))))
			return 0;
		if ((signal->state == DP_SIGNAL_WAITING_OFFER && signal->offer[0]) ||
			((signal->state == DP_SIGNAL_WAITING_ANSWER || signal->state == DP_SIGNAL_ANSWERED) &&
			 !signal->offer[0]))
			return 0;
	}
	return 1;
}

int DpDirectory_ReadHostSignals(const cJSON* json, DpDirectoryHostSignals* signals,
								char cursor[DP_DIRECTORY_CURSOR]) {
	const cJSON* connections = Field(json, "connections");
	if (!Fields(json, 2) || !Text(Field(json, "cursor"), cursor, DP_DIRECTORY_CURSOR, 0, 0) ||
		!cJSON_IsArray(connections) || cJSON_GetArraySize(connections) > 8)
		return 0;
	for (const char* p = cursor; *p; ++p)
		if ((unsigned char)*p > 126)
			return 0;
	signals->count = 0;
	for (const cJSON* p = connections->child; p; p = p->next) {
		unsigned i = signals->count++;
		if (!DpDirectory_ReadSignal(p, 1, &signals->connections[i]) ||
			(i && GuidOrder(&signals->connections[i - 1].identity.connection_id,
							&signals->connections[i].identity.connection_id) >= 0))
			return 0;
	}
	return 1;
}

AeronDplayDirectoryError DpDirectory_ReadError(const cJSON* json) {
	const cJSON* error   = Field(json, "error");
	const char*  names[] = { "",
							 "invalid_request",
							 "unauthorized",
							 "not_found",
							 "conflict",
							 "incompatible",
							 "full",
							 "not_joinable",
							 "closed",
							 "body_too_large",
							 "unsupported_media_type",
							 "rate_limited",
							 "capacity",
							 "unavailable",
							 "method_not_allowed",
							 "connection_failed" };
	if (Fields(json, 1) && cJSON_IsString(error))
		for (unsigned i = 1; i < sizeof(names) / sizeof(names[0]); ++i)
			if (!strcmp(error->valuestring, names[i]))
				return (AeronDplayDirectoryError)i;
	return AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST;
}

char* DpDirectory_RoomJSON(const AeronDplayRoomMetadata* m, uint32_t revision, const char* version) {
	cJSON *root = cJSON_CreateObject(), *mission = NULL, *roster = NULL;
	char*  result = NULL;
	if (!root || !cJSON_AddNumberToObject(root, "revision", revision) ||
		!cJSON_AddNumberToObject(root, "protocol", 2) ||
		!cJSON_AddStringToObject(root, "game_version", version) ||
		!cJSON_AddStringToObject(root, "name", m->name) ||
		!cJSON_AddNumberToObject(root, "players", m->players) ||
		!cJSON_AddNumberToObject(root, "max_players", m->max_players) ||
		!cJSON_AddBoolToObject(root, "password_required", m->password_required) ||
		!cJSON_AddBoolToObject(root, "joinable", m->joinable) ||
		!cJSON_AddStringToObject(root, "state", m->state == AERON_DPLAY_ROOM_FLIGHT ? "flight" : "lobby"))
		goto done;
	if (m->mission.present) {
		mission = cJSON_AddObjectToObject(root, "mission");
		if (!mission || !cJSON_AddNumberToObject(mission, "directory", m->mission.directory) ||
			!cJSON_AddNumberToObject(mission, "id", m->mission.id) ||
			!(m->mission.name[0] ? cJSON_AddStringToObject(mission, "name", m->mission.name)
								 : cJSON_AddNullToObject(mission, "name")))
			goto done;
	} else if (!cJSON_AddNullToObject(root, "mission"))
		goto done;
	roster = cJSON_AddArrayToObject(root, "roster");
	if (!roster)
		goto done;
	for (unsigned i = 0; i < m->players; ++i) {
		cJSON* player = cJSON_CreateObject();
		if (!player)
			goto done;
		if (!cJSON_AddItemToArray(roster, player)) {
			cJSON_Delete(player);
			goto done;
		}
		if (!cJSON_AddStringToObject(player, "name", m->roster[i].name) ||
			!cJSON_AddNumberToObject(player, "rating", m->roster[i].rating))
			goto done;
	}
	result = cJSON_PrintUnformatted(root);
done:
	cJSON_Delete(root);
	return result;
}

char* DpDirectory_ConnectionJSON(const char* version) {
	cJSON* root   = cJSON_CreateObject();
	char*  result = NULL;
	if (root && cJSON_AddNumberToObject(root, "protocol", 2) &&
		cJSON_AddStringToObject(root, "game_version", version))
		result = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return result;
}

char* DpDirectory_DescriptionJSON(const char* sdp, AeronDplayDirectoryError rejection) {
	const char* error  = rejection == AERON_DPLAY_DIRECTORY_ERROR_FULL           ? "full"
						 : rejection == AERON_DPLAY_DIRECTORY_ERROR_NOT_JOINABLE ? "not_joinable"
																				 : "connection_failed";
	cJSON*      root   = cJSON_CreateObject();
	char*       result = NULL;
	if (root && cJSON_AddStringToObject(root, rejection ? "error" : "sdp", rejection ? error : sdp))
		result = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return result;
}
