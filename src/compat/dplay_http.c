#include "dplay_http.h"
#include "aeron/sync.h"
#include "aeron/time.h"
#include "dplay_internal.h"
#include <curl/curl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum SlotState { HTTP_FREE, HTTP_QUEUED, HTTP_ACTIVE, HTTP_DONE } SlotState;

typedef struct HttpSlot {
	SlotState     state;
	int           cancelled;
	DpHttpRequest request;
	DpHttpResult  result;
	/* Worker-owned until completion is published under the mutex. */
	CURL*              easy;
	struct curl_slist* headers;
	size_t             capacity;
} HttpSlot;

struct DpHttp {
	AeronMutex*  mutex;
	AeronThread* thread;
	CURLM*       multi;
	char         origin[AERON_DPLAY_DIRECTORY_URL_CAPACITY];
	uint64_t     next_id, stop_at;
	int          failed;
	HttpSlot     slots[DP_HTTP_SLOTS];
};

int DpHttp_Origin(const char* input, char output[AERON_DPLAY_DIRECTORY_URL_CAPACITY]) {
	size_t n = 0;
	while (n < AERON_DPLAY_DIRECTORY_URL_CAPACITY && input[n])
		++n;
	if (n < 9 || n >= AERON_DPLAY_DIRECTORY_URL_CAPACITY || strncmp(input, "https://", 8))
		return 0;
	for (size_t i = 0; i < n; ++i)
		if ((unsigned char)input[i] <= 32 || (unsigned char)input[i] >= 127 || strchr("?#@\\", input[i]))
			return 0;
	CURLU* url   = curl_url();
	char*  host  = NULL;
	char*  path  = NULL;
	int    valid = url && curl_url_set(url, CURLUPART_URL, input, 0) == CURLUE_OK &&
				   curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK && host[0] &&
				   curl_url_get(url, CURLUPART_PATH, &path, 0) == CURLUE_OK && !strcmp(path, "/");
	/* Reject normalized dot paths as well as actual non-origin paths. */
	const char* slash = strchr(input + 8, '/');
	valid             = valid && (!slash || slash == input + n - 1);
	if (valid) {
		memcpy(output, input, n + 1);
		if (output[n - 1] == '/')
			output[n - 1] = 0;
	}
	curl_free(host);
	curl_free(path);
	curl_url_cleanup(url);
	return valid;
}

static size_t HttpBody(char* bytes, size_t size, size_t count, void* user) {
	HttpSlot* slot = user;
	if (size && count > SIZE_MAX / size)
		return 0;
	size_t n = size * count;
	if (n > DP_HTTP_RESPONSE - slot->result.size) {
		slot->result.error = AERON_DPLAY_DIRECTORY_ERROR_BODY_TOO_LARGE;
		return 0;
	}
	size_t needed = slot->result.size + n + 1;
	if (needed > slot->capacity) {
		size_t capacity = slot->capacity ? slot->capacity * 2 : 4096;
		if (capacity < needed)
			capacity = needed;
		if (capacity > DP_HTTP_RESPONSE + 1)
			capacity = DP_HTTP_RESPONSE + 1;
		char* body = realloc(slot->result.body, capacity);
		if (!body) {
			slot->result.error = AERON_DPLAY_DIRECTORY_ERROR_NO_MEMORY;
			return 0;
		}
		slot->result.body = body;
		slot->capacity    = capacity;
	}
	memcpy(slot->result.body + slot->result.size, bytes, n);
	slot->result.size += n;
	slot->result.body[slot->result.size] = 0;
	return n;
}

static size_t HttpHeader(char* bytes, size_t size, size_t count, void* user) {
	HttpSlot* slot = user;
	if (size && count > SIZE_MAX / size)
		return 0;
	size_t n = size * count;
	if (n >= 5 && !memcmp(bytes, "HTTP/", 5))
		slot->result.retry_after = 0;
	if (n >= 12 && !curl_strnequal(bytes, "Retry-After:", 12))
		return n;
	if (n < 12)
		return n;
	size_t i = 12;
	while (i < n && (bytes[i] == ' ' || bytes[i] == '\t'))
		++i;
	uint64_t value  = 0;
	int      digits = 0;
	while (i < n && bytes[i] >= '0' && bytes[i] <= '9') {
		value = value * 10 + (unsigned)(bytes[i++] - '0');
		if (value > UINT32_MAX)
			value = UINT32_MAX;
		digits = 1;
	}
	while (i < n && strchr(" \t\r\n", bytes[i]))
		++i;
	if (digits && i == n)
		slot->result.retry_after = (uint32_t)value;
	return n;
}

static int HttpStart(DpHttp* http, HttpSlot* slot) {
	char           url[AERON_DPLAY_DIRECTORY_URL_CAPACITY + DP_HTTP_PATH];
	char           authorization[80];
	DpHttpRequest* r         = &slot->request;
	const char*    methods[] = { "GET", "PUT", "DELETE" };
	snprintf(url, sizeof(url), "%s%s", http->origin, r->path);
	slot->easy = curl_easy_init();
	if (!slot->easy)
		return 0;
	const char* headers[] = { "Accept: application/json", "Content-Type: application/json", "Expect:", NULL };
	for (unsigned i = 0; headers[i]; ++i) {
		struct curl_slist* list = curl_slist_append(slot->headers, headers[i]);
		if (!list)
			return 0;
		slot->headers = list;
	}
	if (r->token[0]) {
		snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", r->token);
		struct curl_slist* list = curl_slist_append(slot->headers, authorization);
		if (!list)
			return 0;
		slot->headers = list;
	}
#define SET(option, value)                                                                                   \
	do {                                                                                                     \
		if (curl_easy_setopt(slot->easy, option, value) != CURLE_OK)                                         \
			return 0;                                                                                        \
	} while (0)
	SET(CURLOPT_URL, url);
	SET(CURLOPT_CUSTOMREQUEST, methods[r->method]);
	SET(CURLOPT_HTTPHEADER, slot->headers);
	SET(CURLOPT_NOSIGNAL, 1L);
	SET(CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
	SET(CURLOPT_FOLLOWLOCATION, 0L);
	SET(CURLOPT_SSL_VERIFYPEER, 1L);
	SET(CURLOPT_SSL_VERIFYHOST, 2L);
	SET(CURLOPT_CONNECTTIMEOUT_MS, 5000L);
	SET(CURLOPT_TIMEOUT_MS, (long)r->timeout_ms);
	SET(CURLOPT_WRITEFUNCTION, HttpBody);
	SET(CURLOPT_WRITEDATA, slot);
	SET(CURLOPT_HEADERFUNCTION, HttpHeader);
	SET(CURLOPT_HEADERDATA, slot);
	SET(CURLOPT_PRIVATE, slot);
	if (r->method == DP_HTTP_PUT) {
		SET(CURLOPT_POSTFIELDS, r->body);
		SET(CURLOPT_POSTFIELDSIZE, (long)strlen(r->body));
	}
#undef SET
	return curl_multi_add_handle(http->multi, slot->easy) == CURLM_OK;
}

static void HttpComplete(DpHttp* http, HttpSlot* slot, CURLcode code) {
	if (slot->easy) {
		curl_easy_getinfo(slot->easy, CURLINFO_RESPONSE_CODE, &slot->result.status);
		curl_multi_remove_handle(http->multi, slot->easy);
		curl_easy_cleanup(slot->easy);
		slot->easy = NULL;
	}
	curl_slist_free_all(slot->headers);
	slot->headers = NULL;
	if (!slot->result.error && code != CURLE_OK)
		slot->result.error = code == CURLE_OPERATION_TIMEDOUT ? AERON_DPLAY_DIRECTORY_ERROR_TIMEOUT
							 : code == CURLE_OUT_OF_MEMORY    ? AERON_DPLAY_DIRECTORY_ERROR_NO_MEMORY
															  : AERON_DPLAY_DIRECTORY_ERROR_NETWORK;
	/* The worker never writes this slot again after publishing HTTP_DONE. */
	Aeron_MutexLock(http->mutex);
	slot->state = HTTP_DONE;
	Aeron_MutexUnlock(http->mutex);
	DpWake();
}

static void HttpFail(DpHttp* http) {
	Aeron_MutexLock(http->mutex);
	http->failed = 1;
	Aeron_MutexUnlock(http->mutex);
	for (unsigned i = 0; i < DP_HTTP_SLOTS; ++i) {
		HttpSlot* slot = &http->slots[i];
		Aeron_MutexLock(http->mutex);
		int pending = slot->state == HTTP_QUEUED || slot->state == HTTP_ACTIVE;
		if (pending)
			slot->state = HTTP_ACTIVE;
		Aeron_MutexUnlock(http->mutex);
		if (pending) {
			slot->result.error = AERON_DPLAY_DIRECTORY_ERROR_UNAVAILABLE;
			HttpComplete(http, slot, CURLE_RECV_ERROR);
		}
	}
	DpWake();
}

static int HttpWorker(void* user) {
	DpHttp* http = user;
	for (;;) {
		int      active = 0;
		uint64_t now    = Aeron_NowUs(), stop;
		for (unsigned i = 0; i < DP_HTTP_SLOTS; ++i) {
			HttpSlot* slot = &http->slots[i];
			Aeron_MutexLock(http->mutex);
			SlotState state = slot->state;
			stop            = http->stop_at;
			int cancel = slot->cancelled || (stop && (now >= stop || slot->request.method != DP_HTTP_DELETE));
			if (state == HTTP_QUEUED)
				slot->state = HTTP_ACTIVE;
			Aeron_MutexUnlock(http->mutex);
			if (state != HTTP_QUEUED && state != HTTP_ACTIVE)
				continue;
			if (cancel) {
				HttpComplete(http, slot, CURLE_ABORTED_BY_CALLBACK);
				continue;
			}
			if (state == HTTP_QUEUED && !HttpStart(http, slot)) {
				HttpComplete(http, slot, CURLE_OUT_OF_MEMORY);
				continue;
			}
			++active;
		}
		Aeron_MutexLock(http->mutex);
		stop = http->stop_at;
		Aeron_MutexUnlock(http->mutex);
		if (stop && !active)
			break;
		int       running, remaining;
		CURLMcode code = curl_multi_perform(http->multi, &running);
		CURLMsg*  message;
		while ((message = curl_multi_info_read(http->multi, &remaining))) {
			if (message->msg == CURLMSG_DONE) {
				HttpSlot* slot = NULL;
				curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &slot);
				HttpComplete(http, slot, message->data.result);
			}
		}
		if (code != CURLM_OK) {
			HttpFail(http);
			break;
		}
		int delay = INT_MAX;
		if (stop) {
			now   = Aeron_NowUs();
			delay = now >= stop ? 0 : (int)((stop - now + 999) / 1000);
		}
		if (curl_multi_poll(http->multi, NULL, 0, delay, NULL) != CURLM_OK) {
			/* End outstanding requests instead of spinning on a broken multi handle. */
			HttpFail(http);
			break;
		}
	}
	return 0;
}

DpHttp* DpHttp_Create(const char* origin) {
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		return NULL;
	DpHttp* http = calloc(1, sizeof(*http));
	if (!http) {
		curl_global_cleanup();
		return NULL;
	}
	strcpy(http->origin, origin);
	http->mutex = Aeron_MutexCreate();
	http->multi = curl_multi_init();
	if (http->mutex && http->multi)
		http->thread = Aeron_ThreadCreate("dplay-https", HttpWorker, http);
	if (!http->thread) {
		curl_multi_cleanup(http->multi);
		if (http->mutex)
			Aeron_MutexDestroy(http->mutex);
		free(http);
		curl_global_cleanup();
		return NULL;
	}
	return http;
}

uint64_t DpHttp_Submit(DpHttp* http, const DpHttpRequest* request) {
	uint64_t id        = 0;
	int      completed = 0;
	Aeron_MutexLock(http->mutex);
	for (unsigned i = 0; !http->stop_at && i < DP_HTTP_SLOTS; ++i) {
		HttpSlot* slot = &http->slots[i];
		if (slot->state != HTTP_FREE)
			continue;
		memset(slot, 0, sizeof(*slot));
		slot->request   = *request;
		id              = ++http->next_id;
		slot->result.id = id;
		slot->state     = HTTP_QUEUED;
		if (http->failed) {
			slot->result.error = AERON_DPLAY_DIRECTORY_ERROR_UNAVAILABLE;
			slot->state        = HTTP_DONE;
			completed          = 1;
		}
		break;
	}
	Aeron_MutexUnlock(http->mutex);
	if (id)
		curl_multi_wakeup(http->multi);
	if (completed)
		DpWake();
	return id;
}

void DpHttp_Cancel(DpHttp* http, uint64_t id) {
	if (!http || !id)
		return;
	Aeron_MutexLock(http->mutex);
	for (unsigned i = 0; i < DP_HTTP_SLOTS; ++i)
		if (http->slots[i].result.id == id)
			http->slots[i].cancelled = 1;
	Aeron_MutexUnlock(http->mutex);
	curl_multi_wakeup(http->multi);
}

int DpHttp_Take(DpHttp* http, DpHttpResult* result) {
	int found = 0;
	Aeron_MutexLock(http->mutex);
	for (unsigned i = 0; i < DP_HTTP_SLOTS; ++i) {
		HttpSlot* slot = &http->slots[i];
		if (slot->state != HTTP_DONE)
			continue;
		*result           = slot->result;
		slot->result.body = NULL;
		slot->state       = HTTP_FREE;
		found             = 1;
		break;
	}
	Aeron_MutexUnlock(http->mutex);
	return found;
}

int DpHttp_Pending(DpHttp* http) {
	int pending = 0;
	if (!http)
		return 0;
	Aeron_MutexLock(http->mutex);
	for (unsigned i = 0; i < DP_HTTP_SLOTS; ++i)
		pending |= http->slots[i].state != HTTP_FREE;
	Aeron_MutexUnlock(http->mutex);
	return pending;
}

void DpHttp_Destroy(DpHttp* http) {
	if (!http)
		return;
	Aeron_MutexLock(http->mutex);
	http->stop_at = Aeron_NowUs() + 2000000;
	Aeron_MutexUnlock(http->mutex);
	curl_multi_wakeup(http->multi);
	Aeron_ThreadJoin(http->thread);
	for (unsigned i = 0; i < DP_HTTP_SLOTS; ++i)
		free(http->slots[i].result.body);
	curl_multi_cleanup(http->multi);
	Aeron_MutexDestroy(http->mutex);
	memset(http, 0, sizeof(*http));
	free(http);
	curl_global_cleanup();
}
