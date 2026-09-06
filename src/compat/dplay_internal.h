#ifndef AERON_DPLAY_INTERNAL_H
#define AERON_DPLAY_INTERNAL_H
#include "aeron/aeron.h"
#include "aeron/compat/dplay.h"
#include "dplay_ice.h"
#include "dplay_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	DP_GAME_QUEUE            = 256,
	DP_CONTROL_QUEUE         = 64,
	DP_NORMAL_CONTROL_LIMIT  = DP_CONTROL_QUEUE - 1,
	DP_RECEIVE_BUDGET        = 64,
	DP_FRAGMENT_SEND_BUDGET  = 64,
	DP_WAKE_DELAY_US         = 10000,
	DP_CONTROL_RETRY_MS      = 250,
	DP_OPERATION_TIMEOUT_MS  = 10000,
	DP_ROSTER_TIMEOUT_MS     = 10000,
	DP_KEEPALIVE_INTERVAL_MS = 1000,
	DP_PEER_TIMEOUT_MS       = 30000,
	DP_CANCEL_RETENTION_MS   = 30000,
	DP_CLOSE_RETRY_MS        = 50,
	DP_CLOSE_DRAIN_MS        = 250
};

/* Consumed service-provider GUID discriminators from the DirectX SDK. */
enum { DP_PROVIDER_TCPIP_DATA1 = 0x36e95ee0, DP_PROVIDER_IPX_DATA1 = 0x685bc400 };

typedef struct DpPeer {
	DPID     id;
	DpLink   link;
	uint32_t active, groups;
	char     short_name[DP_SHORT_NAME_CAPACITY], long_name[DP_LONG_NAME_CAPACITY];
	uint64_t last_seen;
	uint32_t request;
	uint32_t open_request, open_version;
	HRESULT  result;
	DPID     result_id;
} DpPeer;

typedef struct DpPacket {
	uint64_t order;
	DPID     from, to;
	uint32_t size;
	uint8_t  bytes[DP_PAYLOAD];
} DpPacket;

typedef struct DpTransaction {
	uint32_t       version, pending;
	uint64_t       deadline[DP_PEERS], next_send[DP_PEERS];
	uint8_t        cursor[DP_PEERS];
	DpWireSnapshot snapshot;
	uint32_t       event, event_id;
	char           short_name[DP_SHORT_NAME_CAPACITY], long_name[DP_LONG_NAME_CAPACITY];
} DpTransaction;

typedef struct DpOperation {
	uint32_t kind, request, size;
	uint8_t  bytes[sizeof(DpWireControl)];
	uint64_t start, next_send;
	HRESULT  result;
	DPID     id;
} DpOperation;

typedef struct DpCore {
	DpLink   host_link;
	GUID     app, session;
	char     name[DP_SESSION_NAME_CAPACITY];
	uint32_t maximum, local_id, next_id, host_id, version;
	int      host, open, closing, failed;
	uint64_t close_until, keepalive;
	uint32_t next_request;
	uint32_t cancel_open_request;

	struct {
		DpLink   link;
		uint32_t request;
		uint64_t expires;
	} cancelled[DP_CONTROL_QUEUE];

	DpPeer         peers[DP_PEERS];
	DPID           groups[DP_GROUPS];
	DpOperation    operation;
	DpPacket       gameplay[DP_GAME_QUEUE], control[DP_CONTROL_QUEUE];
	unsigned       game_read, game_count, control_read, control_count;
	uint64_t       delivery_order;
	DpTransaction  transactions[DP_CONTROL_QUEUE];
	unsigned       transaction_read, transaction_count;
	uint32_t       assembly_version, assembly_mask;
	DpWireSnapshot assembly;
} DpCore;

extern DpCore g_dp;

void     DpWake(void);
void     DpPumpIncoming(void);
uint32_t DpRead32(const uint8_t* data);
void     DpWrite32(uint8_t* data, uint32_t value);
uint16_t DpRead16(const uint8_t* data);
void     DpWrite16(uint8_t* data, uint16_t value);
void     DpReadGuid(const uint8_t* data, GUID* value);
void     DpWriteGuid(uint8_t* data, const GUID* value);
uint64_t DpNow(void);
int      DpFindPeer(DPID id);
int DpSend(DpLink link, unsigned kind, uint32_t request, DPID from, DPID to, const void* data, unsigned size);
int DpQueueEvent(uint32_t kind, DPID id);
int DpQueueNameEvent(DPID id, const char* short_name, const char* long_name);
int DpPublish(uint32_t event, DPID id);
void    DpServiceControl(void);
void    DpHandleControl(DpLink link, unsigned kind, uint32_t request, DPID from, DPID to, const uint8_t* data,
						unsigned size);
void    DpHandleOpen(DpLink link, unsigned kind, uint32_t request, DPID from, DPID to, const uint8_t* data,
					 unsigned size);
void    DpLoseSession(void);
int     DpLinkLost(DpLink link);
HRESULT DpBeginOperation(unsigned kind, const void* data, unsigned size, DPID* id);
int     DpOperationWaiting(void);
HRESULT DpLocalControl(unsigned kind, DPID id, DPID group, const DPNAME* name, DPID* created);
void    DpResetSession(void);
HRESULT DpOpen(DPSESSIONDESC2* desc, uint32_t flags);
HRESULT DpClose(void);
#endif
