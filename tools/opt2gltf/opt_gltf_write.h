#ifndef AERON_OPT2GLTF_OPT_GLTF_WRITE_H
#define AERON_OPT2GLTF_OPT_GLTF_WRITE_H

/*
 * Writes a converted document (opt_gltf.h) as <out_dir>/<basename>.gltf and
 * <out_dir>/<basename>.bin. Its images are written as PNG files under
 * <out_dir>/textures/, at the path given by the URI of each image. A summary
 * goes to the tool log.
 */

#include <stdbool.h>

#include "opt_gltf.h"

/* Writes `document`. Returns false on an I/O error. Creates <out_dir> and
 * its missing parents. */
bool OptGltf_WriteFiles(const OptGltfDocument* document, const char* out_dir, const char* basename);

/* The JSON that OptGltf_WriteFiles writes into the .gltf. `*size` doesn't
 * count the 0 byte that ends the text. Returns NULL when out of memory. */
char* OptGltf_Json(const OptGltfDocument* document, size_t* size);

/* Converts `opt` with OptGltf_BuildMemory. Errors and warnings are reported
 * under `label`, and notes are dropped. This replaces the log set in
 * `options`. */
bool OptGltf_Build(const opt_file_t* opt, const char* basename, const OptGltfBuildOptions* options,
				   const char* label, OptGltfDocument** document);

#endif /* AERON_OPT2GLTF_OPT_GLTF_WRITE_H */
