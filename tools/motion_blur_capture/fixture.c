#include "capture.h"

#include <math.h>
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

int Fixture_Create(Fixture* f, const CaptureConfig* config) {
	memset(f, 0, sizeof *f);
	f->config               = *config;
	AeronScene3DDesc desc   = { .rt_width             = config->width,
								.rt_height            = config->height,
								.color_format         = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
								.with_normal_rt       = 1,
								.sample_count         = AERON_SAMPLE_COUNT_1,
								.view_space_to_meters = 1 };
	f->scene                = AeronScene_Create(&desc);
	f->reference_scene      = AeronScene_Create(&desc);
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!f->scene || !f->reference_scene || !cmd) {
		if (cmd)
			Aeron_CancelCommandBuffer(cmd);
		Fixture_Destroy(f);
		return 0;
	}
	f->mesh = create_cube(cmd);
	if (!f->mesh) {
		Aeron_CancelCommandBuffer(cmd);
		Fixture_Destroy(f);
		return 0;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Fixture_Destroy(f);
		return 0;
	}
	AeronScene_SetClearColor(f->scene, (const float[4]) { .005f, .005f, .005f, 1 });
	AeronScene_SetClearColor(f->reference_scene, (const float[4]) { .005f, .005f, .005f, 1 });
	return 1;
}

void Fixture_Destroy(Fixture* f) {
	AeronScene_Destroy(f->scene);
	AeronScene_Destroy(f->reference_scene);
	AeronScene_MeshDestroy(f->mesh);
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

static void add_objects(AeronScene3D* scene, const AeronSceneMesh* mesh) {
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
		const float*           o  = objects[i];
		float                  cs = cosf(o[6]), sn = sinf(o[6]);
		AeronSceneMeshInstance instance = {
			.mesh                         = mesh,
			.base_color_emissive_strength = o[7],
			.no_local_lights              = 1,
			.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW | AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW,
			.transform = { cs * o[0], -sn * o[1], 0, o[3], sn * o[0], cs * o[1], 0, o[4], 0, 0, o[2], o[5], 0,
						   0, 0, 1 }
		};
		memcpy(instance.prev_transform, instance.transform, sizeof instance.transform);
		AeronScene_AddMeshInstance(scene, &instance);
	}
}

AeronRenderTarget* Fixture_Render(Fixture* f, double time, int blur) {
	Aeron_PumpEvents();
	if (Aeron_QuitRequested() || Aeron_FatalErrorRequested())
		return NULL;
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
	add_objects(scene, f->mesh);
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd)
		return NULL;
	if (!AeronScene_Render(scene, cmd)) {
		Aeron_CancelCommandBuffer(cmd);
		return NULL;
	}
	if (!Aeron_SubmitCommandBuffer(cmd))
		return NULL;
	return AeronScene_SceneRt(scene);
}
