#ifndef AERON_DPLAY_DIRECTORY_JSON_H
#define AERON_DPLAY_DIRECTORY_JSON_H
#include "cJSON.h"
#include "dplay_directory_internal.h"

void   DpDirectory_GuidText(const GUID* id, char text[37]);
int    DpDirectory_ParseGuid(const char* text, GUID* id);
int    DpDirectory_Text(const char* text, size_t capacity, int controls);
int    DpDirectory_Version(const char* text);
int    DpDirectory_Metadata(const AeronDplayRoomMetadata* input, AeronDplayRoomMetadata* clean);
cJSON* DpDirectory_ParseJSON(const char* data, size_t size);
char*  DpDirectory_RoomJSON(const AeronDplayRoomMetadata* metadata, uint32_t revision, const char* version);
char*  DpDirectory_ConnectionJSON(const char* version);
char*  DpDirectory_DescriptionJSON(const char* sdp, AeronDplayDirectoryError rejection);
int    DpDirectory_ReadRoom(const cJSON* json, AeronDplayDirectoryRoom* room);
int    DpDirectory_ReadRooms(const cJSON* json, AeronDplayDirectorySnapshot* snapshot);
int    DpDirectory_ReadSignal(const cJSON* json, int host, DpDirectorySignal* signal);
int    DpDirectory_ReadHostSignals(const cJSON* json, DpDirectoryHostSignals* signals,
								   char cursor[DP_DIRECTORY_CURSOR]);
AeronDplayDirectoryError DpDirectory_ReadError(const cJSON* json);
#endif
