#include "opt_gltf_write.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "cgltf_write.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static void path_join(char* out, size_t cap, const char* a, const char* b) {
	size_t la       = strlen(a);
	int    need_sep = (la > 0 && a[la - 1] != '/');
	snprintf(out, cap, "%s%s%s", a, need_sep ? "/" : "", b);
}

/* Creates folder `path` when missing (its parent must exist); 0 when it
 * exists. */
static int ensure_dir(const char* path) {
	struct stat st;
	if (stat(path, &st) == 0)
		return S_ISDIR(st.st_mode) ? 0 : -1;
#if defined(_WIN32)
	return _mkdir(path);
#else
	return mkdir(path, 0755);
#endif
}

bool OptGltf_WriteFiles(const OptGltfDocument* document, const char* out_dir, const char* basename) {
	if (!document || !out_dir || !basename)
		return false;
	if (ensure_dir(out_dir) != 0)
		return false;
	char tex_dir[1024];
	path_join(tex_dir, sizeof tex_dir, out_dir, "textures");
	if (ensure_dir(tex_dir) != 0)
		return false;

	const cgltf_data* data = OptGltf_Data((OptGltfDocument*)document);
	for (size_t i = 0; i < data->images_count; ++i) {
		const cgltf_image* image = &data->images[i];
		OptGltfImageView   pixels;
		if (!image->uri || !OptGltf_ImageView(document, image, &pixels))
			continue;
		char png_path[1024];
		path_join(png_path, sizeof png_path, out_dir, image->uri);
		if (!stbi_write_png(png_path, (int)pixels.width, (int)pixels.height, 4, pixels.rgba,
							(int)pixels.width * 4))
			return false;
	}

	char gltf_path[1024], bin_path[1024], filename[256];
	snprintf(filename, sizeof filename, "%s.gltf", basename);
	path_join(gltf_path, sizeof gltf_path, out_dir, filename);
	snprintf(filename, sizeof filename, "%s.bin", basename);
	path_join(bin_path, sizeof bin_path, out_dir, filename);

	cgltf_options opts = { 0 };
	if (cgltf_write_file(&opts, gltf_path, data) != cgltf_result_success)
		return false;
	FILE* fp = fopen(bin_path, "wb");
	if (!fp)
		return false;
	const cgltf_buffer* buffer       = &data->buffers[0];
	const size_t        wrote        = fwrite(buffer->data, 1, buffer->size, fp);
	const int           close_result = fclose(fp);
	if (wrote != buffer->size || close_result != 0)
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

bool OptGltf_Convert(const opt_file_t* opt, const char* out_dir, const char* basename,
					 const OptGltfBuildOptions* options, const char* label) {
	OptGltfDocument* document = NULL;
	opt_error_t      error    = { { 0 } };
	if (!OptGltf_BuildMemory(opt, basename, options, &document, &error)) {
		if (error.msg[0])
			fprintf(stderr, "opt2gltf: %s: %s\n", label, error.msg);
		return false;
	}
	const bool succeeded = OptGltf_WriteFiles(document, out_dir, basename);
	if (!succeeded)
		fprintf(stderr, "opt2gltf: %s: could not write %s\n", label, out_dir);
	OptGltf_Free(document);
	return succeeded;
}
