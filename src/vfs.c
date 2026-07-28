#include "internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct AeronVfsCaseEntry {
	char* name;
	int   ambiguous;
} AeronVfsCaseEntry;

struct AeronVfsCaseDirectory {
	char*                         host_path;
	AeronVfsCaseEntry*            entries;
	size_t                        entry_count;
	struct AeronVfsCaseDirectory* next;
};

typedef struct AeronVfsCaseBuild {
	AeronVfsCaseEntry* entries;
	size_t             count;
	size_t             capacity;
} AeronVfsCaseBuild;

static int Aeron_VfsRootValid(AeronVfsRoot root) {
	return root >= AERON_VFS_ROOT_ASSET && root < AERON_VFS_ROOT_COUNT;
}

static void Aeron_FreeCaseDirectory(AeronVfsCaseDirectory* directory) {
	size_t i;

	if (!directory) {
		return;
	}
	for (i = 0; i < directory->entry_count; ++i) {
		SDL_free(directory->entries[i].name);
	}
	SDL_free(directory->entries);
	SDL_free(directory->host_path);
	SDL_free(directory);
}

static void Aeron_InvalidateCaseCache(AeronVfs* vfs, AeronVfsRoot root) {
	AeronVfsCaseDirectory* directory;

	if (!vfs || !Aeron_VfsRootValid(root)) {
		return;
	}
	directory = vfs->case_directories[root];
	while (directory) {
		AeronVfsCaseDirectory* next = directory->next;
		Aeron_FreeCaseDirectory(directory);
		directory = next;
	}
	vfs->case_directories[root] = NULL;
}

void AeronVfs_DeinitInternal(AeronVfs* vfs) {
	AeronVfsRoot root;

	if (!vfs) {
		return;
	}
	for (root = AERON_VFS_ROOT_ASSET; root < AERON_VFS_ROOT_COUNT; root = (AeronVfsRoot)(root + 1)) {
		Aeron_InvalidateCaseCache(vfs, root);
		vfs->root_options[root] = AERON_VFS_ROOT_OPTION_NONE;
	}
}

static SDL_EnumerationResult SDLCALL Aeron_CollectCaseEntry(void* userdata, const char* dirname,
															const char* filename) {
	AeronVfsCaseBuild* build = (AeronVfsCaseBuild*)userdata;
	AeronVfsCaseEntry* grown;
	char*              name;
	size_t             new_capacity;

	(void)dirname;
	if (build->count == build->capacity) {
		new_capacity = build->capacity ? build->capacity * 2 : 32;
		if (new_capacity < build->capacity || new_capacity > SIZE_MAX / sizeof(*grown)) {
			return SDL_ENUM_FAILURE;
		}
		grown = (AeronVfsCaseEntry*)SDL_realloc(build->entries, new_capacity * sizeof(*grown));
		if (!grown) {
			return SDL_ENUM_FAILURE;
		}
		build->entries  = grown;
		build->capacity = new_capacity;
	}

	name = SDL_strdup(filename);
	if (!name) {
		return SDL_ENUM_FAILURE;
	}
	build->entries[build->count].name      = name;
	build->entries[build->count].ambiguous = 0;
	++build->count;
	return SDL_ENUM_CONTINUE;
}

static int Aeron_CompareCaseEntries(const void* left, const void* right) {
	const AeronVfsCaseEntry* a      = (const AeronVfsCaseEntry*)left;
	const AeronVfsCaseEntry* b      = (const AeronVfsCaseEntry*)right;
	const int                folded = SDL_strcasecmp(a->name, b->name);

	return folded != 0 ? folded : strcmp(a->name, b->name);
}

static void Aeron_MarkAmbiguousCaseEntries(AeronVfsCaseDirectory* directory) {
	size_t first;

	for (first = 0; first < directory->entry_count;) {
		size_t end = first + 1;
		while (end < directory->entry_count &&
			   SDL_strcasecmp(directory->entries[first].name, directory->entries[end].name) == 0) {
			++end;
		}
		if (end - first > 1) {
			size_t i;
			for (i = first; i < end; ++i) {
				directory->entries[i].ambiguous = 1;
			}
			Aeron_LogWarn("aeron.vfs", "ambiguous case-insensitive names in '%s': '%s' and '%s'",
					  directory->host_path, directory->entries[first].name,
					  directory->entries[first + 1].name);
		}
		first = end;
	}
}

static AeronVfsCaseDirectory* Aeron_GetCaseDirectory(AeronVfs* vfs, AeronVfsRoot root,
													 const char* host_path) {
	AeronVfsCaseDirectory* directory;
	AeronVfsCaseBuild      build;
	size_t                 i;

	for (directory = vfs->case_directories[root]; directory; directory = directory->next) {
		if (strcmp(directory->host_path, host_path) == 0) {
			return directory;
		}
	}

	memset(&build, 0, sizeof(build));
	if (!SDL_EnumerateDirectory(host_path, Aeron_CollectCaseEntry, &build)) {
		for (i = 0; i < build.count; ++i) {
			SDL_free(build.entries[i].name);
		}
		SDL_free(build.entries);
		return NULL;
	}

	directory = (AeronVfsCaseDirectory*)SDL_calloc(1, sizeof(*directory));
	if (!directory) {
		for (i = 0; i < build.count; ++i) {
			SDL_free(build.entries[i].name);
		}
		SDL_free(build.entries);
		return NULL;
	}
	directory->host_path = SDL_strdup(host_path);
	if (!directory->host_path) {
		for (i = 0; i < build.count; ++i) {
			SDL_free(build.entries[i].name);
		}
		SDL_free(build.entries);
		SDL_free(directory);
		return NULL;
	}
	directory->entries     = build.entries;
	directory->entry_count = build.count;
	if (directory->entry_count > 1) {
		qsort(directory->entries, directory->entry_count, sizeof(*directory->entries),
			  Aeron_CompareCaseEntries);
		Aeron_MarkAmbiguousCaseEntries(directory);
	}
	directory->next             = vfs->case_directories[root];
	vfs->case_directories[root] = directory;
	return directory;
}

static int Aeron_FindCaseEntry(const AeronVfsCaseDirectory* directory, const char* requested,
							   const char** actual) {
	size_t low;
	size_t high;

	if (!directory || !requested || !actual) {
		return 0;
	}
	low  = 0;
	high = directory->entry_count;
	while (low < high) {
		const size_t middle = low + (high - low) / 2;
		if (SDL_strcasecmp(directory->entries[middle].name, requested) < 0) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	if (low >= directory->entry_count || SDL_strcasecmp(directory->entries[low].name, requested) != 0 ||
		directory->entries[low].ambiguous) {
		return 0;
	}
	*actual = directory->entries[low].name;
	return 1;
}

static int Aeron_AppendPath(char* dst, size_t dst_size, const char* base, const char* component) {
	const size_t base_len = strlen(base);
	const int    length   = SDL_snprintf(
		dst, dst_size, "%s%s%s", base,
		(base_len && base[base_len - 1] != '/' && base[base_len - 1] != '\\') ? "/" : "", component);

	return length >= 0 && (size_t)length < dst_size;
}

static int Aeron_RootUsesCaseInsensitiveReads(const AeronVfs* vfs, AeronVfsRoot root) {
	return vfs && Aeron_VfsRootValid(root) &&
		   (vfs->root_options[root] & AERON_VFS_ROOT_OPTION_CASE_INSENSITIVE_READ_LOOKUP) != 0;
}

static void Aeron_SetDefaultPath(char* dst, size_t dst_size, const char* src) {
	char* current_dir;

	if (src && src[0]) {
		Aeron_CopyString(dst, dst_size, src);
		return;
	}

	current_dir = SDL_GetCurrentDirectory();
	if (current_dir) {
		Aeron_CopyString(dst, dst_size, current_dir);
		SDL_free(current_dir);
		return;
	}

	Aeron_CopyString(dst, dst_size, ".");
}

static void Aeron_NormalizePath(char* path) {
	if (!path) {
		return;
	}

	for (; *path; ++path) {
		if (*path == '\\') {
			*path = '/';
		}
	}
}

static const char* Aeron_SkipRootSeparators(const char* path) {
	if (!path) {
		return path;
	}

	while (*path == '/' || *path == '\\') {
		++path;
	}

	return path;
}

void Aeron_InitVfs(const AeronConfig* config) {
	AeronVfsConfig vfs_config;

	memset(&vfs_config, 0, sizeof(vfs_config));
	vfs_config.org_name      = config ? config->org_name : NULL;
	vfs_config.app_name      = config ? config->app_name : NULL;
	vfs_config.asset_root    = config ? config->asset_root : NULL;
	vfs_config.resource_root = config ? config->resource_root : NULL;
	vfs_config.user_root     = NULL;
	vfs_config.temp_root     = NULL;

	AeronVfs_Init(&g_aeron.vfs, &vfs_config);
}

void AeronVfs_Init(AeronVfs* vfs, const AeronVfsConfig* config) {
	char* pref_path;

	if (!vfs) {
		return;
	}
	memset(vfs->root_options, 0, sizeof(vfs->root_options));
	memset(vfs->case_directories, 0, sizeof(vfs->case_directories));

	Aeron_SetDefaultPath(vfs->asset_root, sizeof(vfs->asset_root), config ? config->asset_root : NULL);
	Aeron_NormalizePath(vfs->asset_root);
	Aeron_SetDefaultPath(vfs->resource_root, sizeof(vfs->resource_root),
						 config ? config->resource_root : NULL);
	Aeron_NormalizePath(vfs->resource_root);

	if (config && config->user_root) {
		Aeron_SetDefaultPath(vfs->user_root, sizeof(vfs->user_root), config->user_root);
	} else {
		pref_path = SDL_GetPrefPath(config && config->org_name ? config->org_name : "aeron",
									config && config->app_name ? config->app_name : "aeron");
		if (pref_path) {
			Aeron_CopyString(vfs->user_root, sizeof(vfs->user_root), pref_path);
			SDL_free(pref_path);
		} else {
			Aeron_SetDefaultPath(vfs->user_root, sizeof(vfs->user_root), NULL);
		}
	}
	Aeron_NormalizePath(vfs->user_root);

	if (config && config->temp_root) {
		Aeron_SetDefaultPath(vfs->temp_root, sizeof(vfs->temp_root), config->temp_root);
	} else {
		Aeron_CopyString(vfs->temp_root, sizeof(vfs->temp_root), vfs->user_root);
	}
	Aeron_NormalizePath(vfs->temp_root);
}

int AeronVfs_SetRoot(AeronVfs* vfs, AeronVfsRoot root, const char* path) {
	char*  destination;
	size_t capacity;
	if (!vfs || !path || !path[0]) {
		return 0;
	}
	switch (root) {
		case AERON_VFS_ROOT_ASSET:
			destination = vfs->asset_root;
			capacity    = sizeof vfs->asset_root;
			break;
		case AERON_VFS_ROOT_RESOURCE:
			destination = vfs->resource_root;
			capacity    = sizeof vfs->resource_root;
			break;
		default:
			return 0;
	}
	if (strlen(path) >= capacity) {
		return 0;
	}
	Aeron_CopyString(destination, capacity, path);
	Aeron_NormalizePath(destination);
	Aeron_InvalidateCaseCache(vfs, root);
	return 1;
}

int AeronVfs_SetRootOptions(AeronVfs* vfs, AeronVfsRoot root, uint32_t options) {
	const uint32_t supported = AERON_VFS_ROOT_OPTION_CASE_INSENSITIVE_READ_LOOKUP;

	if (!vfs || !Aeron_VfsRootValid(root) || (options & ~supported) != 0) {
		return 0;
	}
	if (vfs->root_options[root] != options) {
		Aeron_InvalidateCaseCache(vfs, root);
		vfs->root_options[root] = options;
	}
	return 1;
}

AeronVfs* AeronVfs_Create(const AeronVfsConfig* config) {
	AeronVfs* vfs;

	vfs = (AeronVfs*)SDL_calloc(1, sizeof(*vfs));
	if (!vfs) {
		return NULL;
	}

	AeronVfs_Init(vfs, config);
	return vfs;
}

void AeronVfs_Destroy(AeronVfs* vfs) {
	if (vfs) {
		AeronVfs_DeinitInternal(vfs);
		SDL_free(vfs);
	}
}

static const char* Aeron_VfsRootPath(AeronVfs* vfs, AeronVfsRoot root) {
	if (!vfs) {
		return NULL;
	}

	switch (root) {
		case AERON_VFS_ROOT_ASSET:
			return vfs->asset_root;
		case AERON_VFS_ROOT_USER:
			return vfs->user_root;
		case AERON_VFS_ROOT_TEMP:
			return vfs->temp_root;
		case AERON_VFS_ROOT_RESOURCE:
			return vfs->resource_root;
		default:
			return NULL;
	}
}

static int Aeron_BuildPath(AeronVfs* vfs, AeronVfsRoot root, const char* path, char* dst, size_t dst_size) {
	const char* base;
	const char* relative_path;
	size_t      base_len;
	int         length;

	if (!dst || !dst_size || !path) {
		return 0;
	}

	base = Aeron_VfsRootPath(vfs, root);
	if (!base || !base[0]) {
		return 0;
	}

	relative_path = Aeron_SkipRootSeparators(path);
	base_len      = strlen(base);
	if (base_len > 0 && (base[base_len - 1] == '/' || base[base_len - 1] == '\\')) {
		length = SDL_snprintf(dst, dst_size, "%s%s", base, relative_path);
	} else {
		length = SDL_snprintf(dst, dst_size, "%s/%s", base, relative_path);
	}
	if (length < 0 || (size_t)length >= dst_size) {
		return 0;
	}

	Aeron_NormalizePath(dst);
	return 1;
}

static int Aeron_ResolveExistingReadPath(AeronVfs* vfs, AeronVfsRoot root, const char* path, char* resolved,
										 size_t resolved_size) {
	char        relative[AERON_MAX_PATH];
	char        current[AERON_MAX_PATH];
	char        candidate[AERON_MAX_PATH];
	char*       cursor;
	const char* base;

	if (!resolved || !resolved_size || !path || !Aeron_RootUsesCaseInsensitiveReads(vfs, root)) {
		return 0;
	}
	base = Aeron_VfsRootPath(vfs, root);
	if (!base || !base[0] || strlen(base) >= sizeof(current)) {
		return 0;
	}
	Aeron_CopyString(current, sizeof(current), base);
	Aeron_NormalizePath(current);

	path = Aeron_SkipRootSeparators(path);
	if (strlen(path) >= sizeof(relative)) {
		return 0;
	}
	Aeron_CopyString(relative, sizeof(relative), path);
	Aeron_NormalizePath(relative);
	cursor = relative;
	while (*cursor) {
		AeronVfsCaseDirectory* directory;
		SDL_PathInfo           info;
		const char*            component = cursor;
		const char*            actual;
		int                    is_last;

		while (*cursor && *cursor != '/') {
			++cursor;
		}
		if (*cursor) {
			*cursor++ = '\0';
			while (*cursor == '/') {
				++cursor;
			}
		}
		is_last = *cursor == '\0';
		if (!component[0]) {
			continue;
		}

		if (!Aeron_AppendPath(candidate, sizeof(candidate), current, component)) {
			return 0;
		}
		if (!SDL_GetPathInfo(candidate, &info)) {
			if (strcmp(component, ".") == 0 || strcmp(component, "..") == 0) {
				return 0;
			}
			directory = Aeron_GetCaseDirectory(vfs, root, current);
			if (!Aeron_FindCaseEntry(directory, component, &actual) ||
				!Aeron_AppendPath(candidate, sizeof(candidate), current, actual) ||
				!SDL_GetPathInfo(candidate, &info)) {
				return 0;
			}
		}
		if (!is_last && info.type != SDL_PATHTYPE_DIRECTORY) {
			return 0;
		}
		Aeron_CopyString(current, sizeof(current), candidate);
	}

	if (strlen(current) >= resolved_size) {
		return 0;
	}
	Aeron_CopyString(resolved, resolved_size, current);
	return 1;
}

static const char* Aeron_OpenModeString(AeronVfsOpenMode mode) {
	switch (mode) {
		case AERON_VFS_READ:
			return "rb";
		case AERON_VFS_WRITE:
			return "wb";
		case AERON_VFS_READ_WRITE:
			return "r+b";
		case AERON_VFS_WRITE_READ:
			return "w+b";
		case AERON_VFS_APPEND:
			return "ab";
		default:
			return "rb";
	}
}

int AeronVfs_Open(AeronVfs* vfs, AeronVfsRoot root, const char* path, AeronVfsOpenMode mode,
				  AeronFile** out_file) {
	char       host_path[AERON_MAX_PATH];
	char       exact_error[256];
	AeronFile* file;

	if (!out_file) {
		return 0;
	}
	*out_file = NULL;
	if ((root == AERON_VFS_ROOT_RESOURCE && mode != AERON_VFS_READ) ||
		!Aeron_BuildPath(vfs, root, path, host_path, sizeof(host_path))) {
		return 0;
	}

	file = (AeronFile*)SDL_calloc(1, sizeof(*file));
	if (!file) {
		return 0;
	}

	file->stream = SDL_IOFromFile(host_path, Aeron_OpenModeString(mode));
	if (!file->stream && mode == AERON_VFS_READ && Aeron_RootUsesCaseInsensitiveReads(vfs, root)) {
		SDL_PathInfo exact_info;
		int          retried = 0;

		Aeron_CopyString(exact_error, sizeof(exact_error), SDL_GetError());
		if (!SDL_GetPathInfo(host_path, &exact_info) &&
			Aeron_ResolveExistingReadPath(vfs, root, path, host_path, sizeof(host_path))) {
			retried      = 1;
			file->stream = SDL_IOFromFile(host_path, Aeron_OpenModeString(mode));
		}
		if (!file->stream && !retried && exact_error[0]) {
			SDL_SetError("%s", exact_error);
		}
	}
	if (!file->stream) {
		SDL_free(file);
		return 0;
	}

	*out_file = file;
	return 1;
}

int AeronVfs_Read(AeronFile* file, void* dst, size_t size, size_t* out_read) {
	size_t count;

	if (!file || !file->stream || !dst) {
		return 0;
	}

	count = SDL_ReadIO(file->stream, dst, size);
	if (out_read) {
		*out_read = count;
	}

	return count == size;
}

int AeronVfs_ReadAll(AeronVfs* vfs, AeronVfsRoot root, const char* path, size_t max_size,
					 uint8_t** out_data, size_t* out_size) {
	AeronFile* file = NULL;
	if (!out_data || !out_size)
		return 0;
	*out_data = NULL;
	*out_size = 0;
	if (!AeronVfs_Open(vfs, root, path, AERON_VFS_READ, &file))
		return 0;
	const int64_t file_size = AeronVfs_GetSize(file);
	if (file_size <= 0 || (uint64_t)file_size > SIZE_MAX ||
		(max_size && (uint64_t)file_size > max_size)) {
		AeronVfs_Close(file);
		return 0;
	}
	uint8_t* data = (uint8_t*)malloc((size_t)file_size);
	if (!data) {
		AeronVfs_Close(file);
		return 0;
	}
	size_t read_size = 0;
	const int read_ok = AeronVfs_Read(file, data, (size_t)file_size, &read_size);
	const int close_ok = AeronVfs_Close(file);
	if (!read_ok || !close_ok || read_size != (size_t)file_size) {
		free(data);
		return 0;
	}
	*out_data = data;
	*out_size = (size_t)file_size;
	return 1;
}

int AeronVfs_Write(AeronFile* file, const void* src, size_t size, size_t* out_written) {
	size_t count;

	if (!file || !file->stream || !src) {
		return 0;
	}

	count = SDL_WriteIO(file->stream, src, size);
	if (out_written) {
		*out_written = count;
	}

	return count == size;
}

int AeronVfs_Seek(AeronFile* file, int64_t offset, int origin) {
	SDL_IOWhence whence;

	if (!file || !file->stream) {
		return 0;
	}

	switch (origin) {
		case 0:
			whence = SDL_IO_SEEK_SET;
			break;
		case 1:
			whence = SDL_IO_SEEK_CUR;
			break;
		case 2:
			whence = SDL_IO_SEEK_END;
			break;
		default:
			return 0;
	}

	return SDL_SeekIO(file->stream, (Sint64)offset, whence) >= 0;
}

int64_t AeronVfs_Tell(AeronFile* file) {
	if (!file || !file->stream) {
		return -1;
	}

	return (int64_t)SDL_TellIO(file->stream);
}

int64_t AeronVfs_GetSize(AeronFile* file) {
	int64_t current;
	int64_t size;

	if (!file || !file->stream) {
		return -1;
	}

	size = (int64_t)SDL_GetIOSize(file->stream);
	if (size >= 0) {
		return size;
	}

	current = AeronVfs_Tell(file);
	if (current < 0) {
		return -1;
	}

	if (!AeronVfs_Seek(file, 0, 2)) {
		return -1;
	}

	size = AeronVfs_Tell(file);
	AeronVfs_Seek(file, current, 0);
	return size;
}

int AeronVfs_ReadLine(AeronFile* file, char* dst, size_t dst_size) {
	size_t count;

	if (!file || !file->stream || !dst || dst_size == 0) {
		return 0;
	}

	count = 0;
	while (count + 1 < dst_size) {
		char   ch;
		size_t bytes_read;

		bytes_read = SDL_ReadIO(file->stream, &ch, 1);
		if (bytes_read != 1) {
			break;
		}

		if (ch == '\n' && count != 0 && dst[count - 1] == '\r') {
			dst[count - 1] = '\n';
			break;
		}

		dst[count++] = ch;
		if (ch == '\n') {
			break;
		}
	}

	dst[count] = '\0';
	return count != 0;
}

int AeronVfs_Flush(AeronFile* file) {
	if (!file || !file->stream) {
		return 0;
	}

	return SDL_FlushIO(file->stream);
}

int AeronVfs_Close(AeronFile* file) {
	int result;

	if (!file) {
		return 0;
	}

	result = file->stream ? SDL_CloseIO(file->stream) : 1;
	SDL_free(file);
	return result;
}

int AeronVfs_Stat(AeronVfs* vfs, AeronVfsRoot root, const char* path, AeronFileInfo* out_info) {
	char         host_path[AERON_MAX_PATH];
	SDL_PathInfo info;

	if (!out_info || !Aeron_BuildPath(vfs, root, path, host_path, sizeof(host_path))) {
		return 0;
	}

	memset(out_info, 0, sizeof(*out_info));
	if (!SDL_GetPathInfo(host_path, &info)) {
		if (!Aeron_ResolveExistingReadPath(vfs, root, path, host_path, sizeof(host_path)) ||
			!SDL_GetPathInfo(host_path, &info)) {
			return 0;
		}
	}

	out_info->exists       = info.type != SDL_PATHTYPE_NONE;
	out_info->is_directory = info.type == SDL_PATHTYPE_DIRECTORY;
	out_info->size         = (int64_t)info.size;
	return 1;
}

int AeronVfs_Exists(AeronVfs* vfs, AeronVfsRoot root, const char* path) {
	AeronFileInfo info;

	return AeronVfs_Stat(vfs, root, path, &info) && info.exists;
}

int AeronVfs_Remove(AeronVfs* vfs, AeronVfsRoot root, const char* path) {
	char host_path[AERON_MAX_PATH];

	if (root == AERON_VFS_ROOT_RESOURCE || !Aeron_BuildPath(vfs, root, path, host_path, sizeof(host_path))) {
		return 0;
	}

	return SDL_RemovePath(host_path) ? 1 : 0;
}

int AeronVfs_Rename(AeronVfs* vfs, AeronVfsRoot root, const char* old_path, const char* new_path) {
	char old_host_path[AERON_MAX_PATH];
	char new_host_path[AERON_MAX_PATH];

	if (root == AERON_VFS_ROOT_RESOURCE ||
		!Aeron_BuildPath(vfs, root, old_path, old_host_path, sizeof(old_host_path)) ||
		!Aeron_BuildPath(vfs, root, new_path, new_host_path, sizeof(new_host_path))) {
		return 0;
	}

	return SDL_RenamePath(old_host_path, new_host_path) ? 1 : 0;
}

int AeronVfs_Glob(AeronVfs* vfs, AeronVfsRoot root, const char* directory, const char* pattern,
				  uint32_t flags, AeronVfsGlobCallback callback, void* userdata) {
	char          host_path[AERON_MAX_PATH];
	SDL_GlobFlags glob_flags;
	char**        matches;
	int           count;
	int           i;
	int           want_files;
	int           want_directories;
	int           result;

	if (callback == NULL) {
		return 0;
	}

	if (directory == NULL || directory[0] == '\0') {
		directory = ".";
	}

	if (pattern == NULL || pattern[0] == '\0') {
		pattern = "*";
	}

	if (!Aeron_BuildPath(vfs, root, directory, host_path, sizeof(host_path))) {
		return 0;
	}

	glob_flags = 0;
	if (flags & AERON_VFS_GLOB_CASE_INSENSITIVE) {
		glob_flags |= SDL_GLOB_CASEINSENSITIVE;
	}

	count   = 0;
	matches = SDL_GlobDirectory(host_path, pattern, glob_flags, &count);
	if (matches == NULL && Aeron_RootUsesCaseInsensitiveReads(vfs, root)) {
		char         exact_error[256];
		SDL_PathInfo exact_info;
		int          retried = 0;

		Aeron_CopyString(exact_error, sizeof(exact_error), SDL_GetError());
		if (!SDL_GetPathInfo(host_path, &exact_info) &&
			Aeron_ResolveExistingReadPath(vfs, root, directory, host_path, sizeof(host_path))) {
			retried = 1;
			matches = SDL_GlobDirectory(host_path, pattern, glob_flags, &count);
		}
		if (matches == NULL && !retried && exact_error[0]) {
			SDL_SetError("%s", exact_error);
		}
	}
	if (matches == NULL) {
		return 0;
	}

	want_files       = (flags & AERON_VFS_GLOB_FILES) != 0;
	want_directories = (flags & AERON_VFS_GLOB_DIRECTORIES) != 0;
	if (!want_files && !want_directories) {
		want_files       = 1;
		want_directories = 1;
	}

	result = 1;
	for (i = 0; i < count; ++i) {
		char          entry_path[AERON_MAX_PATH];
		SDL_PathInfo  info;
		AeronVfsEntry entry;
		int           length;

		length = SDL_snprintf(entry_path, sizeof(entry_path), "%s/%s", host_path, matches[i]);
		if (length < 0 || (size_t)length >= sizeof(entry_path)) {
			result = 0;
			break;
		}

		if (!SDL_GetPathInfo(entry_path, &info)) {
			continue;
		}

		if ((info.type == SDL_PATHTYPE_FILE && !want_files) ||
			(info.type == SDL_PATHTYPE_DIRECTORY && !want_directories) || info.type == SDL_PATHTYPE_OTHER ||
			info.type == SDL_PATHTYPE_NONE) {
			continue;
		}

		entry.name         = matches[i];
		entry.size         = (int64_t)info.size;
		entry.is_directory = info.type == SDL_PATHTYPE_DIRECTORY;

		if (!callback(userdata, &entry)) {
			result = 0;
			break;
		}
	}

	SDL_free(matches);
	return result;
}
