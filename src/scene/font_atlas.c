/* AeronFontAtlas — TFNT metrics plus PNG, KTX2, or runtime RGBA pixels. */

#include "aeron/scene/font_atlas.h"

#include "aeron/log.h"
#include "aeron/scene/image_cache.h"
#include "aeron/scene/ktx2_reader.h"
#include "rgba_upload.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AERON_FNT_MAGIC 0x544E4654u /* 'TFNT' */

typedef struct FontMetrics {
	int atlas_w, atlas_h;
	uint16_t first_char, glyph_count;
	uint16_t cell_w, cell_h, baseline;
	AeronFontGlyph *glyphs;
} FontMetrics;

static uint32_t rd_u32le(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16le(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

static int font_atlas_is_empty(const AeronFontAtlas *out) {
	return out && !out->loaded && !out->texture && !out->glyphs;
}

static int font_metrics_valid(const FontMetrics *metrics) {
	if (!metrics || !metrics->glyphs || metrics->atlas_w <= 0 ||
		metrics->atlas_h <= 0 || metrics->atlas_w > UINT16_MAX ||
		metrics->atlas_h > UINT16_MAX || !metrics->glyph_count ||
		!metrics->cell_w || !metrics->cell_h ||
		metrics->baseline > metrics->cell_h ||
		(uint32_t)metrics->first_char + metrics->glyph_count > UINT16_MAX + 1u)
		return 0;
	for (uint16_t index = 0; index < metrics->glyph_count; ++index) {
		const AeronFontGlyph *glyph = &metrics->glyphs[index];
		if ((uint32_t)glyph->atlas_x + glyph->atlas_w >
				(uint32_t)metrics->atlas_w ||
			(uint32_t)glyph->atlas_y + glyph->atlas_h >
				(uint32_t)metrics->atlas_h)
			return 0;
	}
	return 1;
}

static void font_atlas_assign(AeronFontAtlas *out, AeronTexture *texture,
		FontMetrics *metrics) {
	out->texture = texture;
	out->atlas_w = metrics->atlas_w;
	out->atlas_h = metrics->atlas_h;
	out->first_char = metrics->first_char;
	out->num_chars = metrics->glyph_count;
	out->cell_w = metrics->cell_w;
	out->cell_h = metrics->cell_h;
	out->baseline = metrics->baseline;
	out->glyphs = metrics->glyphs;
	out->loaded = 1;
	metrics->glyphs = NULL;
}

static int font_metrics_parse(const uint8_t *fnt, size_t size,
		const char *label, FontMetrics *out) {
	memset(out, 0, sizeof *out);
	if (!fnt || size < 24 || rd_u32le(fnt) != AERON_FNT_MAGIC) {
		Aeron_LogError("aeron.scene", "font: '%s' bad magic / truncated", label);
		return 0;
	}
	const uint16_t version = rd_u16le(fnt + 4);
	if (version != 2) {
		Aeron_LogError("aeron.scene", "font: '%s' unsupported version %u",
				label, version);
		return 0;
	}
	out->first_char = rd_u16le(fnt + 6);
	out->glyph_count = rd_u16le(fnt + 8);
	out->atlas_w = rd_u16le(fnt + 10);
	out->atlas_h = rd_u16le(fnt + 12);
	out->cell_w = rd_u16le(fnt + 14);
	out->cell_h = rd_u16le(fnt + 16);
	out->baseline = rd_u16le(fnt + 18);
	if (!out->glyph_count || size != 24u + (size_t)out->glyph_count * 10u) {
		Aeron_LogError("aeron.scene", "font: '%s' invalid record length", label);
		return 0;
	}
	out->glyphs = calloc(out->glyph_count, sizeof *out->glyphs);
	if (!out->glyphs) return 0;
	for (uint16_t index = 0; index < out->glyph_count; ++index) {
		const uint8_t *record = fnt + 24u + (size_t)index * 10u;
		out->glyphs[index] = (AeronFontGlyph){
				.atlas_x = rd_u16le(record),
				.atlas_y = rd_u16le(record + 2),
				.atlas_w = rd_u16le(record + 4),
				.atlas_h = rd_u16le(record + 6),
				.advance = rd_u16le(record + 8),
		};
	}
	if (!font_metrics_valid(out)) {
		Aeron_LogError("aeron.scene", "font: '%s' invalid metrics", label);
		free(out->glyphs);
		memset(out, 0, sizeof *out);
		return 0;
	}
	return 1;
}

int AeronFontAtlas_InitRgba8(AeronFontAtlas *out,
		AeronCommandBuffer *cmd, const AeronFontAtlasRgba8Desc *desc) {
	if (!font_atlas_is_empty(out) || !cmd || !desc || !desc->pixels ||
		!desc->glyphs || desc->pitch < (size_t)desc->width * 4 ||
		(size_t)desc->height > SIZE_MAX / desc->pitch ||
		(desc->alpha_mode != AERON_IMAGE_ALPHA_STRAIGHT &&
		 desc->alpha_mode != AERON_IMAGE_ALPHA_PREMULTIPLIED) ||
		(desc->format != AERON_TEXTURE_FORMAT_RGBA8_UNORM &&
		 desc->format != AERON_TEXTURE_FORMAT_RGBA8_SRGB))
		return 0;
	FontMetrics metrics = {
			.atlas_w = desc->width,
			.atlas_h = desc->height,
			.first_char = desc->first_char,
			.glyph_count = desc->glyph_count,
			.cell_w = desc->cell_w,
			.cell_h = desc->cell_h,
			.baseline = desc->baseline,
			.glyphs = (AeronFontGlyph *)desc->glyphs,
	};
	if (!font_metrics_valid(&metrics)) return 0;
	AeronFontGlyph *glyphs = malloc((size_t)metrics.glyph_count * sizeof *glyphs);
	if (!glyphs) return 0;
	memcpy(glyphs, metrics.glyphs,
		   (size_t)metrics.glyph_count * sizeof *glyphs);
	metrics.glyphs = glyphs;
	AeronTexture *texture = aeron_scene_upload_rgba8(
			cmd, desc->pixels, desc->width, desc->height, desc->pitch,
			desc->format, desc->color_space, desc->alpha_mode,
			desc->generate_mips, desc->debug_name);
	if (!texture) {
		free(glyphs);
		return 0;
	}
	font_atlas_assign(out, texture, &metrics);
	return 1;
}

static uint8_t *read_file(const char *path, size_t *out_size) {
	FILE *file = fopen(path, "rb");
	if (!file) return NULL;
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	const long length = ftell(file);
	if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	uint8_t *bytes = malloc(length > 0 ? (size_t)length : 1u);
	if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
		free(bytes);
		fclose(file);
		return NULL;
	}
	fclose(file);
	*out_size = (size_t)length;
	return bytes;
}

static int load_png_bytes(AeronFontAtlas *out, AeronCommandBuffer *cmd,
		const uint8_t *fnt, size_t fnt_size, const uint8_t *png,
		size_t png_size, const char *label) {
	if (!font_atlas_is_empty(out) || !cmd || !fnt || !png || !label ||
		png_size > INT_MAX)
		return 0;
	FontMetrics metrics;
	if (!font_metrics_parse(fnt, fnt_size, label, &metrics)) return 0;
	int width = 0, height = 0, channels = 0;
	uint8_t *pixels = stbi_load_from_memory(png, (int)png_size, &width,
			&height, &channels, 4);
	if (!pixels || width != metrics.atlas_w || height != metrics.atlas_h) {
		Aeron_LogError("aeron.scene", "font: PNG '%s' missing or dimensions mismatch",
				label);
		stbi_image_free(pixels);
		free(metrics.glyphs);
		return 0;
	}
	const AeronFontAtlasRgba8Desc desc = {
			.pixels = pixels,
			.width = width,
			.height = height,
			.pitch = (size_t)width * 4,
			.first_char = metrics.first_char,
			.cell_w = metrics.cell_w,
			.cell_h = metrics.cell_h,
			.baseline = metrics.baseline,
			.glyphs = metrics.glyphs,
			.glyph_count = metrics.glyph_count,
			.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
			.color_space = AERON_COLOR_SPACE_SRGB,
			.alpha_mode = AERON_IMAGE_ALPHA_PREMULTIPLIED,
			.generate_mips = false,
			.debug_name = label,
	};
	const int initialized = AeronFontAtlas_InitRgba8(out, cmd, &desc);
	stbi_image_free(pixels);
	free(metrics.glyphs);
	return initialized;
}

static int load_ktx2_bytes(AeronFontAtlas *out, AeronCommandBuffer *cmd,
		const uint8_t *fnt, size_t fnt_size, const uint8_t *ktx_bytes,
		size_t ktx_size, const char *label) {
	if (!font_atlas_is_empty(out) || !cmd || !fnt || !ktx_bytes || !label)
		return 0;
	FontMetrics metrics;
	if (!font_metrics_parse(fnt, fnt_size, label, &metrics)) return 0;
	Ktx2 *ktx = ktx2_open_mem(ktx_bytes, ktx_size, label);
	if (!ktx || ktx2_face_count(ktx) != 1 ||
		ktx2_width(ktx) != metrics.atlas_w ||
		ktx2_height(ktx) != metrics.atlas_h) {
		Aeron_LogError("aeron.scene", "font: KTX2 '%s' missing or dimensions mismatch",
				label);
		ktx2_close(ktx);
		free(metrics.glyphs);
		return 0;
	}
	AeronTexture *texture = Aeron_ImageUploadKtx2(cmd, ktx, label);
	ktx2_close(ktx);
	if (!texture) {
		free(metrics.glyphs);
		return 0;
	}
	font_atlas_assign(out, texture, &metrics);
	return 1;
}

static int load_files(AeronFontAtlas *out, AeronCommandBuffer *cmd,
		const char *basename, const char *image_extension, int ktx2) {
	if (!out || !cmd || !basename) return 0;
	char path[1024];
	size_t fnt_size = 0, image_size = 0;
	if (snprintf(path, sizeof path, "%s.fnt", basename) >= (int)sizeof path)
		return 0;
	uint8_t *fnt = read_file(path, &fnt_size);
	if (!fnt || snprintf(path, sizeof path, "%s.%s", basename,
			image_extension) >= (int)sizeof path) {
		free(fnt);
		return 0;
	}
	uint8_t *image = read_file(path, &image_size);
	const int result = image ?
			(ktx2 ? load_ktx2_bytes(out, cmd, fnt, fnt_size, image,
					image_size, basename)
				  : load_png_bytes(out, cmd, fnt, fnt_size, image,
					image_size, basename)) : 0;
	free(image);
	free(fnt);
	return result;
}

static int load_vfs(AeronFontAtlas *out, AeronCommandBuffer *cmd,
		AeronVfs *vfs, AeronVfsRoot root, const char *basename,
		size_t maximum_file_size, const char *image_extension, int ktx2) {
	if (!out || !cmd || !vfs || !basename || !maximum_file_size) return 0;
	char path[1024];
	uint8_t *fnt = NULL, *image = NULL;
	size_t fnt_size = 0, image_size = 0;
	if (snprintf(path, sizeof path, "%s.fnt", basename) >= (int)sizeof path ||
		!AeronVfs_ReadAll(vfs, root, path, maximum_file_size, &fnt, &fnt_size))
		return 0;
	if (snprintf(path, sizeof path, "%s.%s", basename, image_extension) >=
			(int)sizeof path ||
		!AeronVfs_ReadAll(vfs, root, path, maximum_file_size, &image,
			&image_size)) {
		free(fnt);
		return 0;
	}
	const int result = ktx2 ?
			load_ktx2_bytes(out, cmd, fnt, fnt_size, image, image_size, basename) :
			load_png_bytes(out, cmd, fnt, fnt_size, image, image_size, basename);
	free(image);
	free(fnt);
	return result;
}

int AeronFontAtlas_Load(AeronFontAtlas *out, AeronCommandBuffer *cmd,
		const char *basename) {
	return load_files(out, cmd, basename, "png", 0);
}

int AeronFontAtlas_LoadVfs(AeronFontAtlas *out, AeronCommandBuffer *cmd,
		AeronVfs *vfs, AeronVfsRoot root, const char *basename,
		size_t maximum_file_size) {
	return load_vfs(out, cmd, vfs, root, basename, maximum_file_size, "png", 0);
}

int AeronFontAtlas_LoadKtx2(AeronFontAtlas *out, AeronCommandBuffer *cmd,
		const char *basename) {
	return load_files(out, cmd, basename, "ktx2", 1);
}

int AeronFontAtlas_LoadKtx2Vfs(AeronFontAtlas *out,
		AeronCommandBuffer *cmd, AeronVfs *vfs, AeronVfsRoot root,
		const char *basename, size_t maximum_file_size) {
	return load_vfs(out, cmd, vfs, root, basename, maximum_file_size,
			"ktx2", 1);
}

void AeronFontAtlas_Release(AeronFontAtlas *out) {
	if (!out) return;
	if (out->texture) Aeron_DestroyTexture(out->texture);
	free(out->glyphs);
	memset(out, 0, sizeof *out);
}
