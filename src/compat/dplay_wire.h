#ifndef AERON_DPLAY_WIRE_H
#define AERON_DPLAY_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* ADP2 fields contain bytes, not native integers or pointers. Integer and GUID
 * fields use the original explicitly little-endian DirectPlay layout. */
typedef uint8_t DpWireU16[2];
typedef uint8_t DpWireU32[4];
typedef uint8_t DpWireGuid[16];

enum {
	DP_PROTOCOL              = 2,
	DP_MAX_DATAGRAM          = 1200,
	DP_PEERS                 = 32,
	DP_GROUPS                = 8,
	DP_PAYLOAD               = 1024,
	DP_FRAGMENT              = 1000,
	DP_SESSION_NAME_CAPACITY = 64,
	DP_SHORT_NAME_CAPACITY   = 32,
	DP_LONG_NAME_CAPACITY    = 52
};

/* Envelope message kinds. */
enum {
	DP_OPEN      = 3,
	DP_ACCEPT    = 4,
	DP_CONTROL   = 5,
	DP_RESULT    = 6,
	DP_SNAPSHOT  = 7,
	DP_ACK       = 8,
	DP_GAME      = 9,
	DP_KEEPALIVE = 10,
	DP_LEAVE     = 11
};

/* CONTROL operations; only create-player and rename-player are accepted remotely. */
enum {
	DP_CREATE_PLAYER  = 1,
	DP_DESTROY_PLAYER = 2,
	DP_RENAME_PLAYER  = 3,
	DP_CREATE_GROUP   = 4,
	DP_DESTROY_GROUP  = 5,
	DP_ADD_GROUP      = 6,
	DP_DELETE_GROUP   = 7
};

typedef struct DpWireHeader {
	uint8_t    magic[4];
	DpWireU32  version;
	DpWireGuid application, session;
	DpWireU32  sender, recipient;
	uint8_t    kind, reserved;
	DpWireU16  payload_size;
	DpWireU32  request;
} DpWireHeader;

typedef struct DpWireAccept {
	DpWireU32 result, player, revision, maximum;
} DpWireAccept;

typedef struct DpWireControl {
	DpWireU32 operation, player, reserved;
	char      short_name[DP_SHORT_NAME_CAPACITY], long_name[DP_LONG_NAME_CAPACITY];
} DpWireControl;

typedef struct DpWireResult {
	DpWireU32 result, player;
} DpWireResult;

typedef struct DpWirePeer {
	DpWireU32 player;
	uint8_t   active, groups;
	char      short_name[DP_SHORT_NAME_CAPACITY], long_name[DP_LONG_NAME_CAPACITY];
	DpWireU32 reserved;
} DpWirePeer;

typedef struct DpWireSnapshot {
	DpWireU32  maximum, host;
	DpWireU32  groups[DP_GROUPS];
	DpWirePeer peers[DP_PEERS];
} DpWireSnapshot;

typedef struct DpWireFragment {
	DpWireU32 index;
	uint8_t   data[DP_FRAGMENT];
} DpWireFragment;

typedef struct DpWireAck {
	DpWireU32 revision;
} DpWireAck;

enum {
	DP_HEADER                 = sizeof(DpWireHeader),
	DP_SNAPSHOT_SIZE          = sizeof(DpWireSnapshot),
	DP_FRAGMENT_HEADER        = offsetof(DpWireFragment, data),
	DP_FRAGMENT_COUNT         = (DP_SNAPSHOT_SIZE + DP_FRAGMENT - 1) / DP_FRAGMENT,
	DP_FRAGMENT_COMPLETE_MASK = (1u << DP_FRAGMENT_COUNT) - 1
};

/* These sizes are the published ADP2 wire contract. */
typedef char dp_wire_header_size[(sizeof(DpWireHeader) == 56) ? 1 : -1];
typedef char dp_wire_accept_size[(sizeof(DpWireAccept) == 16) ? 1 : -1];
typedef char dp_wire_control_size[(sizeof(DpWireControl) == 96) ? 1 : -1];
typedef char dp_wire_result_size[(sizeof(DpWireResult) == 8) ? 1 : -1];
typedef char dp_wire_peer_size[(sizeof(DpWirePeer) == 94) ? 1 : -1];
typedef char dp_wire_snapshot_size[(sizeof(DpWireSnapshot) == 3048) ? 1 : -1];
typedef char dp_wire_fragment_header_size[(DP_FRAGMENT_HEADER == 4) ? 1 : -1];

#endif
