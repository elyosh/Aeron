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
/* Builds a path relative to the application executable or bundle resources. */
int Aeron_ApplicationPath(const char* relative_path, char* out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
