#ifndef AERON_VFS_H
#define AERON_VFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque virtual filesystem instance with asset, resource, user, and temp roots.
 * Instances and their file handles are owner-thread confined: do not perform
 * concurrent operations, or reconfigure/destroy an instance during an operation.
 * A file handle may be moved to another thread while no operation is active;
 * after the move, only the new owner may read, seek, query, or close it. The VFS
 * instance itself remains on its original owner thread. */
typedef struct AeronVfs AeronVfs;

/* Root path configuration for an AeronVfs instance. */
typedef struct AeronVfsConfig {
	const char* org_name;
	const char* app_name;
	const char* asset_root;
	const char* resource_root;
	const char* user_root;
	const char* temp_root;
} AeronVfsConfig;

/* Logical root selected for VFS operations. */
typedef enum AeronVfsRoot {
	AERON_VFS_ROOT_ASSET,
	AERON_VFS_ROOT_USER,
	AERON_VFS_ROOT_TEMP,
	AERON_VFS_ROOT_RESOURCE,
	AERON_VFS_ROOT_COUNT
} AeronVfsRoot;

/* Optional behavior configured independently for each logical root.
 * CASE_INSENSITIVE_LOOKUP resolves the on-disk case of existing path
 * components when an exact-case access fails: reads, stat, and glob require
 * the full path to exist; open-for-write, remove, and rename resolve the
 * existing portion and let a new final component keep its requested case. */
typedef enum AeronVfsRootOptions {
	AERON_VFS_ROOT_OPTION_NONE                    = 0,
	AERON_VFS_ROOT_OPTION_CASE_INSENSITIVE_LOOKUP = 1u << 0
} AeronVfsRootOptions;

/* File open modes mapped to binary SDL_IOStream modes. */
typedef enum AeronVfsOpenMode {
	AERON_VFS_READ,
	AERON_VFS_WRITE,
	AERON_VFS_READ_WRITE,
	AERON_VFS_WRITE_READ,
	AERON_VFS_APPEND
} AeronVfsOpenMode;

/* Opaque VFS file handle. */
typedef struct AeronFile AeronFile;

/* Basic stat result for a resolved VFS path. */
typedef struct AeronFileInfo {
	int64_t size;
	int     exists;
	int     is_directory;
} AeronFileInfo;

/* Filters and matching behavior for AeronVfs_Glob. */
typedef enum AeronVfsGlobFlags {
	AERON_VFS_GLOB_FILES            = 1u << 0,
	AERON_VFS_GLOB_DIRECTORIES      = 1u << 1,
	AERON_VFS_GLOB_CASE_INSENSITIVE = 1u << 2
} AeronVfsGlobFlags;

/* Directory entry passed to AeronVfsGlobCallback; name is valid only during the callback. */
typedef struct AeronVfsEntry {
	const char* name;
	int64_t     size;
	int         is_directory;
} AeronVfsEntry;

/* Callback for AeronVfs_Glob; return zero to stop iteration and make glob fail. */
typedef int (*AeronVfsGlobCallback)(void* userdata, const AeronVfsEntry* entry);

/* Initializes existing VFS storage, normally runtime-owned, with configured or default roots. */
void AeronVfs_Init(AeronVfs* vfs, const AeronVfsConfig* config);
/* Replaces an initialized asset or resource root. User/temp roots are fixed
 * for the VFS lifetime. The path is copied and normalized. */
int AeronVfs_SetRoot(AeronVfs* vfs, AeronVfsRoot root, const char* path);
/* Sets optional lookup behavior for one root. Options and roots must be
 * configured before normal use; AeronVfs instances are owner-thread confined. */
int AeronVfs_SetRootOptions(AeronVfs* vfs, AeronVfsRoot root, uint32_t options);
/* Allocates and initializes a VFS instance; destroy with AeronVfs_Destroy. */
AeronVfs* AeronVfs_Create(const AeronVfsConfig* config);
/* Frees a VFS instance allocated by AeronVfs_Create. */
void AeronVfs_Destroy(AeronVfs* vfs);
/* Opens a file relative to the selected root and writes the handle to out_file. */
int AeronVfs_Open(AeronVfs* vfs, AeronVfsRoot root, const char* path, AeronVfsOpenMode mode,
				  AeronFile** out_file);
/* Reads exactly size bytes unless EOF/error occurs; out_read receives the actual byte count. */
int AeronVfs_Read(AeronFile* file, void* dst, size_t size, size_t* out_read);
/* Reads a complete non-empty file into malloc-owned memory. `max_size == 0`
 * applies no caller limit. The caller releases `out_data` with free(). */
int AeronVfs_ReadAll(AeronVfs* vfs, AeronVfsRoot root, const char* path, size_t max_size,
					 uint8_t** out_data, size_t* out_size);
/* Writes exactly size bytes unless an error occurs; out_written receives the actual byte count. */
int AeronVfs_Write(AeronFile* file, const void* src, size_t size, size_t* out_written);
/* Seeks using origin 0=start, 1=current, or 2=end; returns nonzero on success. */
int AeronVfs_Seek(AeronFile* file, int64_t offset, int origin);
/* Returns the current file offset, or -1 on failure. */
int64_t AeronVfs_Tell(AeronFile* file);
/* Returns the file size, preserving the current offset when fallback seeking is needed. */
int64_t AeronVfs_GetSize(AeronFile* file);
/* Reads a line into dst, preserving a consumed newline and normalizing CRLF to LF. */
int AeronVfs_ReadLine(AeronFile* file, char* dst, size_t dst_size);
/* Flushes pending writes for a file handle. */
int AeronVfs_Flush(AeronFile* file);
/* Closes and frees a file handle. */
int AeronVfs_Close(AeronFile* file);
/* Retrieves path metadata relative to the selected root. */
int AeronVfs_Stat(AeronVfs* vfs, AeronVfsRoot root, const char* path, AeronFileInfo* out_info);
/* Returns nonzero when a path exists relative to the selected root. */
int AeronVfs_Exists(AeronVfs* vfs, AeronVfsRoot root, const char* path);
/* Removes a file or empty directory relative to the selected root. */
int AeronVfs_Remove(AeronVfs* vfs, AeronVfsRoot root, const char* path);
/* Renames a file or directory within the selected root. */
int AeronVfs_Rename(AeronVfs* vfs, AeronVfsRoot root, const char* old_path, const char* new_path);
/* Iterates matching directory entries; with no file/dir filter flags, both kinds are included. */
int AeronVfs_Glob(AeronVfs* vfs, AeronVfsRoot root, const char* directory, const char* pattern,
				  uint32_t flags, AeronVfsGlobCallback callback, void* userdata);

#ifdef __cplusplus
}
#endif

#endif
