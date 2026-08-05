#include "aeron/asset/opt_model.h"

#include "gltf_cook.h"
#include "opt2gltf.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct OptModelCookContext {
	OptGltfDocument *document;
	AeronGltfModel *model;
	const char *label;
} OptModelCookContext;

static bool opt_model_error(AeronOptModelError *error, int code,
							const char *message) {
	if (error) {
		error->code = code;
		snprintf(error->message, sizeof error->message, "%s",
				 message ? message : "OPT conversion failed");
	}
	return false;
}

static bool opt_model_image_provider(void *context, const cgltf_image *image,
									 AeronGltfCookImageView *out_view) {
	OptModelCookContext *cook = context;
	OptGltfImageView image_view;
	if (!OptGltf_ImageView(cook->document, image, &image_view))
		return false;
	out_view->rgba = image_view.rgba;
	out_view->width = (int)image_view.width;
	out_view->height = (int)image_view.height;
	return true;
}

static bool opt_model_consumer(void *context, const cgltf_data *cooked_data) {
	OptModelCookContext *cook = context;
	return Aeron_GltfMeshBuildData(cooked_data, cook->label, cook->model);
}

bool Aeron_OptModelBuildMemory(const void *bytes, size_t size,
							   const char *label,
							   const AeronOptModelBuildOptions *options,
							   AeronGltfModel *out,
							   AeronOptModelError *error) {
	if (out) memset(out, 0, sizeof *out);
	if (error) memset(error, 0, sizeof *error);
	if (!bytes || size == 0 || !label || !label[0] || !options || !out ||
		!isfinite(options->vertex_scale) || options->vertex_scale <= 0.0f ||
		!isfinite(options->smooth_angle_degrees) ||
		options->smooth_angle_degrees < 0.0f ||
		options->smooth_angle_degrees > 180.0f ||
		!isfinite(options->emissive_strength) ||
		options->emissive_strength < 0.0f)
		return opt_model_error(error, 1, "invalid OPT build arguments");

	opt_error_t parser_error = {{0}};
	opt_file_t *opt = opt_load_memory(bytes, size, &parser_error);
	if (!opt)
		return opt_model_error(error, 2, parser_error.msg);
	const OptGltfBuildOptions build_options = {
			.vertex_scale = options->vertex_scale,
			.smooth_angle_degrees = options->smooth_angle_degrees,
			.repair_normals = true,
			.emissive = options->emissive,
	};
	OptGltfDocument *document = NULL;
	if (!OptGltf_BuildMemory(opt, label, &build_options, &document,
							  &parser_error)) {
		opt_free(opt);
		return opt_model_error(error, 3, parser_error.msg);
	}
	opt_free(opt);

	AeronGltfCookOptions cook_options;
	aeron_gltf_cook_default_options(&cook_options);
	cook_options.encoding = AERON_GLTF_COOK_ENCODING_RGBA8;
	cook_options.zstd_supercompress = false;
	cook_options.verbose = false;
	OptModelCookContext context = {
			.document = document,
			.model = out,
			.label = label,
	};
	const bool cooked = aeron_gltf_cook_data(
			OptGltf_Data(document), label, opt_model_image_provider, &context,
			opt_model_consumer, &context, &cook_options);
	OptGltf_Free(document);
	if (!cooked) {
		Aeron_GltfMeshFree(out);
		return opt_model_error(error, 4,
						   "in-memory OPT conversion and cook failed");
	}
	if (options->emissive) {
		for (uint32_t index = 0; index < out->material_count; ++index) {
			AeronGltfMaterial *material = &out->materials[index];
			const float *rect =
					material->uv_xform[AERON_GLTF_CHANNEL_EMISSIVE];
			if (rect[2] > 0.0f && rect[3] > 0.0f)
				material->emissive_strength *= options->emissive_strength;
		}
	}
	return true;
}
