/*
 * A model's environment probes on the GPU: the cell table, the cube array,
 * the grid repacked into a 3D texture and the set buffer, from a flight
 * model's cooked probes.
 */

#include "aeron/scene/probe_set.h"

#include "aeron/log.h"
#include "aeron/scene/image_cache.h"
#include "aeron/scene/ktx2_reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- probe cells ------------------------------------------------------ */

static float probe_volume(const AeronFlightProbe* p) {
	const float box = p->size[0] * p->size[1] * p->size[2];
	return p->shape == AERON_FLIGHT_PROBE_ELLIPSOID ? box * 0.5235988f : box; /* pi / 6 */
}

bool AeronProbeCells_Build(const AeronFlightProbeLayout* d, AeronProbeCells* out) {
	if (!out)
		return false;
	memset(out, 0, sizeof *out);
	if (!d || d->probe_count == 0 || d->probe_count > AERON_FLIGHT_MAX_PROBES)
		return false;

	/* Influence boxes, their union, and the probes smallest first (ties
	 * by index). */
	float    lo[AERON_FLIGHT_MAX_PROBES][3], hi[AERON_FLIGHT_MAX_PROBES][3], max[3];
	uint32_t order[AERON_FLIGHT_MAX_PROBES];
	for (uint32_t i = 0; i < d->probe_count; ++i) {
		for (int a = 0; a < 3; ++a) {
			lo[i][a]    = d->probes[i].center[a] - 0.5f * d->probes[i].size[a];
			hi[i][a]    = d->probes[i].center[a] + 0.5f * d->probes[i].size[a];
			out->min[a] = i == 0 ? lo[i][a] : fminf(out->min[a], lo[i][a]);
			max[a]      = i == 0 ? hi[i][a] : fmaxf(max[a], hi[i][a]);
		}
		uint32_t at = i;
		for (; at > 0 && probe_volume(&d->probes[order[at - 1]]) > probe_volume(&d->probes[i]); --at)
			order[at] = order[at - 1];
		order[at] = i;
	}
	const float largest = fmaxf(max[0] - out->min[0], fmaxf(max[1] - out->min[1], max[2] - out->min[2]));
	out->cell_size      = fmaxf(AERON_PROBE_CELL_MIN_SIZE, largest / AERON_PROBE_CELL_MAX_AXIS);
	size_t cell_count   = 1;
	for (int a = 0; a < 3; ++a) {
		const float cells = ceilf((max[a] - out->min[a]) / out->cell_size);
		out->dims[a]      = cells < 1.0f                        ? 1u
							: cells > AERON_PROBE_CELL_MAX_AXIS ? AERON_PROBE_CELL_MAX_AXIS
																: (uint32_t)cells;
		cell_count *= out->dims[a];
	}
	out->words = calloc(cell_count, sizeof *out->words);
	if (!out->words)
		return false;

	for (uint32_t z = 0; z < out->dims[2]; ++z)
		for (uint32_t y = 0; y < out->dims[1]; ++y)
			for (uint32_t x = 0; x < out->dims[0]; ++x) {
				const uint32_t cell[3] = { x, y, z };
				float          cell_lo[3], cell_hi[3];
				for (int a = 0; a < 3; ++a) {
					cell_lo[a] = out->min[a] + (float)cell[a] * out->cell_size;
					cell_hi[a] = cell_lo[a] + out->cell_size;
				}
				uint32_t word = 0, count = 0;
				for (uint32_t k = 0; k < d->probe_count; ++k) {
					const uint32_t i = order[k];
					if (lo[i][0] >= cell_hi[0] || hi[i][0] <= cell_lo[0] || lo[i][1] >= cell_hi[1] ||
						hi[i][1] <= cell_lo[1] || lo[i][2] >= cell_hi[2] || hi[i][2] <= cell_lo[2])
						continue;
					if (count < AERON_PROBE_CELL_PROBES)
						word |= (i + 1u) << (8u * count);
					++count;
				}
				out->words[((size_t)z * out->dims[1] + y) * out->dims[0] + x] = word;
				if (count > out->most_probes)
					out->most_probes = count;
				if (count > AERON_PROBE_CELL_PROBES)
					++out->overflow_cells;
			}
	return true;
}

void AeronProbeCells_Free(AeronProbeCells* cells) {
	if (!cells)
		return;
	free(cells->words);
	memset(cells, 0, sizeof *cells);
}

/* ---- probe sets ------------------------------------------------------- */

/* The 3D grid texture from the grid image's level 0: point (x, y, z) at
 * column 6 x + f, row y + ry z for face f goes to texel (x, y, f rz + z). */
static AeronTexture* upload_grid(AeronCommandBuffer* cmd, const Ktx2Level* level, const uint32_t r[3],
								 const char* name) {
	const size_t texel = 4u * sizeof(uint16_t);
	const size_t slice = (size_t)r[0] * r[1] * texel;
	uint8_t*     data  = (uint8_t*)malloc(slice * 6u * r[2]);
	if (!data)
		return NULL;
	for (uint32_t f = 0; f < 6; ++f)
		for (uint32_t z = 0; z < r[2]; ++z)
			for (uint32_t y = 0; y < r[1]; ++y)
				for (uint32_t x = 0; x < r[0]; ++x)
					memcpy(data + slice * (f * r[2] + z) + ((size_t)y * r[0] + x) * texel,
						   level->data + (((size_t)(y + r[1] * z) * 6u * r[0]) + 6u * x + f) * texel, texel);
	const AeronTextureDesc desc = { .width  = (int)r[0],
									.height = (int)r[1],
									.format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
									.usage  = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
									.depth  = (int)(6u * r[2]),
									.debug_name = name };
	AeronTexture*          texture      = Aeron_CreateTexture(&desc);
	const AeronTextureUploadDesc upload = { .texture  = texture,
											.width    = (int)r[0],
											.height   = (int)r[1],
											.raw_data = data,
											.raw_size = (uint32_t)(slice * 6u * r[2]),
											.depth    = (int)(6u * r[2]) };
	if (texture && !Aeron_UploadTextureDataCmd(cmd, &upload)) {
		Aeron_DestroyTexture(texture);
		texture = NULL;
	}
	free(data);
	return texture;
}

static float half_to_float(uint16_t h) {
	const uint32_t sign = (uint32_t)(h & 0x8000u) << 16, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
	uint32_t       bits;
	if (exponent == 0) {
		const float value = ldexpf((float)mantissa, -24);
		return sign ? -value : value;
	}
	bits = sign | (exponent == 31 ? 0x7F800000u | mantissa << 13 : (exponent + 112u) << 23 | mantissa << 13);
	float f;
	memcpy(&f, &bits, sizeof f);
	return f;
}

/* The luminance of the grid's light at each probe's centre, toward +x, -x,
 * +y, -y, +z, -z and the mean, so the shader can compare the light where a
 * pixel is with the light where the probe was captured. Sampled
 * trilinearly, as the shader does. Zero for a probe whose centre does not
 * get the grid's full weight. */
static void probe_capture_light(const AeronFlightProbeLayout* def, const Ktx2Level* level, float out[][8]) {
	memset(out, 0, sizeof(float[8]) * def->probe_count);
	const uint16_t*    texels       = (const uint16_t*)level->data;
	const uint32_t*    r            = def->grid_resolution;
	static const float luminance[3] = { 0.2126f, 0.7152f, 0.0722f };
	for (uint32_t k = 0; k < def->probe_count; ++k) {
		/* Texel coordinates (point i at i + 0.5); the shader's weight is 1
		 * from -0.5 to r + 0.5. Then point coordinates, clamped. */
		float g[3];
		bool  inside = true;
		for (int a = 0; a < 3; ++a) {
			const float s = ((float)r[a] + 1.0f) / def->grid_size[a];
			g[a] = (def->probes[k].center[a] - def->grid_center[a]) * s + 0.5f * ((float)r[a] + 1.0f) - 0.5f;
			inside = inside && g[a] >= -0.5f && g[a] <= (float)r[a] + 0.5f;
			g[a]   = fminf(fmaxf(g[a] - 0.5f, 0.0f), (float)r[a] - 1.0f);
		}
		if (!inside)
			continue;
		uint32_t i0[3], i1[3];
		float    t[3];
		for (int a = 0; a < 3; ++a) {
			i0[a] = (uint32_t)g[a];
			i1[a] = i0[a] + 1u < r[a] ? i0[a] + 1u : i0[a];
			t[a]  = g[a] - (float)i0[a];
		}
		for (int corner = 0; corner < 8; ++corner) {
			const uint32_t x = corner & 1 ? i1[0] : i0[0], y = corner & 2 ? i1[1] : i0[1],
						   z = corner & 4 ? i1[2] : i0[2];
			const float    w = (corner & 1 ? t[0] : 1.0f - t[0]) * (corner & 2 ? t[1] : 1.0f - t[1]) *
							   (corner & 4 ? t[2] : 1.0f - t[2]);
			for (int f = 0; f < 6; ++f) {
				/* Point (x, y, z), face f: texel 6 x + f of row y + ry z. */
				const uint16_t* texel = texels + (((size_t)(y + r[1] * z) * 6u * r[0]) + 6u * x + f) * 4u;
				for (int c = 0; c < 3; ++c)
					out[k][f] += w * luminance[c] * half_to_float(texel[c]);
				if (f == 0)
					out[k][6] += w * half_to_float(texel[3]);
			}
		}
	}
}

/* The set buffer; NULL on failure. */
static AeronBuffer* build_probe_buffer(AeronCommandBuffer* cmd, const AeronFlightProbeLayout* def,
									   const AeronProbeCells* cells, const Ktx2Level* grid,
									   uint32_t* out_bytes) {
	const size_t cell_count = (size_t)cells->dims[0] * cells->dims[1] * cells->dims[2];
	const size_t elements =
		AERON_PROBE_BUFFER_HEADER + AERON_PROBE_BUFFER_RECORD * def->probe_count + (cell_count + 3u) / 4u;
	const uint32_t bytes = (uint32_t)(elements * 16u);
	uint32_t*      words = (uint32_t*)calloc(elements, 16u);
	if (!words)
		return NULL;
	float* f = (float*)words; /* same storage: floats where the layout has floats */
	for (int a = 0; a < 3; ++a) {
		f[a]         = cells->min[a];
		words[4 + a] = cells->dims[a];
	}
	f[3]     = 1.0f / cells->cell_size;
	words[7] = def->probe_count;
	float capture[AERON_FLIGHT_MAX_PROBES][8];
	probe_capture_light(def, grid, capture);
	for (uint32_t k = 0; k < def->probe_count; ++k) {
		const AeronFlightProbe* p = &def->probes[k];
		uint32_t* record          = words + 4u * (AERON_PROBE_BUFFER_HEADER + AERON_PROBE_BUFFER_RECORD * k);
		float*    r               = (float*)record;
		for (int a = 0; a < 3; ++a) {
			r[a]      = p->center[a];
			r[4 + a]  = 2.0f / p->size[a];
			r[8 + a]  = 0.5f * p->parallax_size[a];
			r[12 + a] = p->parallax_center[a];
		}
		memcpy(r + 16, capture[k], sizeof capture[k]); /* elements 4 and 5 */
		r[3]       = 1.0f / p->falloff;
		record[7]  = k;
		record[11] = (p->shape == AERON_FLIGHT_PROBE_ELLIPSOID ? 1u : 0u) |
					 (p->parallax_shape == AERON_FLIGHT_PROBE_ELLIPSOID ? 2u : 0u);
	}
	memcpy(words + 4u * (AERON_PROBE_BUFFER_HEADER + AERON_PROBE_BUFFER_RECORD * def->probe_count),
		   cells->words, cell_count * sizeof *cells->words);
	AeronBuffer* buffer = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size         = bytes,
		.usage        = AERON_BUFFER_USAGE_STORAGE | AERON_BUFFER_USAGE_COMPUTE_STORAGE_READ,
		.memory_usage = AERON_MEMORY_USAGE_GPU_ONLY,
		.debug_name   = "scene.probe_set",
	});
	if (buffer && !Aeron_UploadBufferDataCmd(cmd, buffer, 0, words, bytes)) {
		Aeron_DestroyBuffer(buffer);
		buffer = NULL;
	}
	free(words);
	*out_bytes = buffer ? bytes : 0;
	return buffer;
}

AeronProbeSet* AeronProbeSet_Create(AeronCommandBuffer* cmd, const AeronFlightProbes* probes,
									const char* name) {
	if (!cmd || !probes || !probes->cubes || !probes->grid)
		return NULL;
	if (!name)
		name = "model";
	char label[128];
	snprintf(label, sizeof label, "%s.probes", name);
	AeronProbeSet*  set   = (AeronProbeSet*)calloc(1, sizeof *set);
	Ktx2*           cubes = ktx2_open_mem(probes->cubes, probes->cubes_size, label);
	Ktx2*           grid  = ktx2_open_mem(probes->grid, probes->grid_size, label);
	AeronProbeCells cells;
	const char*     failure = NULL;
	memset(&cells, 0, sizeof cells);
	if (!set || !AeronProbeCells_Build(&probes->layout, &cells)) {
		failure = "out of memory";
	} else if (!cubes || !grid) {
		failure = "the images are not supported KTX2";
	} else if (!Aeron_TextureFormatSupported(ktx2_format(cubes), AERON_TEXTURE_USAGE_SAMPLED) ||
			   !Aeron_TextureFormatSupported(AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
											 AERON_TEXTURE_USAGE_SAMPLED)) {
		failure = "this device cannot sample their formats";
	} else {
		const Ktx2Level level        = ktx2_level(grid, 0);
		uint32_t        buffer_bytes = 0;
		set->layout                  = probes->layout;
		set->cubes                   = Aeron_ImageUploadKtx2(cmd, cubes, label);
		set->grid                    = upload_grid(cmd, &level, probes->layout.grid_resolution, label);
		set->buffer         = build_probe_buffer(cmd, &probes->layout, &cells, &level, &buffer_bytes);
		set->sampler        = Aeron_CreateSampler(&(AeronSamplerDesc) {
			.min_filter = AERON_FILTER_LINEAR,
			.mag_filter = AERON_FILTER_LINEAR,
			.mip_filter = AERON_FILTER_LINEAR,
			.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
			.max_lod    = 1000.0f,
		});
		set->mip_count      = (uint32_t)ktx2_level_count(cubes);
		set->overflow_cells = cells.overflow_cells;
		const uint32_t* r   = probes->layout.grid_resolution;
		set->bytes          = (uint64_t)r[0] * r[1] * 6u * r[2] * 8u + buffer_bytes;
		for (int i = 0; i < ktx2_level_count(cubes); ++i)
			set->bytes += ktx2_level(cubes, i).size;
		if (!set->cubes || !set->grid || !set->buffer || !set->sampler)
			failure = "upload failed";
	}
	if (!failure && cells.overflow_cells)
		Aeron_LogWarn("aeron.scene",
					  "%s probes: %u cells overlap more than %d probes; the smallest are used there", name,
					  cells.overflow_cells, AERON_PROBE_CELL_PROBES);
	AeronProbeCells_Free(&cells);
	ktx2_close(cubes);
	ktx2_close(grid);
	if (failure) {
		Aeron_LogError("aeron.scene", "%s probes: %s", name, failure);
		AeronProbeSet_Destroy(set);
		return NULL;
	}
	const AeronFlightProbeLayout* d = &set->layout;
	for (int a = 0; a < 3; ++a) {
		/* The grid's light reaches one point spacing beyond its box. */
		const float reach  = d->grid_size[a] * (0.5f + 1.0f / (float)(d->grid_resolution[a] + 1u));
		set->bounds_min[a] = d->grid_center[a] - reach;
		set->bounds_max[a] = d->grid_center[a] + reach;
		for (uint32_t k = 0; k < d->probe_count; ++k) {
			const AeronFlightProbe* p = &d->probes[k];
			set->bounds_min[a]        = fminf(set->bounds_min[a], p->center[a] - 0.5f * p->size[a]);
			set->bounds_max[a]        = fmaxf(set->bounds_max[a], p->center[a] + 0.5f * p->size[a]);
		}
	}
	return set;
}

void AeronProbeSet_Destroy(AeronProbeSet* set) {
	if (!set)
		return;
	Aeron_DestroyTexture(set->cubes);
	Aeron_DestroyTexture(set->grid);
	Aeron_DestroyBuffer(set->buffer);
	Aeron_DestroySampler(set->sampler);
	free(set);
}
