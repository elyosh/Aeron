/*
 * fontbake — TTF/TTC → Aeron font atlas CLI.
 *
 * Rasterizes a TrueType font into the <basename>.png + <basename>.fnt
 * (TFNT v2) pair consumed by AeronFontAtlas_Load. All rasterization
 * lives in imgbake's font_atlas.c; this file is argv parsing, file
 * I/O, and a stderr report.
 */

#include "font_atlas.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONTBAKE_DEFAULT_CELL_H 72

static void usage(const char* argv0) {
	fprintf(stderr,
			"usage: %s <input.ttf> <output_basename> [options]\n"
			"\n"
			"Bakes a TrueType font into <output_basename>.png + .fnt (TFNT v2)\n"
			"for AeronFontAtlas_Load. Glyphs are packed into uniform cells.\n"
			"\n"
			"Options:\n"
			"  --cell-h <px>         atlas cell height (default %d). Text renders\n"
			"                        at theme size via downscaling, so bake at or\n"
			"                        above the largest on-screen pixel size.\n"
			"  --first-char <N>      first codepoint (default 32, ' ')\n"
			"  --num-chars <N>       consecutive codepoint count (default 95,\n"
			"                        printable ASCII 32..126)\n"
			"  --cell-w <px>         uniform cell width (default: auto from the\n"
			"                        widest advance)\n"
			"  --baseline <px>       baseline offset from cell top (default: auto)\n"
			"  --cap-height <px>     target 'H' ink height. Default scales\n"
			"                        ascent+descent to fill the cell; raising this\n"
			"                        tightens headroom but may clip ascenders\n"
			"                        (reported after the bake).\n"
			"  --tracking <px>       signed advance offset applied to every glyph\n"
			"                        (negative tightens letter-spacing)\n"
			"  --space-advance <px>  visible stride of the space glyph (default:\n"
			"                        the font's natural advance)\n"
			"  --font-index <N>      face index in a .ttc collection (default 0)\n",
			argv0, FONTBAKE_DEFAULT_CELL_H);
}

/* Parses a decimal integer within [min, max]; exits with a diagnostic on
 * malformed input so every option shares one error path. */
static int parse_int(const char* s, const char* flag, long min, long max) {
	char* end = NULL;
	errno     = 0;
	long v    = strtol(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || v < min || v > max) {
		fprintf(stderr, "fontbake: invalid value for %s: '%s' (expected %ld..%ld)\n", flag, s, min, max);
		exit(2);
	}
	return (int)v;
}

static uint8_t* read_file(const char* path, size_t* out_size) {
	FILE* fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	long size = ftell(fp);
	if (size <= 0) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	uint8_t* data = malloc((size_t)size);
	if (!data) {
		fclose(fp);
		return NULL;
	}
	if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
		free(data);
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	*out_size = (size_t)size;
	return data;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}

	const char* ttf_path = argv[1];
	const char* out_base = argv[2];

	FontAtlasParams params = {
		.first_char = 32,
		.num_chars  = 95, /* printable ASCII 32..126 */
		.cell_h     = FONTBAKE_DEFAULT_CELL_H,
		.baseline   = -1,
	};
	/* The per-glyph LSB override interprets 0 as "ink flush left", not
	 * "no override" — natural placement needs an explicit -1 fill. */
	for (int i = 0; i < 256; i++)
		params.compression_glyph_lsb_atlas[i] = -1;

	/* Every accepted option consumes one value; the help and error paths
	 * return, so the loop always steps by two. */
	for (int i = 3; i < argc; i += 2) {
		const char* arg   = argv[i];
		const char* value = (i + 1 < argc) ? argv[i + 1] : NULL;
		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			usage(argv[0]);
			return 0;
		}
		if (!value) {
			fprintf(stderr, "fontbake: missing value for %s\n", arg);
			usage(argv[0]);
			return 2;
		}
		if (strcmp(arg, "--cell-h") == 0)
			params.cell_h = parse_int(value, arg, 1, 4096);
		else if (strcmp(arg, "--first-char") == 0)
			params.first_char = parse_int(value, arg, 0, 255);
		else if (strcmp(arg, "--num-chars") == 0)
			params.num_chars = parse_int(value, arg, 1, 256);
		else if (strcmp(arg, "--cell-w") == 0)
			params.cell_w = parse_int(value, arg, 1, 4096);
		else if (strcmp(arg, "--baseline") == 0)
			params.baseline = parse_int(value, arg, 0, 4096);
		else if (strcmp(arg, "--cap-height") == 0)
			params.cap_height = parse_int(value, arg, 1, 4096);
		else if (strcmp(arg, "--tracking") == 0)
			params.tracking_atlas = parse_int(value, arg, -1024, 1024);
		else if (strcmp(arg, "--space-advance") == 0)
			params.space_advance_atlas = parse_int(value, arg, 1, 4096);
		else if (strcmp(arg, "--font-index") == 0)
			params.font_index = parse_int(value, arg, 0, 255);
		else {
			fprintf(stderr, "fontbake: unknown option: %s\n", arg);
			usage(argv[0]);
			return 2;
		}
	}

	size_t   ttf_size = 0;
	uint8_t* ttf_data = read_file(ttf_path, &ttf_size);
	if (!ttf_data) {
		fprintf(stderr, "fontbake: cannot read '%s'\n", ttf_path);
		return 1;
	}

	FontAtlasResult result;
	char            err[512];
	if (!font_atlas_build(ttf_data, ttf_size, &params, &result, err, sizeof err)) {
		fprintf(stderr, "fontbake: build failed: %s\n", err);
		free(ttf_data);
		return 1;
	}
	free(ttf_data);

	if (!font_atlas_write(&result, out_base, err, sizeof err)) {
		fprintf(stderr, "fontbake: write failed: %s\n", err);
		font_atlas_free(&result);
		return 1;
	}

	/* Echo the chosen + auto-derived metrics and any clipping the build
	 * detected, so a bad bake is visible without opening the atlas. */
	fprintf(stderr,
			"fontbake: '%s' face=%d first=%d count=%d missing=%d\n"
			"          cell=%dx%d baseline=%d ascent=%d cap=%d descent=%d\n"
			"          atlas=%dx%d  wrote %s.png, %s.fnt\n",
			ttf_path, params.font_index, result.first_char, result.num_chars, result.missing_glyphs,
			result.cell_w, result.cell_h, result.baseline, result.ascent_atlas, result.cap_height_atlas,
			result.descent_atlas, result.atlas_w, result.atlas_h, out_base, out_base);
	if (result.neg_lsb_clip_px)
		fprintf(stderr, "fontbake: warning: %d px of negative left side bearing clipped\n",
				result.neg_lsb_clip_px);
	if (result.oversize_x || result.oversize_y)
		fprintf(stderr, "fontbake: warning: glyph bitmaps clipped (worst overflow x=%d y=%d)\n",
				result.oversize_x, result.oversize_y);
	if (result.ascender_clip_px)
		fprintf(stderr,
				"fontbake: warning: ascenders clip %d px above the cell top; lower --cap-height or"
				" raise --baseline\n",
				result.ascender_clip_px);
	if (result.descender_clip_px)
		fprintf(stderr, "fontbake: warning: descenders clip %d px below the cell bottom\n",
				result.descender_clip_px);

	font_atlas_free(&result);
	return 0;
}
