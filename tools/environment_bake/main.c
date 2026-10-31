/*
 * environment_bake — the command line of the offline HDR environment baker.
 * It reads the arguments and runs one bake mode.
 */

#include "env_bake.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "envmap.h"

static void usage(void) {
	fprintf(
		stderr,
		"usage:\n"
		"  environment_bake --input sky.hdr --name NAME --radiance-out P --diffuse-out P --specular-out P\n"
		"                   [--lights-input lights.yaml] [--environment-out FILE] [--manifest-root DIR]\n"
		"                   [--radiance-size 2048] [--diffuse-size 32] [--specular-size 512]\n"
		"                   [--specular-samples 1024] [--jobs 8]\n"
		"  environment_bake --faces DIR --name NAME --radiance-out P --diffuse-out P --specular-out P\n"
		"                   [same options as --input]\n"
		"  environment_bake --overlay-faces DIR --overlay-out P [--radiance-size 2048]\n"
		"  environment_bake --input space.hdr --name NAME --diffuse-out P --specular-out P\n"
		"                   [--lights-input lights.yaml] [--environment-out FILE] [--manifest-root DIR]\n"
		"                   [--diffuse-size 32] [--specular-size 512] [--specular-samples 1024] [--jobs 8]\n"
		"  environment_bake --diagnostic-faces out.ktx2 [--radiance-size 2048] [--jobs 8]\n"
		"  environment_bake --sprite in.hdr --sprite-out out.ktx2 [--sprite-size 256]\n"
		"                   [--sprite-format bc6h|rgba16f] [--jobs 8]\n"
		"  environment_bake --capture DIR --bake-root DIR [--radiance-size 2048] [--diffuse-size 32]\n"
		"                   [--specular-size 512] [--specular-samples 1024] [--sprite-size 256] [--jobs 8]\n"
		"  environment_bake --probes job.json --cubes-out P --grid-out P --bake-out bake.json\n"
		"                   [--specular-size 256] [--specular-samples 1024] [--jobs 8]\n"
		"\n"
		"The sky is an equirectangular panorama (2:1) without suns, in the authored\n"
		"frame: +X right, +Y up, -Z forward at the image centre. --faces reads an XWA\n"
		"Upgrade skybox folder (Effects/CubeMaps/<name>): six PNG or JPEG faces, the\n"
		"digit before the extension giving the face (0 Right, 1 Left, 2 Top, 3 Bottom,\n"
		"4 Front, 5 Back). They are sRGB, and are turned to OpenXWA's cube frame.\n"
		"--overlay-faces reads an overlay folder, the same six faces with alpha.\n"
		"The light list is a\n"
		"YAML map with a `directional_lights` sequence in the environment light schema.\n"
		"Texture paths in the environment entry are relative to --manifest-root when given.\n"
		"A capture is baked into <bake root>/flight/environments/captured/<mission>/region_<n>/;\n"
		"its environment.yaml and mapping.yaml are merged into the manifest by hand.\n"
		"A probes job is a JSON object with the probe layout fields (the AERON_flight_model\n"
		"probeData fields, except the images), plus `probePanoramas`, `gridPanoramas`\n"
		"(x fastest, then y, then z) and `gridInvalid` (indices of points inside geometry).\n"
		"Panorama paths are relative to the job file. --bake-out writes the probe layout\n"
		"used for the bake. aeron_gltf_cook --probes embeds it with the images.\n");
}

static bool parse_int(const char* text, int* out) {
	char* end   = NULL;
	long  value = strtol(text, &end, 10);
	if (!text[0] || *end || value < 0 || value > 1 << 20)
		return false;
	*out = (int)value;
	return true;
}

static bool parse_args(int argc, char** argv, EnvBakeOptions* o) {
	EnvBake_DefaultOptions(o);
	for (int i = 1; i < argc; i += 2) {
		const char*  arg    = argv[i];
		const char*  value  = i + 1 < argc ? argv[i + 1] : NULL;
		const char** text   = NULL;
		int*         number = NULL;
		if (strcmp(arg, "--input") == 0)
			text = &o->input;
		else if (strcmp(arg, "--faces") == 0)
			text = &o->faces_dir;
		else if (strcmp(arg, "--overlay-faces") == 0)
			text = &o->overlay_faces_dir;
		else if (strcmp(arg, "--overlay-out") == 0)
			text = &o->overlay_out;
		else if (strcmp(arg, "--lights-input") == 0)
			text = &o->lights_input;
		else if (strcmp(arg, "--name") == 0)
			text = &o->name;
		else if (strcmp(arg, "--radiance-out") == 0)
			text = &o->radiance_out;
		else if (strcmp(arg, "--diffuse-out") == 0)
			text = &o->diffuse_out;
		else if (strcmp(arg, "--specular-out") == 0)
			text = &o->specular_out;
		else if (strcmp(arg, "--environment-out") == 0)
			text = &o->environment_out;
		else if (strcmp(arg, "--manifest-root") == 0)
			text = &o->manifest_root;
		else if (strcmp(arg, "--diagnostic-faces") == 0)
			text = &o->diagnostic_out;
		else if (strcmp(arg, "--sprite") == 0)
			text = &o->sprite_input;
		else if (strcmp(arg, "--sprite-out") == 0)
			text = &o->sprite_out;
		else if (strcmp(arg, "--sprite-format") == 0)
			text = &o->sprite_format;
		else if (strcmp(arg, "--capture") == 0)
			text = &o->capture_dir;
		else if (strcmp(arg, "--bake-root") == 0)
			text = &o->bake_root;
		else if (strcmp(arg, "--probes") == 0)
			text = &o->probes_input;
		else if (strcmp(arg, "--cubes-out") == 0)
			text = &o->cubes_out;
		else if (strcmp(arg, "--grid-out") == 0)
			text = &o->grid_out;
		else if (strcmp(arg, "--bake-out") == 0)
			text = &o->bake_out;
		else if (strcmp(arg, "--radiance-size") == 0)
			number = &o->radiance_size;
		else if (strcmp(arg, "--diffuse-size") == 0)
			number = &o->diffuse_size;
		else if (strcmp(arg, "--specular-size") == 0)
			number = &o->specular_size;
		else if (strcmp(arg, "--specular-samples") == 0)
			number = &o->specular_samples;
		else if (strcmp(arg, "--sprite-size") == 0)
			number = &o->sprite_size;
		else if (strcmp(arg, "--jobs") == 0)
			number = &o->jobs;
		else {
			fprintf(stderr, "environment_bake: unknown argument '%s'\n", arg);
			return false;
		}
		if (!value) {
			fprintf(stderr, "environment_bake: '%s' needs a value\n", arg);
			return false;
		}
		if (number == &o->specular_size)
			o->specular_size_given = true;
		if (text) {
			*text = value;
		} else if (!parse_int(value, number)) {
			fprintf(stderr, "environment_bake: '%s' needs a non-negative integer\n", arg);
			return false;
		}
	}
	if (o->jobs < 1 || o->jobs > ENVMAP_MAX_JOBS) {
		fprintf(stderr, "environment_bake: --jobs must be 1 to %d\n", ENVMAP_MAX_JOBS);
		return false;
	}
	return true;
}

int main(int argc, char** argv) {
	EnvBakeOptions o;
	if (argc < 2 || !parse_args(argc, argv, &o)) {
		usage();
		return 2;
	}
	const int modes = (o.input != NULL) + (o.faces_dir != NULL) + (o.overlay_faces_dir != NULL) +
					  (o.diagnostic_out != NULL) + (o.sprite_input != NULL) + (o.capture_dir != NULL) +
					  (o.probes_input != NULL);
	if (modes != 1) {
		fprintf(stderr,
				"environment_bake: give exactly one of --input, --faces, --overlay-faces, --diagnostic-faces, "
				"--sprite, --capture or --probes\n");
		usage();
		return 2;
	}
	const bool missing = o.overlay_faces_dir ? !o.overlay_out
						 : o.sprite_input    ? !o.sprite_out
						 : o.capture_dir     ? !o.bake_root
						 : o.probes_input    ? !o.cubes_out || !o.grid_out || !o.bake_out
						 : o.diagnostic_out  ? false
											 : !o.name || !o.diffuse_out || !o.specular_out;
	if (missing) {
		usage();
		return 2;
	}
	o.environment_stream = stdout;
	if (o.overlay_faces_dir)
		return EnvBake_Overlay(&o);
	if (o.diagnostic_out)
		return EnvBake_Diagnostic(&o);
	if (o.sprite_input)
		return EnvBake_Sprite(&o);
	if (o.capture_dir)
		return EnvBake_Capture(&o);
	if (o.probes_input)
		return EnvBake_Probes(&o);
	return EnvBake_Sky(&o);
}
