#include "internal.h"

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
