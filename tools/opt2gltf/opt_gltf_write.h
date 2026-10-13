#ifndef AERON_OPT2GLTF_OPT_GLTF_WRITE_H
#define AERON_OPT2GLTF_OPT_GLTF_WRITE_H

/*
 * Writes a converted document (opt_gltf.h) as <out_dir>/<basename>.gltf and
 * <out_dir>/<basename>.bin. Its images are written as PNG files under
 * <out_dir>/textures/, at the path given by the URI of each image. A summary
 * goes to stdout.
 */

#include <stdbool.h>

#include "opt_gltf.h"

/* Writes `document`. Returns false on an I/O error. Creates <out_dir> and
 * its missing parents. */
bool OptGltf_WriteFiles(const OptGltfDocument* document, const char* out_dir, const char* basename);

/* Converts `opt` with OptGltf_BuildMemory and writes the result. Errors and
 * warnings are reported under `label`, and notes are dropped. This replaces
 * the log set in `options`. */
bool OptGltf_Convert(const opt_file_t* opt, const char* out_dir, const char* basename,
					 const OptGltfBuildOptions* options, const char* label);

#endif /* AERON_OPT2GLTF_OPT_GLTF_WRITE_H */
