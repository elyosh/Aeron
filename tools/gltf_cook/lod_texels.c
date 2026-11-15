#include "lod_texels.h"

#include "aeron/scene/ktx2_reader.h"
#include "srgb8.h"

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool lod_channel_open(LodChannel* c, const AeronGltfChannelKtx2* source) {
	memset(c, 0, sizeof *c);
	if (!source->data || !source->size)
		return false;
	c->ktx2 = ktx2_open_mem(source->data, source->size, "lod appearance");
	if (!c->ktx2)
		return false;
	const AeronTextureFormat f = ktx2_format(c->ktx2);
	if (f != AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB && f != AERON_TEXTURE_FORMAT_BC7_RGBA_UNORM &&
		f != AERON_TEXTURE_FORMAT_RGBA8_SRGB && f != AERON_TEXTURE_FORMAT_RGBA8_UNORM) {
		ktx2_close(c->ktx2);
		c->ktx2 = NULL;
		return false;
	}
	c->srgb = f == AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB || f == AERON_TEXTURE_FORMAT_RGBA8_SRGB;
	/* The first level 256 texels wide or less: enough to tell a material's
	 * parts apart, small enough to decode quickly. */
	const int levels = ktx2_level_count(c->ktx2);
	while (c->level + 1 < levels && ktx2_level(c->ktx2, c->level).width > 256)
		c->level++;
	c->pages = ktx2_layer_count(c->ktx2) > 0 ? ktx2_layer_count(c->ktx2) : 1;
	c->page  = calloc((size_t)c->pages, sizeof *c->page);
	if (!c->page) {
		ktx2_close(c->ktx2);
		c->ktx2 = NULL;
		return false;
	}
	return true;
}

void lod_channel_close(LodChannel* c) {
	for (int i = 0; c->page && i < c->pages; i++)
		free(c->page[i].rgb);
	free(c->page);
	if (c->ktx2)
		ktx2_close(c->ktx2);
	memset(c, 0, sizeof *c);
}

static const LodPage* channel_page(LodChannel* c, uint32_t page) {
	if (!c->ktx2 || (int)page >= c->pages)
		return NULL;
	LodPage* p = &c->page[page];
	if (p->rgb)
		return p;
	const Ktx2Level level = ktx2_level_face(c->ktx2, c->level, (int)page);
	const int       w = level.width, h = level.height;
	uint8_t*        rgba = malloc((size_t)((w + 3) & ~3) * (size_t)((h + 3) & ~3) * 4);
	p->rgb               = malloc(sizeof(float) * 3 * (size_t)w * (size_t)h);
	if (!rgba || !p->rgb || !level.data) {
		free(rgba);
		free(p->rgb);
		p->rgb = NULL;
		return NULL;
	}
	const AeronTextureFormat f     = ktx2_format(c->ktx2);
	const int                pitch = ((w + 3) & ~3) * 4;
	if (f == AERON_TEXTURE_FORMAT_BC7_RGBA_SRGB || f == AERON_TEXTURE_FORMAT_BC7_RGBA_UNORM) {
		const int bw = (w + 3) / 4, bh = (h + 3) / 4;
		for (int by = 0; by < bh; by++)
			for (int bx = 0; bx < bw; bx++)
				bcdec_bc7(level.data + 16 * ((size_t)by * (size_t)bw + (size_t)bx),
						  rgba + (size_t)(4 * by) * (size_t)pitch + 16 * (size_t)bx, pitch);
	} else {
		for (int y = 0; y < h; y++)
			memcpy(rgba + (size_t)y * (size_t)pitch, level.data + (size_t)y * (size_t)w * 4, (size_t)w * 4);
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			for (int k = 0; k < 3; k++) {
				const uint8_t code = rgba[(size_t)y * (size_t)pitch + 4 * (size_t)x + (size_t)k];
				p->rgb[3 * ((size_t)y * (size_t)w + (size_t)x) + (size_t)k] =
					c->srgb ? srgb8_to_linear(code) : (float)code / 255.0f;
			}
	free(rgba);
	p->width  = w;
	p->height = h;
	return p;
}

bool lod_material_texel(LodChannel* c, const AeronGltfMaterial* m, int channel, const float uv[2],
						float out[3]) {
	const float* x = m->uv_xform[channel];
	if (x[2] == 0.0f || x[3] == 0.0f)
		return false;
	const LodPage* p = channel_page(c, m->atlas_page[channel]);
	if (!p)
		return false;
	const float u  = x[0] + (uv[0] - floorf(uv[0])) * x[2];
	const float v  = x[1] + (uv[1] - floorf(uv[1])) * x[3];
	int         tx = (int)(u * (float)p->width), ty = (int)(v * (float)p->height);
	tx = tx < 0 ? 0 : tx >= p->width ? p->width - 1 : tx;
	ty = ty < 0 ? 0 : ty >= p->height ? p->height - 1 : ty;
	memcpy(out, &p->rgb[3 * ((size_t)ty * (size_t)p->width + (size_t)tx)], sizeof(float) * 3);
	return true;
}

float lod_emitted_luminance(LodChannel* emissive, const AeronGltfMaterial* m, const float uv[2]) {
	float light[3] = { 1.0f, 1.0f, 1.0f };
	if (m->uv_xform[AERON_GLTF_CHANNEL_EMISSIVE][2] != 0.0f &&
		!(emissive && lod_material_texel(emissive, m, AERON_GLTF_CHANNEL_EMISSIVE, uv, light)))
		return 0.0f;
	return (0.2126f * light[0] * m->emissive_factor[0] + 0.7152f * light[1] * m->emissive_factor[1] +
			0.0722f * light[2] * m->emissive_factor[2]) *
		   m->emissive_strength;
}

const AeronGltfMaterial* lod_primitive_material(const AeronGltfModel* model, uint32_t prim) {
	if (prim >= model->total_prim_count || !model->prim_variant_material || !model->variant_columns)
		return NULL;
	const uint32_t material = model->prim_variant_material[(size_t)prim * model->variant_columns];
	return material < model->material_count ? &model->materials[material] : NULL;
}
