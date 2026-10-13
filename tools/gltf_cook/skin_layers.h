/*
 * Skin layers (tools/extensions/AERON_flight_model, "Skins"): a material's
 * `skins` and the model root's `skinVariants` become KHR_materials_variants
 * variants with composite materials, before the cook.
 *
 * SkinLayers_Expand rewrites the glTF JSON in place: it appends one variant
 * per skin variant after the file's variants (a skin variant with the name
 * of one of them changes that one instead), adds a material for each
 * distinct (material, layers) composite and maps the primitives, then
 * removes `skins` and `skinVariants`. The composite images get URIs
 * "aeron-composite:<n>", and SkinLayers_ImageProvider gives their pixels
 * (and those of the file's PNG images) to aeron_gltf_cook_data.
 */
#ifndef AERON_GLTF_COOK_SKIN_LAYERS_H
#define AERON_GLTF_COOK_SKIN_LAYERS_H

#include <stdbool.h>

#include "cJSON.h"
#include "gltf_cook.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SkinLayers SkinLayers;

/* Whether the document has skins or skin variants to expand. */
bool SkinLayers_Present(const cJSON* document);

/* Expands the skins of `document`, the glTF read from `gltf_path`. Returns
 * NULL after reporting the problem on stderr. */
SkinLayers* SkinLayers_Expand(cJSON* document, const char* gltf_path, bool verbose);

/* AeronGltfCookImageProvider: the pixels of a composite, or of a PNG of the
 * file. `context` is the SkinLayers. */
bool SkinLayers_ImageProvider(void* context, const struct cgltf_image* image, AeronGltfCookImageView* out);

/* Reports the composites and the time spent compositing (verbose). */
void SkinLayers_Report(const SkinLayers* layers);

void SkinLayers_Free(SkinLayers* layers);

#ifdef __cplusplus
}
#endif

#endif
