#include "aeron/asset/flight_model.h"
#include "aeron/asset/opt_model.h"
#include "aeron/config_file.h"
#include "aeron/scene/environment.h"
#include "aeron/scene/image_cache.h"
#include "aeron/scene/mesh_overlay.h"
#include "aeron/scene/probe_set.h"
#include "capture.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

static AeronSceneMesh* create_cube(AeronCommandBuffer* cmd) {
	static const float corners[8][3] = { { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
										 { -1, -1, 1 },  { 1, -1, 1 },  { 1, 1, 1 },  { -1, 1, 1 } };
	static const int   faces[6][4]   = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 4, 7, 3 },
										 { 1, 2, 6, 5 }, { 0, 1, 5, 4 }, { 3, 7, 6, 2 } };
	static const float normals[6][3] = { { 0, 0, -1 }, { 0, 0, 1 },  { -1, 0, 0 },
										 { 1, 0, 0 },  { 0, -1, 0 }, { 0, 1, 0 } };
	AeronGltfVertex    vertices[24]  = { 0 };
	uint32_t           indices[36];
	for (int f = 0; f < 6; ++f) {
		for (int v = 0; v < 4; ++v) {
			memcpy(vertices[f * 4 + v].pos, corners[faces[f][v]], sizeof vertices[0].pos);
			memcpy(vertices[f * 4 + v].normal, normals[f], sizeof vertices[0].normal);
			vertices[f * 4 + v].tangent[3] = 1;
		}
		const int triangle[6] = { 0, 1, 2, 0, 2, 3 };
		for (int i = 0; i < 6; ++i)
			indices[f * 6 + i] = (uint32_t)(f * 4 + triangle[i]);
	}
	AeronGltfMaterial material = { .base_color_factor     = { .8f, .8f, .8f, 1 },
								   .roughness_factor      = 1,
								   .double_sided          = 1,
								   .specular_factor       = 1,
								   .specular_color_factor = { 1, 1, 1 } };
	uint32_t          variant  = 0;
	AeronFlightModel  model    = { .render     = { .vertices              = vertices,
												   .vertex_count          = 24,
												   .indices               = indices,
												   .index_count           = 36,
												   .opaque_index_count    = 36,
												   .mask_index_offset     = 36,
												   .blend_index_offset    = 36,
												   .material_count        = 1,
												   .materials             = &material,
												   .variant_columns       = 1,
												   .total_prim_count      = 1,
												   .prim_variant_material = &variant },
								   .bounds     = { .min = { -1, -1, -1 }, .max = { 1, 1, 1 } },
								   .max_extent = 2 };
	return AeronScene_MeshCreate(cmd, &model, "capture.cube", NULL);
}

static int create_screen_textures(Fixture* f, AeronCommandBuffer* cmd);

/* A model whose game_scene instances give markings values gets one variant
 * index per markings value up to the highest: a used value shows its
 * markings list entry, as the game maps markings, any other the own
 * materials, and only the used variants' textures are loaded. Others get
 * their whole markings list. */
static AeronSceneMesh* create_mesh(AeronCommandBuffer* cmd, const Fixture* f, int model_index,
								   const AeronFlightModel* model, const char* path) {
	bool used[CAPTURE_MAX_MARKINGS + 1] = { false };
	int  highest                        = -1;
	for (int i = 0; i < f->config.game.instance_count; ++i) {
		const CaptureInstance* in = &f->config.game.instances[i];
		if (in->model == model_index && in->markings >= 0) {
			used[in->markings] = true;
			highest            = in->markings > highest ? in->markings : highest;
		}
	}
	AeronSceneMeshCreateInfo info = {
		.model = model, .debug_name = path, .effect_atlas = f->effect_atlas, .effect_data = f->effect_data
	};
	if (highest < 0)
		return AeronScene_MeshCreateEx(cmd, &info, NULL);
	const AeronGltfModel* render = &model->render;
	uint32_t              variants[CAPTURE_MAX_MARKINGS + 1];
	for (int m = 0; m <= highest; ++m)
		variants[m] = used[m] ? Aeron_GltfMarkingsVariant(render, (uint32_t)m) : AERON_GLTF_NO_VARIANT;
	info.variants = &(AeronSceneMeshVariants) { .variants = variants, .count = (uint32_t)highest + 1u };
	return AeronScene_MeshCreateEx(cmd, &info, NULL);
}

static int load_models(Fixture* f, AeronCommandBuffer* cmd) {
	/* An environment folder is the resource root. */
	const char* folder = f->config.game.environment_folder;
	AeronVfs*   vfs = f->vfs = AeronVfs_Create(
		&(AeronVfsConfig) { .asset_root = f->config.asset_root, .resource_root = folder[0] ? folder : NULL });
	if (!vfs)
		return 0;
	if (f->config.effect_dir[0]) {
		f->effect_atlas = AeronEffectAtlas_Create(vfs, AERON_VFS_ROOT_ASSET, f->config.effect_dir);
		if (!f->effect_atlas)
			return 0;
		f->effect_data = AeronEffectData_Load(f->effect_atlas, cmd);
	}
	int ok = 1;
	for (int i = 0; ok && i < f->config.model_count; ++i) {
		const char* path  = f->config.models[i];
		uint8_t*    bytes = NULL;
		size_t      size  = 0;
		/* A cooked .glb loads as a game loads it, without a size limit (skinned
		 * models exceed 100 MiB); anything else is an OPT. */
		const size_t length = strlen(path);
		const bool   glb    = length > 4 && SDL_strcasecmp(path + length - 4, ".glb") == 0;
		if (!AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, path, glb ? 0 : 64u * 1024u * 1024u, &bytes, &size)) {
			Aeron_LogError("scene_capture", "Cannot read model '%s' under '%s'", path, f->config.asset_root);
			ok = 0;
			break;
		}
		AeronFlightModel   model  = { 0 };
		AeronOptModelError error  = { 0 };
		ok = glb ? Aeron_FlightModelBuildMemory(bytes, size, path, &model)
				 : Aeron_OptModelBuildMemory(
					   bytes, size, path,
					   &(AeronOptModelBuildOptions) { .smooth_angle_degrees = 90,
													  .emissive             = true,
													  .emissive_strength    = f->config.emissive_scale },
					   &model, &error);
		free(bytes);
		if (!ok) {
			Aeron_LogError("scene_capture", "Cannot build model '%s': %s", path,
						   glb ? "invalid cooked GLB" : error.message);
		} else if (!isfinite(model.max_extent) || model.max_extent <= 1e-6f) {
			Aeron_LogError("scene_capture", "OPT '%s' has invalid bounds", path);
			ok = 0;
		} else {
			CaptureMesh* mesh = &f->meshes[i];
			mesh->extent      = model.max_extent;
			mesh->center[0]   = (model.bounds.min.x + model.bounds.max.x) * .5f;
			mesh->center[1]   = (model.bounds.min.y + model.bounds.max.y) * .5f;
			mesh->center[2]   = (model.bounds.min.z + model.bounds.max.z) * .5f;
			mesh->mesh        = create_mesh(cmd, f, i, &model, path);
			ok                = mesh->mesh != NULL;
		}
		Aeron_FlightModelFree(&model);
	}
	return ok;
}

/* Loads an instance's probes from a bake folder (bake.json and its two
 * images) with `cmd`. Logs an error and returns NULL on failure. */
static AeronProbeSet* load_probes(Fixture* f, AeronCommandBuffer* cmd, AeronVfsRoot root,
								  const char* folder) {
	static const char* const names[3]   = { "bake.json", "probes.ktx2", "grid.ktx2" };
	uint8_t*                 bytes[3]   = { NULL, NULL, NULL };
	size_t                   sizes[3]   = { 0, 0, 0 };
	char                     error[256] = "cannot read the bake files";
	bool                     ok         = true;
	for (int i = 0; ok && i < 3; ++i) {
		char path[1100];
		snprintf(path, sizeof path, "%s/%s", folder, names[i]);
		ok = AeronVfs_ReadAll(f->vfs, root, path, 0, &bytes[i], &sizes[i]);
	}
	AeronFlightProbes probes;
	memset(&probes, 0, sizeof probes);
	ok = ok && Aeron_FlightProbesBuild((const char*)bytes[0], sizes[0], bytes[1], sizes[1], bytes[2],
									   sizes[2], &probes, error, sizeof error);
	AeronProbeSet* set = ok ? AeronProbeSet_Create(cmd, &probes, folder) : NULL;
	if (!ok)
		Aeron_LogError("scene_capture", "probes %s: %s", folder, error);
	Aeron_FlightProbesFree(&probes);
	for (int i = 0; i < 3; ++i)
		free(bytes[i]);
	return set;
}

/* game_scene.environment: loads the instances' probes bake folders, then
 * parses the manifest's packages and prepares the environment with `cmd`. */
static int prepare_environment(Fixture* f, AeronCommandBuffer* cmd) {
	const CaptureGameScene* game = &f->config.game;
	const AeronVfsRoot root  = game->environment_folder[0] ? AERON_VFS_ROOT_RESOURCE : AERON_VFS_ROOT_ASSET;
	f->environment_reference = -1;
	for (int i = 0; i < game->instance_count; ++i)
		if (game->instances[i].probes[0] &&
			!(f->probes[i] = load_probes(f, cmd, root, game->instances[i].probes)))
			return 0;
	if (!game->environment_manifest[0])
		return 1;
	AeronConfigError error;
	memset(&error, 0, sizeof error);
	if (!AeronConfigFile_LoadYamlEx(f->vfs, root, game->environment_manifest, &f->environment_file, &error) ||
		!AeronEnvironment_ParsePackages(
			AeronConfigNode_MapGet(AeronConfigFile_Root(f->environment_file), "packages"),
			&f->environment_definitions, &error)) {
		Aeron_LogError("scene_capture", "game_scene.environment: %s:%d: %s", game->environment_manifest,
					   error.line, error.message);
		return 0;
	}
	/* Scene world (+X right, +Y down, +Z forward) to the cube frame (+X
	 * right, +Y up, -Z forward). */
	f->environment_library = AeronEnvironmentLibrary_Create(&(AeronEnvironmentLibraryDesc) {
		.definitions        = f->environment_definitions,
		.vfs                = f->vfs,
		.root               = root,
		.base_world_to_cube = { 1, 0, 0, 0, -1, 0, 0, 0, -1 },
	});
	if (!f->environment_library)
		return 0;
	AeronEnvironmentReference references[1];
	uint32_t                  count = 0;
	memset(references, 0, sizeof references);
	if (game->environment_package[0]) {
		f->environment_reference = (int)count;
		snprintf(references[count++].package, AERON_ENV_NAME_MAX, "%s", game->environment_package);
	}
	if (!AeronEnvironmentLibrary_Prepare(f->environment_library, cmd, references, count))
		return 0;
	for (uint32_t r = 0; r < count; ++r) {
		if (!AeronEnvironmentLibrary_Find(f->environment_library, r)) {
			Aeron_LogError("scene_capture", "game_scene.environment: package '%s': %s", references[r].package,
						   AeronEnvironmentLibrary_FailureReason(f->environment_library, r));
			return 0;
		}
	}
	return 1;
}

int Fixture_Create(Fixture* f, const CaptureConfig* config) {
	memset(f, 0, sizeof *f);
	f->config = *config;
	if (config->bloom_chart)
		return BloomChart_Create(f);
	AeronScene3DDesc desc   = { .rt_width             = config->width,
								.rt_height            = config->height,
								.color_format         = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
								.with_normal_rt       = 1,
								.sample_count         = (AeronSampleCount)config->msaa_samples,
								.view_space_to_meters = config->game.enabled ? AERON_OPT_METERS_PER_UNIT : 1 };
	f->scene                = AeronScene_Create(&desc);
	f->reference_scene      = config->benchmark_mode ? NULL : AeronScene_Create(&desc);
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!f->scene || (!config->benchmark_mode && !f->reference_scene) || !cmd) {
		if (cmd)
			Aeron_CancelCommandBuffer(cmd);
		Fixture_Destroy(f);
		return 0;
	}
	int loaded;
	if (config->model_count) {
		loaded = load_models(f, cmd);
	} else {
		f->meshes[0].mesh = create_cube(cmd);
		loaded            = f->meshes[0].mesh != NULL;
	}
	if (loaded && config->screen_test)
		loaded = create_screen_textures(f, cmd);
	if (loaded && config->game.enabled)
		loaded = prepare_environment(f, cmd);
	if (!loaded) {
		Aeron_CancelCommandBuffer(cmd);
		Fixture_Destroy(f);
		return 0;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Fixture_Destroy(f);
		return 0;
	}
	AeronEffectAtlas_Commit(f->effect_atlas);
	if (config->blast_image[0] && !f->effect_data) {
		Aeron_LogError("scene_capture", "blast_mark: no effects.yaml under '%s'", config->effect_dir);
		Fixture_Destroy(f);
		return 0;
	}
	AeronScene_SetEffectAtlas(f->scene, f->effect_atlas);
	AeronScene_SetEffectAtlas(f->reference_scene, f->effect_atlas);
	float       background = config->model_count ? 0 : .005f;
	const float clear[4]   = { background, background, background, 1 };
	AeronScene_SetClearColor(f->scene, clear);
	AeronScene_SetClearColor(f->reference_scene, clear);
	if (config->game.enabled && config->game.light_count) {
		/* resources/remaster/config.yaml's point_lights defaults. */
		const AeronSceneClusteredLightDesc lights = { .enabled      = 1,
													  .depth_slices = 24,
													  .debug_view   = config->game.cluster_debug };
		if (!AeronScene_SetClusteredLights(f->scene, &lights) ||
			(f->reference_scene && !AeronScene_SetClusteredLights(f->reference_scene, &lights))) {
			Fixture_Destroy(f);
			return 0;
		}
	}
	return 1;
}

/* Screen test inputs: a colour gradient with a checker as graphics, and a
 * white "F" with a 32-pixel grid as text, to read orientation and mirroring
 * on every screen rectangle. */
static int create_screen_textures(Fixture* f, AeronCommandBuffer* cmd) {
	enum { SIZE = 256 };

	uint8_t* graphics = (uint8_t*)malloc(SIZE * SIZE * 4);
	uint8_t* text     = (uint8_t*)calloc(SIZE * SIZE * 4, 1);
	if (!graphics || !text) {
		free(graphics);
		free(text);
		return 0;
	}
	for (int y = 0; y < SIZE; ++y) {
		for (int x = 0; x < SIZE; ++x) {
			uint8_t*  g     = &graphics[(y * SIZE + x) * 4];
			const int dark  = ((x / 32) + (y / 32)) & 1;
			g[0]            = (uint8_t)(x * (dark ? 128 : 255) / SIZE);
			g[1]            = (uint8_t)(y * (dark ? 128 : 255) / SIZE);
			g[2]            = dark ? 60 : 150;
			g[3]            = 255;
			const int grid  = x % 32 == 0 || y % 32 == 0;
			const int glyph = (x >= 40 && x < 60 && y >= 40 && y < 200) ||
							  (y >= 40 && y < 60 && x >= 40 && x < 160) ||
							  (y >= 110 && y < 130 && x >= 40 && x < 130);
			if (grid || glyph)
				memset(&text[(y * SIZE + x) * 4], 255, 4);
		}
	}
	f->screen_graphics = Aeron_ImageUploadRgba8(
		cmd, graphics, SIZE, SIZE, SIZE * 4, AERON_TEXTURE_FORMAT_RGBA8_SRGB, AERON_COLOR_SPACE_SRGB,
		AERON_IMAGE_ALPHA_PREMULTIPLIED, false, "screen_test.graphics");
	f->screen_text = Aeron_ImageUploadRgba8(cmd, text, SIZE, SIZE, SIZE * 4, AERON_TEXTURE_FORMAT_RGBA8_SRGB,
											AERON_COLOR_SPACE_SRGB, AERON_IMAGE_ALPHA_PREMULTIPLIED, false,
											"screen_test.text");
	free(graphics);
	free(text);
	return f->screen_graphics && f->screen_text;
}

static void set_screen_sources(AeronScene3D* scene, const Fixture* f, double time) {
	AeronSceneScreenFrame frame = { .valid            = { 0xFFFFFFFFu, 0xFFFFFFFFu },
									.flags            = AERON_SCENE_SCREEN_BAR_VISIBLE,
									.display_strength = 1.0f,
									.bar_level        = .6f,
									.holo_time        = (float)time,
									.holo_fade        = 1.0f,
									.holo_color       = { .01f, .35f, .01f, 1.5f } };
	for (unsigned source = 0; source < AERON_GLTF_SCREEN_MAX_SOURCES; ++source) {
		frame.source[source][2] = 1.0f;
		frame.source[source][3] = 1.0f;
	}
	AeronScene_SetScreenSources(scene, f->screen_graphics, f->screen_text, &frame);
}

/* The model unscaled with `screen_eye` at the camera: model right/forward/
 * up (+X, -Y, +Z) on camera +X, +Z, -Y, then pitch and yaw. */
static void place_screen_test_model(float transform[16], const CaptureConfig* c) {
	const float rad = 3.14159265f / 180.0f;
	const float cy = cosf(c->screen_yaw * rad), sy = sinf(c->screen_yaw * rad);
	const float cp = cosf(c->screen_pitch * rad), sp = sinf(c->screen_pitch * rad);
	const float base[3][3]  = { { 1, 0, 0 }, { 0, 0, -1 }, { 0, -1, 0 } };
	const float yaw[3][3]   = { { cy, 0, -sy }, { 0, 1, 0 }, { sy, 0, cy } };
	const float pitch[3][3] = { { 1, 0, 0 }, { 0, cp, -sp }, { 0, sp, cp } };
	float       tmp[3][3], rot[3][3];
	for (int r = 0; r < 3; ++r)
		for (int k = 0; k < 3; ++k) {
			tmp[r][k] = 0;
			for (int j = 0; j < 3; ++j)
				tmp[r][k] += yaw[r][j] * base[j][k];
		}
	for (int r = 0; r < 3; ++r)
		for (int k = 0; k < 3; ++k) {
			rot[r][k] = 0;
			for (int j = 0; j < 3; ++j)
				rot[r][k] += pitch[r][j] * tmp[j][k];
		}
	memset(transform, 0, 16 * sizeof *transform);
	transform[15] = 1;
	for (int r = 0; r < 3; ++r) {
		transform[r * 4 + 3] = 0;
		for (int k = 0; k < 3; ++k) {
			transform[r * 4 + k] = rot[r][k];
			transform[r * 4 + 3] -= rot[r][k] * c->screen_eye[k];
		}
	}
}

void Fixture_Destroy(Fixture* f) {
	Aeron_DestroyTexture(f->screen_graphics);
	Aeron_DestroyTexture(f->screen_text);
	AeronDrawList_Destroy(f->chart_draws);
	Aeron_DestroyRenderTarget(f->chart_rt);
	AeronScene_Destroy(f->scene);
	AeronScene_Destroy(f->reference_scene);
	AeronEnvironmentLibrary_Destroy(f->environment_library);
	AeronEnvironmentDefinitionSet_Destroy(f->environment_definitions);
	for (int i = 0; i < CAPTURE_MAX_INSTANCES; ++i)
		AeronProbeSet_Destroy(f->probes[i]);
	AeronConfigFile_Destroy(f->environment_file);
	for (int i = 0; i < CAPTURE_MAX_MODELS; ++i)
		AeronScene_MeshDestroy(f->meshes[i].mesh);
	AeronEffectData_Destroy(f->effect_data);
	AeronEffectAtlas_Destroy(f->effect_atlas);
	AeronVfs_Destroy(f->vfs);
	memset(f, 0, sizeof *f);
}

static AeronSceneCamera camera_at(const CaptureConfig* c, double time) {
	const double     rad        = 3.14159265358979323846 / 180;
	double           half_angle = c->pan_speed * time * rad * .5;
	double           direction  = c->pan_direction * rad;
	AeronSceneCamera camera = { .ori = { (float)cos(half_angle), (float)(-sin(direction) * sin(half_angle)),
										 (float)(cos(direction) * sin(half_angle)), 0 },
								.v_half_rad = .523598776f,
								/* The game's near plane is one OPT unit. */
								.near_z     = c->game.enabled ? 1.0f : .1f,
								.viewport   = { 0, 0, c->width, c->height } };
	camera.h_half_rad       = atanf(tanf(camera.v_half_rad) * (float)c->width / (float)c->height);
	return camera;
}

static void place_model(float transform[16], const CaptureMesh* mesh, const float object[8], size_t index) {
	/* Fit the longest model dimension to the target without stretching it.
	 * Fixed pitch variations expose both hull textures and thin silhouettes. */
	float scale = 2 * fmaxf(object[0], fmaxf(object[1], object[2])) / mesh->extent;
	/* Keep the eight scattered ships large enough to resolve hull detail. */
	if (index >= 4)
		scale *= 3.0f;
	float       cs = cosf(object[6]), sn = sinf(object[6]);
	float       pitch = -.65f + .5f * (float)(index % 3);
	float       cp = cosf(pitch), sp = sinf(pitch);
	const float rotation[3][3] = { { cs, -sn * cp, sn * sp }, { sn, cs * cp, -cs * sp }, { 0, sp, cp } };
	for (int row = 0; row < 3; ++row) {
		transform[row * 4 + 3] = object[row + 3];
		for (int col = 0; col < 3; ++col) {
			transform[row * 4 + col] = rotation[row][col] * scale;
			transform[row * 4 + 3] -= transform[row * 4 + col] * mesh->center[col];
		}
	}
}

/* The blast_mark image's frame at `time`, as glow_marks.c draws an animated
 * blast mark: the classic square (`size` wide) shows the image's crop, and
 * the image spreads around it, edge texels clamped, over the faces the
 * classic pass draws, here a square three times as wide. */
static void add_blast_mark(AeronScene3D* scene, const Fixture* f, const float transform[16], double time) {
	const int        index = AeronEffectData_FindImage(f->effect_data, f->config.blast_image);
	AeronEffectImage image;
	const float      page = (float)AeronEffectAtlas_PageSize(f->effect_atlas);
	if (index < 0 ||
		!AeronEffectData_ImageFrame(f->effect_data, index, f->config.animation_time + time, &image)) {
		Aeron_LogWarn("scene_capture", "blast_mark: no frame of '%s'", f->config.blast_image);
		return;
	}
	const float* c = f->config.blast_center;
	const float  h = f->config.blast_size * 1.5f;
	/* Two triangles, counter-clockwise seen from +Z; uv -1.5..1.5, the
	 * classic square being -0.5..0.5. */
	static const float corners[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
	AeronSceneMeshOverlayVertex vertices[6];
	for (int v = 0; v < 6; ++v)
		vertices[v] = (AeronSceneMeshOverlayVertex) {
			.pos = { c[0] + corners[v][0] * h, c[1] + corners[v][1] * h, c[2] },
			.uv  = { corners[v][0] * 1.5f, corners[v][1] * 1.5f },
		};
	const uint32_t* r       = image.rect[AERON_EFFECT_LAYER];
	const float     rect[4] = { ((float)r[0] + .5f) / page, ((float)r[1] + .5f) / page,
								((float)(r[0] + r[2]) - .5f) / page, ((float)(r[1] + r[3]) - .5f) / page };
	const float     width = rect[2] - rect[0], height = rect[3] - rect[1];
	AeronSceneMeshOverlayDesc overlay = {
		.vertices        = vertices,
		.vertex_count    = 6,
		.texture_array   = AeronEffectAtlas_Texture(f->effect_atlas, AERON_EFFECT_LAYER),
		.texture_layer   = image.layer[AERON_EFFECT_LAYER],
		.texture_max_lod = image.lod[AERON_EFFECT_LAYER],
		.texture_clamp   = 1,
		.uv_xform = { rect[0] + (image.crop[0] + image.crop[2]) * .5f * width,
					  rect[1] + (image.crop[1] + image.crop[3]) * .5f * height,
					  (image.crop[2] - image.crop[0]) * width, (image.crop[3] - image.crop[1]) * height },
		.uv_rect  = { rect[0], rect[1], rect[2], rect[3] },
		.color    = { 1, 1, 1, 1 },
		.depth_bias_view = .05f,
		.blend           = AERON_SCENE_MESH_OVERLAY_BLEND_PMA,
		.cull_mode       = AERON_CULL_NONE,
	};
	memcpy(overlay.transform, transform, sizeof overlay.transform);
	AeronScene_AddMeshOverlay(scene, &overlay);
}

/* The `anim` state of instance `index`, borrowed for the frame; NULL
 * without one. */
static const AeronSceneMaterialAnim* instance_anim(const Fixture* f, int index) {
	static AeronSceneMaterialAnim anims[CAPTURE_MAX_INSTANCES + 16];
	if (!f->config.anim_enabled || index < 0 || index >= (int)(sizeof anims / sizeof anims[0]))
		return NULL;
	anims[index] = f->config.anim;
	anims[index].seed += (uint32_t)index;
	return &anims[index];
}

static void add_objects(AeronScene3D* scene, const Fixture* f, double time) {
	/* Half-extents, center, in-plane rotation, and emissive multiplier. */
	static const float objects[][8] = {
		{ 2.4f, 1.6f, .35f, -3.0f, -.6f, 12, 0, 1 },
		{ .6f, 1.25f, .6f, .25f, 1.1f, 9, .3f, .7f },
		{ .14f, 1.8f, .2f, 2.5f, -.4f, 10, -.45f, 1.2f },
		{ .4f, .4f, .4f, 3.8f, 1.8f, 13, .6f, .9f },
		/* Fixed small targets exercise streaks narrower than the sample spacing. */
		{ .035f, .035f, .035f, -5.4f, -3.6f, 12, .2f, 1.2f },
		{ .065f, .065f, .065f, -1.5f, -4.0f, 14, .5f, .9f },
		{ .10f, .06f, .08f, 3.7f, -3.8f, 13, -.3f, .8f },
		{ .035f, .08f, .035f, 7.5f, -2.3f, 16, .8f, 1.1f },
		{ .07f, .07f, .07f, -8.0f, .5f, 14, .45f, 1.0f },
		{ .12f, .07f, .09f, -6.4f, 3.9f, 16, -.6f, .75f },
		{ .035f, .035f, .035f, -2.6f, 4.0f, 11, .2f, 1.2f },
		{ .09f, .06f, .07f, 1.7f, 5.0f, 15, -.7f, 1.0f }
	};
	for (size_t i = 0; i < sizeof objects / sizeof objects[0]; ++i) {
		const CaptureMesh*     mesh = &f->meshes[f->config.model_count ? i % f->config.model_count : 0];
		const float*           o    = objects[i];
		float                  cs = cosf(o[6]), sn = sinf(o[6]);
		AeronSceneMeshInstance instance = {
			.mesh                         = mesh->mesh,
			.base_color_emissive_strength = f->config.model_count ? 0 : o[7] * f->config.emissive_scale,
			.cull_mode                    = (AeronCullMode)f->config.cull_mode,
			.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW | AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW,
			.transform = { cs * o[0], -sn * o[1], 0, o[3], sn * o[0], cs * o[1], 0, o[4], 0, 0, o[2], o[5], 0,
						   0, 0, 1 }
		};
		if (f->config.screen_test && i > 0)
			break;
		if (f->config.screen_test)
			place_screen_test_model(instance.transform, &f->config);
		else if (f->config.model_count)
			place_model(instance.transform, mesh, o, i);
		memcpy(instance.prev_transform, instance.transform, sizeof instance.transform);
		instance.anim = instance_anim(f, (int)i);
		AeronScene_AddMeshInstance(scene, &instance);
		if (i == 0 && f->config.blast_image[0])
			add_blast_mark(scene, f, instance.transform, time);
	}
}

static void set_lighting(AeronScene3D* scene, const CaptureConfig* config) {
	/* Float4 rows of PbrLightFS in scene_pbr_lighting.hlsli, FS b1.
	 * Camera stays at the origin; the light remains fixed in world space.
	 * Its illuminance of 0.9 pi lights a white Lambert surface to 0.9. */
	float light[23][4] = {
		[1] = { 0, 0, 1, (float)config->width },
		[2] = { (float)config->height, 0, 1, 0 },
		[4] = { .32444284f, .48666426f, .81110711f, 0 },
		[5] = { 2.8274334f, 2.8274334f, 2.8274334f, 0 },
	};
	for (int row = 6; row < 12; ++row)
		for (int channel = 0; channel < 3; ++channel)
			light[row][channel] = .08f;
	AeronScene_SetFrameUniformData(scene, AERON_SHADER_STAGE_FRAGMENT, 1, light, sizeof light);
}

/* ===== game_scene ===================================================== */

/* Metres to game world units (OPT units). */
#define GAME_UNITS_PER_METER (1.0f / AERON_OPT_METERS_PER_UNIT)

static void scale_transform(float transform[16], float scale) {
	for (int i = 0; i < 12; ++i)
		transform[i] *= scale;
}

/* Model axes (right +X, forward -Y, up +Z) onto camera axes (+X right, +Y
 * down, +Z forward), then roll about forward, pitch about right and yaw
 * about down, placed at `position` metres. */
static void place_ship(float transform[16], const CaptureInstance* in) {
	const float rad = 3.14159265f / 180.0f;
	const float cy = cosf(in->rotation[0] * rad), sy = sinf(in->rotation[0] * rad);
	const float cp = cosf(in->rotation[1] * rad), sp = sinf(in->rotation[1] * rad);
	const float cr = cosf(in->rotation[2] * rad), sr = sinf(in->rotation[2] * rad);
	const float base[3][3]  = { { 1, 0, 0 }, { 0, 0, -1 }, { 0, -1, 0 } };
	const float roll[3][3]  = { { cr, -sr, 0 }, { sr, cr, 0 }, { 0, 0, 1 } };
	const float pitch[3][3] = { { 1, 0, 0 }, { 0, cp, -sp }, { 0, sp, cp } };
	const float yaw[3][3]   = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
	const float(*chain[3])[3] = { roll, pitch, yaw };
	float rot[3][3];
	memcpy(rot, base, sizeof rot);
	for (int c = 0; c < 3; ++c) {
		float next[3][3];
		for (int r = 0; r < 3; ++r)
			for (int k = 0; k < 3; ++k) {
				next[r][k] = 0;
				for (int j = 0; j < 3; ++j)
					next[r][k] += chain[c][r][j] * rot[j][k];
			}
		memcpy(rot, next, sizeof rot);
	}
	memset(transform, 0, 16 * sizeof *transform);
	transform[15] = 1;
	for (int r = 0; r < 3; ++r) {
		for (int k = 0; k < 3; ++k)
			transform[r * 4 + k] = rot[r][k];
		transform[r * 4 + 3] = in->position[r];
	}
}

/* Rotates a camera-space placement into the world by the camera's
 * orientation (unit quaternion w, x, y, z; the camera sits at the origin),
 * as the game carries its cockpit with the view. */
static void attach_to_camera(float transform[16], const AeronSceneCamera* camera) {
	/* The conjugate: `ori` maps world directions into the camera. */
	const float w = camera->ori[0], x = -camera->ori[1], y = -camera->ori[2], z = -camera->ori[3];
	const float rotation[3][3] = {
		{ 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y) },
		{ 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x) },
		{ 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) },
	};
	float local[16];
	memcpy(local, transform, sizeof local);
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c)
			transform[r * 4 + c] = rotation[r][0] * local[0 * 4 + c] + rotation[r][1] * local[1 * 4 + c] +
								   rotation[r][2] * local[2 * 4 + c];
}

static void add_game_instances(AeronScene3D* scene, const Fixture* f, const AeronSceneCamera* camera,
							   const AeronSceneCamera* previous_camera) {
	for (int i = 0; i < f->config.game.instance_count; ++i) {
		const CaptureInstance* in       = &f->config.game.instances[i];
		AeronSceneMeshInstance instance = {
			.mesh      = f->meshes[in->model].mesh,
			.cull_mode = (AeronCullMode)f->config.cull_mode,
			.variant   = in->markings > 0 ? (uint32_t)in->markings : 0u,
		};
		const int placed = in->role == CAPTURE_ROLE_SHIP || in->role == CAPTURE_ROLE_HANGAR;
		if (placed)
			place_ship(instance.transform, in);
		else
			place_screen_test_model(instance.transform, &f->config);
		scale_transform(instance.transform, GAME_UNITS_PER_METER);
		memcpy(instance.prev_transform, instance.transform, sizeof instance.transform);
		/* When the craft turns, the cockpit and the hidden exterior turn
		 * with the view; when the pilot looks around, they stay put. */
		if (!placed && f->config.game.cockpit_follows_camera) {
			attach_to_camera(instance.transform, camera);
			attach_to_camera(instance.prev_transform, previous_camera);
		}
		/* As flight.c submits the hangar: it receives the ships' shadows but
		 * does not cast into its ceiling light. */
		if (in->role == CAPTURE_ROLE_HANGAR)
			instance.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW;
		/* As flight.c submits the cockpit view's cockpit and hidden exterior. */
		if (in->role == CAPTURE_ROLE_EXTERIOR) {
			instance.shadow_flags = AERON_SCENE_INSTANCE_EXCLUDE_FROM_RECEIVER_LOCAL_SHADOW;
			AeronScene_AddShadowCaster(scene, &instance);
			continue;
		}
		if (in->role == CAPTURE_ROLE_COCKPIT) {
			instance.shadow_flags = AERON_SCENE_INSTANCE_USE_RECEIVER_LOCAL_SHADOW |
									AERON_SCENE_INSTANCE_CAST_RECEIVER_LOCAL_ONLY |
									AERON_SCENE_INSTANCE_SHADOW_DOUBLE_SIDED;
			instance.velocity_stamp_screens = 1;
		}
		/* The model's authored lights, of every drawn instance. */
		for (uint32_t li = 0; li < instance.mesh->light_count && f->config.game.model_light_intensity > 0; ++li) {
			AeronSceneLight light;
			if (AeronScene_MeshLight(instance.mesh, li, instance.transform, GAME_UNITS_PER_METER, NULL,
									 f->config.game.model_light_intensity, &light))
				AeronScene_AddLight(scene, &light);
		}
		instance.anim = instance_anim(f, (int)i);
		/* Use the probes from the bake folder, or else the model's own. */
		const AeronProbeSet* probes = f->probes[i] ? f->probes[i] : instance.mesh->probes;
		if (probes) {
			const int set            = AeronScene_AddProbeSet(scene, probes, instance.transform);
			instance.probe_set_owner = set >= 0 ? (uint8_t)(set + 1) : 0u;
		}
		AeronScene_AddMeshInstance(scene, &instance);
	}
	/* As XwaRemasterShip_ScenePointLight: 0.25 m spheres ending under
	 * 1/250 of a unit of illuminance. */
	for (int i = 0; i < f->config.game.light_count; ++i) {
		const CaptureLight* l     = &f->config.game.lights[i];
		AeronSceneLight     light = {
				.source_radius = 0.25f * GAME_UNITS_PER_METER,
		};
		float peak = 0;
		for (int k = 0; k < 3; ++k) {
			light.pos[k]       = l->position[k] * GAME_UNITS_PER_METER;
			light.half_axis[k] = l->half_axis[k] * GAME_UNITS_PER_METER;
			light.direction[k] = l->direction[k];
			light.color[k]     = l->intensity[k] * GAME_UNITS_PER_METER * GAME_UNITS_PER_METER;
			peak           = fmaxf(peak, l->intensity[k]);
		}
		light.radius = sqrtf(peak / 0.004f) * GAME_UNITS_PER_METER;
		AeronScene_AddLight(scene, &light);
	}
}

/* The PbrLightFS block as XwaRemasterShip_SetPbrEnv fills it; sun_color is
 * the illuminance, after the game's lighting.sun_illuminance_scale. */
static void set_game_lighting(AeronScene3D* scene, const CaptureConfig* config) {
	const CaptureGameScene* game = &config->game;
	float                   light[23][4] = {
		[0] = { game->key_probe_shadow >= 0 ? 1.0f : 0.0f, fmaxf(game->key_probe_shadow, 0.0f), 0,
				game->key_shadow_all_sides ? 0.0f : 1.0f },
		[1] = { 0, game->ssao.ssao_intensity, game->ssao.ssao_power, 0 }, /* reserved, AO, reserved */
		[2] = { 0, game->ssao.ssao_direct, 1, 0 },                        /* reserved, AO, spec_geom_adapt */
	};
	for (int k = 0; k < 3; ++k) {
		light[4][k] = game->sun_direction[k];
		light[5][k] = game->sun_color[k];
		for (int row = 6; row < 12; ++row)
			light[row][k] = game->ambient;
	}
	AeronScene_SetFrameUniformData(scene, AERON_SHADER_STAGE_FRAGMENT, 1, light, sizeof light);
	AeronScene_SetProbeKeyShadow(scene, game->key_probe_shadow >= 0.0f && game->key_shadow_all_sides);
	AeronScene_SetProbeOcclusion(scene, game->probe_occlusion);
	AeronScene_SetPlanarReflection(scene, game->planar_reflection);
}

static void set_game_effects(AeronScene3D* scene, const CaptureConfig* config, int blur) {
	const AeronSceneSsaoSettings* ssao = &config->game.ssao;
	AeronScene_SetPost(scene, &(AeronScenePostDesc) {
								  .ssao_quality         = ssao->ssao_quality,
								  .ssao_intensity       = ssao->ssao_intensity,
								  .ssao_power           = ssao->ssao_power,
								  .ssao_radius_view     = ssao->ssao_radius_view,
								  .ssao_bias_view       = ssao->ssao_bias_view,
								  .ssao_direct          = ssao->ssao_direct,
								  .ssao_min_screen_frac = ssao->ssao_min_screen_frac,
								  .ssao_max_screen_frac = ssao->ssao_max_screen_frac,
								  .ssao_sample_jitter   = ssao->ssao_sample_jitter,
								  .mb_quality           = blur ? config->quality : 0,
								  .mb_shutter           = (float)(config->shutter * .032 * config->fps),
								  .mb_camera_blur       = 1,
								  /* As the game ships (motion_blur.fsr_direct_motion). */
								  .mb_fsr_direct_motion = config->game.temporal_mode != AERON_TEMPORAL_OFF,
							  });
	const AeronSceneShadowSettings* shadows = &config->game.shadows;
	if (!shadows->enabled)
		return;
	AeronSceneDirectionalShadowDesc desc = {
		.enabled                      = 1,
		.atlas_size                   = shadows->atlas_size,
		.cascade_count                = shadows->cascade_count,
		.fit_mode                     = shadows->fit_mode,
		.max_distance                 = shadows->max_distance,
		.split_lambda                 = shadows->split_lambda,
		.explicit_splits              = shadows->explicit_splits,
		.light_angular_radius_degrees = shadows->light_angular_radius_degrees,
		.max_filter_radius            = shadows->max_filter_radius,
		.pcss_min_filter_radius       = shadows->pcss_min_filter_radius,
		.normal_bias_texels           = shadows->normal_bias_texels,
		.depth_bias_texels            = shadows->depth_bias_texels,
		.transition_fraction          = shadows->transition_fraction,
		.distance_fade_fraction       = shadows->distance_fade_fraction,
		.receiver_local_near_distance = shadows->receiver_local_near_distance_m * GAME_UNITS_PER_METER,
	};
	memcpy(desc.split_positions, shadows->split_positions, sizeof desc.split_positions);
	memcpy(desc.light_dir, config->game.sun_direction, sizeof desc.light_dir);
	AeronScene_SetDirectionalShadow(scene, &desc);
}

AeronRenderTarget* Fixture_Record(Fixture* f, double time, int blur, AeronCommandBuffer* cmd) {
	if (f->config.bloom_chart)
		return BloomChart_Record(f, cmd);
	AeronScene3D*    scene    = blur ? f->scene : f->reference_scene;
	AeronSceneCamera current  = camera_at(&f->config, time);
	AeronSceneCamera previous = camera_at(&f->config, time - 1.0 / f->config.fps);
	float            previous_matrix[16];
	AeronScene_ComputeViewProj(&previous, previous_matrix);
	if (f->config.game.enabled && blur) {
		/* The pose sequence restarts with a camera jump: start a new history. */
		const int frame = (int)floor(time * f->config.fps + 0.5);
		AeronScene_SetTemporal(scene, &(AeronSceneTemporalDesc) {
										  .mode                = f->config.game.temporal_mode,
										  .frame_time_delta_ms = (float)(1000.0 / f->config.fps),
										  .sharpness           = f->config.game.temporal_sharpness,
										  .reset_history       = frame % f->config.frames == 0,
									  });
	}
	if (!AeronScene_Begin(scene, &current))
		return NULL;
	AeronScene_SetAnimationTime(scene, f->config.animation_time + time);
	if (f->config.game.enabled) {
		set_game_effects(scene, &f->config, blur);
		AeronScene_SetMotionContext(scene, previous_matrix, 1);
		AeronScene_SetBackfaceFill(scene, f->config.backface_fill);
		set_game_lighting(scene, &f->config);
		if (f->environment_reference >= 0)
			AeronScene_SetPbrEnvironmentPackage(
				scene,
				AeronEnvironmentLibrary_Find(f->environment_library, (uint32_t)f->environment_reference));
		if (f->config.screen_test)
			set_screen_sources(scene, f, time);
		add_game_instances(scene, f, &current, &previous);
		return AeronScene_Render(scene, cmd) ? AeronScene_SceneRt(scene) : NULL;
	}
	AeronScene_SetPost(
		scene, &(AeronScenePostDesc) { .mb_quality     = blur ? f->config.quality : 0,
									   .mb_shutter     = (float)(f->config.shutter * .032 * f->config.fps),
									   .mb_camera_blur = 1 });
	AeronScene_SetMotionContext(scene, previous_matrix, 1);
	AeronScene_SetBackfaceFill(scene, f->config.backface_fill);
	/* Emissive shapes also read the PBR lighting/mode uniforms. */
	set_lighting(scene, &f->config);
	if (f->config.screen_test)
		set_screen_sources(scene, f, time);
	add_objects(scene, f, time);
	if (!AeronScene_Render(scene, cmd))
		return NULL;
	return AeronScene_SceneRt(scene);
}

AeronRenderTarget* Fixture_Render(Fixture* f, double time, int blur) {
	Aeron_PumpEvents();
	if (Aeron_QuitRequested() || Aeron_FatalErrorRequested())
		return NULL;
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd)
		return NULL;
	AeronRenderTarget* target = Fixture_Record(f, time, blur, cmd);
	if (!target) {
		Aeron_CancelCommandBuffer(cmd);
		return NULL;
	}
	return Aeron_SubmitCommandBuffer(cmd) ? target : NULL;
}
