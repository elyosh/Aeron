#ifndef AERON_DPLAY_HTTP_H
#define AERON_DPLAY_HTTP_H

#include "aeron/compat/dplay_directory.h"
#include <stddef.h>

enum { DP_HTTP_SLOTS = 32, DP_HTTP_PATH = 512, DP_HTTP_BODY = 8192, DP_HTTP_RESPONSE = 1024 * 1024 };
typedef struct DpHttp DpHttp;

typedef enum DpHttpMethod { DP_HTTP_GET, DP_HTTP_PUT, DP_HTTP_DELETE } DpHttpMethod;

typedef struct DpHttpRequest {
	DpHttpMethod method;
	char         path[DP_HTTP_PATH];
	char         token[44];
	char         body[DP_HTTP_BODY + 1];
	unsigned     timeout_ms;
} DpHttpRequest;

typedef struct DpHttpResult {
	uint64_t                 id;
	long                     status;
	uint32_t                 retry_after;
	AeronDplayDirectoryError error;
	char*                    body;
	size_t                   size;
} DpHttpResult;

/* Application-thread API. Submit copies the request and reserves its completion
 * slot until Take. Zero means queue capacity is exhausted. Only the worker uses
 * curl handles; Cancel/Destroy wake it without waiting for network activity. */
int      DpHttp_Origin(const char* input, char output[AERON_DPLAY_DIRECTORY_URL_CAPACITY]);
DpHttp*  DpHttp_Create(const char* origin);
uint64_t DpHttp_Submit(DpHttp* http, const DpHttpRequest* request);
void     DpHttp_Cancel(DpHttp* http, uint64_t id);
int      DpHttp_Take(DpHttp* http, DpHttpResult* result); /* Caller frees result.body. */
int      DpHttp_Pending(DpHttp* http);
/* Cancels ordinary work and drains submitted DELETEs for at most two seconds. */
void DpHttp_Destroy(DpHttp* http);
#endif
