#include "capture.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>

/* Integrals use image area, not pixel count, so different capture resolutions
 * are comparable. Moments use image-height units on both axes. No brightness
 * cutoff is applied: faint halo pixels are part of the measurement. */
typedef struct Moments {
	double energy, peak, x, y, sigma_x, sigma_y;
	double rgb[3];
} Moments;

static Moments measure(const float* rgb, int width, int height, int column, int row, int columns, int rows) {
	Moments m      = { 0 };
	double  weight = 0, xx = 0, yy = 0;
	int     x0 = column * width / columns, x1 = (column + 1) * width / columns;
	int     y0 = row * height / rows, y1 = (row + 1) * height / rows;
	for (int y = y0; y < y1; ++y) {
		for (int x = x0; x < x1; ++x) {
			const float* p = rgb + ((size_t)y * width + x) * 3;
			double       v = 0;
			for (int k = 0; k < 3; ++k) {
				m.rgb[k] += p[k];
				m.peak = fmax(m.peak, p[k]);
				v += p[k];
			}
			double u = (x + .5) / height, t = (y + .5) / height;
			weight += v;
			m.x += v * u;
			m.y += v * t;
			xx += v * u * u;
			yy += v * t * t;
		}
	}
	if (weight > 0) {
		m.x /= weight;
		m.y /= weight;
		m.sigma_x = sqrt(fmax(0, xx / weight - m.x * m.x));
		m.sigma_y = sqrt(fmax(0, yy / weight - m.y * m.y));
	}
	for (int k = 0; k < 3; ++k)
		m.rgb[k] /= (double)width * height;
	m.energy = weight / ((double)width * height);
	return m;
}

static int write_region(SDL_IOStream* out, const char* name, Moments source, Moments bloom) {
	char text[1800];
	char gain[64];
	if (source.energy > 0)
		snprintf(gain, sizeof gain, "%.9g", bloom.energy / source.energy);
	else
		snprintf(gain, sizeof gain, "null");
	int n = snprintf(text, sizeof text,
					 "  %s:\n"
					 "    scene_rgb_integral: [%.9g, %.9g, %.9g]\n"
					 "    bloom_rgb_integral: [%.9g, %.9g, %.9g]\n"
					 "    bloom_to_scene_energy_ratio: %s\n"
					 "    bloom_peak: %.9g\n"
					 "    scene_sigma_over_height: [%.9g, %.9g]\n"
					 "    bloom_sigma_over_height: [%.9g, %.9g]\n"
					 "    bloom_centroid_over_height: [%.9g, %.9g]\n",
					 name, source.rgb[0], source.rgb[1], source.rgb[2], bloom.rgb[0], bloom.rgb[1],
					 bloom.rgb[2], gain, bloom.peak, source.sigma_x, source.sigma_y, bloom.sigma_x,
					 bloom.sigma_y, bloom.x, bloom.y);
	return n > 0 && (size_t)n < sizeof text && SDL_WriteIO(out, text, (size_t)n) == (size_t)n;
}

int CaptureBloom_Report(const CaptureConfig* c, int frame, const float* source, const float* bloom, int bw,
						int bh) {
	char path[1200], header[800];
	int  n = snprintf(path, sizeof path, "%s/frame_%03d_bloom_metrics.yaml", c->output_dir, frame);
	if (n < 0 || (size_t)n >= sizeof path)
		return 0;
	SDL_IOStream* out = SDL_IOFromFile(path, "wb");
	if (!out)
		return 0;
	n      = snprintf(header, sizeof header,
					  "frame: %d\ntime_seconds: %.9g\nscene_size: [%d, %d]\nbloom_size: [%d, %d]\n"
					  "measurement: raw_bloom_before_present_kernel_and_intensity\n"
					  "integral_units: RGB_sum_divided_by_image_pixel_count\n"
					  "moment_weight: RGB_sum\nintensity: %.9g\npresent_kernel: %d\n"
					  "chart_grid: [%d, %d]\nregions:\n",
					  frame, frame / c->fps, c->width, c->height, bw, bh, c->bloom_intensity,
					  c->bloom_kernel == AERON_SCENE_BLOOM_KERNEL_1_TAP ? 1 : 4, c->bloom_chart ? 4 : 1,
					  c->bloom_chart ? 3 : 1);
	int ok = n > 0 && (size_t)n < sizeof header && SDL_WriteIO(out, header, (size_t)n) == (size_t)n;
	if (ok)
		ok = write_region(out, "whole_image", measure(source, c->width, c->height, 0, 0, 1, 1),
						  measure(bloom, bw, bh, 0, 0, 1, 1));
	for (int i = 0; ok && c->bloom_chart && i < 12; ++i) {
		ok = write_region(out, BloomChart_Label(i), measure(source, c->width, c->height, i % 4, i / 4, 4, 3),
						  measure(bloom, bw, bh, i % 4, i / 4, 4, 3));
	}
	if (!SDL_CloseIO(out))
		ok = 0;
	return ok;
}
