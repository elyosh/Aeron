#include "cook_model.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "flight_data.h"
#include "tool_log.h"

/* Rewrites the cooked GLB `path` with cooked AERON_flight_model data. */
static bool add_flight_data(const char* path, const char* probes_dir) {
	FILE* file = fopen(path, "rb");
	if (!file) {
		ToolLog_Write(TOOL_LOG_ERROR, "[aeron_gltf_cook] cannot reopen %s", path);
		return false;
	}
	fseek(file, 0, SEEK_END);
	const long length = ftell(file);
	rewind(file);
	uint8_t*   bytes = length > 0 ? malloc((size_t)length) : NULL;
	const bool read  = bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length;
	fclose(file);
	if (!read) {
		free(bytes);
		ToolLog_Write(TOOL_LOG_ERROR, "[aeron_gltf_cook] cannot read %s", path);
		return false;
	}
	uint8_t*   cooked      = NULL;
	size_t     cooked_size = 0;
	bool       applied     = false;
	char       error[256]  = { 0 };
	const bool ok = aeron_gltf_cook_flight_data(bytes, (size_t)length, probes_dir, &cooked, &cooked_size,
												&applied, error, sizeof error);
	free(bytes);
	if (!ok) {
		ToolLog_Write(TOOL_LOG_ERROR, "[aeron_gltf_cook] flight data: %s", error);
		return false;
	}
	if (!applied)
		return true;
	file               = fopen(path, "wb");
	const bool written = file && fwrite(cooked, 1, cooked_size, file) == cooked_size;
	const bool closed  = file && fclose(file) == 0;
	free(cooked);
	if (!written || !closed)
		ToolLog_Write(TOOL_LOG_ERROR, "[aeron_gltf_cook] cannot write %s", path);
	return written && closed;
}

bool aeron_gltf_cook_model(const char* gltf, const char* glb, const AeronGltfCookOptions* opts,
						   const char* probes_dir) {
	char temporary[4096];
	if (snprintf(temporary, sizeof temporary, "%s.tmp", glb) >= (int)sizeof temporary) {
		ToolLog_Write(TOOL_LOG_ERROR, "[aeron_gltf_cook] path too long: %s", glb);
		return false;
	}
	bool ok = aeron_gltf_cook(gltf, temporary, opts) && add_flight_data(temporary, probes_dir);
#ifdef _WIN32
	/* Windows' rename doesn't replace an existing file. */
	if (ok)
		remove(glb);
#endif
	if (ok && rename(temporary, glb) != 0) {
		ToolLog_Write(TOOL_LOG_ERROR, "[aeron_gltf_cook] cannot rename %s to %s", temporary, glb);
		ok = false;
	}
	if (!ok)
		remove(temporary);
	return ok;
}
