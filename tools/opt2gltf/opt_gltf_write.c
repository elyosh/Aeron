#include "opt_gltf_write.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cgltf_write.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "tool_log.h"
#include "util.h"

/* The textures are read once more by the cooker. Unfiltered rows at a low
   level write them about twice as fast as stb's defaults, which try every
   filter on each row, and the game's flat-coloured textures come out
   smaller too. The settings are stb's globals, so they are set once for all
   threads. */
static pthread_once_t g_png_settings_once = PTHREAD_ONCE_INIT;

static void set_png_settings(void) {
	stbi_write_png_compression_level = 2;
	stbi_write_force_png_filter      = 0;
}

bool OptGltf_WriteFiles(const OptGltfDocument* document, const char* out_dir, const char* basename) {
	if (!document || !out_dir || !basename)
		return false;
	/* Add no separator when out_dir already ends with one. */
	const size_t len = strlen(out_dir);
	const char*  sep = len && out_dir[len - 1] == '/' ? "" : "/";
	char         tex_dir[1024];
	if (!PathFormat(tex_dir, sizeof tex_dir, "%s%stextures", out_dir, sep) || !MakeDirs(tex_dir))
		return false;

	pthread_once(&g_png_settings_once, set_png_settings);
	const cgltf_data* data = OptGltf_Data((OptGltfDocument*)document);
	for (size_t i = 0; i < data->images_count; ++i) {
		const cgltf_image* image = &data->images[i];
		OptGltfImageView   pixels;
		if (!image->uri || !OptGltf_ImageView(document, image, &pixels))
			continue;
		char png_path[1024];
		if (!PathFormat(png_path, sizeof png_path, "%s%s%s", out_dir, sep, image->uri))
			return false;
		/* Images may be in folders under textures/ (skins/<skin>/). */
		char* slash = strrchr(png_path, '/');
		if (slash && slash > png_path) {
			*slash        = 0;
			const bool ok = MakeDirs(png_path);
			*slash        = '/';
			if (!ok)
				return false;
		}
		if (!stbi_write_png(png_path, (int)pixels.width, (int)pixels.height, 4, pixels.rgba, (int)pixels.width * 4))
			return false;
	}

	char gltf_path[1024], bin_path[1024];
	if (!PathFormat(gltf_path, sizeof gltf_path, "%s%s%s.gltf", out_dir, sep, basename) ||
		!PathFormat(bin_path, sizeof bin_path, "%s%s%s.bin", out_dir, sep, basename))
		return false;

	size_t     json_size = 0;
	char*      json      = OptGltf_Json(document, &json_size);
	const bool written   = json && WriteWholeFile(gltf_path, json, json_size);
	free(json);
	if (!written)
		return false;
	const cgltf_buffer* buffer = &data->buffers[0];
	if (!WriteWholeFile(bin_path, buffer->data, buffer->size))
		return false;

	size_t primitives = 0;
	for (size_t m = 0; m < data->meshes_count; ++m)
		primitives += data->meshes[m].primitives_count;
	ToolLog_Write(TOOL_LOG_INFO, "%s -> %s", basename, gltf_path);
	ToolLog_Write(TOOL_LOG_INFO, "  meshes=%zu primitives=%zu vertices=(per-mesh) variants=%zu",
				  data->meshes_count, primitives, data->variants_count);
	ToolLog_Write(TOOL_LOG_INFO, "  textures=%zu materials=%zu bin=%zu bytes", data->textures_count,
				  data->materials_count, buffer->size);
	return true;
}

/* Reports the warnings of the conversion under the label (log_context). */
static void report_warning(void* label, bool warning, const char* message) {
	if (warning)
		Report((const char*)label, "%s", message);
}

char* OptGltf_Json(const OptGltfDocument* document, size_t* size) {
	const cgltf_data* data     = OptGltf_Data((OptGltfDocument*)document);
	cgltf_options     options  = { 0 };
	const cgltf_size  expected = cgltf_write(&options, NULL, 0, data);
	char*             json     = expected ? (char*)malloc(expected) : NULL;
	/* The count includes the 0 byte that ends the text. */
	const cgltf_size written = json ? cgltf_write(&options, json, expected, data) : 0;
	if (!written || written > expected) {
		free(json);
		return NULL;
	}
	*size = written - 1;
	return json;
}

bool OptGltf_Build(const opt_file_t* opt, const char* basename, const OptGltfBuildOptions* options,
				   const char* label, OptGltfDocument** document) {
	OptGltfBuildOptions reported = *options;
	reported.log                 = report_warning;
	reported.log_context         = (void*)label;
	opt_error_t error            = { { 0 } };
	if (OptGltf_BuildMemory(opt, basename, &reported, document, &error))
		return true;
	if (error.msg[0])
		Report(label, "%s", error.msg);
	return false;
}
