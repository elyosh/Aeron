#ifndef AERON_DPLAY_ICE_H
#define AERON_DPLAY_ICE_H
#include "aeron/compat/dplay_directory.h"
#include <stddef.h>

/* Monotonic local identity; zero is no link. A retired identity is never reused. */
typedef uint64_t DpLink;
void             DpIce_Update(void);
int              DpIce_Receive(DpLink* link, void* data, unsigned* size);
/* 1: sent, 0: would block/not connected, -1: failed. */
int              DpIce_Send(DpLink link, const void* data, unsigned size);
int              DpIce_Matches(DpLink link, const GUID* application, const GUID* room);
DpLink           DpIce_Prepared(const GUID* application, const GUID* room);
void             DpIce_Claim(DpLink link);
void             DpIce_Admit(DpLink link);
void             DpIce_Retire(DpLink link);
void             DpIce_CancelJoin(uint64_t generation);
int              DpIce_Active(void);
void             DpIce_Reset(void);
#endif
