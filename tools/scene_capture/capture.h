#ifndef AERON_SCENE_CAPTURE_H
#define AERON_SCENE_CAPTURE_H

#include "aeron/aeron.h"
#include "aeron/scene/bloom.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/present.h"
#include "aeron/scene/scene3d.h"

#define CAPTURE_MAX_MODELS 8

int Capture_Compare(const char* baseline, const char* candidate);

typedef struct CaptureBenchmarkConfig {
	double warmup_seconds, sample_seconds;
	int    samples, min_frames, frames_in_flight;
} CaptureBenchmarkConfig;

typedef struct CaptureConfig {
	int                    width, height, frames, reference_samples, quality;
	double                 fps, pan_speed, pan_direction, shutter;
	char                   output_dir[1024];
	char                   asset_root[1024];
	char                   models[CAPTURE_MAX_MODELS][1024];
	int                    model_count;
	int                    bloom_chart, bloom_enabled, bloom_kernel, msaa_samples, tonemap;
	float                  bloom_intensity, emissive_scale;
	CaptureBenchmarkConfig benchmark;
	int                    benchmark_mode;
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
