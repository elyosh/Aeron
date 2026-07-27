#include "internal.h"

#define AERON_MAX_TIMERS 4

/* One registration slot per live timer. `guard` is created on first use and
   never destroyed: SDL_RemoveTimer does not wait for an in-flight callback, so
   the trampoline must always find a valid mutex. A callback whose SDL_TimerID no
   longer matches the slot's is stale. */
typedef struct AeronTimerSlot {
	SDL_Mutex*  guard;
	SDL_TimerID id;
	void (*callback)(void* user);
	void* user;
	int   in_use;
} AeronTimerSlot;

static AeronTimerSlot g_timers[AERON_MAX_TIMERS];

static Uint32 SDLCALL Aeron_TimerTrampoline(void* userdata, SDL_TimerID id, Uint32 interval) {
	AeronTimerSlot* slot = (AeronTimerSlot*)userdata;
	void (*callback)(void* user);
	void* user;

	SDL_LockMutex(slot->guard);
	if (slot->id != id) {
		/* Cancelled, and possibly re-issued to a new timer, while this call was
		   already scheduled. Returning 0 retires this invocation. */
		SDL_UnlockMutex(slot->guard);
		return 0;
	}
	callback = slot->callback;
	user     = slot->user;
	if (callback) {
		callback(user);
	}
	SDL_UnlockMutex(slot->guard);
	return interval;
}

AeronTimer Aeron_TimerCreate(unsigned int interval_ms, void (*callback)(void* user), void* user) {
	if (interval_ms == 0 || !callback) {
		return 0;
	}

	for (int i = 0; i < AERON_MAX_TIMERS; ++i) {
		AeronTimerSlot* slot = &g_timers[i];
		if (slot->in_use) {
			continue;
		}
		if (!slot->guard) {
			slot->guard = SDL_CreateMutex();
			if (!slot->guard) {
				Aeron_Log("aeron.sync", "SDL_CreateMutex failed: %s", SDL_GetError());
				return 0;
			}
		}

		/* Held across SDL_AddTimer and the id publication, because SDL can run a
		   short-interval callback before SDL_AddTimer returns and that callback
		   must wait for the real id. SDL_AddTimer only queues work for the timer
		   thread, so this cannot deadlock. */
		SDL_LockMutex(slot->guard);
		slot->callback = callback;
		slot->user     = user;
		slot->id       = 0;
		slot->in_use   = 1;

		SDL_TimerID id = SDL_AddTimer(interval_ms, Aeron_TimerTrampoline, slot);
		if (!id) {
			Aeron_Log("aeron.sync", "SDL_AddTimer failed: %s", SDL_GetError());
			slot->callback = NULL;
			slot->user     = NULL;
			slot->in_use   = 0;
			SDL_UnlockMutex(slot->guard);
			return 0;
		}
		slot->id = id;
		SDL_UnlockMutex(slot->guard);
		return (AeronTimer)(i + 1);
	}

	Aeron_Log("aeron.sync", "no free timer slot (max %d)", AERON_MAX_TIMERS);
	return 0;
}

void Aeron_TimerDestroy(AeronTimer timer) {
	if (timer == 0 || timer > AERON_MAX_TIMERS) {
		return;
	}

	AeronTimerSlot* slot = &g_timers[timer - 1];
	if (!slot->in_use) {
		return;
	}

	SDL_RemoveTimer(slot->id);
	/* Taking the guard waits out a callback that was already running when
	   SDL_RemoveTimer returned; clearing the id under it retires any invocation
	   still queued behind us. Both together make this a quiescing stop. */
	SDL_LockMutex(slot->guard);
	slot->id       = 0;
	slot->callback = NULL;
	slot->user     = NULL;
	slot->in_use   = 0;
	SDL_UnlockMutex(slot->guard);
}

AeronMutex* Aeron_MutexCreate(void) {
	SDL_Mutex* mutex = SDL_CreateMutex();
	if (!mutex) {
		Aeron_Log("aeron.sync", "SDL_CreateMutex failed: %s", SDL_GetError());
	}
	return (AeronMutex*)mutex;
}

void Aeron_MutexLock(AeronMutex* mutex) { SDL_LockMutex((SDL_Mutex*)mutex); }

void Aeron_MutexUnlock(AeronMutex* mutex) { SDL_UnlockMutex((SDL_Mutex*)mutex); }

void Aeron_MutexDestroy(AeronMutex* mutex) { SDL_DestroyMutex((SDL_Mutex*)mutex); }

/* Compiler atomics over plain int storage, so callers keep their shared globals
   declared as ordinary ints. */
#if defined(_MSC_VER)
#include <intrin.h>

int Aeron_AtomicLoad(volatile int* value) {
	return (int)_InterlockedOr((volatile long*)value, 0);
}

void Aeron_AtomicStore(volatile int* value, int newValue) {
	(void)_InterlockedExchange((volatile long*)value, (long)newValue);
}

int Aeron_AtomicAdd(volatile int* value, int delta) {
	return (int)_InterlockedExchangeAdd((volatile long*)value, (long)delta);
}
#else
int Aeron_AtomicLoad(volatile int* value) { return __atomic_load_n(value, __ATOMIC_SEQ_CST); }

void Aeron_AtomicStore(volatile int* value, int newValue) {
	__atomic_store_n(value, newValue, __ATOMIC_SEQ_CST);
}

int Aeron_AtomicAdd(volatile int* value, int delta) {
	return __atomic_fetch_add(value, delta, __ATOMIC_SEQ_CST);
}
#endif
