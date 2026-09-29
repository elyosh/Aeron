#include "capture.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define BENCHMARK_MAX_SAMPLES 31
#define BENCHMARK_MAX_IN_FLIGHT 4

typedef struct BenchmarkSample {
	uint64_t frames;
	double   elapsed_ms, cpu_ms;
} BenchmarkSample;

typedef struct BenchmarkSummary {
	double   mean_ms, median_ms, min_ms, max_ms, variation_percent;
	double   elapsed_ms, cpu_ms;
	uint64_t frames;
} BenchmarkSummary;

static int run_sample(Fixture* fixture, CaptureBloom* post, double seconds, int min_frames,
					  BenchmarkSample* sample) {
	const CaptureConfig* c                                = &fixture->config;
	AeronGpuFence*       pending[BENCHMARK_MAX_IN_FLIGHT] = { 0 };
	const double         frequency                        = (double)SDL_GetPerformanceFrequency();
	uint64_t             frames = 0, cpu_ticks = 0;
	int                  ok    = 1;
	const uint64_t       start = SDL_GetPerformanceCounter();
	while (ok) {
		Aeron_PumpEvents();
		if (Aeron_QuitRequested() || Aeron_FatalErrorRequested()) {
			ok = 0;
			break;
		}
		AeronGpuFence** slot    = &pending[frames % (uint64_t)c->benchmark.frames_in_flight];
		AeronGpuFence*  retired = *slot;
		if (retired && !Aeron_WaitForGpuFence(retired)) {
			ok = 0;
			break;
		}
		/* Repeat the exact captured poses, including each pose's previous
		 * camera matrix. Longer timing runs cannot pan into an empty scene. */
		double              time      = (double)(frames % (uint64_t)c->frames) / c->fps;
		uint64_t            cpu_start = SDL_GetPerformanceCounter();
		AeronCommandBuffer* cmd       = Aeron_AcquireCommandBuffer();
		if (!cmd) {
			ok = 0;
			break;
		}
		AeronRenderTarget* target = Fixture_Record(fixture, time, 1, cmd);
		if (!target || !CaptureBloom_Record(post, c, target, cmd)) {
			Aeron_CancelCommandBuffer(cmd);
			ok = 0;
			break;
		}
		AeronGpuFence* next = Aeron_SubmitCommandBufferAndAcquireFence(cmd);
		if (!next) {
			ok = 0;
			break;
		}
		*slot = next;
		/* SDL Metal retires submitted command buffers during the next submit.
		 * Keep their completed fences alive until then: early fence recycling
		 * can prevent retirement and retain render resources on SDL 3.4.14. */
		Aeron_ReleaseGpuFence(retired);
		cpu_ticks += SDL_GetPerformanceCounter() - cpu_start;
		++frames;
		/* Stop only on a complete sequence, so every sample weights the same
		 * poses equally. Fill and drain overhead remain in the measurement. */
		if (frames >= (uint64_t)min_frames && frames % (uint64_t)c->frames == 0 &&
			(double)(SDL_GetPerformanceCounter() - start) / frequency >= seconds)
			break;
	}
	for (int i = 0; i < c->benchmark.frames_in_flight; ++i)
		if (pending[i] && !Aeron_WaitForGpuFence(pending[i]))
			ok = 0;
	/* No next submission follows this window. Retire command buffers before
	 * returning their fences to the backend pool, including on error paths. */
	if (!Aeron_WaitForGpuIdle())
		ok = 0;
	for (int i = 0; i < c->benchmark.frames_in_flight; ++i)
		Aeron_ReleaseGpuFence(pending[i]);
	const uint64_t end = SDL_GetPerformanceCounter();
	*sample            = (BenchmarkSample) { .frames     = frames,
											 .elapsed_ms = (end - start) * 1000.0 / frequency,
											 .cpu_ms     = cpu_ticks * 1000.0 / frequency };
	return ok && frames > 0 && sample->elapsed_ms > 0;
}

static BenchmarkSummary summarize(const BenchmarkSample* samples, int count) {
	BenchmarkSummary s = { 0 };
	double           sorted[BENCHMARK_MAX_SAMPLES], mean = 0, variance = 0;
	for (int i = 0; i < count; ++i) {
		s.elapsed_ms += samples[i].elapsed_ms;
		s.cpu_ms += samples[i].cpu_ms;
		s.frames += samples[i].frames;
		double value = samples[i].elapsed_ms / (double)samples[i].frames;
		mean += value / count;
		int j = i;
		while (j && sorted[j - 1] > value) {
			sorted[j] = sorted[j - 1];
			--j;
		}
		sorted[j] = value;
	}
	for (int i = 0; i < count; ++i)
		variance += (sorted[i] - mean) * (sorted[i] - mean);
	s.mean_ms           = s.elapsed_ms / (double)s.frames;
	s.median_ms         = (sorted[(count - 1) / 2] + sorted[count / 2]) * .5;
	s.min_ms            = sorted[0];
	s.max_ms            = sorted[count - 1];
	s.variation_percent = mean > 0 ? 100 * sqrt(variance / (count - 1)) / mean : 0;
	return s;
}

static int write_string(SDL_IOStream* out, const char* key, const char* value) {
	char prefix[128];
	int  n = snprintf(prefix, sizeof prefix, "%s: \"", key);
	if (n < 0 || (size_t)n >= sizeof prefix || SDL_WriteIO(out, prefix, (size_t)n) != (size_t)n)
		return 0;
	for (const unsigned char* p = (const unsigned char*)value; *p; ++p) {
		char escaped[8];
		if (*p < 32 || *p == 127)
			n = snprintf(escaped, sizeof escaped, "\\u%04x", *p);
		else if (*p == '"' || *p == '\\')
			n = snprintf(escaped, sizeof escaped, "\\%c", *p);
		else {
			escaped[0] = (char)*p;
			n          = 1;
		}
		if (SDL_WriteIO(out, escaped, (size_t)n) != (size_t)n)
			return 0;
	}
	return SDL_WriteIO(out, "\"\n", 2) == 2;
}

static int write_report(const CaptureConfig* c, const BenchmarkSample* priming, const BenchmarkSample* warmup,
						const BenchmarkSample* samples, BenchmarkSummary summary) {
	char path[1200], text[4096];
	int  n = snprintf(path, sizeof path, "%s/benchmark.yaml", c->output_dir);
	if (n < 0 || (size_t)n >= sizeof path)
		return 0;
	SDL_IOStream* out = SDL_IOFromFile(path, "wb");
	if (!out)
		return 0;
	AeronGpuDeviceInfo gpu = Aeron_RenderGpuDeviceInfo();
	int                ok =
		write_string(out, "backend", Aeron_RenderDriverName()) && write_string(out, "device", gpu.name) &&
		write_string(out, "driver_version", gpu.driver_version) &&
		write_string(out, "platform", SDL_GetPlatform()) && write_string(out, "build", CAPTURE_BUILD_TYPE) &&
		write_string(out, "compiler", CAPTURE_COMPILER);
	n = snprintf(
		text, sizeof text,
		"schema_version: 2\nsdl_version: %d\ngpu_validation_enabled: %s\n"
		"method: bounded_queue_completed_frame_throughput\ncompleted_fence_retirement: "
		"after_next_submission_or_idle\nclock: SDL_performance_counter\n"
		"gpu_execution_ms: null\nincludes: [scene_preparation, uploads, rendering, enabled_effects, "
		"tonemap, submission, fence_waits, event_pump]\n"
		"excludes: [resource_setup, priming, warmup, reference_renders, bloom_readback_conversion, "
		"image_readback, image_encoding, disk_io, vsync, swapchain_present]\n"
		"scene_size: [%d, %d]\nsequence_frames: %d\nsimulation_fps: %.9g\n"
		"frames_in_flight: %d\nbloom_chain_executed: %s\nmotion_blur_quality: %d\n"
		"sample_count: %d\nminimum_frames_per_sample: %d\nrequested_sample_seconds: %.9g\n"
		"priming_frames: %llu\npriming_elapsed_ms: %.9g\nwarmup_frames: %llu\nwarmup_elapsed_ms: %.9g\n"
		"measured_frames: %llu\nmeasured_elapsed_ms: %.9g\n"
		"mean_frame_ms: %.9g\nthroughput_fps: %.9g\nmean_cpu_record_submit_ms: %.9g\n"
		"sample_mean_frame_ms:\n  median: %.9g\n  min: %.9g\n  max: %.9g\n  "
		"coefficient_of_variation_percent: %.9g\n"
		"sample_variation_within_5_percent: %s\n"
		"statistics_note: Sample statistics describe window averages, not individual frame latency "
		"or GPU time.\n"
		"samples:\n",
		SDL_GetVersion(), gpu.validation_enabled ? "true" : "false", c->width, c->height, c->frames, c->fps,
		c->benchmark.frames_in_flight, c->bloom_enabled && c->bloom_intensity > 0 ? "true" : "false",
		c->quality, c->benchmark.samples, c->benchmark.min_frames, c->benchmark.sample_seconds,
		(unsigned long long)priming->frames, priming->elapsed_ms, (unsigned long long)warmup->frames,
		warmup->elapsed_ms, (unsigned long long)summary.frames, summary.elapsed_ms, summary.mean_ms,
		1000 / summary.mean_ms, summary.cpu_ms / (double)summary.frames, summary.median_ms, summary.min_ms,
		summary.max_ms, summary.variation_percent, summary.variation_percent <= 5 ? "true" : "false");
	ok = ok && n > 0 && (size_t)n < sizeof text && SDL_WriteIO(out, text, (size_t)n) == (size_t)n;
	for (int i = 0; ok && i < c->benchmark.samples; ++i) {
		const BenchmarkSample* s = &samples[i];
		n  = snprintf(text, sizeof text,
					  "  - index: %d\n    frames: %llu\n    elapsed_ms: %.9g\n    mean_frame_ms: %.9g\n    "
					  "mean_cpu_record_submit_ms: %.9g\n",
					  i, (unsigned long long)s->frames, s->elapsed_ms, s->elapsed_ms / (double)s->frames,
					  s->cpu_ms / (double)s->frames);
		ok = n > 0 && (size_t)n < sizeof text && SDL_WriteIO(out, text, (size_t)n) == (size_t)n;
	}
	if (!SDL_CloseIO(out))
		ok = 0;
	return ok;
}

int Capture_Benchmark(Fixture* fixture) {
	const CaptureConfig* c       = &fixture->config;
	CaptureBloom         post    = { 0 };
	BenchmarkSample      priming = { 0 }, warmup = { 0 }, samples[BENCHMARK_MAX_SAMPLES] = { 0 };
	if (!CaptureBloom_Create(&post, c, 0))
		return 0;
	AeronGpuDeviceInfo gpu = Aeron_RenderGpuDeviceInfo();
	if (gpu.validation_enabled)
		Aeron_LogWarn("scene_capture",
					  "GPU validation is enabled; use a Release build for performance comparisons");
	Aeron_LogInfo("scene_capture", "Benchmark warmup: at least %.2f s and %d frames",
				  c->benchmark.warmup_seconds, c->benchmark.min_frames);
	/* Prime every pose first: lazy pipeline creation cannot consume the
	 * subsequent timed warm-up period. Both phases drain their GPU work. */
	int ok = Aeron_WaitForGpuIdle() && run_sample(fixture, &post, 0, c->frames, &priming) &&
			 run_sample(fixture, &post, c->benchmark.warmup_seconds, c->benchmark.min_frames, &warmup);
	for (int i = 0; ok && i < c->benchmark.samples; ++i) {
		ok = run_sample(fixture, &post, c->benchmark.sample_seconds, c->benchmark.min_frames, &samples[i]);
		if (ok)
			Aeron_LogInfo("scene_capture",
						  "Benchmark sample %d/%d: %.4f ms per completed frame (%llu frames)", i + 1,
						  c->benchmark.samples, samples[i].elapsed_ms / (double)samples[i].frames,
						  (unsigned long long)samples[i].frames);
	}
	if (ok) {
		BenchmarkSummary summary = summarize(samples, c->benchmark.samples);
		ok                       = write_report(c, &priming, &warmup, samples, summary);
		if (ok) {
			Aeron_LogInfo("scene_capture",
						  "Completed-frame mean %.4f ms, sample median %.4f ms, variation %.2f%%",
						  summary.mean_ms, summary.median_ms, summary.variation_percent);
			if (summary.variation_percent > 5)
				Aeron_LogWarn(
					"scene_capture",
					"Timing variation exceeds 5%%; repeat under steady load or increase sample duration");
		}
	}
	CaptureBloom_Destroy(&post);
	return ok;
}
