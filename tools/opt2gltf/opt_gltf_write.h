#ifndef AERON_OPT2GLTF_OPT_GLTF_WRITE_H
#define AERON_OPT2GLTF_OPT_GLTF_WRITE_H

/*
 * Writing a converted document (opt_gltf.h) as files: <out_dir>/<basename>.gltf,
 * <out_dir>/<basename>.bin and its images as PNG under <out_dir>/textures/,
 * as each image's URI says. A summary goes to stdout.
 */

#include <stdbool.h>

#include "opt_gltf.h"

/* Writes `document`; false on an I/O error. <out_dir> is created, and its
 * parent must exist. */
bool OptGltf_WriteFiles(const OptGltfDocument* document, const char* out_dir, const char* basename);

/* Builds `opt` (OptGltf_BuildMemory) and writes it; errors and warnings
 * are reported under `label` (its log is replaced). */
bool OptGltf_Convert(const opt_file_t* opt, const char* out_dir, const char* basename,
					 const OptGltfBuildOptions* options, const char* label);

#endif /* AERON_OPT2GLTF_OPT_GLTF_WRITE_H */
