/*
 * Environment package library: texture loading and sharing by path,
 * rotation and light preparation, publication and retirement.
 */

#include "environment_internal.h"

#include "aeron/log.h"
#include "aeron/scene/image_cache.h"
#include "aeron/scene/ktx2_reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENV_PI 3.14159265358979323846
/* Largest environment texture file read (a 4096 BC6H cube with mips is
 * about 128 MiB before supercompression). */
#define ENV_MAX_FILE_BYTES ((size_t)1 << 30)

#ifdef AERON_DEBUG_UI
void env_debug_detach(AeronEnvironmentLibrary* library);
#endif

/* ---- matrices (row-major 3x3, column vectors) ----------------------------- */

static void mat3_mul(const float a[9], const float b[9], float out[9]) {
	float r[9];
	for (int row = 0; row < 3; ++row)
		for (int col = 0; col < 3; ++col)
			r[row * 3 + col] = a[row * 3 + 0] * b[0 * 3 + col] + a[row * 3 + 1] * b[1 * 3 + col] +
							   a[row * 3 + 2] * b[2 * 3 + col];
	memcpy(out, r, sizeof r);
}

static void mat3_transpose(const float a[9], float out[9]) {
	float r[9];
	for (int row = 0; row < 3; ++row)
		for (int col = 0; col < 3; ++col)
			r[row * 3 + col] = a[col * 3 + row];
	memcpy(out, r, sizeof r);
}

/* Q = Ry(yaw) * Rx(pitch) * Rz(roll), right-handed, in the cube frame. */
static void package_rotation(const float degrees[3], float out[9]) {
	const float y = (float)(degrees[0] * ENV_PI / 180.0), p = (float)(degrees[1] * ENV_PI / 180.0),
				r     = (float)(degrees[2] * ENV_PI / 180.0);
	const float ry[9] = { cosf(y), 0.0f, sinf(y), 0.0f, 1.0f, 0.0f, -sinf(y), 0.0f, cosf(y) };
	const float rx[9] = { 1.0f, 0.0f, 0.0f, 0.0f, cosf(p), -sinf(p), 0.0f, sinf(p), cosf(p) };
	const float rz[9] = { cosf(r), -sinf(r), 0.0f, sinf(r), cosf(r), 0.0f, 0.0f, 0.0f, 1.0f };
	float       ryx[9];
	mat3_mul(ry, rx, ryx);
	mat3_mul(ryx, rz, out);
}

/* ---- textures -------------------------------------------------------------- */

static const EnvTexture* find_texture(const EnvTexture* textures, uint32_t count, const char* path) {
	for (uint32_t i = 0; i < count; ++i)
		if (strcmp(textures[i].path, path) == 0)
			return &textures[i];
	return NULL;
}

static bool hdr_format(AeronTextureFormat format) {
	return format == AERON_TEXTURE_FORMAT_BC6H_RGB_UFLOAT || format == AERON_TEXTURE_FORMAT_BC6H_RGB_FLOAT ||
		   format == AERON_TEXTURE_FORMAT_RGBA16_FLOAT;
}

/* The 3D grid texture from the grid file's level 0: point (x, y, z) at
 * column 4 x + c, row y + ry z for channel c goes to texel (x, y, c rz + z). */
static AeronTexture* upload_grid(AeronCommandBuffer* cmd, const Ktx2Level* level, const uint32_t r[3],
								 const char* name) {
	const size_t texel = 4u * sizeof(uint16_t);
	const size_t slice = (size_t)r[0] * r[1] * texel;
	uint8_t*     data  = (uint8_t*)malloc(slice * 3u * r[2]);
	if (!data)
		return NULL;
	for (uint32_t c = 0; c < 3; ++c)
		for (uint32_t z = 0; z < r[2]; ++z)
			for (uint32_t y = 0; y < r[1]; ++y)
				for (uint32_t x = 0; x < r[0]; ++x)
					memcpy(data + slice * (c * r[2] + z) + ((size_t)y * r[0] + x) * texel,
						   level->data + (((size_t)(y + r[1] * z) * 4u * r[0]) + 4u * x + c) * texel, texel);
	const AeronTextureDesc desc = { .width  = (int)r[0],
									.height = (int)r[1],
									.format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
									.usage  = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
									.depth  = (int)(3u * r[2]),
									.debug_name = name };
	AeronTexture*          texture      = Aeron_CreateTexture(&desc);
	const AeronTextureUploadDesc upload = { .texture  = texture,
											.width    = (int)r[0],
											.height   = (int)r[1],
											.raw_data = data,
											.raw_size = (uint32_t)(slice * 3u * r[2]),
											.depth    = (int)(3u * r[2]) };
	if (texture && !Aeron_UploadTextureDataCmd(cmd, &upload)) {
		Aeron_DestroyTexture(texture);
		texture = NULL;
	}
	free(data);
	return texture;
}

static bool env_load_texture(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd, EnvTexture* e) {
	char full[AERON_ENV_PATH_MAX * 2];
	snprintf(full, sizeof full, "%s%s", library->path_prefix, e->path);
	uint8_t* bytes = NULL;
	size_t   size  = 0;
	if (!AeronVfs_ReadAll(library->vfs, library->root, full, ENV_MAX_FILE_BYTES, &bytes, &size)) {
		snprintf(e->failure, sizeof e->failure, "cannot read");
		return false;
	}
	Ktx2* ktx = ktx2_open_mem(bytes, size, full);
	if (!ktx) {
		free(bytes);
		snprintf(e->failure, sizeof e->failure, "not a supported KTX2 file");
		return false;
	}
	static const char* const kinds[] = { "2D texture", "cube map", "cube array", "2D grid image" };
	const bool               cube    = e->kind == ENV_TEXTURE_CUBE || e->kind == ENV_TEXTURE_CUBE_ARRAY;
	const bool               array   = e->kind == ENV_TEXTURE_CUBE_ARRAY;
	const uint32_t*          r       = e->grid;
	e->format                        = ktx2_format(ktx);
	e->size                          = ktx2_width(ktx);
	e->level_count                   = ktx2_level_count(ktx);
	e->layers                        = ktx2_layer_count(ktx);
	bool ok                          = true;
	if (ktx2_face_count(ktx) != (cube ? 6 : 1) || (ktx2_layer_count(ktx) > 0) != array) {
		snprintf(e->failure, sizeof e->failure, "expected a %s", kinds[e->kind]);
		ok = false;
	} else if (e->kind == ENV_TEXTURE_GRID) {
		if (e->format != AERON_TEXTURE_FORMAT_RGBA16_FLOAT || e->level_count != 1 ||
			(uint32_t)e->size != 4u * r[0] || (uint32_t)ktx2_height(ktx) != r[1] * r[2]) {
			snprintf(e->failure, sizeof e->failure, "expected one RGBA16F level of %ux%u for a %ux%ux%u grid",
					 4u * r[0], r[1] * r[2], r[0], r[1], r[2]);
			ok = false;
		}
	} else if (ktx2_height(ktx) != e->size) {
		snprintf(e->failure, sizeof e->failure, "faces must be square");
		ok = false;
	} else if (!hdr_format(e->format)) {
		snprintf(e->failure, sizeof e->failure, "format must be BC6H or RGBA16F");
		ok = false;
	} else if (!Aeron_TextureFormatSupported(e->format, AERON_TEXTURE_USAGE_SAMPLED)) {
		snprintf(e->failure, sizeof e->failure, "%s is not supported by this device",
				 e->format == AERON_TEXTURE_FORMAT_RGBA16_FLOAT ? "RGBA16F" : "BC6H");
		if (!library->bc6h_reported) {
			library->bc6h_reported = true;
			Aeron_LogWarn("aeron.scene", "environment maps: %s sampling is unavailable on this device",
						  e->format == AERON_TEXTURE_FORMAT_RGBA16_FLOAT ? "RGBA16F" : "BC6H");
		}
		ok = false;
	}
	if (ok && e->kind == ENV_TEXTURE_GRID) {
		const Ktx2Level level = ktx2_level(ktx, 0);
		e->bytes              = (uint64_t)r[0] * r[1] * 3u * r[2] * 8u;
		e->texture            = upload_grid(cmd, &level, r, full);
		if (!e->texture) {
			snprintf(e->failure, sizeof e->failure, "upload failed");
			ok = false;
		}
	} else if (ok) {
		for (int level = 0; level < e->level_count; ++level)
			e->bytes += ktx2_level(ktx, level).size;
		e->texture = Aeron_ImageUploadKtx2(cmd, ktx, full);
		if (!e->texture) {
			snprintf(e->failure, sizeof e->failure, "upload failed");
			ok = false;
		}
	}
	ktx2_close(ktx);
	free(bytes);
	return ok;
}

typedef struct TextureList {
	EnvTexture* items;
	uint32_t    count;
	uint32_t    capacity;
} TextureList;

/* Returns the index of `path` in `list`, loading it or taking it over from
 * the published set; -1 on allocation failure. A failed load is recorded
 * (texture NULL) so it is attempted once per preparation. */
static int32_t acquire_texture(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd, TextureList* list,
							   const char* path, EnvTextureKind kind, const uint32_t grid[3], bool reload) {
	const EnvTexture* known = find_texture(list->items, list->count, path);
	if (known)
		return (int32_t)(known - list->items);
	if (list->count == list->capacity) {
		const uint32_t capacity = list->capacity ? list->capacity * 2u : 16u;
		EnvTexture*    items    = (EnvTexture*)realloc(list->items, capacity * sizeof *items);
		if (!items)
			return -1;
		list->items    = items;
		list->capacity = capacity;
	}
	EnvTexture* e = &list->items[list->count];
	memset(e, 0, sizeof *e);
	snprintf(e->path, sizeof e->path, "%s", path);
	e->kind = kind;
	if (grid)
		memcpy(e->grid, grid, sizeof e->grid);
	const EnvTexture* shared = reload ? NULL : find_texture(library->textures, library->texture_count, path);
	if (shared && shared->texture && shared->kind == kind &&
		(kind != ENV_TEXTURE_GRID || memcmp(shared->grid, e->grid, sizeof e->grid) == 0)) {
		*e = *shared;
	} else if (!env_load_texture(library, cmd, e)) {
		Aeron_LogError("aeron.scene", "environment texture %s%s: %s", library->path_prefix, path, e->failure);
	}
	return (int32_t)list->count++;
}

/* ---- packages -------------------------------------------------------------- */

void env_library_apply(AeronEnvironmentLibrary* library, EnvSlot* slot) {
	AeronEnvironmentPackage*    pkg = &slot->package;
	AeronEnvironmentDefinition* s   = &pkg->settings;
	float                       q[9], qt[9], bt[9], light_basis[9];
	package_rotation(s->rotation_degrees, q);
	mat3_transpose(q, qt);
	mat3_mul(qt, library->base, pkg->world_to_cube);
	/* A feature at cube direction d appears at world direction B^T Q d. */
	mat3_transpose(library->base, bt);
	mat3_mul(bt, q, light_basis);

	pkg->light_count         = s->light_count;
	pkg->primary_light_index = -1;
	for (uint32_t i = 0; i < s->light_count; ++i) {
		AeronEnvironmentLightDefinition* d = &s->lights[i];
		AeronEnvironmentLight*           l = &pkg->lights[i];
		const float                      length =
			sqrtf(d->direction_cube[0] * d->direction_cube[0] + d->direction_cube[1] * d->direction_cube[1] +
				  d->direction_cube[2] * d->direction_cube[2]);
		if (length > 1e-4f)
			for (int c = 0; c < 3; ++c)
				d->direction_cube[c] /= length;
		memset(l, 0, sizeof *l);
		snprintf(l->name, sizeof l->name, "%s", d->name);
		for (int row = 0; row < 3; ++row) {
			l->direction_world[row] = light_basis[row * 3 + 0] * d->direction_cube[0] +
									  light_basis[row * 3 + 1] * d->direction_cube[1] +
									  light_basis[row * 3 + 2] * d->direction_cube[2];
			l->color_linear[row]    = d->color_linear[row] * d->intensity;
		}
		l->angular_radius_degrees = d->angular_radius_degrees;
		l->primary                = d->primary;
		l->casts_shadows          = d->casts_shadows && d->primary;
		l->lens_flare             = d->lens_flare;
		l->visual                 = d->visual;
		l->radiance_scale         = d->radiance_scale;
		l->glow_radius_degrees    = d->glow_radius_degrees;
		l->glow_strength          = d->glow_strength;
		l->display_max_radiance   = library->sun_max_radiance;
		l->angular_size_degrees   = d->angular_size_degrees;
		l->sprite_strength        = d->sprite_strength;
		if (d->visual == AERON_ENV_SUN_SPRITE) {
			const EnvTexture* sprite =
				find_texture(library->textures, library->texture_count, d->sprite_path);
			l->sprite = sprite ? sprite->texture : NULL;
		}
		if (d->primary && pkg->primary_light_index < 0)
			pkg->primary_light_index = (int32_t)i;
	}
}

void env_library_reset(AeronEnvironmentLibrary* library, EnvSlot* slot) {
	slot->package.settings = *slot->package.definition;
	if (slot->reference.has_rotation)
		memcpy(slot->package.settings.rotation_degrees, slot->reference.rotation_degrees,
			   sizeof slot->reference.rotation_degrees);
	slot->overridden = false;
	env_library_apply(library, slot);
}

static bool same_reference(const AeronEnvironmentReference* a, const AeronEnvironmentReference* b) {
	return strcmp(a->package, b->package) == 0 && a->has_rotation == b->has_rotation &&
		   (!a->has_rotation ||
			memcmp(a->rotation_degrees, b->rotation_degrees, sizeof a->rotation_degrees) == 0);
}

/* The set buffer of a probes package (AeronEnvironmentPackage); NULL on
 * failure. */
static AeronBuffer* build_probe_buffer(AeronCommandBuffer* cmd, const AeronEnvironmentDefinition* def,
									   const AeronEnvironmentProbeCells* cells, uint32_t* out_bytes) {
	const size_t cell_count = (size_t)cells->dims[0] * cells->dims[1] * cells->dims[2];
	const size_t elements = AERON_ENV_PROBE_BUFFER_HEADER + AERON_ENV_PROBE_BUFFER_RECORD * def->probe_count +
							(cell_count + 3u) / 4u;
	const uint32_t bytes  = (uint32_t)(elements * 16u);
	uint32_t*      words  = (uint32_t*)calloc(elements, 16u);
	if (!words)
		return NULL;
	float* f = (float*)words; /* same storage: floats where the layout has floats */
	for (int a = 0; a < 3; ++a) {
		f[a]         = cells->min[a];
		words[4 + a] = cells->dims[a];
	}
	f[3]     = 1.0f / cells->cell_size;
	words[7] = def->probe_count;
	for (uint32_t k = 0; k < def->probe_count; ++k) {
		const AeronEnvironmentProbeDefinition* p = &def->probes[k];
		uint32_t* record = words + 4u * (AERON_ENV_PROBE_BUFFER_HEADER + AERON_ENV_PROBE_BUFFER_RECORD * k);
		float*    r      = (float*)record;
		for (int a = 0; a < 3; ++a) {
			r[a]     = p->center[a];
			r[4 + a] = 2.0f / p->size[a];
			r[8 + a] = 0.5f * p->parallax_size[a];
		}
		r[3]       = 1.0f / p->falloff;
		record[7]  = k;
		record[11] = (p->shape == AERON_ENV_PROBE_ELLIPSOID ? 1u : 0u) |
					 (p->parallax_shape == AERON_ENV_PROBE_ELLIPSOID ? 2u : 0u);
	}
	memcpy(words + 4u * (AERON_ENV_PROBE_BUFFER_HEADER + AERON_ENV_PROBE_BUFFER_RECORD * def->probe_count),
		   cells->words, cell_count * sizeof *cells->words);
	AeronBuffer* buffer = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size         = bytes,
		.usage        = AERON_BUFFER_USAGE_STORAGE,
		.memory_usage = AERON_MEMORY_USAGE_GPU_ONLY,
		.debug_name   = "scene.environment_probes",
	});
	if (buffer && !Aeron_UploadBufferDataCmd(cmd, buffer, 0, words, bytes)) {
		Aeron_DestroyBuffer(buffer);
		buffer = NULL;
	}
	free(words);
	*out_bytes = buffer ? bytes : 0;
	return buffer;
}

/* A probes package: its cube array, grid and set buffer. */
static bool build_probes_slot(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd, TextureList* list,
							  EnvSlot* slot, bool reload, bool* out_of_memory) {
	const AeronEnvironmentDefinition* def = slot->package.definition;
	const int32_t                     cubes =
		acquire_texture(library, cmd, list, def->cubes_path, ENV_TEXTURE_CUBE_ARRAY, NULL, reload);
	const int32_t grid = cubes < 0 ? -1
								   : acquire_texture(library, cmd, list, def->grid_path, ENV_TEXTURE_GRID,
													 def->grid_resolution, reload);
	if (cubes < 0 || grid < 0) {
		*out_of_memory = true;
		return false;
	}
	const EnvTexture* c = &list->items[cubes];
	const EnvTexture* g = &list->items[grid];
	for (int i = 0; i < 2; ++i) {
		const EnvTexture* e = i ? g : c;
		if (!e->texture) {
			snprintf(slot->failure, sizeof slot->failure, "%s: %s", e->path,
					 e->failure[0] ? e->failure : "unavailable");
			return false;
		}
	}
	if (c->kind != ENV_TEXTURE_CUBE_ARRAY || c->layers != (int)def->probe_count ||
		g->kind != ENV_TEXTURE_GRID || memcmp(g->grid, def->grid_resolution, sizeof g->grid) != 0) {
		snprintf(slot->failure, sizeof slot->failure, "%s holds %d probes for %u, or %s is used another way",
				 c->path, c->layers, def->probe_count, g->path);
		return false;
	}

	AeronEnvironmentProbeCells cells;
	if (!AeronEnvironment_BuildProbeCells(def, &cells)) {
		*out_of_memory = true;
		return false;
	}
	AeronEnvironmentPackage* pkg = &slot->package;
	pkg->probe_buffer            = build_probe_buffer(cmd, def, &cells, &slot->probe_buffer_bytes);
	slot->probe_overflow_cells   = cells.overflow_cells;
	if (cells.overflow_cells)
		Aeron_LogWarn(
			"aeron.scene",
			"environment package '%s': %u cells overlap more than %d probes; the smallest are used there",
			def->name, cells.overflow_cells, AERON_ENV_PROBE_CELL_PROBES);
	AeronEnvironment_FreeProbeCells(&cells);
	if (!pkg->probe_buffer) {
		snprintf(slot->failure, sizeof slot->failure, "probe buffer upload failed");
		return false;
	}
	pkg->probe_cubes     = c->texture;
	pkg->probe_grid      = g->texture;
	pkg->probe_mip_count = (uint32_t)c->level_count;
	pkg->sampler         = library->sampler;
	for (int a = 0; a < 3; ++a) {
		pkg->probe_bounds_min[a] = def->grid_center[a] - 0.5f * def->grid_size[a];
		pkg->probe_bounds_max[a] = def->grid_center[a] + 0.5f * def->grid_size[a];
		for (uint32_t k = 0; k < def->probe_count; ++k) {
			const AeronEnvironmentProbeDefinition* p = &def->probes[k];
			pkg->probe_bounds_min[a] = fminf(pkg->probe_bounds_min[a], p->center[a] - 0.5f * p->size[a]);
			pkg->probe_bounds_max[a] = fmaxf(pkg->probe_bounds_max[a], p->center[a] + 0.5f * p->size[a]);
		}
	}
	return true;
}

/* Builds one slot's package from textures in `list`; false when unavailable. */
static bool build_slot(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd, TextureList* list,
					   EnvSlot* slot, bool reload, bool* out_of_memory) {
	const AeronEnvironmentDefinition* def =
		AeronEnvironmentDefinitionSet_Find(library->definitions, slot->reference.package);
	if (!def) {
		snprintf(slot->failure, sizeof slot->failure, "unknown package '%s'", slot->reference.package);
		return false;
	}
	slot->package.definition = def;
	if (def->kind == AERON_ENV_KIND_PROBES)
		return build_probes_slot(library, cmd, list, slot, reload, out_of_memory);
	const char*    paths[3 + AERON_ENV_MAX_LIGHTS];
	EnvTextureKind kinds[3 + AERON_ENV_MAX_LIGHTS];
	int32_t        indices[3 + AERON_ENV_MAX_LIGHTS];
	int            count = 0;
	paths[count]         = def->diffuse_path;
	kinds[count++]       = ENV_TEXTURE_CUBE;
	int specular = -1, radiance = -1;
	if (def->specular_path[0]) {
		specular       = count;
		paths[count]   = def->specular_path;
		kinds[count++] = ENV_TEXTURE_CUBE;
	}
	if (def->kind == AERON_ENV_KIND_FULL) {
		radiance       = count;
		paths[count]   = def->radiance_path;
		kinds[count++] = ENV_TEXTURE_CUBE;
		for (uint32_t i = 0; i < def->light_count; ++i) {
			if (def->lights[i].visual == AERON_ENV_SUN_SPRITE) {
				paths[count]   = def->lights[i].sprite_path;
				kinds[count++] = ENV_TEXTURE_2D;
			}
		}
	}
	bool available = true;
	for (int i = 0; i < count; ++i) {
		indices[i] = acquire_texture(library, cmd, list, paths[i], kinds[i], NULL, reload);
		if (indices[i] < 0) {
			*out_of_memory = true;
			return false;
		}
		const EnvTexture* e = &list->items[indices[i]];
		if (!e->texture && available) {
			snprintf(slot->failure, sizeof slot->failure, "%s: %s", e->path,
					 e->failure[0] ? e->failure : "unavailable");
			available = false;
		} else if (e->texture && e->kind != kinds[i] && available) {
			snprintf(slot->failure, sizeof slot->failure, "%s is used as two kinds of texture", e->path);
			available = false;
		}
	}
	if (!available)
		return false;
	slot->package.diffuse = list->items[indices[0]].texture;
	slot->package.sampler = library->sampler;
	if (specular >= 0) {
		slot->package.specular           = list->items[indices[specular]].texture;
		slot->package.specular_mip_count = (uint32_t)list->items[indices[specular]].level_count;
	}
	if (radiance >= 0)
		slot->package.radiance = list->items[indices[radiance]].texture;
	return true;
}

/* Releases the slots' probe buffers and the slots. */
static void free_slots(EnvSlot* slots, uint32_t count) {
	for (uint32_t i = 0; slots && i < count; ++i)
		Aeron_DestroyBuffer(slots[i].package.probe_buffer);
	free(slots);
}

static bool prepare_set(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd,
						const AeronEnvironmentReference* references, uint32_t count, bool reload);

static bool prepare(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd,
					const AeronEnvironmentReference* references, uint32_t count, bool reload) {
	if (!library || !cmd || (count > 0 && !references))
		return false;
	/* Texture uploads for the whole set, as one group in GPU captures. */
	Aeron_GpuDebugPush(cmd, reload ? "Environment package reload" : "Environment package preparation");
	const bool built = prepare_set(library, cmd, references, count, reload);
	Aeron_GpuDebugPop(cmd);
	return built;
}

static bool prepare_set(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd,
						const AeronEnvironmentReference* references, uint32_t count, bool reload) {
	TextureList list          = { NULL, 0, 0 };
	EnvSlot*    slots         = count ? (EnvSlot*)calloc(count, sizeof *slots) : NULL;
	bool        out_of_memory = count > 0 && !slots;
	for (uint32_t i = 0; i < count && !out_of_memory; ++i) {
		EnvSlot* slot   = &slots[i];
		slot->reference = references[i];
		slot->available = build_slot(library, cmd, &list, slot, reload, &out_of_memory);
	}
	if (out_of_memory) {
		/* Textures loaded for this attempt are not in the published set. */
		for (uint32_t i = 0; i < list.count; ++i) {
			bool published = false;
			for (uint32_t j = 0; j < library->texture_count && !published; ++j)
				published = library->textures[j].texture == list.items[i].texture;
			if (list.items[i].texture && !published)
				Aeron_DestroyTexture(list.items[i].texture);
		}
		free(list.items);
		free_slots(slots, count);
		Aeron_LogError("aeron.scene", "environment library: out of memory preparing %u references", count);
		return false;
	}

	/* Publish, then retire the textures the new set no longer carries. */
	EnvTexture* old_textures = library->textures;
	uint32_t    old_count    = library->texture_count;
	EnvSlot*    old_slots    = library->slots;
	uint32_t    old_slots_n  = library->slot_count;
	library->textures        = list.items;
	library->texture_count   = list.count;
	library->slots           = slots;
	library->slot_count      = count;
	if (library->active >= (int32_t)count)
		library->active = -1;
	uint32_t available = 0;
	for (uint32_t i = 0; i < count; ++i) {
		EnvSlot* slot = &slots[i];
		if (!slot->available) {
			Aeron_LogWarn("aeron.scene", "environment package '%s' unavailable: %s", slot->reference.package,
						  slot->failure);
			continue;
		}
		++available;
		const EnvSlot* previous = NULL;
		for (uint32_t j = 0; j < old_slots_n && !previous; ++j)
			if (old_slots[j].overridden && same_reference(&old_slots[j].reference, &slot->reference))
				previous = &old_slots[j];
		if (previous) {
			slot->package.settings = previous->package.settings;
			slot->overridden       = true;
			env_library_apply(library, slot);
		} else {
			env_library_reset(library, slot);
		}
	}
	for (uint32_t i = 0; i < old_count; ++i) {
		AeronTexture* texture = old_textures[i].texture;
		bool          kept    = false;
		for (uint32_t j = 0; j < list.count && !kept; ++j)
			kept = list.items[j].texture == texture;
		if (texture && !kept)
			Aeron_DestroyTexture(texture);
	}
	free(old_textures);
	free_slots(old_slots, old_slots_n);

	uint64_t bytes = 0, probe_bytes = 0;
	uint32_t loaded = 0;
	for (uint32_t i = 0; i < list.count; ++i) {
		if (list.items[i].texture) {
			bytes += list.items[i].bytes;
			++loaded;
			if (list.items[i].kind == ENV_TEXTURE_CUBE_ARRAY || list.items[i].kind == ENV_TEXTURE_GRID)
				probe_bytes += list.items[i].bytes;
		}
	}
	for (uint32_t i = 0; i < count; ++i) {
		bytes += slots[i].probe_buffer_bytes;
		probe_bytes += slots[i].probe_buffer_bytes;
	}
	Aeron_LogInfo("aeron.scene", "environment library: %u/%u references available, %u textures, %.1f MiB%s",
				  available, count, loaded, (double)bytes / (1024.0 * 1024.0), reload ? " (reloaded)" : "");
	if (probe_bytes > AERON_ENV_WARN_BYTES)
		Aeron_LogWarn("aeron.scene", "environment library: probes take %.1f MiB, above %.0f MiB",
					  (double)probe_bytes / (1024.0 * 1024.0),
					  (double)AERON_ENV_WARN_BYTES / (1024.0 * 1024.0));
	return true;
}

/* ---- public API ------------------------------------------------------------ */

AeronEnvironmentLibrary* AeronEnvironmentLibrary_Create(const AeronEnvironmentLibraryDesc* desc) {
	if (!desc || !desc->definitions || !desc->vfs)
		return NULL;
	AeronEnvironmentLibrary* library = (AeronEnvironmentLibrary*)calloc(1, sizeof *library);
	if (!library)
		return NULL;
	library->definitions = desc->definitions;
	library->vfs         = desc->vfs;
	library->root        = desc->root;
	snprintf(library->path_prefix, sizeof library->path_prefix, "%s",
			 desc->path_prefix ? desc->path_prefix : "");
	memcpy(library->base, desc->base_world_to_cube, sizeof library->base);
	library->sun_max_radiance =
		desc->sun_max_radiance > 0.0f ? desc->sun_max_radiance : AERON_ENV_DEFAULT_SUN_MAX_RADIANCE;
	library->active  = -1;
	library->sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_LINEAR,
		.mag_filter = AERON_FILTER_LINEAR,
		.mip_filter = AERON_FILTER_LINEAR,
		.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.max_lod    = 1000.0f,
	});
	if (!library->sampler) {
		Aeron_LogError("aeron.scene", "environment library: sampler creation failed");
		free(library);
		return NULL;
	}
	return library;
}

void AeronEnvironmentLibrary_Destroy(AeronEnvironmentLibrary* library) {
	if (!library)
		return;
#ifdef AERON_DEBUG_UI
	env_debug_detach(library);
#endif
	for (uint32_t i = 0; i < library->texture_count; ++i)
		if (library->textures[i].texture)
			Aeron_DestroyTexture(library->textures[i].texture);
	Aeron_DestroySampler(library->sampler);
	free(library->textures);
	free_slots(library->slots, library->slot_count);
	free(library);
}

bool AeronEnvironmentLibrary_Prepare(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd,
									 const AeronEnvironmentReference* references, uint32_t count) {
	return prepare(library, cmd, references, count, false);
}

uint32_t AeronEnvironmentLibrary_ReferenceCount(const AeronEnvironmentLibrary* library) {
	return library ? library->slot_count : 0;
}

const AeronEnvironmentPackage* AeronEnvironmentLibrary_Find(const AeronEnvironmentLibrary* library,
															uint32_t                       index) {
	if (!library || index >= library->slot_count || !library->slots[index].available)
		return NULL;
	return &library->slots[index].package;
}

const char* AeronEnvironmentLibrary_FailureReason(const AeronEnvironmentLibrary* library, uint32_t index) {
	if (!library || index >= library->slot_count)
		return "no such reference";
	return library->slots[index].available ? NULL : library->slots[index].failure;
}

AeronSampler* AeronEnvironmentLibrary_Sampler(const AeronEnvironmentLibrary* library) {
	return library ? library->sampler : NULL;
}

void AeronEnvironmentLibrary_RequestReload(AeronEnvironmentLibrary* library) {
	if (library)
		library->reload_requested = true;
}

bool AeronEnvironmentLibrary_ReloadRequested(const AeronEnvironmentLibrary* library) {
	return library && library->reload_requested;
}

bool AeronEnvironmentLibrary_ServiceReload(AeronEnvironmentLibrary* library, AeronCommandBuffer* cmd) {
	if (!library || !library->reload_requested || !cmd)
		return false;
	library->reload_requested        = false;
	const uint32_t             count = library->slot_count;
	AeronEnvironmentReference* references =
		count ? (AeronEnvironmentReference*)malloc(count * sizeof *references) : NULL;
	if (count && !references)
		return false;
	for (uint32_t i = 0; i < count; ++i)
		references[i] = library->slots[i].reference;
	const bool ok = prepare(library, cmd, references, count, true);
	free(references);
	return ok;
}

void AeronEnvironmentLibrary_SetActive(AeronEnvironmentLibrary* library, int32_t index) {
	if (library)
		library->active = index >= 0 && (uint32_t)index < library->slot_count ? index : -1;
}

#ifndef AERON_DEBUG_UI
void AeronEnvironmentLibrary_RegisterDebugTool(AeronEnvironmentLibrary*    library,
											   AeronEnvironmentDebugRowsFn rows, void* user) {
	(void)library;
	(void)rows;
	(void)user;
}
#endif
