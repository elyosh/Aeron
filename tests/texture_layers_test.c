/*
 * Tests cube arrays and 3D textures: the KTX2 writer and reader round trip
 * of a cube array, its upload, and the upload of 3D slices. Every texel
 * holds where it belongs (slice, mip, x, y), and the GPU copies are read
 * back and compared. Runs headless (hidden window). The test is built with
 * the tools as aeron_texture_layers_test. It prints each check and exits
 * with a non-zero status when a check fails.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "aeron/aeron.h"
#include "aeron/render.h"
#include "aeron/scene/image_cache.h"
#include "aeron/scene/ktx2_reader.h"
#include "ktx2_writer.h"

static int g_failures;

static void check(int ok, const char* what) {
	printf("%s %s\n", ok ? "ok" : "FAILED", what);
	g_failures += !ok;
}

/* Small integers and halves only: exact in half precision. */
static uint16_t half(float value) {
	if (value == 0.0f)
		return 0;
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	const uint32_t sign     = (bits >> 16) & 0x8000u;
	const int      exponent = (int)((bits >> 23) & 0xffu) - 127 + 15;
	return (uint16_t)(sign | ((uint32_t)exponent << 10) | ((bits >> 13) & 0x3ffu));
}

/* RGBA16F texels: R = the slice's tag, G = mip, B = x, A = y. */
static void fill(uint16_t* texels, int size, float tag, int mip) {
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++) {
			uint16_t* t = texels + ((size_t)y * size + x) * 4;
			t[0]        = half(tag);
			t[1]        = half((float)mip);
			t[2]        = half((float)x);
			t[3]        = half((float)y);
		}
}

enum { FACE_SIZE = 4, MIPS = 2, CUBES = 2, DEPTH = 6 };

/* A cube array of CUBES cubes, slice s tagged s + 1. */
static int write_cube_array(const char* path, int cubes) {
	const int      slices = 6 * cubes;
	uint16_t*      data[MIPS];
	Ktx2WriteLevel levels[MIPS];
	for (int m = 0; m < MIPS; m++) {
		const int    size  = FACE_SIZE >> m;
		const size_t slice = (size_t)size * size * 4;
		data[m]            = (uint16_t*)malloc(slice * slices * sizeof(uint16_t));
		for (int s = 0; s < slices; s++)
			fill(data[m] + slice * s, size, (float)(s + 1), m);
		levels[m] =
			(Ktx2WriteLevel) { size, size, (const uint8_t*)data[m], slice * slices * sizeof(uint16_t) };
	}
	const int ok = write_ktx2_rgba16f_levels(path, 6, cubes, levels, MIPS, true);
	for (int m = 0; m < MIPS; m++)
		free(data[m]);
	return ok;
}

static int slice_matches(const void* bytes, size_t size, int texels, float tag, int mip) {
	uint16_t* expected = (uint16_t*)malloc((size_t)texels * texels * 4 * sizeof(uint16_t));
	fill(expected, texels, tag, mip);
	const int ok =
		size == (size_t)texels * texels * 4 * sizeof(uint16_t) && memcmp(bytes, expected, size) == 0;
	free(expected);
	return ok;
}

static void test_ktx2(const char* path) {
	check(write_cube_array(path, CUBES), "writer: a cube array of 2 cubes");
	Ktx2* ktx = ktx2_open(path);
	check(ktx && ktx2_face_count(ktx) == 6 && ktx2_layer_count(ktx) == CUBES && ktx2_level_count(ktx) == MIPS,
		  "reader: 6 faces, 2 layers, 2 levels");
	int all = ktx != NULL;
	for (int m = 0; all && m < MIPS; m++)
		for (int s = 0; s < 6 * CUBES; s++) {
			const Ktx2Level level = ktx2_level_face(ktx, m, s);
			all = all && slice_matches(level.data, level.size, FACE_SIZE >> m, (float)(s + 1), m);
		}
	check(all, "reader: every slice of every level holds its own texels");
	check(ktx && !ktx2_level_face(ktx, 0, 6 * CUBES).data, "reader: no slice past the last");
	ktx2_close(ktx);

	/* One layer: a cube array of one cube still has 6 slices. */
	check(write_cube_array(path, 1), "writer: a cube array of 1 cube");
	ktx                  = ktx2_open(path);
	const Ktx2Level last = ktx ? ktx2_level_face(ktx, 0, 5) : (Ktx2Level) { 0 };
	check(ktx && ktx2_layer_count(ktx) == 1 && slice_matches(last.data, last.size, FACE_SIZE, 6.0f, 0),
		  "reader: face 5 of a 1-layer cube array");
	ktx2_close(ktx);
}

static int read_matches(AeronTexture* texture, int mip, int layer, int z, float tag) {
	const int size  = FACE_SIZE >> mip;
	size_t    bytes = (size_t)size * size * 4 * sizeof(uint16_t);
	void*     read  = malloc(bytes);
	const int ok    = Aeron_ReadTextureRawPixels(texture, mip, layer, z, read, bytes) &&
					  slice_matches(read, bytes, size, tag, mip);
	free(read);
	return ok;
}

static void test_gpu_cube_array(const char* path) {
	check(write_cube_array(path, CUBES), "writer: the cube array again");
	Ktx2*                  ktx  = ktx2_open(path);
	const AeronTextureDesc desc = { .width     = FACE_SIZE,
									.height    = FACE_SIZE,
									.mip_count = MIPS,
									.format    = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
									.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
									.cube  = 1,
									.layer_count = CUBES };
	check(Aeron_TextureDescSupported(&desc), "device: RGBA16F cube arrays");
	AeronCommandBuffer* cmd     = Aeron_AcquireCommandBuffer();
	AeronTexture*       texture = ktx && cmd ? Aeron_ImageUploadKtx2(cmd, ktx, "test.cube_array") : NULL;
	check(texture != NULL && Aeron_SubmitCommandBuffer(cmd), "upload: the KTX2 cube array");
	int all = texture != NULL;
	for (int m = 0; all && m < MIPS; m++)
		for (int s = 0; s < 6 * CUBES; s++)
			all = all && read_matches(texture, m, s, 0, (float)(s + 1));
	check(all, "upload: layer cube * 6 + face holds that face, at every mip");
	Aeron_DestroyTexture(texture);
	ktx2_close(ktx);

	const AeronTextureDesc bc6h   = { .width     = 256,
									  .height    = 256,
									  .mip_count = 9,
									  .format    = AERON_TEXTURE_FORMAT_BC6H_RGB_UFLOAT,
									  .usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
									  .cube  = 1,
									  .layer_count = 42 };
	AeronTexture*          probes = Aeron_CreateTexture(&bc6h);
	check(Aeron_TextureDescSupported(&bc6h) && probes != NULL,
		  "device: a BC6H cube array of 42 256 px cubes");
	Aeron_DestroyTexture(probes);
}

static void test_gpu_3d(void) {
	const AeronTextureDesc desc = { .width     = FACE_SIZE,
									.height    = FACE_SIZE,
									.mip_count = 1,
									.format    = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
									.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
									.depth = DEPTH };
	check(Aeron_TextureDescSupported(&desc), "device: RGBA16F 3D textures");
	AeronTexture* texture = Aeron_CreateTexture(&desc);
	check(texture != NULL, "create: a 4x4x6 3D texture");

	/* Slices 0-2 then 3-5, each upload several slices deep; slice z tagged
	 * z + 1. */
	const size_t slice = (size_t)FACE_SIZE * FACE_SIZE * 4;
	uint16_t*    data  = (uint16_t*)malloc(slice * DEPTH * sizeof(uint16_t));
	for (int z = 0; z < DEPTH; z++)
		fill(data + slice * z, FACE_SIZE, (float)(z + 1), 0);
	AeronTextureUploadDesc uploads[2];
	for (int u = 0; u < 2; u++)
		uploads[u] = (AeronTextureUploadDesc) { .texture  = texture,
												.width    = FACE_SIZE,
												.height   = FACE_SIZE,
												.raw_data = data + slice * 3 * u,
												.raw_size = (uint32_t)(slice * 3 * sizeof(uint16_t)),
												.z        = 3 * u,
												.depth    = 3 };
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	check(texture && cmd && Aeron_UploadTextureBatchCmd(cmd, uploads, 2) && Aeron_SubmitCommandBuffer(cmd),
		  "upload: two uploads of 3 slices");
	int all = texture != NULL;
	for (int z = 0; all && z < DEPTH; z++)
		all = read_matches(texture, 0, 0, z, (float)(z + 1));
	check(all, "upload: slice z holds slice z");

	/* A slice past the depth, or several from CPU pixels, are refused. */
	AeronTextureUploadDesc past   = uploads[1];
	past.z                        = 4;
	AeronTextureUploadDesc pixels = { .texture      = texture,
									  .width        = FACE_SIZE,
									  .height       = FACE_SIZE,
									  .pixels       = data,
									  .pitch        = FACE_SIZE * 4,
									  .pixel_format = AERON_PIXEL_FORMAT_RGBA8888,
									  .depth        = 2 };
	cmd                           = Aeron_AcquireCommandBuffer();
	const int refused             = cmd && !Aeron_UploadTextureBatchCmd(cmd, &past, 1);
	if (cmd)
		Aeron_CancelCommandBuffer(cmd);
	cmd                      = Aeron_AcquireCommandBuffer();
	const int refused_pixels = cmd && !Aeron_UploadTextureBatchCmd(cmd, &pixels, 1);
	if (cmd)
		Aeron_CancelCommandBuffer(cmd);
	check(refused && refused_pixels, "upload: slices past the depth, or several from pixels, are refused");

	const AeronTextureDesc bad = { .width  = FACE_SIZE,
								   .height = FACE_SIZE,
								   .format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
								   .usage  = AERON_TEXTURE_USAGE_SAMPLED,
								   .cube   = 1,
								   .depth  = DEPTH };
	check(!Aeron_CreateTexture(&bad) && !Aeron_TextureDescSupported(&bad), "create: no 3D cube");
	free(data);
	Aeron_DestroyTexture(texture);
}

int main(void) {
	char        path[1024];
	const char* tmp = getenv("TMPDIR");
	snprintf(path, sizeof path, "%s/aeron_texture_layers_test.ktx2", tmp && tmp[0] ? tmp : "/tmp");
	test_ktx2(path);

	SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
	AeronConfig config = { .org_name      = "Aeron",
						   .app_name      = "TextureLayersTest",
						   .window_title  = "Aeron texture layers test",
						   .window_width  = 64,
						   .window_height = 64,
						   .window_hidden = 1,
						   .shader_path   = TEST_SHADER_DIR };
	if (!Aeron_Init(&config)) {
		check(0, "start Aeron");
	} else {
		test_gpu_cube_array(path);
		test_gpu_3d();
		Aeron_Shutdown();
	}
	remove(path);
	printf("%s\n", g_failures ? "FAILED" : "passed");
	return g_failures ? 1 : 0;
}
