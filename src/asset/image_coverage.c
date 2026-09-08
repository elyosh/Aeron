#include "aeron/image.h"
#include <stdlib.h>
#include <string.h>

void Aeron_ImageFreeCoverage(AeronImageCoverage* coverage) {
	if (!coverage)
		return;
	free(coverage->rects);
	memset(coverage, 0, sizeof *coverage);
}

bool Aeron_ImageBuildCoverageRgba8(const uint8_t* rgba, int width, int height, AeronImageCoverage* out) {
	if (out)
		memset(out, 0, sizeof *out);
	if (!rgba || !out || width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX)
		return false;
	const int columns = (width + 3) / 4;
	const int rows    = (height + 3) / 4;
	if ((size_t)width > SIZE_MAX / 4 / (size_t)height ||
		(size_t)columns > SIZE_MAX / sizeof(AeronImageCoverageRect) / (size_t)rows)
		return false;
	const size_t            cell_count = (size_t)columns * (size_t)rows;
	uint8_t*                cells      = calloc(cell_count, 1);
	AeronImageCoverageRect* rects      = calloc(cell_count, sizeof *rects);
	if (!cells || !rects) {
		free(cells);
		free(rects);
		return false;
	}
	for (int cell_y = 0; cell_y < rows; ++cell_y)
		for (int cell_x = 0; cell_x < columns; ++cell_x)
			for (int y = cell_y * 4; y < height && y < cell_y * 4 + 4; ++y)
				for (int x = cell_x * 4; x < width && x < cell_x * 4 + 4; ++x)
					if (rgba[((size_t)y * (size_t)width + (size_t)x) * 4 + 3])
						cells[(size_t)cell_y * (size_t)columns + (size_t)cell_x] = 1;
	uint32_t count = 0;
	for (int y = 0; y < rows; ++y) {
		for (int x = 0; x < columns;) {
			if (!cells[(size_t)y * (size_t)columns + (size_t)x]) {
				++x;
				continue;
			}
			int run_width = 1;
			while (x + run_width < columns && cells[(size_t)y * (size_t)columns + (size_t)(x + run_width)])
				++run_width;
			int run_height = 1;
			for (; y + run_height < rows; ++run_height) {
				bool full = true;
				for (int column = 0; column < run_width; ++column)
					if (!cells[(size_t)(y + run_height) * (size_t)columns + (size_t)(x + column)])
						full = false;
				if (!full)
					break;
			}
			for (int row = 0; row < run_height; ++row)
				memset(cells + (size_t)(y + row) * (size_t)columns + (size_t)x, 0, (size_t)run_width);
			rects[count++] = (AeronImageCoverageRect) {
				.x      = (int32_t)(x * 4),
				.y      = (int32_t)(y * 4),
				.width  = (int32_t)((x * 4 + run_width * 4 > width) ? width - x * 4 : run_width * 4),
				.height = (int32_t)((y * 4 + run_height * 4 > height) ? height - y * 4 : run_height * 4),
			};
			x += run_width;
		}
	}
	free(cells);
	out->rects  = rects;
	out->count  = count;
	out->width  = (uint16_t)width;
	out->height = (uint16_t)height;
	return true;
}
