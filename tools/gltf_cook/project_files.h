#ifndef AERON_GLTF_COOK_PROJECT_FILES_H
#define AERON_GLTF_COOK_PROJECT_FILES_H

/* File and thread helpers of the project build. They use the C++ standard
 * library, so that the cooker needs no SDL. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lists the names of the folders in `dir`, sorted, without the ones whose
 * names start with '.'. Returns their count, or -1 when `dir` can't be read.
 * ProjectFiles_FreeNames frees the list. */
int  ProjectFiles_ListFolders(const char* dir, char*** names);
void ProjectFiles_FreeNames(char** names, int count);

bool ProjectFiles_IsFile(const char* path);
bool ProjectFiles_IsFolder(const char* path);
/* Creates `path` and its missing parents. */
bool ProjectFiles_MakeFolders(const char* path);
bool ProjectFiles_CopyFile(const char* from, const char* to);

/* The number of hardware threads, at least 1. */
int ProjectFiles_HardwareThreads(void);
/* Runs task(context, index) for each index from 0 to count - 1, on `jobs`
 * threads. */
void ProjectFiles_RunTasks(int count, int jobs, void (*task)(void* context, int index), void* context);

#ifdef __cplusplus
}
#endif

#endif /* AERON_GLTF_COOK_PROJECT_FILES_H */
