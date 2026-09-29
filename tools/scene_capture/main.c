#include "aeron/main.h"
#include "capture.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
	if (argc == 4 && !strcmp(argv[1], "compare"))
		return Capture_Compare(argv[2], argv[3]) ? 0 : 2;
	const int benchmark = argc == 3 && !strcmp(argv[1], "benchmark");
	if (argc != 2 && !benchmark) {
		fprintf(
			stderr,
			"Usage: scene_capture <capture.yaml relative to working directory>\n"
			"       scene_capture benchmark <capture.yaml>\n"
			"       scene_capture compare <baseline bloom metrics.yaml> <candidate bloom metrics.yaml>\n");
		return 2;
	}
	CaptureConfig c = { 0 };
	if (!CaptureConfig_Load(argv[benchmark ? 2 : 1], &c))
		return 2;
	c.benchmark_mode = benchmark;
	if (!SDL_CreateDirectory(c.output_dir)) {
		fprintf(stderr, "Cannot create output directory: %s\n", SDL_GetError());
		return 1;
	}
	/* Refuse accidental replacement of a previous run's captures. */
	char manifest[1200];
	snprintf(manifest, sizeof manifest, "%s/capture.yaml", c.output_dir);
	SDL_PathInfo info;
	if (SDL_GetPathInfo(manifest, &info)) {
		fprintf(stderr, "Output directory already contains a capture: %s\n", c.output_dir);
		return 2;
	}
	AeronConfig startup = { .org_name          = "Aeron",
							.app_name          = "SceneCapture",
							.window_title      = "Aeron scene capture",
							.window_width      = 800,
							.window_height     = 450,
							.logical_width     = c.width,
							.logical_height    = c.height,
							.shader_path       = SCENE_CAPTURE_SHADER_DIR,
							.presentation_mode = AERON_PRESENTATION_ASPECT_FIT };
	if (!Aeron_Init(&startup))
		return 1;
	/* Captures use a fixed SDR presentation transform, independent of the desktop. */
	Aeron_SetOutputHdr(0);
	AeronScenePresent_SetTonemapOp(c.tonemap);
	AeronScenePresent_SetBloomKernel(c.bloom_kernel);
	Fixture fixture;
	int     ok = Fixture_Create(&fixture, &c);
	if (ok) {
		ok = Capture_SaveConfig(&c) && (benchmark ? Capture_Benchmark(&fixture) : Capture_Run(&fixture));
		Fixture_Destroy(&fixture);
	}
	if (!ok) {
		const char* error = Aeron_RenderLastError();
		if (!error || !error[0])
			error = SDL_GetError();
		Aeron_LogError("scene_capture", "Capture failed (allocation, rendering, or output): %s", error);
	}
	Aeron_Shutdown();
	return ok ? 0 : 1;
}
