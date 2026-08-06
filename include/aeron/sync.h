#ifndef AERON_SYNC_H
#define AERON_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Joinable background thread. The callback runs once and returns the value
   reported by Aeron_ThreadJoin. */
typedef struct AeronThread AeronThread;
typedef int (*AeronThreadFunc)(void* user);

/* Creates a named thread. The name must be non-empty and function must be
   non-NULL. Allocation and platform failures are logged. */
AeronThread* Aeron_ThreadCreate(const char* name, AeronThreadFunc function, void* user);

/* Waits for the callback to finish, releases the thread handle, and returns
   the callback result. Must be called exactly once and not from the thread
   being joined. */
int Aeron_ThreadJoin(AeronThread* thread);

/* Periodic callback on a background thread. Aeron owns the thread; callers only
   supply the callback. */
typedef uint32_t AeronTimer;

/* Starts a periodic timer firing every interval_ms. Returns 0 on failure.
   The callback runs on a background thread and must lock anything it shares
   with the main thread. An interval of 0 is rejected. */
AeronTimer Aeron_TimerCreate(unsigned int interval_ms, void (*callback)(void* user), void* user);

/* Stops the timer and quiesces it: once this returns, the callback is not
   running and will not run again, so resources it touches can be freed.
   Must not be called while holding a lock the callback also takes. */
void Aeron_TimerDestroy(AeronTimer timer);

/* Recursive mutex. */
typedef struct AeronMutex AeronMutex;

AeronMutex* Aeron_MutexCreate(void);
void        Aeron_MutexLock(AeronMutex* mutex);
void        Aeron_MutexUnlock(AeronMutex* mutex);
void        Aeron_MutexDestroy(AeronMutex* mutex);

/* Sequentially consistent access to a plain int shared between threads. `value`
   must be naturally aligned, and once any thread other than the owner can see
   it, only these calls may touch it. Aeron_AtomicAdd returns the previous value. */
int  Aeron_AtomicLoad(volatile int* value);
void Aeron_AtomicStore(volatile int* value, int newValue);
int  Aeron_AtomicAdd(volatile int* value, int delta);

#ifdef __cplusplus
}
#endif

#endif
