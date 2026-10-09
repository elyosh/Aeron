#ifndef AERON_SCENE_CAPTURE_H
#define AERON_SCENE_CAPTURE_H

#include "aeron/aeron.h"
#include "aeron/scene/bloom.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/present.h"
#include "aeron/scene/scene3d.h"
#include "aeron/scene/settings.h"
#include "aeron/temporal.h"

#define CAPTURE_MAX_MODELS 8
#define CAPTURE_MAX_INSTANCES 16
#define CAPTURE_MAX_LIGHTS AERON_SCENE_POINT_LIGHT_CAPACITY

int Capture_Compare(const char* baseline, const char* candidate);

typedef struct CaptureBenchmarkConfig {
	double warmup_seconds, sample_seconds;
	int    samples, min_frames, frames_in_flight;
} CaptureBenchmarkConfig;

/* game_scene instance roles: a lit ship, the cockpit at the screen_test eye
 * (its own receiver-local shadow, casts none), or the craft's exterior at
 * the same place, hidden and casting only (as in the game's cockpit view). */
enum { CAPTURE_ROLE_SHIP, CAPTURE_ROLE_COCKPIT, CAPTURE_ROLE_EXTERIOR };

/* Highest game_scene instance markings value. */
#define CAPTURE_MAX_MARKINGS 255

typedef struct CaptureInstance {
	int   model, role;
	int   markings;                 /* XWAU markings value (variant row), -1 when not given */
	float position[3], rotation[3]; /* metres, camera axes; yaw/pitch/roll degrees */
} CaptureInstance;

/* A game light: position in metres and intensity (the linear illuminance
 * 1 m away), drawn as the game's physical point lights; a non-zero half
 * axis (metres) makes it a tube along position +- half_axis, as the game's
 * laser streaks. */
typedef struct CaptureLight {
	float position[3], intensity[3], half_axis[3];
} CaptureLight;

/* game_scene: the game's Scene3D features (Scene3D defaults file), lighting
 * and instance roles, in game world units (OPT units). */
typedef struct CaptureGameScene {
	int                      enabled;
	AeronSceneSsaoSettings   ssao;
	AeronSceneShadowSettings shadows;
	float                    sun_direction[3], sun_color[3], ambient;
	/* The cockpit and exterior turn with the panning camera (the craft
	 * turns) instead of staying put (the pilot looks around). */
	int                      cockpit_follows_camera;
	/* Intensity multiplier of the cockpit's model lights (0 = off), as
	 * point_lights.model_light_intensity in the game. */
	float                    model_light_intensity;
	/* FSR temporal upscaling, as the game's temporal_upscaling settings;
	 * benchmarks only (output depends on frame history), without MSAA. */
	AeronTemporalMode        temporal_mode;
	float                    temporal_sharpness;
	CaptureInstance          instances[CAPTURE_MAX_INSTANCES];
	int                      instance_count;
	CaptureLight             lights[CAPTURE_MAX_LIGHTS];
	int                      light_count;
} CaptureGameScene;

typedef struct CaptureConfig {
	int                    width, height, frames, reference_samples, quality;
	double                 fps, pan_speed, pan_direction, shutter;
	char                   output_dir[1024];
	char                   asset_root[1024];
	char                   models[CAPTURE_MAX_MODELS][1024];
	int                    model_count;
	int                    bloom_chart, bloom_enabled, bloom_kernel, msaa_samples, tonemap;
	int                    cull_mode, backface_fill;
	float                  bloom_intensity, emissive_scale;
	/* screen_test: the first model alone, seen from `screen_eye` (model
	 * metres) at yaw/pitch degrees, with test screen inputs. */
	int                    screen_test;
	float                  screen_eye[3], screen_yaw, screen_pitch;
	CaptureBenchmarkConfig benchmark;
	int                    benchmark_mode;
	CaptureGameScene       game;
} CaptureConfig;

typedef struct CaptureMesh {
	AeronSceneMesh* mesh;
	float           center[3], extent;
} CaptureMesh;

typedef struct Fixture {
	AeronScene3D *     scene, *reference_scene;
	CaptureMesh        meshes[CAPTURE_MAX_MODELS];
	CaptureConfig      config;
	AeronRenderTarget* chart_rt;
	AeronDrawList2D*   chart_draws;
	AeronTexture *     screen_graphics, *screen_text;
} Fixture;

typedef struct CaptureBloom {
	AeronSceneBloom*        bloom;
	AeronScenePresentChain* present;
	AeronSampler*           sampler;
	AeronDrawList2D*        draws;
	AeronRenderTarget *     readback_rt, *present_rt;
	int                     width, height;
} CaptureBloom;

int                CaptureConfig_Load(const char* path, CaptureConfig* config);
int                Fixture_Create(Fixture* fixture, const CaptureConfig* config);
void               Fixture_Destroy(Fixture* fixture);
AeronRenderTarget* Fixture_Render(Fixture* fixture, double time, int blur);
int  Capture_Read(AeronRenderTarget* target, int width, int height, uint16_t* staging, float* rgb);
int  Capture_Write(const CaptureConfig* config, int frame, const char* name, const float* rgb);
int  Capture_WriteImage(const CaptureConfig* config, int frame, const char* name, int width, int height,
						const float* rgb);
int  Capture_Report(const CaptureConfig* config, int frame, const float* actual, const float* reference);
int  Capture_SaveConfig(const CaptureConfig* config);
int  Capture_Run(Fixture* fixture);
int  CaptureBloom_Create(CaptureBloom* bloom, const CaptureConfig* config, int readback);
void CaptureBloom_Destroy(CaptureBloom* bloom);
int  CaptureBloom_Render(CaptureBloom* bloom, const CaptureConfig* config, AeronRenderTarget* source);
int  CaptureBloom_Report(const CaptureConfig* config, int frame, const float* source, const float* bloom,
						 int bloom_width, int bloom_height);
int  BloomChart_Create(Fixture* fixture);
const char* BloomChart_Label(int index);

/* Record variants borrow cmd; the caller submits or cancels it. */
AeronRenderTarget* Fixture_Record(Fixture* fixture, double time, int blur, AeronCommandBuffer* cmd);
AeronRenderTarget* BloomChart_Record(Fixture* fixture, AeronCommandBuffer* cmd);
int CaptureBloom_Record(CaptureBloom* bloom, const CaptureConfig* config, AeronRenderTarget* source,
						AeronCommandBuffer* cmd);
int Capture_Benchmark(Fixture* fixture);

#endif
