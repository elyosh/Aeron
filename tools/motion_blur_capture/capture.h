#ifndef AERON_MB_CAPTURE_H
#define AERON_MB_CAPTURE_H

#include "aeron/aeron.h"
#include "aeron/scene/scene3d.h"

#define CAPTURE_MAX_MODELS 8

typedef struct CaptureConfig {
	int    width, height, frames, reference_samples, quality;
	double fps, pan_speed, pan_direction, shutter;
	char   output_dir[1024];
	char   asset_root[1024];
	char   models[CAPTURE_MAX_MODELS][1024];
	int    model_count;
} CaptureConfig;

typedef struct CaptureMesh {
	AeronSceneMesh* mesh;
	float           center[3], extent;
} CaptureMesh;

typedef struct Fixture {
	AeronScene3D *scene, *reference_scene;
	CaptureMesh   meshes[CAPTURE_MAX_MODELS];
	CaptureConfig config;
} Fixture;

int                CaptureConfig_Load(const char* path, CaptureConfig* config);
int                Fixture_Create(Fixture* fixture, const CaptureConfig* config);
void               Fixture_Destroy(Fixture* fixture);
AeronRenderTarget* Fixture_Render(Fixture* fixture, double time, int blur);
int Capture_Read(AeronRenderTarget* target, const CaptureConfig* config, uint16_t* staging, float* rgb);
int Capture_Write(const CaptureConfig* config, int frame, const char* name, const float* rgb);
int Capture_Report(const CaptureConfig* config, int frame, const float* actual, const float* reference);
int Capture_SaveConfig(const CaptureConfig* config);

#endif
