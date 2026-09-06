#ifndef AERON_COMPAT_DPLAY_H
#define AERON_COMPAT_DPLAY_H

#include "aeron/compat/win_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Consumed DirectPlay ABI. Network service runs explicitly on the host thread. */

typedef struct DPSESSIONDESC2        DPSESSIONDESC2;
typedef struct DPNAME                DPNAME;
typedef struct DPLCONNECTION         DPLCONNECTION;
typedef struct DPCOMPORTADDRESS      DPCOMPORTADDRESS;
typedef struct DPCAPS                DPCAPS;
typedef struct DPSESSIONDESC         DPSESSIONDESC;
typedef struct DPLAPPINFO            DPLAPPINFO;
typedef struct IDirectPlay           IDirectPlay;
typedef struct IDirectPlayVtbl       IDirectPlayVtbl;
typedef struct IDirectPlay2A         IDirectPlay2A;
typedef struct IDirectPlay2AVtbl     IDirectPlay2AVtbl;
typedef struct IDirectPlayLobbyA     IDirectPlayLobbyA;
typedef struct IDirectPlayLobbyAVtbl IDirectPlayLobbyAVtbl;

typedef uint32_t DPID;

enum {
	DPID_SYSMSG                = 0,
	DPID_ALLPLAYERS            = 0,
	DPOPEN_JOIN                = 1,
	DPOPEN_CREATE              = 2,
	DPENUMSESSIONS_AVAILABLE   = 1,
	DPESC_TIMEDOUT             = 1,
	DPRECEIVE_ALL              = 1,
	DPPLAYERTYPE_PLAYER        = 1,
	DPSYS_CREATEPLAYERORGROUP  = 3,
	DPSYS_DESTROYPLAYERORGROUP = 5,
	DPSYS_SETPLAYERORGROUPNAME = 0x103
};

struct DPSESSIONDESC2 {
	uint32_t dwSize;
	uint32_t dwFlags;
	GUID     guidInstance;
	GUID     guidApplication;
	uint32_t dwMaxPlayers;
	uint32_t dwCurrentPlayers;
	char*    lpszSessionNameA;
	char*    lpszPasswordA;
	uint32_t dwReserved1;
	uint32_t dwReserved2;
	uint32_t dwUser1;
	uint32_t dwUser2;
	uint32_t dwUser3;
	uint32_t dwUser4;
};

struct DPNAME {
	uint32_t dwSize;
	uint32_t dwFlags;
	char*    lpszShortNameA;
	char*    lpszLongNameA;
};

typedef struct DPMSG_SETPLAYERORGROUPNAME {
	uint32_t dwType, dwPlayerType;
	DPID     dpId;
	DPNAME   dpnName;
} DPMSG_SETPLAYERORGROUPNAME;

struct DPLCONNECTION {
	uint32_t        dwSize;
	uint32_t        dwFlags;
	DPSESSIONDESC2* lpSessionDesc;
	DPNAME*         lpPlayerName;
	GUID            guidSP;
	void*           lpAddress;
	uint32_t        dwAddressSize;
};

struct DPCOMPORTADDRESS {
	uint32_t dwComPort;
	uint32_t dwBaudRate;
	uint32_t dwStopBits;
	uint32_t dwParity;
	uint32_t dwFlowControl;
};

struct DPCAPS {
	uint32_t dwSize;
	uint32_t dwFlags;
	uint32_t dwMaxBufferSize;
	uint32_t dwMaxQueueSize;
	uint32_t dwMaxPlayers;
	uint32_t dwHundredBaud;
	uint32_t dwLatency;
	uint32_t dwMaxLocalPlayers;
	uint32_t dwHeaderLength;
	uint32_t dwTimeout;
};

struct DPSESSIONDESC {
	uint32_t dwSize;
	GUID     guidSession;
	uint32_t dwSession;
	uint32_t dwMaxPlayers;
	uint32_t dwCurrentPlayers;
	uint32_t dwFlags;
	char     szSessionName[32];
	char     szUserField[16];
	uint32_t dwReserved1;
	char     szPassword[16];
	uint32_t dwReserved2;
	uint32_t dwUser1;
	uint32_t dwUser2;
	uint32_t dwUser3;
	uint32_t dwUser4;
};

struct DPLAPPINFO {
	uint32_t dwSize;
	GUID     guidApplication;
	char*    lpszAppNameA;
};

struct IDirectPlay {
	IDirectPlayVtbl* lpVtbl;
};

typedef int (*DPEnumPlayersCallback)(DPID playerId, char* friendlyName, char* formalName, uint32_t flags,
									 void* context);

typedef int (*DPEnumSessionsCallback)(DPSESSIONDESC* sessionDesc, void* context, uint32_t* timeoutMs,
									  uint32_t flags);

struct IDirectPlayVtbl {
	HRESULT(AERON_DXAPI* QueryInterface)(IDirectPlay* self, const GUID* iid, void** outObject);
	uint32_t(AERON_DXAPI* AddRef)(IDirectPlay* self);
	uint32_t(AERON_DXAPI* Release)(IDirectPlay* self);
	HRESULT(AERON_DXAPI* AddPlayerToGroup)(IDirectPlay* self, DPID groupId, DPID playerId);
	HRESULT(AERON_DXAPI* Close)(IDirectPlay* self);
	HRESULT(AERON_DXAPI* CreatePlayer)
	(IDirectPlay* self, DPID* outPlayerId, char* playerName, char* formalName, void** outEvent);
	HRESULT(AERON_DXAPI* CreateGroup)(IDirectPlay* self, DPID* outGroupId, char* groupName, char* formalName);
	HRESULT(AERON_DXAPI* DeletePlayerFromGroup)(IDirectPlay* self, DPID groupId, DPID playerId);
	HRESULT(AERON_DXAPI* DestroyPlayer)(IDirectPlay* self, DPID playerId);
	HRESULT(AERON_DXAPI* DestroyGroup)(IDirectPlay* self, DPID groupId);
	HRESULT(AERON_DXAPI* EnableNewPlayers)(IDirectPlay* self, int enable);
	HRESULT(AERON_DXAPI* EnumGroupPlayers)
	(IDirectPlay* self, DPID groupId, DPEnumPlayersCallback callback, void* context, uint32_t flags);
	HRESULT(AERON_DXAPI* EnumGroups)
	(IDirectPlay* self, uint32_t ignored, DPEnumPlayersCallback callback, void* context, uint32_t flags);
	HRESULT(AERON_DXAPI* EnumPlayers)
	(IDirectPlay* self, uint32_t ignored, DPEnumPlayersCallback callback, void* context, uint32_t flags);
	HRESULT(AERON_DXAPI* EnumSessions)
	(IDirectPlay* self, DPSESSIONDESC* sessionDesc, uint32_t timeoutMs, DPEnumSessionsCallback callback,
	 void* context, uint32_t flags);
	HRESULT(AERON_DXAPI* GetCaps)(IDirectPlay* self, DPCAPS* caps);
	HRESULT(AERON_DXAPI* GetMessageCount)(IDirectPlay* self, DPID playerId, uint32_t* outCount);
	HRESULT(AERON_DXAPI* GetPlayerCaps)(IDirectPlay* self, DPID playerId, DPCAPS* caps);
	HRESULT(AERON_DXAPI* GetPlayerName)
	(IDirectPlay* self, DPID playerId, char* friendlyName, uint32_t* friendlyNameSize, char* formalName,
	 uint32_t* formalNameSize);
	HRESULT(AERON_DXAPI* Initialize)(IDirectPlay* self, GUID* serviceProviderGuid);
	HRESULT(AERON_DXAPI* Open)(IDirectPlay* self, DPSESSIONDESC* sessionDesc);
	HRESULT(AERON_DXAPI* Receive)
	(IDirectPlay* self, DPID* fromId, DPID* toId, uint32_t flags, void* data, uint32_t* dataSize);
	HRESULT(AERON_DXAPI* SaveSession)(IDirectPlay* self, char* fileName);
	HRESULT(AERON_DXAPI* Send)(IDirectPlay* self, DPID fromId, DPID toId, uint32_t flags, void* data,
							   uint32_t dataSize);
	HRESULT(AERON_DXAPI* SetPlayerName)(IDirectPlay* self, DPID playerId, char* friendlyName,
										char* formalName);
};

struct IDirectPlay2A {
	IDirectPlay2AVtbl* lpVtbl;
};

typedef int(AERON_DXAPI* DPEnumPlayersCallback2)(DPID playerId, uint32_t playerType, const DPNAME* nameDesc,
												 uint32_t flags, void* context);

typedef int(AERON_DXAPI* DPEnumSessionsCallback2)(const DPSESSIONDESC2* sessionDesc, uint32_t* timeoutMs,
												  uint32_t flags, void* context);

struct IDirectPlay2AVtbl {
	HRESULT(AERON_DXAPI* QueryInterface)(IDirectPlay2A* self, const GUID* iid, void** outObject);
	uint32_t(AERON_DXAPI* AddRef)(IDirectPlay2A* self);
	uint32_t(AERON_DXAPI* Release)(IDirectPlay2A* self);
	HRESULT(AERON_DXAPI* AddPlayerToGroup)(IDirectPlay2A* self, DPID groupId, DPID playerId);
	HRESULT(AERON_DXAPI* Close)(IDirectPlay2A* self);
	HRESULT(AERON_DXAPI* CreateGroup)
	(IDirectPlay2A* self, DPID* outGroupId, DPNAME* groupName, void* data, uint32_t dataSize, uint32_t flags);
	HRESULT(AERON_DXAPI* CreatePlayer)
	(IDirectPlay2A* self, DPID* outPlayerId, DPNAME* playerName, void* eventHandle, void* data,
	 uint32_t dataSize, uint32_t flags);
	HRESULT(AERON_DXAPI* DeletePlayerFromGroup)(IDirectPlay2A* self, DPID groupId, DPID playerId);
	HRESULT(AERON_DXAPI* DestroyGroup)(IDirectPlay2A* self, DPID groupId);
	HRESULT(AERON_DXAPI* DestroyPlayer)(IDirectPlay2A* self, DPID playerId);
	HRESULT(AERON_DXAPI* EnumGroupPlayers)
	(IDirectPlay2A* self, DPID groupId, GUID* instanceGuid, DPEnumPlayersCallback2 callback, void* context,
	 uint32_t flags);
	HRESULT(AERON_DXAPI* EnumGroups)
	(IDirectPlay2A* self, GUID* instanceGuid, DPEnumPlayersCallback2 callback, void* context, uint32_t flags);
	HRESULT(AERON_DXAPI* EnumPlayers)
	(IDirectPlay2A* self, GUID* instanceGuid, DPEnumPlayersCallback2 callback, void* context, uint32_t flags);
	HRESULT(AERON_DXAPI* EnumSessions)
	(IDirectPlay2A* self, DPSESSIONDESC2* sessionDesc, uint32_t timeoutMs, DPEnumSessionsCallback2 callback,
	 void* context, uint32_t flags);
	HRESULT(AERON_DXAPI* GetCaps)(IDirectPlay2A* self, DPCAPS* caps, uint32_t flags);
	HRESULT(AERON_DXAPI* GetGroupData)
	(IDirectPlay2A* self, DPID groupId, void* data, uint32_t* dataSize, uint32_t flags);
	HRESULT(AERON_DXAPI* GetGroupName)(IDirectPlay2A* self, DPID groupId, void* data, uint32_t* dataSize);
	HRESULT(AERON_DXAPI* GetMessageCount)(IDirectPlay2A* self, DPID playerId, uint32_t* outCount);
	HRESULT(AERON_DXAPI* GetPlayerAddress)(IDirectPlay2A* self, DPID playerId, void* data,
										   uint32_t* dataSize);
	HRESULT(AERON_DXAPI* GetPlayerCaps)(IDirectPlay2A* self, DPID playerId, DPCAPS* caps, uint32_t flags);
	HRESULT(AERON_DXAPI* GetPlayerData)
	(IDirectPlay2A* self, DPID playerId, void* data, uint32_t* dataSize, uint32_t flags);
	HRESULT(AERON_DXAPI* GetPlayerName)(IDirectPlay2A* self, DPID playerId, void* data, uint32_t* dataSize);
	HRESULT(AERON_DXAPI* GetSessionDesc)(IDirectPlay2A* self, void* data, uint32_t* dataSize);
	HRESULT(AERON_DXAPI* Initialize)(IDirectPlay2A* self, GUID* serviceProviderGuid);
	HRESULT(AERON_DXAPI* Open)(IDirectPlay2A* self, DPSESSIONDESC2* sessionDesc, uint32_t flags);
	HRESULT(AERON_DXAPI* Receive)
	(IDirectPlay2A* self, DPID* fromId, DPID* toId, uint32_t flags, void* data, uint32_t* dataSize);
	HRESULT(AERON_DXAPI* Send)
	(IDirectPlay2A* self, DPID fromId, DPID toId, uint32_t flags, void* data, uint32_t dataSize);
	HRESULT(AERON_DXAPI* SetGroupData)(IDirectPlay2A* self, DPID groupId, void* data, uint32_t dataSize,
									   uint32_t flags);
	HRESULT(AERON_DXAPI* SetGroupName)(IDirectPlay2A* self, DPID groupId, DPNAME* groupName, uint32_t flags);
	HRESULT(AERON_DXAPI* SetPlayerData)
	(IDirectPlay2A* self, DPID playerId, void* data, uint32_t dataSize, uint32_t flags);
	HRESULT(AERON_DXAPI* SetPlayerName)
	(IDirectPlay2A* self, DPID playerId, DPNAME* playerName, uint32_t flags);
	HRESULT(AERON_DXAPI* SetSessionDesc)(IDirectPlay2A* self, DPSESSIONDESC2* sessionDesc, uint32_t flags);
};

struct IDirectPlayLobbyA {
	IDirectPlayLobbyAVtbl* lpVtbl;
};

typedef int (*DPEnumAddressCallback)(const GUID* dataType, uint32_t dataSize, const void* data,
									 void* context);

typedef int (*DPEnumAddressTypesCallback)(const GUID* dataType, void* context, uint32_t flags);

typedef int (*DPEnumLocalApplicationsCallback)(const DPLAPPINFO* appInfo, void* context, uint32_t flags);

struct IDirectPlayLobbyAVtbl {
	HRESULT(AERON_DXAPI* QueryInterface)(IDirectPlayLobbyA* self, const GUID* iid, void** outObject);
	uint32_t(AERON_DXAPI* AddRef)(IDirectPlayLobbyA* self);
	uint32_t(AERON_DXAPI* Release)(IDirectPlayLobbyA* self);
	HRESULT(AERON_DXAPI* Connect)
	(IDirectPlayLobbyA* self, uint32_t flags, IDirectPlay2A** outDirectPlay, void* outerUnknown);
	HRESULT(AERON_DXAPI* CreateAddress)
	(IDirectPlayLobbyA* self, const GUID* serviceProviderGuid, const GUID* dataTypeGuid, const void* data,
	 uint32_t dataSize, void* address, uint32_t* addressSize);
	HRESULT(AERON_DXAPI* EnumAddress)
	(IDirectPlayLobbyA* self, DPEnumAddressCallback callback, const void* address, uint32_t addressSize,
	 void* context);
	HRESULT(AERON_DXAPI* EnumAddressTypes)
	(IDirectPlayLobbyA* self, DPEnumAddressTypesCallback callback, const GUID* serviceProviderGuid,
	 void* context, uint32_t flags);
	HRESULT(AERON_DXAPI* EnumLocalApplications)
	(IDirectPlayLobbyA* self, DPEnumLocalApplicationsCallback callback, void* context, uint32_t flags);
	HRESULT(AERON_DXAPI* GetConnectionSettings)(IDirectPlayLobbyA* self, uint32_t appId, void* data,
												uint32_t* dataSize);
	HRESULT(AERON_DXAPI* ReceiveLobbyMessage)
	(IDirectPlayLobbyA* self, uint32_t flags, uint32_t appId, uint32_t* messageFlags, void* data,
	 uint32_t* dataSize);
	HRESULT(AERON_DXAPI* RunApplication)
	(IDirectPlayLobbyA* self, uint32_t flags, uint32_t* appId, DPLCONNECTION* connection, void* eventHandle);
	HRESULT(AERON_DXAPI* SendLobbyMessage)
	(IDirectPlayLobbyA* self, uint32_t flags, uint32_t appId, void* data, uint32_t dataSize);
	HRESULT(AERON_DXAPI* SetConnectionSettings)
	(IDirectPlayLobbyA* self, uint32_t flags, uint32_t appId, DPLCONNECTION* connection);
	HRESULT(AERON_DXAPI* SetLobbyMessageEvent)
	(IDirectPlayLobbyA* self, uint32_t flags, uint32_t appId, void* eventHandle);
};

/* Pending is a distinct result; callers must retain arguments until completion. */
#define DPERR_PENDING ((HRESULT)0x8000000a)
#define DPERR_NOINTERFACE ((HRESULT)0x80004002)
#define DPERR_NOMEMORY ((HRESULT)0x8007000e)
#define DPERR_NOMESSAGES ((HRESULT)(0x88770000u + 190))
#define DPERR_BUFFERTOOSMALL ((HRESULT)(0x88770000u + 30))
#define DPERR_BUSY ((HRESULT)(0x88770000u + 270))
#define DPERR_CANTCREATEPLAYER ((HRESULT)(0x88770000u + 60))
#define DPERR_TIMEOUT ((HRESULT)(0x88770000u + 240))
#define DPERR_INVALIDPLAYER ((HRESULT)(0x88770000u + 150))
#define DPERR_SESSIONLOST ((HRESULT)(0x88770000u + 310))
#define DPERR_NONEWPLAYERS ((HRESULT)(0x88770000u + 330))

HRESULT AERON_DXAPI DirectPlayCreate(const GUID* provider, IDirectPlay** out, void* outer);

typedef struct AeronDplayConnectionInfo {
	char local_address[64], remote_address[64];
	int  local_relayed, remote_relayed;
} AeronDplayConnectionInfo;

/* Application-thread diagnostic for the ICE link carrying traffic to a remote
 * player. Clients report their host link. Returns zero until ICE nomination is
 * complete or if no path is available. Never exposes credentials or full SDP. */
int AeronDplay_GetConnectionInfo(DPID player, AeronDplayConnectionInfo* info);

/* Optional worker notification. Register before any directory/DirectPlay work;
 * change or release it only after Shutdown, which clears the registration.
 * The callback may run concurrently on workers: only signal the application's
 * wait primitive, never call back into DirectPlay. Notifications may coalesce. */
void AeronDplay_SetWakeCallback(void (*callback)(void*), void* user);

void     AeronDplay_Update(void);
uint64_t AeronDplay_NextWakeDelayUs(void);
int      AeronDplay_IsActive(void);
void     AeronDplay_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
