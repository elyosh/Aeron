#ifndef AERON_COMPAT_DPLAY_DIRECTORY_H
#define AERON_COMPAT_DPLAY_DIRECTORY_H

#include "aeron/compat/win_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	AERON_DPLAY_DIRECTORY_PROTOCOL              = 2,
	AERON_DPLAY_DIRECTORY_MAX_ROOMS             = 256,
	AERON_DPLAY_DIRECTORY_PAGE_SIZE             = 32,
	AERON_DPLAY_DIRECTORY_MAX_PLAYERS           = 8,
	AERON_DPLAY_DIRECTORY_NAME_CAPACITY         = 32,
	AERON_DPLAY_DIRECTORY_MISSION_NAME_CAPACITY = 64,
	AERON_DPLAY_DIRECTORY_VERSION_CAPACITY      = 33,
	AERON_DPLAY_DIRECTORY_URL_CAPACITY          = 512
};

/* Fixed arrays include their terminating NUL. Names are UTF-8; game_version
 * is ASCII. GUIDs use the DirectPlay field layout, not serialized UUID text.
 * Boolean fields accept 0 or 1. Initialize structures to zero before filling. */
typedef struct AeronDplayDirectoryConfig {
	/* HTTPS origin, at most 511 bytes; optional trailing slash. */
	char lobby_url[AERON_DPLAY_DIRECTORY_URL_CAPACITY];
	GUID application_id;
	char game_version[AERON_DPLAY_DIRECTORY_VERSION_CAPACITY];
} AeronDplayDirectoryConfig;

typedef struct AeronDplayDirectoryPlayer {
	char    name[AERON_DPLAY_DIRECTORY_NAME_CAPACITY];
	uint8_t rating;
} AeronDplayDirectoryPlayer;

typedef struct AeronDplayDirectoryMission {
	uint8_t present;   /* 0 encodes mission:null; remaining fields are ignored. */
	uint8_t directory; /* 0..5; interpreted by the game. */
	int32_t id;        /* 0..INT32_MAX; interpreted by the game. */
	char    name[AERON_DPLAY_DIRECTORY_MISSION_NAME_CAPACITY]; /* Empty encodes name:null. */
} AeronDplayDirectoryMission;

typedef enum AeronDplayRoomState { AERON_DPLAY_ROOM_LOBBY = 0, AERON_DPLAY_ROOM_FLIGHT } AeronDplayRoomState;

/* Game-owned advertisement fields. Aeron supplies revision, protocol and the
 * configured compatibility string. players counts admitted humans and selects
 * the first players entries of roster, in game roster order. */
typedef struct AeronDplayRoomMetadata {
	char                       name[AERON_DPLAY_DIRECTORY_NAME_CAPACITY];
	uint8_t                    players;     /* 1..max_players */
	uint8_t                    max_players; /* 1..8 */
	uint8_t                    password_required;
	uint8_t                    joinable;
	AeronDplayRoomState        state;
	AeronDplayDirectoryMission mission;
	AeronDplayDirectoryPlayer  roster[AERON_DPLAY_DIRECTORY_MAX_PLAYERS];
} AeronDplayRoomMetadata;

typedef struct AeronDplayDirectoryRoom {
	GUID                   room_id;
	uint32_t               revision;
	uint32_t               protocol;
	char                   game_version[AERON_DPLAY_DIRECTORY_VERSION_CAPACITY];
	int64_t                expires_at_unix; /* Server UTC seconds, for display; not a local deadline. */
	AeronDplayRoomMetadata metadata;
} AeronDplayDirectoryRoom;

typedef struct AeronDplayConnectionIdentity {
	GUID application_id;
	GUID room_id;
	GUID connection_id;
} AeronDplayConnectionIdentity;

typedef enum AeronDplayDirectoryError {
	AERON_DPLAY_DIRECTORY_ERROR_NONE = 0,
	AERON_DPLAY_DIRECTORY_ERROR_INVALID_REQUEST,
	AERON_DPLAY_DIRECTORY_ERROR_UNAUTHORIZED,
	AERON_DPLAY_DIRECTORY_ERROR_NOT_FOUND,
	AERON_DPLAY_DIRECTORY_ERROR_CONFLICT,
	AERON_DPLAY_DIRECTORY_ERROR_INCOMPATIBLE,
	AERON_DPLAY_DIRECTORY_ERROR_FULL,
	AERON_DPLAY_DIRECTORY_ERROR_NOT_JOINABLE,
	AERON_DPLAY_DIRECTORY_ERROR_CLOSED,
	AERON_DPLAY_DIRECTORY_ERROR_BODY_TOO_LARGE,
	AERON_DPLAY_DIRECTORY_ERROR_UNSUPPORTED_MEDIA_TYPE,
	AERON_DPLAY_DIRECTORY_ERROR_RATE_LIMITED,
	AERON_DPLAY_DIRECTORY_ERROR_CAPACITY,
	AERON_DPLAY_DIRECTORY_ERROR_UNAVAILABLE,
	AERON_DPLAY_DIRECTORY_ERROR_METHOD_NOT_ALLOWED,
	AERON_DPLAY_DIRECTORY_ERROR_CONNECTION_FAILED,
	AERON_DPLAY_DIRECTORY_ERROR_NETWORK,
	AERON_DPLAY_DIRECTORY_ERROR_TIMEOUT,
	AERON_DPLAY_DIRECTORY_ERROR_NO_MEMORY,
	AERON_DPLAY_DIRECTORY_ERROR_NOT_CONFIGURED,
	AERON_DPLAY_DIRECTORY_ERROR_BUSY
} AeronDplayDirectoryError;

typedef enum AeronDplayDirectoryOperationState {
	AERON_DPLAY_DIRECTORY_IDLE = 0,
	AERON_DPLAY_DIRECTORY_PENDING,
	AERON_DPLAY_DIRECTORY_SUCCEEDED,
	AERON_DPLAY_DIRECTORY_FAILED,
	AERON_DPLAY_DIRECTORY_CANCELLED
} AeronDplayDirectoryOperationState;

typedef struct AeronDplayDirectoryStatus {
	AeronDplayDirectoryOperationState state;
	/* Latest failure, including a transient failure while PENDING and retrying. */
	AeronDplayDirectoryError error;
} AeronDplayDirectoryStatus;

typedef struct AeronDplayDirectorySnapshot {
	GUID                      application_id;
	AeronDplayDirectoryStatus refresh;
	uint8_t                 available; /* A successful fetch exists and its latest refresh has not failed. */
	uint32_t                room_count;
	uint32_t                page_index; /* Zero-based local page; zero when room_count is zero. */
	AeronDplayDirectoryRoom rooms[AERON_DPLAY_DIRECTORY_MAX_ROOMS]; /* UUID order. */
} AeronDplayDirectorySnapshot;

typedef struct AeronDplayJoinStatus {
	AeronDplayDirectoryStatus    preparation;
	AeronDplayConnectionIdentity identity;
	/* Aeron_NowUs() deadline covering ICE, DirectPlay Open and game admission. */
	uint64_t deadline_us;
} AeronDplayJoinStatus;

/* All calls run on the application thread, alongside the DirectPlay methods.
 * Inputs are copied before return; getters copy into caller-owned storage.
 * AeronDplay_Update applies worker completions and advances directory/ICE work;
 * AeronDplay_NextWakeDelayUs and AeronDplay_Shutdown include that work's lifetime.
 * Workers never access game memory. Tokens, SDP and ICE agents are Aeron-owned.
 * Pointer arguments must be non-NULL. Error-returning calls return NONE when
 * accepted; an immediate error leaves the existing operation unchanged. */

/* Configure while idle, before browsing or creating a session. Reconfiguration
 * clears the directory cache. The version is the game's own admission version;
 * the transport uses AERON_DPLAY_DIRECTORY_PROTOCOL. */
AeronDplayDirectoryError AeronDplayDirectory_Configure(const AeronDplayDirectoryConfig* config);

/* Starts a full-directory refresh; repeating while pending coalesces the request.
 * The caller schedules refreshes while the browser is visible. A successful
 * reply atomically replaces rooms and clamps page_index. */
AeronDplayDirectoryError AeronDplayDirectory_Refresh(void);
void                     AeronDplayDirectory_GetSnapshot(AeronDplayDirectorySnapshot* snapshot);

/* Selects the local slice exposed by DirectPlay EnumSessions. An out-of-range
 * page returns INVALID_REQUEST; page zero is valid for an empty directory.
 * The game retains selection by UUID and selects its page after a refresh. */
AeronDplayDirectoryError AeronDplayDirectory_SelectPage(uint32_t page_index);

/* Register the UUID of the live local DirectPlay session. Starts advertisement
 * renewal and host signaling reads. Repeating with the same UUID/metadata retries
 * failed registration. Host and join preparation are mutually exclusive. */
AeronDplayDirectoryError AeronDplayDirectory_StartHosting(const GUID*                   room_id,
														  const AeronDplayRoomMetadata* metadata);

/* Replaces the desired metadata snapshot. Aeron assigns revisions, serializes
 * writes and coalesces pending changes. Status covers publication of the latest
 * snapshot; periodic renewal and signaling continue while hosting is active. */
AeronDplayDirectoryError AeronDplayDirectory_UpdateHost(const AeronDplayRoomMetadata* metadata);
void                     AeronDplayDirectory_GetHostStatus(AeronDplayDirectoryStatus* status);

/* Stops registration/signaling and queues best-effort room deletion. Idempotent.
 * The caller also closes the DirectPlay session when leaving the game. */
void AeronDplayDirectory_StopHosting(void);

/* Captures the selected cached room and creates one join attempt using the
 * configured application/version. The identity and deadline remain fixed across
 * retries. A pending/ready attempt returns BUSY until finished or cancelled.
 * SUCCEEDED means ICE is ready for DirectPlay Open with this room UUID; the
 * prepared link is retained through Open and the existing game admission. */
AeronDplayDirectoryError AeronDplayDirectory_BeginJoin(const GUID* room_id);
void                     AeronDplayDirectory_GetJoinStatus(AeronDplayJoinStatus* status);

/* Call after game admission or terminal rejection to delete signaling and reset
 * join status to IDLE. An admitted link belongs to DirectPlay; on rejection the
 * caller closes DirectPlay before finishing the attempt. Idempotent. */
void AeronDplayDirectory_FinishJoin(void);

/* Invalidates pending completions, closes the attempt's prepared link and queues
 * best-effort signaling deletion. Sets CANCELLED; a subsequent BeginJoin starts
 * a fresh identity/deadline. Idempotent; used before admission succeeds. */
void AeronDplayDirectory_CancelJoin(void);

#ifdef __cplusplus
}
#endif

#endif
