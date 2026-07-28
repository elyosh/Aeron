/*
 * AeronFontAtlas — TFNT v2 + PNG to sampled texture and glyph metrics.
 * See aeron/scene/font_atlas.h.
 */

#include "aeron/scene/font_atlas.h"

#include "aeron/log.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AERON_FNT_MAGIC 0x544E4654u /* 'TFNT' */

static uint32_t rd_u32le(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
		   ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Read a whole binary file into a malloc'd buffer; NULL on failure. */
static uint8_t* read_file(const char* path, size_t* out_size) {
	FILE* fp = fopen(path, "rb");
	if (!fp) {
		return NULL;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	long sz = ftell(fp);
	if (sz < 0) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	uint8_t* buf = (uint8_t*)malloc((size_t)sz);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
		free(buf);
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	*out_size = (size_t)sz;
	return buf;
}

int AeronFontAtlas_Load(AeronFontAtlas* out, AeronCommandBuffer* cmd, const char* basename) {
	if (!out || !cmd || !basename) {
		return 0;
	}
	memset(out, 0, sizeof *out);

	char   path[1024];
	size_t fnt_size = 0;
	snprintf(path, sizeof path, "%s.fnt", basename);
	uint8_t* fnt = read_file(path, &fnt_size);
	if (!fnt) {
		Aeron_LogError("aeron.scene", "font: open '%s' failed", path);
		return 0;
	}
	if (fnt_size < 24 || rd_u32le(fnt) != AERON_FNT_MAGIC) {
		Aeron_LogError("aeron.scene", "font: '%s' bad magic / truncated", path);
		free(fnt);
		return 0;
	}
	const uint16_t version = rd_u16le(fnt + 4);
	if (version != 2) {
		Aeron_LogError("aeron.scene", "font: '%s' unsupported version %u", path, version);
		free(fnt);
		return 0;
	}
	const uint16_t first    = rd_u16le(fnt + 6);
	const uint16_t count    = rd_u16le(fnt + 8);
	const uint16_t atlas_w  = rd_u16le(fnt + 10);
	const uint16_t atlas_h  = rd_u16le(fnt + 12);
	const uint16_t cell_w   = rd_u16le(fnt + 14);
	const uint16_t cell_h   = rd_u16le(fnt + 16);
	const uint16_t baseline = rd_u16le(fnt + 18);
	if (!count || fnt_size < (size_t)24 + (size_t)count * 10) {
		Aeron_LogError("aeron.scene", "font: '%s' truncated records", path);
		free(fnt);
		return 0;
	}
	AeronFontGlyph* glyphs = (AeronFontGlyph*)calloc(count, sizeof(AeronFontGlyph));
	if (!glyphs) {
		free(fnt);
		return 0;
	}
	const uint8_t* recs = fnt + 24;
	for (uint16_t i = 0; i < count; i++) {
		const uint8_t* r = recs + (size_t)i * 10;
		glyphs[i].atlas_x = rd_u16le(r + 0);
		glyphs[i].atlas_y = rd_u16le(r + 2);
		glyphs[i].atlas_w = rd_u16le(r + 4);
		glyphs[i].atlas_h = rd_u16le(r + 6);
		glyphs[i].advance = rd_u16le(r + 8);
	}
	free(fnt);

	snprintf(path, sizeof path, "%s.png", basename);
	int      png_w = 0, png_h = 0, png_n = 0;
	uint8_t* pixels = stbi_load(path, &png_w, &png_h, &png_n, 4);
	if (!pixels) {
		Aeron_LogError("aeron.scene", "font: stbi_load '%s' failed: %s", path, stbi_failure_reason());
		free(glyphs);
		return 0;
	}
	if (png_w != (int)atlas_w || png_h != (int)atlas_h) {
		Aeron_LogError("aeron.scene", "font: '%s' dims %dx%d don't match .fnt %ux%u", path, png_w, png_h,
					   atlas_w, atlas_h);
		stbi_image_free(pixels);
		free(glyphs);
		return 0;
	}

	AeronTexture* tex = Aeron_CreateTexture(&(AeronTextureDesc){
		.width     = png_w,
		.height    = png_h,
		.mip_count = 1,
		.format    = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage     = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = path,
	});
	if (tex && !Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc){
				   .texture      = tex,
				   .width        = png_w,
				   .height       = png_h,
				   .pixels       = pixels,
				   .pitch        = png_w * 4,
				   .pixel_format = AERON_PIXEL_FORMAT_RGBA8888,
				   .color_space  = AERON_COLOR_SPACE_SRGB,
			   })) {
		Aeron_DestroyTexture(tex);
		tex = NULL;
	}
	stbi_image_free(pixels);
	if (!tex) {
		Aeron_LogError("aeron.scene", "font: atlas GPU upload failed (%s)", path);
		free(glyphs);
		return 0;
	}

	out->texture    = tex;
	out->atlas_w    = atlas_w;
	out->atlas_h    = atlas_h;
	out->first_char = first;
	out->num_chars  = count;
	out->cell_w     = cell_w;
	out->cell_h     = cell_h;
	out->baseline   = baseline;
	out->glyphs     = glyphs;
	out->loaded     = 1;
	return 1;
}

void AeronFontAtlas_Release(AeronFontAtlas* out) {
	if (!out) {
		return;
	}
	if (out->texture) {
		Aeron_DestroyTexture(out->texture);
	}
	free(out->glyphs);
	memset(out, 0, sizeof *out);
}
