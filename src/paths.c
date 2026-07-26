#include "internal.h"

const char* Aeron_UserPath(void) { return g_aeron.vfs.user_root; }

const char* Aeron_AssetRoot(void) { return g_aeron.vfs.asset_root; }

const char* Aeron_ResourceRoot(void) { return g_aeron.vfs.resource_root; }

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
