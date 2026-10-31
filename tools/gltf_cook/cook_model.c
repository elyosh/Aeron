#include "cook_model.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "flight_data.h"
#include "tool_log.h"

/* Writes `size` bytes to `path`. */
static bool write_file(const char* path, const uint8_t* bytes, size_t size) {
	FILE*      file    = fopen(path, "wb");
	const bool written = file && fwrite(bytes, 1, size, file) == size;
	const bool closed  = file && fclose(file) == 0;
	if (!written || !closed)
		ToolLog_Write(TOOL_LOG_ERROR, "[aeron_gltf_cook] cannot write %s", path);
	return written && closed;
}

bool aeron_gltf_cook_model_input(const AeronGltfCookInput* input, const char* glb,
								 const AeronGltfCookOptions* opts, const char* probes_dir) {
	char temporary[4096];
	if (snprintf(temporary, sizeof temporary, "%s.tmp", glb) >= (int)sizeof temporary) {
		ToolLog_Write(TOOL_LOG_ERROR, "[aeron_gltf_cook] path too long: %s", glb);
		return false;
	}
	uint8_t* bytes = NULL;
	size_t   size  = 0;
	if (!aeron_gltf_cook_glb(input, opts, &bytes, &size))
		return false;
	uint8_t* cooked      = NULL;
	size_t   cooked_size = 0;
	bool     applied     = false;
	char     error[256]  = { 0 };
	bool     ok = aeron_gltf_cook_flight_data(bytes, size, probes_dir, &cooked, &cooked_size, &applied, error,
											  sizeof error);
	if (!ok)
		ToolLog_Write(TOOL_LOG_ERROR, "[aeron_gltf_cook] flight data: %s", error);
	ok = ok && write_file(temporary, applied ? cooked : bytes, applied ? cooked_size : size);
	free(bytes);
	free(cooked);
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

bool aeron_gltf_cook_model(const char* gltf, const char* glb, const AeronGltfCookOptions* opts,
						   const char* probes_dir) {
	FILE* file = fopen(gltf, "rb");
	long  size = -1;
	char* text = NULL;
	if (file && fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0 &&
		(text = (char*)malloc((size_t)size + 1)) != NULL &&
		fread(text, 1, (size_t)size, file) != (size_t)size) {
		free(text);
		text = NULL;
	}
	if (file)
		fclose(file);
	if (!text) {
		ToolLog_Write(TOOL_LOG_ERROR, "[aeron_gltf_cook] cannot read %s", gltf);
		return false;
	}
	text[size]                     = '\0';
	const AeronGltfCookInput input = { .json = text, .json_size = (size_t)size, .label = gltf };
	const bool               ok    = aeron_gltf_cook_model_input(&input, glb, opts, probes_dir);
	free(text);
	return ok;
}
