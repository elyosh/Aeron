#ifndef AERON_ENVIRONMENT_BAKE_ENV_BAKE_H
#define AERON_ENVIRONMENT_BAKE_ENV_BAKE_H

/*
 * The offline HDR environment baker: the bake modes of environment_bake,
 * which the mod importer also runs.
 *
 * Bake mode converts an equirectangular Radiance .hdr panorama, or six cube
 * faces, and an optional light list into BC6H KTX2 cubes and an
 * `environments:` entry. With --radiance-out, the input is a sky without suns and is also
 * written as a radiance cube with box mips. Without it, the panorama is only
 * used for lighting (an enclosed space, such as a hangar). In both cases the
 * baker writes the Lambert irradiance / pi cube and the GGX cube with
 * roughness mips.
 * Overlay mode converts six cube faces with alpha into a BC7 sRGB cube
 * whose colour is premultiplied by alpha, for an environment's `overlay`.
 * Diagnostic mode writes a labelled cube for checking orientation.
 * Sprite mode converts an HDR image to a KTX2 sun sprite. Capture mode
 * bakes a capture directory written by the game (six cube faces, sun
 * images, capture.yaml) into an environment under the game's mod folder,
 * with the `environments:` and `xwa:` snippets to merge into the manifest. Probes
 * mode bakes the panoramas that the Blender add-on renders for a model's
 * reflection probes and irradiance grid. It writes two KTX2 images and the
 * probe layout used for the bake (bake.json). aeron_gltf_cook embeds them
 * in the model's GLB.
 *
 * See envmap.h for the cube face and panorama conventions. Output is
 * deterministic for identical inputs and does not depend on --jobs.
 */

#include <stdbool.h>
#include <stdio.h>

#include "cube_faces.h"

/* The options of every mode. Each mode reads the ones that it uses. */
typedef struct EnvBakeOptions {
	const char* input;
	const char* faces;         /* the prefix of the sky faces */
	const char* overlay_faces; /* the prefix of the overlay faces */
	/* Faces in memory, used instead of the files of `faces` or
	   `overlay_faces`. */
	const CubeFacePixels* face_pixels;
	float                 ldr_intensity; /* multiplies PNG and JPEG sky faces */
	const char* overlay_out;
	const char* lights_input;
	const char* name;
	const char* radiance_out;
	const char* diffuse_out;
	const char* specular_out;
	const char* environment_out;
	FILE*       environment_stream; /* the entry when there is no environment_out; NULL: none */
	const char* manifest_root;
	const char* diagnostic_out;
	const char* sprite_input;
	const char* sprite_out;
	const char* sprite_format;
	const char* capture_dir;
	const char* bake_root;
	const char* probes_input;
	const char* cubes_out;
	const char* grid_out;
	const char* bake_out;
	int         radiance_size;
	int         diffuse_size;
	int         specular_size;
	int         specular_samples;
	int         sprite_size;
	int         jobs;
	bool        specular_size_given; /* probes mode defaults to 256 */
} EnvBakeOptions;

/* The result of a bake. Its values are environment_bake's exit codes. */
typedef enum EnvBakeResult {
	ENV_BAKE_OK              = 0,
	ENV_BAKE_FAILED          = 1,
	ENV_BAKE_INVALID_OPTIONS = 2,
} EnvBakeResult;

/* Sets the default sizes, samples, sprite format, LDR intensity and jobs,
 * and clears the rest. */
void EnvBake_DefaultOptions(EnvBakeOptions* options);

/* Bakes the sky panorama `input`, or the sky faces `faces`, into the
 * radiance, diffuse and specular cubes, and writes the environment entry to
 * `environment_out`, or to `environment_stream`. */
EnvBakeResult EnvBake_Sky(const EnvBakeOptions* options);
/* Bakes the overlay faces `overlay_faces` into `overlay_out`. */
EnvBakeResult EnvBake_Overlay(const EnvBakeOptions* options);
/* Bakes the probes job `probes_input`. */
EnvBakeResult EnvBake_Probes(const EnvBakeOptions* options);
/* Writes the labelled cube `diagnostic_out`. */
EnvBakeResult EnvBake_Diagnostic(const EnvBakeOptions* options);
/* Bakes the sun sprite `sprite_input` into `sprite_out`. */
EnvBakeResult EnvBake_Sprite(const EnvBakeOptions* options);
/* Bakes the capture directory `capture_dir` under `bake_root`. */
EnvBakeResult EnvBake_Capture(const EnvBakeOptions* options);

#endif /* AERON_ENVIRONMENT_BAKE_ENV_BAKE_H */
