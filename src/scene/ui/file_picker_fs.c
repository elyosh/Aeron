#include "file_picker_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static uint64_t picker_hash_path(const char* path) {
	uint64_t hash = UINT64_C(1469598103934665603);
	for (const unsigned char* p = (const unsigned char*)path; *p; p++) {
		hash ^= *p;
		hash *= UINT64_C(1099511628211);
	}
	return hash ? hash : 1;
}

static size_t picker_root_length(const char* path) {
	if (!path)
		return 0;
	if (path[0] == '/' && path[1] != '/')
		return 1;
	if (isalpha((unsigned char)path[0]) && path[1] == ':' && path[2] == '/')
		return 3;
	if (path[0] == '/' && path[1] == '/') {
		const char* server_end = strchr(path + 2, '/');
		if (!server_end)
			return strlen(path);
		const char* share_end = strchr(server_end + 1, '/');
		return share_end ? (size_t)(share_end - path + 1) : strlen(path);
	}
	return 0;
}

static char* picker_absolute_input(const char* path, const char* base) {
	if (!path || !path[0])
		return NULL;
	char* copy = SDL_strdup(path);
	if (!copy)
		return NULL;
	for (char* p = copy; *p; p++)
		if (*p == '\\')
			*p = '/';
	if (picker_root_length(copy))
		return copy;
	char* owned_base = NULL;
	if (!base || !base[0]) {
		owned_base = SDL_GetCurrentDirectory();
		base       = owned_base;
	}
	if (!base) {
		SDL_free(copy);
		return NULL;
	}
	const size_t base_len = strlen(base);
	const size_t path_len = strlen(copy);
	char*        joined   = (char*)SDL_malloc(base_len + path_len + 2);
	if (joined)
		SDL_snprintf(joined, base_len + path_len + 2, "%s%s%s", base,
					 base_len && base[base_len - 1] == '/' ? "" : "/", copy);
	SDL_free(owned_base);
	SDL_free(copy);
	if (joined)
		for (char* p = joined; *p; p++)
			if (*p == '\\')
				*p = '/';
	return joined;
}

char* picker_path_normalize(const char* path, const char* base) {
	char* input = picker_absolute_input(path, base);
	if (!input)
		return NULL;
	const size_t length = strlen(input);
	const size_t root   = picker_root_length(input);
	if (!root) {
		SDL_free(input);
		return NULL;
	}
	char*   output = (char*)SDL_malloc(length + 2);
	size_t* starts = (size_t*)SDL_malloc((length / 2 + 2) * sizeof *starts);
	if (!output || !starts) {
		SDL_free(starts);
		SDL_free(output);
		SDL_free(input);
		return NULL;
	}
	memcpy(output, input, root);
	size_t out_len         = root;
	size_t component_count = 0;
	for (size_t i = root; i <= length;) {
		while (i < length && input[i] == '/')
			i++;
		const size_t begin = i;
		while (i < length && input[i] != '/')
			i++;
		const size_t part_len = i - begin;
		if (part_len == 0)
			break;
		if (part_len == 1 && input[begin] == '.')
			continue;
		if (part_len == 2 && input[begin] == '.' && input[begin + 1] == '.') {
			if (component_count)
				out_len = starts[--component_count];
			continue;
		}
		if (out_len && output[out_len - 1] != '/')
			output[out_len++] = '/';
		starts[component_count++] = out_len > root ? out_len - 1 : out_len;
		memcpy(output + out_len, input + begin, part_len);
		out_len += part_len;
	}
	while (out_len > root && output[out_len - 1] == '/')
		out_len--;
	output[out_len] = '\0';
	SDL_free(starts);
	SDL_free(input);
	return output;
}

char* picker_path_parent(const char* path) {
	char* normalized = picker_path_normalize(path, NULL);
	if (!normalized)
		return NULL;
	const size_t root   = picker_root_length(normalized);
	size_t       length = strlen(normalized);
	if (length <= root)
		return normalized;
	while (length > root && normalized[length - 1] != '/')
		length--;
	while (length > root && normalized[length - 1] == '/')
		length--;
	if (length < root)
		length = root;
	normalized[length] = '\0';
	return normalized;
}

int picker_path_is_directory(const char* path) {
	SDL_PathInfo info;
	return path && SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

typedef struct PickerFsBuild {
	PickerFsEntry* entries;
	size_t         count;
	size_t         capacity;
	int            failed;
} PickerFsBuild;

static void picker_format_size(char detail[32], Uint64 size) {
	static const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	double             value   = (double)size;
	int                unit    = 0;
	while (value >= 1024.0 && unit < 4) {
		value /= 1024.0;
		unit++;
	}
	if (unit == 0)
		SDL_snprintf(detail, 32, "%llu B", (unsigned long long)size);
	else
		SDL_snprintf(detail, 32, value < 10.0 ? "%.1f %s" : "%.0f %s", value, units[unit]);
}

static int picker_entry_hidden(const char* name, const char* path) {
	if (name[0] == '.')
		return 1;
#if defined(_WIN32)
	const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
	if (length <= 0 || length > PICKER_PATH_CAPACITY)
		return 0;
	wchar_t wide[PICKER_PATH_CAPACITY];
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, length)) {
		return 0;
	}
	const DWORD attributes = GetFileAttributesW(wide);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
#else
	(void)path;
	return 0;
#endif
}

static SDL_EnumerationResult SDLCALL picker_collect(void* user, const char* dirname, const char* fname) {
	PickerFsBuild* build = (PickerFsBuild*)user;
	if (!fname || !fname[0] || strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0)
		return SDL_ENUM_CONTINUE;
	const size_t dir_len  = strlen(dirname);
	const size_t name_len = strlen(fname);
	char*        path     = (char*)SDL_malloc(dir_len + name_len + 1);
	if (!path) {
		build->failed = 1;
		return SDL_ENUM_FAILURE;
	}
	memcpy(path, dirname, dir_len);
	memcpy(path + dir_len, fname, name_len + 1);
	SDL_PathInfo info;
	if (!SDL_GetPathInfo(path, &info) ||
		(info.type != SDL_PATHTYPE_FILE && info.type != SDL_PATHTYPE_DIRECTORY)) {
		SDL_free(path);
		return SDL_ENUM_CONTINUE;
	}
	if (build->count == build->capacity) {
		const size_t capacity = build->capacity ? build->capacity * 2 : 64;
		if (capacity < build->capacity || capacity > SIZE_MAX / sizeof *build->entries) {
			SDL_free(path);
			build->failed = 1;
			return SDL_ENUM_FAILURE;
		}
		PickerFsEntry* grown = (PickerFsEntry*)SDL_realloc(build->entries, capacity * sizeof *build->entries);
		if (!grown) {
			SDL_free(path);
			build->failed = 1;
			return SDL_ENUM_FAILURE;
		}
		build->entries  = grown;
		build->capacity = capacity;
	}
	PickerFsEntry* entry = &build->entries[build->count];
	memset(entry, 0, sizeof *entry);
	entry->name = SDL_strdup(fname);
	entry->path = path;
	if (!entry->name) {
		SDL_free(path);
		build->failed = 1;
		return SDL_ENUM_FAILURE;
	}
	entry->id           = picker_hash_path(path);
	entry->is_directory = info.type == SDL_PATHTYPE_DIRECTORY;
	entry->hidden       = (uint8_t)picker_entry_hidden(fname, path);
	if (!entry->is_directory)
		picker_format_size(entry->detail, info.size);
	build->count++;
	return SDL_ENUM_CONTINUE;
}

static int picker_entry_compare(const void* left, const void* right) {
	const PickerFsEntry* a = (const PickerFsEntry*)left;
	const PickerFsEntry* b = (const PickerFsEntry*)right;
	if (a->is_directory != b->is_directory)
		return a->is_directory ? -1 : 1;
	const int folded = SDL_strcasecmp(a->name, b->name);
	return folded ? folded : strcmp(a->name, b->name);
}

PickerFsResult* picker_fs_enumerate(const char* path, uint64_t generation) {
	PickerFsResult* result = (PickerFsResult*)SDL_calloc(1, sizeof *result);
	if (!result)
		return NULL;
	result->generation  = generation;
	result->path        = SDL_strdup(path);
	PickerFsBuild build = { 0 };
	if (!result->path) {
		SDL_snprintf(result->error, sizeof result->error, "%s", "not enough memory to read this folder");
	} else if (!SDL_EnumerateDirectory(path, picker_collect, &build)) {
		SDL_snprintf(result->error, sizeof result->error, "%s",
					 build.failed ? "not enough memory to read this folder" : SDL_GetError());
	}
	if (result->error[0]) {
		for (size_t i = 0; i < build.count; i++) {
			SDL_free(build.entries[i].name);
			SDL_free(build.entries[i].path);
		}
		SDL_free(build.entries);
	} else {
		if (build.count > 1)
			qsort(build.entries, build.count, sizeof *build.entries, picker_entry_compare);
		result->entries = build.entries;
		result->count   = build.count;
	}
	return result;
}

void picker_fs_result_free(PickerFsResult* result) {
	if (!result)
		return;
	for (size_t i = 0; i < result->count; i++) {
		SDL_free(result->entries[i].name);
		SDL_free(result->entries[i].path);
	}
	SDL_free(result->entries);
	SDL_free(result->path);
	SDL_free(result);
}
