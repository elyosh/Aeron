#ifndef AERON_DPLAY_DIRECTORY_INTERNAL_H
#define AERON_DPLAY_DIRECTORY_INTERNAL_H
#include "aeron/compat/dplay_directory.h"

enum { DP_DIRECTORY_SDP = 4097, DP_DIRECTORY_HOST = 254, DP_DIRECTORY_CURSOR = 97 };

typedef struct DpDirectoryEndpoint {
	char     host[DP_DIRECTORY_HOST];
	uint16_t port;
} DpDirectoryEndpoint;

typedef struct DpDirectoryIce {
	DpDirectoryEndpoint stun, turn;
	char                username[96], password[64];
	int64_t             expires_at_unix;
} DpDirectoryIce;

typedef enum DpDirectorySignalState {
	DP_SIGNAL_WAITING_OFFER,
	DP_SIGNAL_WAITING_ANSWER,
	DP_SIGNAL_ANSWERED,
	DP_SIGNAL_REJECTED
} DpDirectorySignalState;

typedef struct DpDirectorySignal {
	AeronDplayConnectionIdentity identity;
	uint64_t                     generation, deadline_us;
	int64_t                      expires_at_unix;
	DpDirectorySignalState       state;
	DpDirectoryIce               ice;
	char                         offer[DP_DIRECTORY_SDP], answer[DP_DIRECTORY_SDP];
	AeronDplayDirectoryError     rejection;
	AeronDplayDirectoryStatus    publication;
} DpDirectorySignal;

typedef struct DpDirectoryHostSignals {
	AeronDplayDirectoryStatus status;
	unsigned                  count;
	DpDirectorySignal         connections[AERON_DPLAY_DIRECTORY_MAX_PLAYERS];
} DpDirectoryHostSignals;

/* Application-thread boundary consumed by the ICE transport. Getters copy.
 * A generation identifies a local resource lifetime, not just its remote UUID.
 * Replaced generations invalidate unadmitted agents. After SDP exchange,
 * transport progress can outlive signaling until the setup deadline; admitted
 * links remain transport-owned when their signaling record disappears. */
void                     DpDirectory_GetHostSignals(DpDirectoryHostSignals* signals);
unsigned                 DpDirectory_GetPage(const GUID*             application,
											 AeronDplayDirectoryRoom rooms[AERON_DPLAY_DIRECTORY_PAGE_SIZE]);
int                      DpDirectory_GetJoinSignal(DpDirectorySignal* signal);
int                      DpDirectory_GetJoinRoom(AeronDplayDirectoryRoom* room);
AeronDplayDirectoryError DpDirectory_SetDescription(const AeronDplayConnectionIdentity* identity,
													uint64_t generation, const char* sdp,
													AeronDplayDirectoryError rejection);
void                     DpDirectory_JoinReady(uint64_t generation, AeronDplayDirectoryError error);

void     DpDirectory_Update(void);
uint64_t DpDirectory_NextWakeDelayUs(void);
void     DpDirectory_Shutdown(void);
#endif
