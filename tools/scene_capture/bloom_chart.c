#include "capture.h"

#include <math.h>

/* A fixed UV layout isolates each target in its own quarter-width/third-height
 * region. Sizes are fractions of image height so aspect ratio cannot stretch
 * the emitter. This fixture bypasses mesh lighting and MSAA deliberately. */
static const struct {
	const char* label;
	float       rgb[3];
	float       width, height;
} targets[] = { { "below_threshold", { .8f, .8f, .8f }, .035f, .035f },
				{ "threshold", { 1, 1, 1 }, .035f, .035f },
				{ "soft_knee", { 1.15f, 1.15f, 1.15f }, .035f, .035f },
				{ "knee_end", { 1.3f, 1.3f, 1.3f }, .035f, .035f },
				{ "small_2", { 2, 2, 2 }, .01f, .01f },
				{ "small_4", { 4, 4, 4 }, .02f, .02f },
				{ "medium_8", { 8, 8, 8 }, .04f, .04f },
				{ "large_16", { 16, 16, 16 }, .06f, .06f },
				{ "red", { 8, 0, 0 }, .02f, .02f },
				{ "green", { 0, 8, 0 }, .02f, .02f },
				{ "blue", { 0, 0, 8 }, .02f, .02f },
				{ "thin_line", { 8, 8, 8 }, 1.0f / 1080.0f, .1f } };

const char* BloomChart_Label(int index) {
	return index >= 0 && index < (int)(sizeof targets / sizeof targets[0]) ? targets[index].label : NULL;
}

typedef struct CoverageSpan {
	float start, length, coverage;
} CoverageSpan;

static int coverage_spans(float start, float length, CoverageSpan spans[3]) {
	float first = floorf(start), end = start + length, last = floorf(end);
	int   count    = 0;
	spans[count++] = (CoverageSpan) { first, 1, fminf(end, first + 1) - start };
	if (last > first + 1)
		spans[count++] = (CoverageSpan) { first + 1, last - first - 1, 1 };
	if (last > first && end > last)
		spans[count++] = (CoverageSpan) { last, 1, end - last };
	return count;
}

static void add_emitter(AeronDrawList2D* draws, float x, float y, float w, float h, const float rgb[3]) {
	CoverageSpan xs[3], ys[3];
	int          nx = coverage_spans(x, w, xs), ny = coverage_spans(y, h, ys);
	/* Exact box coverage keeps subpixel emitters' input energy stable across
	 * resolutions. Coverage is resolved before the production bright pass. */
	for (int j = 0; j < ny; ++j) {
		for (int i = 0; i < nx; ++i) {
			float coverage = xs[i].coverage * ys[j].coverage;
			float color[4] = { rgb[0] * coverage, rgb[1] * coverage, rgb[2] * coverage, 1 };
			AeronDrawList_AddFill(draws, xs[i].start, ys[j].start, xs[i].length, ys[j].length, color,
								  AERON_BLIT2D_BLEND_NONE, NULL);
		}
	}
}

int BloomChart_Create(Fixture* f) {
	f->chart_rt =
		Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) { .width      = f->config.width,
															.height     = f->config.height,
															.format     = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
															.debug_name = "capture.bloom.chart" });
	f->chart_draws = AeronDrawList_Create(12 * 9);
	if (!f->chart_rt || !f->chart_draws) {
		Fixture_Destroy(f);
		return 0;
	}
	return 1;
}

AeronRenderTarget* BloomChart_Record(Fixture* f, AeronCommandBuffer* cmd) {
	const int   width = f->config.width, height = f->config.height;
	const float black[4] = { 0, 0, 0, 1 };
	AeronDrawList_Begin(f->chart_draws, f->chart_rt, width, height, AERON_DRAWLIST2D_CLEAR, black);
	for (int i = 0; i < (int)(sizeof targets / sizeof targets[0]); ++i) {
		float w = targets[i].width * height, h = targets[i].height * height;
		float x = (i % 4 + .5f) * width / 4, y = (i / 4 + .5f) * height / 3;
		add_emitter(f->chart_draws, x - w * .5f, y - h * .5f, w, h, targets[i].rgb);
	}
	if (!AeronDrawList_Prepare(f->chart_draws, cmd)) {
		return NULL;
	}
	AeronDrawList_Render(f->chart_draws, cmd);
	return f->chart_rt;
}
