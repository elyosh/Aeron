#include "opt_gltf_write.h"

#include <stdio.h>
#include <string.h>

#include "cgltf_write.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "util.h"

bool OptGltf_WriteFiles(const OptGltfDocument* document, const char* out_dir, const char* basename) {
	if (!document || !out_dir || !basename)
		return false;
	/* No separator after one ending out_dir. */
	const size_t len = strlen(out_dir);
	const char*  sep = len && out_dir[len - 1] == '/' ? "" : "/";
	char         tex_dir[1024];
	if (!PathFormat(tex_dir, sizeof tex_dir, "%s%stextures", out_dir, sep) || !MakeDirs(tex_dir))
		return false;

	const cgltf_data* data = OptGltf_Data((OptGltfDocument*)document);
	for (size_t i = 0; i < data->images_count; ++i) {
		const cgltf_image* image = &data->images[i];
		OptGltfImageView   pixels;
		if (!image->uri || !OptGltf_ImageView(document, image, &pixels))
			continue;
		char png_path[1024];
		if (!PathFormat(png_path, sizeof png_path, "%s%s%s", out_dir, sep, image->uri) ||
			!stbi_write_png(png_path, (int)pixels.width, (int)pixels.height, 4, pixels.rgba,
							(int)pixels.width * 4))
			return false;
	}

	char gltf_path[1024], bin_path[1024];
	if (!PathFormat(gltf_path, sizeof gltf_path, "%s%s%s.gltf", out_dir, sep, basename) ||
		!PathFormat(bin_path, sizeof bin_path, "%s%s%s.bin", out_dir, sep, basename))
		return false;

	cgltf_options opts = { 0 };
	if (cgltf_write_file(&opts, gltf_path, data) != cgltf_result_success)
		return false;
	const cgltf_buffer* buffer = &data->buffers[0];
	if (!WriteWholeFile(bin_path, buffer->data, buffer->size))
		return false;

	size_t primitives = 0;
	for (size_t m = 0; m < data->meshes_count; ++m)
		primitives += data->meshes[m].primitives_count;
	fprintf(stdout,
			"%s -> %s\n"
			"  meshes=%zu primitives=%zu vertices=(per-mesh) variants=%zu\n"
			"  textures=%zu materials=%zu bin=%zu bytes\n",
			basename, gltf_path, data->meshes_count, primitives, data->variants_count, data->textures_count,
			data->materials_count, buffer->size);
	return true;
}

/* The conversion's warnings, reported under the label (log_context). */
static void report_warning(void* label, bool warning, const char* message) {
	if (warning)
		Report((const char*)label, "%s", message);
}

bool OptGltf_Convert(const opt_file_t* opt, const char* out_dir, const char* basename,
					 const OptGltfBuildOptions* options, const char* label) {
	OptGltfBuildOptions reported = *options;
	reported.log                 = report_warning;
	reported.log_context         = (void*)label;
	OptGltfDocument* document    = NULL;
	opt_error_t      error       = { { 0 } };
	if (!OptGltf_BuildMemory(opt, basename, &reported, &document, &error)) {
		if (error.msg[0])
			Report(label, "%s", error.msg);
		return false;
	}
	const bool succeeded = OptGltf_WriteFiles(document, out_dir, basename);
	if (!succeeded)
		Report(label, "could not write %s", out_dir);
	OptGltf_Free(document);
	return succeeded;
}
