#ifndef AERON_LOG_H
#define AERON_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Writes a formatted message through SDL_Log using category or "aeron" when NULL. */
void Aeron_Log(const char* category, const char* fmt, ...);
/* Shows a modal fatal-error message box attached to the Aeron window when available. */
void Aeron_FatalError(const char* title, const char* message);

#ifdef __cplusplus
}
#endif

#endif
