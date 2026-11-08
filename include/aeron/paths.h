#ifndef AERON_PATHS_H
#define AERON_PATHS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the initialized user-root path from the runtime VFS. */
const char* Aeron_UserPath(void);
/* Returns the initialized asset-root path from the runtime VFS. */
const char* Aeron_AssetRoot(void);
/* Returns the initialized application-resource root path. */
const char* Aeron_ResourceRoot(void);
/* Returns the mod root path, or an empty string when none is set. */
const char* Aeron_ModRoot(void);
/* Writes the folder for large per-user data that stays on this computer, such
 * as installed content, with a final '/'. It is <LOCALAPPDATA>/<org>/<app>/
 * on Windows, since the user folder there is copied between computers on
 * networks with roaming profiles. Elsewhere it is the user folder. The folder
 * may not exist yet. */
int Aeron_LocalDataPath(char* out, size_t capacity);
/* Builds a path relative to the application executable or bundle resources. */
int Aeron_ApplicationPath(const char* relative_path, char* out, size_t capacity);
/* Opens folder `path` in the file manager (Explorer, Finder, or the desktop's
 * file manager). Returns 0 and logs an error when it can't. */
int Aeron_OpenFolder(const char* path);

#ifdef __cplusplus
}
#endif

#endif
