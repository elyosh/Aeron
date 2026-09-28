#ifndef AERON_MB_CAPTURE_H
#define AERON_MB_CAPTURE_H

#include "aeron/aeron.h"
#include "aeron/scene/scene3d.h"

typedef struct CaptureConfig {
	int    width, height, frames, reference_samples, quality;
	double fps, pan_speed, pan_direction, shutter;
	char   output_dir[1024];
} CaptureConfig;

typedef struct Fixture {
	AeronScene3D *  scene, *reference_scene;
	AeronSceneMesh* mesh;
	CaptureConfig   config;
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
