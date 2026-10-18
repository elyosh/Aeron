/*
 * The environment builder makes the source, diffuse and specular cubes from
 * six rendered faces (see aeron/scene/environment_builder.h).
 *
 * Each kind of result is first written into a 2D atlas that holds the six
 * faces side by side, face f at x = f * n in a level n texels wide. A face
 * size that is a power of two keeps every mip of the atlas six faces wide.
 */

#include "aeron/scene/environment_builder.h"

#include "aeron/log.h"

#include <stdlib.h>

#define BUILD_PI 3.14159265358979323846
#define BUILD_THREADS 8
#define BUILD_DIFFUSE_SIZE 32
/* The diffuse filter reads the largest source level of at most this size. */
#define BUILD_DIFFUSE_SOURCE_MAX 64
/* StoreFace writes levels 0 to 4 of a face; Finish makes the others from
 * level 4. */
#define BUILD_FACE_LEVELS 5
/* The number of threads per output texel in the diffuse filter. Its groups
 * then have 512 threads and 28 KiB of shared memory, within the D3D12
 * limits (1024 and 32 KiB). */
#define BUILD_DIFFUSE_SPLIT 8

/* Mirrors env_build_store_face.comp.hlsl. */
typedef struct StoreFaceUniform {
	int32_t params[4]; /* face, unused, face size at level 0, mirror */
} StoreFaceUniform;

/* Mirrors env_build_downsample.comp.hlsl. */
typedef struct DownsampleUniform {
	int32_t params[4]; /* level, level 4 face size, unused, unused */
} DownsampleUniform;

/* Mirrors env_build_irradiance.comp.hlsl. */
typedef struct IrradianceUniform {
	int32_t params[4]; /* source level, source size at that level, output size, unused */
} IrradianceUniform;

/* Mirrors env_build_ggx.comp.hlsl. */
typedef struct GgxUniform {
	float   params[4]; /* roughness, source texel solid angle, highest source lod, unused */
	int32_t counts[4]; /* output size, sample count, unused, unused */
} GgxUniform;

struct AeronEnvironmentBuilder {
	int  size;
	int  levels; /* log2(size) + 1 */
	int  specular_samples;
	int  faces_stored; /* bit f: face f stored since the last Finish */
	bool built;

	AeronTexture* source_atlas;   /* 6 size x size, all levels */
	AeronTexture* level4;         /* a copy of the source atlas's level 4 */
	AeronTexture* source_cube;    /* size, all levels */
	AeronTexture* specular_atlas; /* 6 size/2 x size/2: specular levels 1 and up */
	AeronTexture* specular_cube;  /* size, all levels */
	AeronTexture* diffuse_atlas;  /* 6 32 x 32 */
	AeronTexture* diffuse_cube;   /* 32, one level */
	AeronSampler* sampler;        /* trilinear, clamped */

	AeronComputePipeline* store_face;
	AeronComputePipeline* downsample;
	AeronComputePipeline* irradiance;
	AeronComputePipeline* ggx;
};

static AeronComputePipeline* create_pipeline(const char* name, uint32_t outputs, uint32_t threads_xy,
											 uint32_t threads_z) {
	return Aeron_CreateComputePipeline(&(AeronComputePipelineDesc) {
		.name                            = name,
		.sampler_count                   = 1,
		.readwrite_storage_texture_count = outputs,
		.uniform_buffer_count            = 1,
		.thread_count_x                  = threads_xy,
		.thread_count_y                  = threads_xy,
		.thread_count_z                  = threads_z,
	});
}

static AeronTexture* create_atlas(int face_size, int mip_count, bool sampled, const char* name) {
	uint32_t usage = AERON_TEXTURE_USAGE_COMPUTE_STORAGE_WRITE | AERON_TEXTURE_USAGE_TRANSFER_SRC;
	if (sampled) {
		usage |= AERON_TEXTURE_USAGE_SAMPLED;
	}
	return Aeron_CreateTexture(&(AeronTextureDesc) {
		.width      = 6 * face_size,
		.height     = face_size,
		.mip_count  = mip_count,
		.format     = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
		.usage      = usage,
		.debug_name = name,
	});
}

static AeronTexture* create_cube(int size, int mip_count, const char* name) {
	return Aeron_CreateTexture(&(AeronTextureDesc) {
		.width      = size,
		.height     = size,
		.mip_count  = mip_count,
		.format     = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
		.usage      = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.cube       = 1,
		.debug_name = name,
	});
}

AeronEnvironmentBuilder* AeronEnvironmentBuilder_Create(const AeronEnvironmentBuilderDesc* desc) {
	if (!desc || desc->size < 64 || desc->size > 512 || (desc->size & (desc->size - 1)) != 0 ||
		desc->specular_samples < 1 || desc->specular_samples > 1024) {
		Aeron_LogError("aeron.scene", "environment builder: size must be a power of two from 64 to 512, "
									  "and specular_samples from 1 to 1024");
		return NULL;
	}
	AeronEnvironmentBuilder* b = (AeronEnvironmentBuilder*)calloc(1, sizeof *b);
	if (!b) {
		return NULL;
	}
	b->size             = desc->size;
	b->specular_samples = desc->specular_samples;
	while ((1 << b->levels) <= b->size) {
		b->levels++;
	}
	b->source_atlas   = create_atlas(b->size, b->levels, true, "aeron.env_build.source_atlas");
	b->level4         = create_atlas(b->size >> 4, 1, true, "aeron.env_build.level4");
	b->source_cube    = create_cube(b->size, b->levels, "aeron.env_build.source");
	b->specular_atlas = create_atlas(b->size / 2, b->levels - 1, false, "aeron.env_build.specular_atlas");
	b->specular_cube  = create_cube(b->size, b->levels, "aeron.env_build.specular");
	b->diffuse_atlas  = create_atlas(BUILD_DIFFUSE_SIZE, 1, false, "aeron.env_build.diffuse_atlas");
	b->diffuse_cube   = create_cube(BUILD_DIFFUSE_SIZE, 1, "aeron.env_build.diffuse");
	b->sampler        = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_LINEAR,
		.mag_filter = AERON_FILTER_LINEAR,
		.mip_filter = AERON_FILTER_LINEAR,
		.address_u  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w  = AERON_ADDRESS_CLAMP_TO_EDGE,
		.max_lod    = 1000.0f,
	});
	b->store_face     = create_pipeline("env_build_store_face.comp", BUILD_FACE_LEVELS + 1, 16, 1);
	b->downsample     = create_pipeline("env_build_downsample.comp", 1, BUILD_THREADS, 1);
	b->irradiance     = create_pipeline("env_build_irradiance.comp", 1, BUILD_THREADS, BUILD_DIFFUSE_SPLIT);
	b->ggx            = create_pipeline("env_build_ggx.comp", 1, BUILD_THREADS, 1);
	if (!b->source_atlas || !b->level4 || !b->source_cube || !b->specular_atlas || !b->specular_cube ||
		!b->diffuse_atlas || !b->diffuse_cube || !b->sampler || !b->store_face || !b->downsample ||
		!b->irradiance || !b->ggx) {
		Aeron_LogError("aeron.scene", "environment builder: texture, sampler or pipeline creation failed");
		AeronEnvironmentBuilder_Destroy(b);
		return NULL;
	}
	return b;
}

void AeronEnvironmentBuilder_Destroy(AeronEnvironmentBuilder* b) {
	if (!b) {
		return;
	}
	Aeron_DestroyTexture(b->source_atlas);
	Aeron_DestroyTexture(b->level4);
	Aeron_DestroyTexture(b->source_cube);
	Aeron_DestroyTexture(b->specular_atlas);
	Aeron_DestroyTexture(b->specular_cube);
	Aeron_DestroyTexture(b->diffuse_atlas);
	Aeron_DestroyTexture(b->diffuse_cube);
	Aeron_DestroySampler(b->sampler);
	Aeron_DestroyComputePipeline(b->store_face);
	Aeron_DestroyComputePipeline(b->downsample);
	Aeron_DestroyComputePipeline(b->irradiance);
	Aeron_DestroyComputePipeline(b->ggx);
	free(b);
}

/* Starts a compute pass that writes level `level` of `texture`. */
static AeronComputePass* begin_pass(AeronCommandBuffer* cmd, AeronTexture* texture, int level,
									const char* label) {
	const AeronComputeTextureBinding output = { .texture = texture, .mip_level = (uint32_t)level };
	return Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer      = cmd,
		.write_textures      = &output,
		.write_texture_count = 1,
		.debug_label         = label,
	});
}

bool AeronEnvironmentBuilder_StoreFace(AeronEnvironmentBuilder* b, AeronCommandBuffer* cmd, int face,
									   AeronTexture* rendered, bool mirror_x) {
	if (!b || !cmd || face < 0 || face > 5 || !rendered) {
		return false;
	}
	/* The pass writes levels 0 to 4 of the atlas, and the copy of level 4. */
	AeronComputeTextureBinding outputs[BUILD_FACE_LEVELS + 1];
	for (int level = 0; level < BUILD_FACE_LEVELS; level++) {
		outputs[level] =
			(AeronComputeTextureBinding) { .texture = b->source_atlas, .mip_level = (uint32_t)level };
	}
	outputs[BUILD_FACE_LEVELS] = (AeronComputeTextureBinding) { .texture = b->level4 };
	AeronComputePass* pass     = Aeron_BeginComputePass(&(AeronComputePassDesc) {
		.command_buffer      = cmd,
		.write_textures      = outputs,
		.write_texture_count = BUILD_FACE_LEVELS + 1,
		.debug_label         = "Environment build: store face",
	});
	if (!pass) {
		return false;
	}
	const StoreFaceUniform u = { { face, 0, b->size, mirror_x ? 1 : 0 } };
	Aeron_BindComputePipeline(pass, b->store_face);
	Aeron_BindComputeTextureSampler(pass, 0, rendered, b->sampler);
	Aeron_BindComputeUniformData(pass, 0, &u, sizeof u);
	Aeron_DispatchCompute(pass, (uint32_t)b->size / 16, (uint32_t)b->size / 16, 1);
	Aeron_EndComputePass(pass);
	b->faces_stored |= 1 << face;
	return true;
}

/* Copies `levels` levels of each face of `atlas` into `cube`, atlas level k
 * into cube level k + `first`. */
static bool copy_to_cube(AeronCommandBuffer* cmd, AeronTexture* atlas, AeronTexture* cube, int face_size,
						 int levels, int first) {
	for (int level = 0; level < levels; level++) {
		const uint32_t n = (uint32_t)(face_size >> level);
		for (uint32_t face = 0; face < 6; face++) {
			const AeronTextureCopyDesc copy = {
				.source                = atlas,
				.source_mip_level      = (uint32_t)level,
				.source_x              = face * n,
				.destination           = cube,
				.destination_mip_level = (uint32_t)(level + first),
				.destination_layer     = face,
				.width                 = n,
				.height                = n,
			};
			if (!Aeron_CopyTextureCmd(cmd, &copy)) {
				return false;
			}
		}
	}
	return true;
}

/* Makes levels 5 and up of the source atlas from the copy of level 4.
 * Level k averages 2^(k-4) x 2^(k-4) texels of level 4. Up to 8 x 8, one
 * thread writes one texel; above that, one group of 8 x 8 threads does. */
static bool record_downsample(AeronEnvironmentBuilder* b, AeronCommandBuffer* cmd) {
	for (int level = BUILD_FACE_LEVELS; level < b->levels; level++) {
		AeronComputePass* pass = begin_pass(cmd, b->source_atlas, level, "Environment build: downsample");
		if (!pass) {
			return false;
		}
		const DownsampleUniform u = { { level, b->size >> 4, 0, 0 } };
		const uint32_t          n = (uint32_t)(b->size >> level);
		const uint32_t          groups =
			(1 << (level - 4)) <= BUILD_THREADS ? (n + BUILD_THREADS - 1) / BUILD_THREADS : n;
		Aeron_BindComputePipeline(pass, b->downsample);
		Aeron_BindComputeTextureSampler(pass, 0, b->level4, b->sampler);
		Aeron_BindComputeUniformData(pass, 0, &u, sizeof u);
		Aeron_DispatchCompute(pass, groups, groups, 6);
		Aeron_EndComputePass(pass);
	}
	return true;
}

static bool record_irradiance(AeronEnvironmentBuilder* b, AeronCommandBuffer* cmd) {
	int level = 0;
	while (level + 1 < b->levels && (b->size >> level) > BUILD_DIFFUSE_SOURCE_MAX) {
		level++;
	}
	AeronComputePass* pass = begin_pass(cmd, b->diffuse_atlas, 0, "Environment build: diffuse");
	if (!pass) {
		return false;
	}
	const IrradianceUniform u = { { level, b->size >> level, BUILD_DIFFUSE_SIZE, 0 } };
	Aeron_BindComputePipeline(pass, b->irradiance);
	Aeron_BindComputeTextureSampler(pass, 0, b->source_atlas, b->sampler);
	Aeron_BindComputeUniformData(pass, 0, &u, sizeof u);
	Aeron_DispatchCompute(pass, BUILD_DIFFUSE_SIZE / BUILD_THREADS, BUILD_DIFFUSE_SIZE / (2 * BUILD_THREADS),
						  6);
	Aeron_EndComputePass(pass);
	return true;
}

static bool record_ggx(AeronEnvironmentBuilder* b, AeronCommandBuffer* cmd) {
	const float texel_omega = (float)(4.0 * BUILD_PI / (6.0 * (double)b->size * (double)b->size));
	for (int level = 1; level < b->levels; level++) {
		AeronComputePass* pass = begin_pass(cmd, b->specular_atlas, level - 1, "Environment build: specular");
		if (!pass) {
			return false;
		}
		const int        n = b->size >> level;
		const GgxUniform u = {
			.params = { (float)level / (float)(b->levels - 1), texel_omega, (float)(b->levels - 1), 0.0f },
			.counts = { n, b->specular_samples, 0, 0 },
		};
		const uint32_t groups = (uint32_t)(n + BUILD_THREADS - 1) / BUILD_THREADS;
		Aeron_BindComputePipeline(pass, b->ggx);
		Aeron_BindComputeTextureSampler(pass, 0, b->source_cube, b->sampler);
		Aeron_BindComputeUniformData(pass, 0, &u, sizeof u);
		Aeron_DispatchCompute(pass, groups, groups, 6);
		Aeron_EndComputePass(pass);
	}
	return true;
}

bool AeronEnvironmentBuilder_Finish(AeronEnvironmentBuilder* b, AeronCommandBuffer* cmd) {
	if (!b || !cmd || b->faces_stored != 0x3F) {
		return false;
	}
	b->faces_stored = 0;
	/* The specular cube's level 0 is the source's. */
	if (!record_downsample(b, cmd) ||
		!copy_to_cube(cmd, b->source_atlas, b->source_cube, b->size, b->levels, 0) ||
		!copy_to_cube(cmd, b->source_atlas, b->specular_cube, b->size, 1, 0) || !record_irradiance(b, cmd) ||
		!copy_to_cube(cmd, b->diffuse_atlas, b->diffuse_cube, BUILD_DIFFUSE_SIZE, 1, 0) ||
		!record_ggx(b, cmd) ||
		!copy_to_cube(cmd, b->specular_atlas, b->specular_cube, b->size / 2, b->levels - 1, 1)) {
		return false;
	}
	b->built = true;
	return true;
}

bool AeronEnvironmentBuilder_Result(const AeronEnvironmentBuilder* b, AeronScenePbrEnvironment* out) {
	if (!b || !out || !b->built) {
		return false;
	}
	*out = (AeronScenePbrEnvironment) {
		.diffuse            = b->diffuse_cube,
		.specular           = b->specular_cube,
		.sampler            = b->sampler,
		.specular_mip_count = (uint32_t)b->levels,
		.diffuse_strength   = 1.0f,
		.specular_strength  = 1.0f,
		.world_to_local     = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f },
	};
	return true;
}
