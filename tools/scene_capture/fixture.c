#include "aeron/asset/opt_model.h"
#include "capture.h"

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
	uint16_t           indices[36];
	for (int f = 0; f < 6; ++f) {
		for (int v = 0; v < 4; ++v) {
			memcpy(vertices[f * 4 + v].pos, corners[faces[f][v]], sizeof vertices[0].pos);
			memcpy(vertices[f * 4 + v].normal, normals[f], sizeof vertices[0].normal);
			vertices[f * 4 + v].tangent[3] = 1;
		}
		const int triangle[6] = { 0, 1, 2, 0, 2, 3 };
		for (int i = 0; i < 6; ++i)
			indices[f * 6 + i] = (uint16_t)(f * 4 + triangle[i]);
	}
	AeronGltfMaterial material = { .base_color_factor = { .8f, .8f, .8f, 1 },
								   .roughness_factor  = 1,
								   .double_sided      = 1 };
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
												   .variant_slots         = 1,
												   .total_prim_count      = 1,
												   .prim_variant_material = &variant },
								   .bounds     = { .min = { -1, -1, -1 }, .max = { 1, 1, 1 } },
								   .max_extent = 2 };
	return AeronScene_MeshCreate(cmd, &model, "capture.cube", NULL);
}

static int load_models(Fixture* f, AeronCommandBuffer* cmd) {
	AeronVfs* vfs = AeronVfs_Create(&(AeronVfsConfig) { .asset_root = f->config.asset_root });
	if (!vfs)
		return 0;
	int ok = 1;
	for (int i = 0; ok && i < f->config.model_count; ++i) {
		const char* path  = f->config.models[i];
		uint8_t*    bytes = NULL;
		size_t      size  = 0;
		if (!AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, path, 64u * 1024u * 1024u, &bytes, &size)) {
			Aeron_LogError("scene_capture", "Cannot read OPT '%s' under '%s'", path, f->config.asset_root);
			ok = 0;
			break;
		}
		AeronFlightModel   model = { 0 };
		AeronOptModelError error = { 0 };
		ok                       = Aeron_OptModelBuildMemory(
			bytes, size, path,
			&(AeronOptModelBuildOptions) {
				.smooth_angle_degrees = 90, .emissive = true, .emissive_strength = f->config.emissive_scale },
			&model, &error);
		free(bytes);
		if (!ok) {
			Aeron_LogError("scene_capture", "Cannot build OPT '%s': %s", path, error.message);
		} else if (!isfinite(model.max_extent) || model.max_extent <= 1e-6f) {
			Aeron_LogError("scene_capture", "OPT '%s' has invalid bounds", path);
			ok = 0;
		} else {
			CaptureMesh* mesh = &f->meshes[i];
			mesh->extent      = model.max_extent;
			mesh->center[0]   = (model.bounds.min.x + model.bounds.max.x) * .5f;
			mesh->center[1]   = (model.bounds.min.y + model.bounds.max.y) * .5f;
			mesh->center[2]   = (model.bounds.min.z + model.bounds.max.z) * .5f;
			mesh->mesh        = AeronScene_MeshCreate(cmd, &model, path, NULL);
			ok                = mesh->mesh != NULL;
		}
		Aeron_FlightModelFree(&model);
	}
	AeronVfs_Destroy(vfs);
	return ok;
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
								.view_space_to_meters = 1 };
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
	if (!loaded) {
		Aeron_CancelCommandBuffer(cmd);
		Fixture_Destroy(f);
		return 0;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Fixture_Destroy(f);
		return 0;
	}
	float       background = config->model_count ? 0 : .005f;
	const float clear[4]   = { background, background, background, 1 };
	AeronScene_SetClearColor(f->scene, clear);
	AeronScene_SetClearColor(f->reference_scene, clear);
	return 1;
}

void Fixture_Destroy(Fixture* f) {
	AeronDrawList_Destroy(f->chart_draws);
	Aeron_DestroyRenderTarget(f->chart_rt);
	AeronScene_Destroy(f->scene);
	AeronScene_Destroy(f->reference_scene);
	for (int i = 0; i < CAPTURE_MAX_MODELS; ++i)
		AeronScene_MeshDestroy(f->meshes[i].mesh);
	memset(f, 0, sizeof *f);
}

static AeronSceneCamera camera_at(const CaptureConfig* c, double time) {
	const double     rad        = 3.14159265358979323846 / 180;
	double           half_angle = c->pan_speed * time * rad * .5;
	double           direction  = c->pan_direction * rad;
	AeronSceneCamera camera = { .ori = { (float)cos(half_angle), (float)(-sin(direction) * sin(half_angle)),
										 (float)(cos(direction) * sin(half_angle)), 0 },
								.v_half_rad = .523598776f,
								.near_z     = .1f,
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

static void add_objects(AeronScene3D* scene, const Fixture* f) {
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
			.no_local_lights              = 1,
			.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW | AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW,
			.transform = { cs * o[0], -sn * o[1], 0, o[3], sn * o[0], cs * o[1], 0, o[4], 0, 0, o[2], o[5], 0,
						   0, 0, 1 }
		};
		if (f->config.model_count)
			place_model(instance.transform, mesh, o, i);
		memcpy(instance.prev_transform, instance.transform, sizeof instance.transform);
		AeronScene_AddMeshInstance(scene, &instance);
	}
}

static void set_lighting(AeronScene3D* scene, const CaptureConfig* config) {
	/* Float4 rows of PbrLightFS in scene_pbr_lighting.hlsli, FS b1.
	 * Camera stays at the origin; the light remains fixed in world space. */
	float light[23][4] = {
		[0] = { 1, 0, 0, 0 }, /* diffuse intensity; specular disabled */
		[1] = { 0, 0, 1, (float)config->width },
		[2] = { (float)config->height, 0, 1, 0 },
		[4] = { .32444284f, .48666426f, .81110711f, 0 },
		[5] = { .9f, .9f, .9f, 0 },
	};
	for (int row = 6; row < 12; ++row)
		for (int channel = 0; channel < 3; ++channel)
			light[row][channel] = .08f;
	AeronScene_SetFrameUniformData(scene, AERON_SHADER_STAGE_FRAGMENT, 1, light, sizeof light);
}

AeronRenderTarget* Fixture_Record(Fixture* f, double time, int blur, AeronCommandBuffer* cmd) {
	if (f->config.bloom_chart)
		return BloomChart_Record(f, cmd);
	AeronScene3D*    scene    = blur ? f->scene : f->reference_scene;
	AeronSceneCamera current  = camera_at(&f->config, time);
	AeronSceneCamera previous = camera_at(&f->config, time - 1.0 / f->config.fps);
	float            previous_matrix[16];
	AeronScene_ComputeViewProj(&previous, previous_matrix);
	if (!AeronScene_Begin(scene, &current))
		return NULL;
	AeronScene_SetPost(
		scene, &(AeronScenePostDesc) { .mb_quality     = blur ? f->config.quality : 0,
									   .mb_shutter     = (float)(f->config.shutter * .032 * f->config.fps),
									   .mb_camera_blur = 1 });
	AeronScene_SetMotionContext(scene, previous_matrix, 1);
	/* Emissive shapes also read the PBR lighting/mode uniforms. */
	set_lighting(scene, &f->config);
	add_objects(scene, f);
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
