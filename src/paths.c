#include "internal.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#else
#include <unistd.h>
#endif

const char* Aeron_UserPath(void) { return g_aeron.vfs.user_root; }

const char* Aeron_AssetRoot(void) { return g_aeron.vfs.asset_root; }

const char* Aeron_ResourceRoot(void) { return g_aeron.vfs.resource_root; }

const char* Aeron_ModRoot(void) { return g_aeron.vfs.mod_root; }

int Aeron_LocalDataPath(char* out, size_t capacity) {
	int length;

	if (!out || !capacity) {
		SDL_SetError("invalid local data path buffer");
		return 0;
	}
#ifdef _WIN32
	{
		const char* local = SDL_getenv("LOCALAPPDATA");
		if (local && local[0]) {
			length = SDL_snprintf(out, capacity, "%s/%s/%s/", local, g_aeron.org_name, g_aeron.app_name);
			if (length < 0 || (size_t)length >= capacity) {
				SDL_SetError("local data path is too long");
				return 0;
			}
			for (char* c = out; *c; ++c) {
				if (*c == '\\') {
					*c = '/';
				}
			}
			return 1;
		}
	}
#endif
	length = SDL_snprintf(out, capacity, "%s", g_aeron.vfs.user_root);
	if (length <= 0 || (size_t)length >= capacity) {
		SDL_SetError("local data path is unavailable");
		return 0;
	}
	return 1;
}

int Aeron_ApplicationPath(const char* relative_path, char* out, size_t capacity) {
	const char* base_path;
	int         length;

	if (!relative_path || !relative_path[0] || !out || !capacity) {
		SDL_SetError("invalid application-relative path");
		return 0;
	}
	base_path = SDL_GetBasePath();
	if (!base_path) {
		return 0;
	}
	length = SDL_snprintf(out, capacity, "%s%s", base_path, relative_path);
	if (length < 0 || (size_t)length >= capacity) {
		SDL_SetError("application-relative path is too long");
		return 0;
	}
	return 1;
}

int Aeron_ExecutablePath(char* out, size_t capacity) {
	if (!out || !capacity) {
		SDL_SetError("invalid executable path buffer");
		return 0;
	}
#if defined(_WIN32)
	{
		wchar_t     wide[AERON_MAX_PATH];
		const DWORD length = GetModuleFileNameW(NULL, wide, AERON_MAX_PATH);
		if (length == 0 || length >= AERON_MAX_PATH) {
			SDL_SetError("cannot read the executable path");
			return 0;
		}
		if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)capacity, NULL, NULL)) {
			SDL_SetError("executable path is too long");
			return 0;
		}
	}
#elif defined(__APPLE__)
	{
		char     path[PATH_MAX];
		char     resolved[PATH_MAX];
		uint32_t size = sizeof path;
		if (_NSGetExecutablePath(path, &size) != 0 || !realpath(path, resolved)) {
			SDL_SetError("cannot read the executable path");
			return 0;
		}
		if (SDL_strlcpy(out, resolved, capacity) >= capacity) {
			SDL_SetError("executable path is too long");
			return 0;
		}
	}
#else
	{
		const ssize_t length = readlink("/proc/self/exe", out, capacity);
		if (length <= 0 || (size_t)length >= capacity) {
			SDL_SetError("cannot read the executable path");
			return 0;
		}
		out[length] = '\0';
	}
#endif
	return 1;
}

static int is_absolute_path(const char* path) {
#ifdef _WIN32
	/* C:\..., \\server\..., and \... on the current drive. */
	if (SDL_isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
		return 1;
	}
	return path[0] == '\\' || path[0] == '/';
#else
	return path[0] == '/';
#endif
}

int Aeron_AbsolutePath(const char* path, char* out, size_t capacity) {
	char* current;
	int   length;

	if (!path || !path[0] || !out || !capacity) {
		SDL_SetError("invalid path");
		return 0;
	}
	if (is_absolute_path(path)) {
		if (SDL_strlcpy(out, path, capacity) >= capacity) {
			SDL_SetError("path is too long");
			return 0;
		}
		return 1;
	}
	/* Ends with a separator. */
	current = SDL_GetCurrentDirectory();
	if (!current) {
		return 0;
	}
	if (path[0] == '.' && (path[1] == '/' || path[1] == '\\')) {
		path += 2;
	}
	length = SDL_snprintf(out, capacity, "%s%s", current, path);
	SDL_free(current);
	if (length < 0 || (size_t)length >= capacity) {
		SDL_SetError("path is too long");
		return 0;
	}
	return 1;
}

/* Windows opens the path itself, with backslashes. macOS needs a file URL,
 * with spaces and other characters escaped, and Linux takes one. */
int Aeron_OpenFolder(const char* path) {
	char url[3 * AERON_MAX_PATH + 8];
	int  opened;

	if (!path || !path[0]) {
		SDL_SetError("no folder to open");
		return 0;
	}
#ifdef _WIN32
	if (SDL_strlcpy(url, path, sizeof url) >= sizeof url) {
		SDL_SetError("folder path is too long");
		return 0;
	}
	for (char* c = url; *c; ++c) {
		if (*c == '/') {
			*c = '\\';
		}
	}
#else
	static const char k_hex[] = "0123456789ABCDEF";
	size_t            length  = SDL_strlcpy(url, "file://", sizeof url);

	for (const unsigned char* c = (const unsigned char*)path; *c; ++c) {
		if (length + 4 >= sizeof url) {
			SDL_SetError("folder path is too long");
			return 0;
		}
		if (SDL_isalnum(*c) || SDL_strchr("/-_.~", *c)) {
			url[length++] = (char)*c;
		} else {
			url[length++] = '%';
			url[length++] = k_hex[*c >> 4];
			url[length++] = k_hex[*c & 15];
		}
	}
	url[length] = '\0';
#endif
	opened = SDL_OpenURL(url);
	if (!opened) {
		Aeron_LogError("aeron", "cannot open %s: %s", path, SDL_GetError());
	}
	return opened ? 1 : 0;
}
