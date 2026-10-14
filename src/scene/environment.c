#include "aeron/scene/environment.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AeronEnvironmentDefinitionSet {
	AeronEnvironmentDefinition* definitions;
	uint32_t                    count;
};

static bool env_error(AeronConfigError* error, const AeronConfigNode* node, const char* format, ...) {
	if (error) {
		va_list args;
		memset(error, 0, sizeof *error);
		error->code = AERON_CONFIG_ERROR_INVALID_ARGUMENT;
		if (node) {
			const char* source_path = AeronConfigNode_SourcePath(node);
			error->root             = AeronConfigNode_SourceRoot(node);
			error->line             = AeronConfigNode_Line(node);
			error->column           = AeronConfigNode_Column(node);
			snprintf(error->path, sizeof error->path, "%s", source_path ? source_path : "<configuration>");
		}
		va_start(args, format);
		vsnprintf(error->message, sizeof error->message, format, args);
		va_end(args);
	}
	return false;
}

bool AeronEnvironment_IsValidName(const char* name) {
	if (!name)
		return false;
	const size_t length = strlen(name);
	if (length == 0 || length >= AERON_ENV_NAME_MAX)
		return false;
	for (size_t i = 0; i < length; ++i) {
		const char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_'))
			return false;
	}
	return true;
}

static bool key_allowed(const char* key, const char* const* allowed, size_t count) {
	for (size_t i = 0; i < count; ++i)
		if (strcmp(key, allowed[i]) == 0)
			return true;
	return false;
}

/* `what` names the map in messages, e.g. "package 'red_nebula'". */
static bool check_fields(const AeronConfigNode* map, const char* what, const char* const* allowed,
						 size_t count, AeronConfigError* error) {
	for (size_t i = 0; i < AeronConfigNode_MapCount(map); ++i) {
		const char* key = AeronConfigNode_MapKeyAt(map, i);
		if (!key_allowed(key, allowed, count))
			return env_error(error, AeronConfigNode_MapValueAt(map, i),
							 "%s: unknown or disallowed field '%s'", what, key);
	}
	return true;
}

static const AeronConfigNode* require_field(const AeronConfigNode* map, const char* what, const char* key,
											AeronConfigError* error) {
	const AeronConfigNode* node = AeronConfigNode_MapGet(map, key);
	if (!node)
		env_error(error, map, "%s: missing required field '%s'", what, key);
	return node;
}

static bool node_number(const AeronConfigNode* node, double* out) {
	const AeronConfigNodeType type = AeronConfigNode_Type(node);
	if (type != AERON_CONFIG_INT && type != AERON_CONFIG_FLOAT)
		return false;
	*out = AeronConfigNode_Float(node, NAN);
	return isfinite(*out) != 0;
}

static bool read_number(const AeronConfigNode* map, const char* what, const char* key, bool required,
						double min, double max, float* out, AeronConfigError* error) {
	const AeronConfigNode* node = AeronConfigNode_MapGet(map, key);
	if (!node) {
		if (required)
			return env_error(error, map, "%s: missing required field '%s'", what, key);
		return true;
	}
	double value = 0.0;
	if (!node_number(node, &value))
		return env_error(error, node, "%s: '%s' must be a finite number", what, key);
	if (value < min || value > max)
		return env_error(error, node, "%s: '%s' must be between %g and %g", what, key, min, max);
	*out = (float)value;
	return true;
}

static bool read_bool(const AeronConfigNode* map, const char* what, const char* key, bool* out,
					  AeronConfigError* error) {
	const AeronConfigNode* node = require_field(map, what, key, error);
	if (!node)
		return false;
	if (AeronConfigNode_Type(node) != AERON_CONFIG_BOOL)
		return env_error(error, node, "%s: '%s' must be boolean", what, key);
	*out = AeronConfigNode_Bool(node, 0) != 0;
	return true;
}

static bool read_path(const AeronConfigNode* map, const char* what, const char* key, char* out, size_t size,
					  AeronConfigError* error) {
	const AeronConfigNode* node = require_field(map, what, key, error);
	if (!node)
		return false;
	if (AeronConfigNode_Type(node) != AERON_CONFIG_STRING)
		return env_error(error, node, "%s: '%s' must be a string", what, key);
	const char*  value  = AeronConfigNode_String(node, "");
	const size_t length = strlen(value);
	if (length == 0)
		return env_error(error, node, "%s: '%s' must not be empty", what, key);
	if (length >= size)
		return env_error(error, node, "%s: '%s' must be shorter than %zu bytes", what, key, size);
	memcpy(out, value, length + 1);
	return true;
}

/* Reads a three-element numeric sequence. */
static bool read_vec3(const AeronConfigNode* map, const char* what, const char* key, float out[3],
					  const AeronConfigNode** out_node, AeronConfigError* error) {
	const AeronConfigNode* node = require_field(map, what, key, error);
	if (!node)
		return false;
	if (AeronConfigNode_Type(node) != AERON_CONFIG_SEQUENCE || AeronConfigNode_SequenceCount(node) != 3)
		return env_error(error, node, "%s: '%s' must be a sequence of three numbers", what, key);
	for (size_t i = 0; i < 3; ++i) {
		double value = 0.0;
		if (!node_number(AeronConfigNode_SequenceGet(node, i), &value))
			return env_error(error, node, "%s: '%s' must contain three finite numbers", what, key);
		out[i] = (float)value;
	}
	if (out_node)
		*out_node = node;
	return true;
}

/* Linear RGB radiance added uniformly to the diffuse ambient. */
static bool read_ambient_fill(const AeronConfigNode* map, const char* what, float out[3],
							  AeronConfigError* error) {
	const AeronConfigNode* node = NULL;
	if (!read_vec3(map, what, "ambient_fill", out, &node, error))
		return false;
	for (int i = 0; i < 3; ++i)
		if (out[i] < 0.0f || out[i] > 4.0f)
			return env_error(error, node, "%s: 'ambient_fill' values must be between 0 and 4", what);
	return true;
}

/* A three-number size, each above zero. */
static bool read_size(const AeronConfigNode* map, const char* what, const char* key, float out[3],
					  AeronConfigError* error) {
	const AeronConfigNode* node = NULL;
	if (!read_vec3(map, what, key, out, &node, error))
		return false;
	for (int i = 0; i < 3; ++i)
		if (!(out[i] > 0.0f))
			return env_error(error, node, "%s: '%s' values must be above 0", what, key);
	return true;
}

static bool read_shape(const AeronConfigNode* map, const char* what, const char* key,
					   AeronEnvironmentProbeShape* out, AeronConfigError* error) {
	const AeronConfigNode* node = require_field(map, what, key, error);
	if (!node)
		return false;
	const char* shape = AeronConfigNode_String(node, NULL);
	if (shape && strcmp(shape, "box") == 0)
		*out = AERON_ENV_PROBE_BOX;
	else if (shape && strcmp(shape, "ellipsoid") == 0)
		*out = AERON_ENV_PROBE_ELLIPSOID;
	else
		return env_error(error, node, "%s: '%s' must be 'box' or 'ellipsoid'", what, key);
	return true;
}

static bool parse_probe(const AeronConfigNode* map, const char* owner, size_t index,
						AeronEnvironmentProbeDefinition* probe, AeronConfigError* error) {
	static const char* const fields[] = { "center",          "size",          "shape",         "falloff",
										  "parallax_center", "parallax_size", "parallax_shape" };
	char                     what[160];
	snprintf(what, sizeof what, "%s probe %zu", owner, index);
	if (AeronConfigNode_Type(map) != AERON_CONFIG_MAP)
		return env_error(error, map, "%s must be a map", what);
	if (!check_fields(map, what, fields, sizeof fields / sizeof fields[0], error))
		return false;
	const AeronConfigNode* falloff = AeronConfigNode_MapGet(map, "falloff");
	if (!read_vec3(map, what, "center", probe->center, NULL, error) ||
		!read_size(map, what, "size", probe->size, error) ||
		!read_shape(map, what, "shape", &probe->shape, error) ||
		!read_number(map, what, "falloff", true, 0.0, 1.0, &probe->falloff, error) ||
		!read_size(map, what, "parallax_size", probe->parallax_size, error) ||
		!read_shape(map, what, "parallax_shape", &probe->parallax_shape, error))
		return false;
	memcpy(probe->parallax_center, probe->center, sizeof probe->parallax_center);
	if (AeronConfigNode_MapGet(map, "parallax_center") &&
		!read_vec3(map, what, "parallax_center", probe->parallax_center, NULL, error))
		return false;
	if (!(probe->falloff > 0.0f))
		return env_error(error, falloff, "%s: 'falloff' must be above 0", what);
	return true;
}

bool AeronEnvironment_ParseProbeSet(const AeronConfigNode* map, const char* what,
									AeronEnvironmentDefinition* def, AeronConfigError* error) {
	if (!what)
		what = "probes";
	if (!map || !def || AeronConfigNode_Type(map) != AERON_CONFIG_MAP)
		return env_error(error, map, "%s must be a map", what);
	const AeronConfigNode* box = require_field(map, what, "grid_box", error);
	if (!box)
		return false;
	char box_what[192];
	snprintf(box_what, sizeof box_what, "%s grid_box", what);
	static const char* const box_fields[] = { "center", "size" };
	if (AeronConfigNode_Type(box) != AERON_CONFIG_MAP)
		return env_error(error, box, "%s must be a map", box_what);
	if (!check_fields(box, box_what, box_fields, 2, error) ||
		!read_vec3(box, box_what, "center", def->grid_center, NULL, error) ||
		!read_size(box, box_what, "size", def->grid_size, error))
		return false;

	const AeronConfigNode* resolution = require_field(map, what, "grid_resolution", error);
	if (!resolution)
		return false;
	if (AeronConfigNode_Type(resolution) != AERON_CONFIG_SEQUENCE ||
		AeronConfigNode_SequenceCount(resolution) != 3)
		return env_error(error, resolution, "%s: 'grid_resolution' must be a sequence of three integers",
						 what);
	for (size_t i = 0; i < 3; ++i) {
		const AeronConfigNode* axis  = AeronConfigNode_SequenceGet(resolution, i);
		const int64_t          value = AeronConfigNode_Int(axis, 0);
		if (AeronConfigNode_Type(axis) != AERON_CONFIG_INT || value < AERON_ENV_PROBE_GRID_MIN ||
			value > AERON_ENV_PROBE_GRID_MAX)
			return env_error(error, resolution, "%s: 'grid_resolution' values must be integers from %d to %d",
							 what, AERON_ENV_PROBE_GRID_MIN, AERON_ENV_PROBE_GRID_MAX);
		def->grid_resolution[i] = (uint32_t)value;
	}

	const AeronConfigNode* probes = require_field(map, what, "probes", error);
	if (!probes)
		return false;
	const size_t count = AeronConfigNode_SequenceCount(probes);
	if (AeronConfigNode_Type(probes) != AERON_CONFIG_SEQUENCE || count < 1 || count > AERON_ENV_MAX_PROBES)
		return env_error(error, probes, "%s: 'probes' must be a sequence of 1 to %d probes", what,
						 AERON_ENV_MAX_PROBES);
	for (size_t i = 0; i < count; ++i)
		if (!parse_probe(AeronConfigNode_SequenceGet(probes, i), what, i, &def->probes[i], error))
			return false;
	def->probe_count = (uint32_t)count;
	return true;
}

float AeronEnvironment_NormalizeDegrees(float degrees) {
	float value = fmodf(degrees, 360.0f);
	if (value < 0.0f)
		value += 360.0f;
	return value >= 360.0f ? 0.0f : value;
}

static bool parse_visual(const AeronConfigNode* light_map, const char* what,
						 AeronEnvironmentLightDefinition* light, AeronConfigError* error) {
	static const char* const disc_fields[]   = { "kind", "radiance_scale", "glow_radius_degrees",
												 "glow_strength" };
	static const char* const sprite_fields[] = { "kind", "sprite", "angular_size_degrees",
												 "sprite_strength" };

	const AeronConfigNode* visual = require_field(light_map, what, "visual", error);
	if (!visual)
		return false;
	if (AeronConfigNode_Type(visual) != AERON_CONFIG_MAP)
		return env_error(error, visual, "%s: 'visual' must be a map", what);
	const AeronConfigNode* kind = require_field(visual, what, "kind", error);
	if (!kind)
		return false;
	const char* kind_name = AeronConfigNode_String(kind, NULL);
	char        visual_what[160];
	snprintf(visual_what, sizeof visual_what, "%s visual", what);

	if (kind_name && strcmp(kind_name, "disc") == 0) {
		light->visual = AERON_ENV_SUN_DISC;
		if (!check_fields(visual, visual_what, disc_fields, sizeof disc_fields / sizeof disc_fields[0],
						  error) ||
			!read_number(visual, visual_what, "radiance_scale", true, 0.0, 16.0, &light->radiance_scale,
						 error) ||
			!read_number(visual, visual_what, "glow_radius_degrees", false, 0.0, 30.0,
						 &light->glow_radius_degrees, error) ||
			!read_number(visual, visual_what, "glow_strength", false, 0.0, 16.0, &light->glow_strength,
						 error))
			return false;
		if (light->glow_strength > 0.0f && light->glow_radius_degrees < light->angular_radius_degrees)
			return env_error(error, visual,
							 "%s: 'glow_radius_degrees' must be at least the light's angular radius when "
							 "'glow_strength' is nonzero",
							 visual_what);
		return true;
	}
	if (kind_name && strcmp(kind_name, "sprite") == 0) {
		light->visual = AERON_ENV_SUN_SPRITE;
		return check_fields(visual, visual_what, sprite_fields,
							sizeof sprite_fields / sizeof sprite_fields[0], error) &&
			   read_path(visual, visual_what, "sprite", light->sprite_path, sizeof light->sprite_path,
						 error) &&
			   read_number(visual, visual_what, "angular_size_degrees", true, 0.01, 60.0,
						   &light->angular_size_degrees, error) &&
			   read_number(visual, visual_what, "sprite_strength", true, 0.0, 16.0, &light->sprite_strength,
						   error);
	}
	return env_error(error, kind, "%s: 'kind' must be 'disc' or 'sprite'", visual_what);
}

static bool parse_light(const AeronConfigNode* map, const char* owner, size_t index,
						AeronEnvironmentLightDefinition* light, AeronConfigError* error) {
	static const char* const fields[] = {
		"name",          "direction_cube",         "color_linear", "intensity", "primary",
		"casts_shadows", "angular_radius_degrees", "lens_flare",   "visual",
	};
	char what[160];
	snprintf(what, sizeof what, "%s light %zu", owner, index);
	if (AeronConfigNode_Type(map) != AERON_CONFIG_MAP)
		return env_error(error, map, "%s must be a map", what);
	if (!check_fields(map, what, fields, sizeof fields / sizeof fields[0], error))
		return false;

	const AeronConfigNode* name = require_field(map, what, "name", error);
	if (!name)
		return false;
	if (!AeronEnvironment_IsValidName(AeronConfigNode_String(name, NULL)))
		return env_error(error, name,
						 "%s: 'name' must be 1 to 63 characters of lower-case letters, digits, '.' and '_'",
						 what);
	snprintf(light->name, sizeof light->name, "%s", AeronConfigNode_String(name, ""));
	snprintf(what, sizeof what, "%s light '%s'", owner, light->name);

	const AeronConfigNode* direction_node = NULL;
	if (!read_vec3(map, what, "direction_cube", light->direction_cube, &direction_node, error))
		return false;
	const float length = sqrtf(light->direction_cube[0] * light->direction_cube[0] +
							   light->direction_cube[1] * light->direction_cube[1] +
							   light->direction_cube[2] * light->direction_cube[2]);
	if (!(length > 1e-4f))
		return env_error(error, direction_node, "%s: 'direction_cube' must have a nonzero length", what);
	for (int i = 0; i < 3; ++i)
		light->direction_cube[i] /= length;

	const AeronConfigNode* color_node = NULL;
	if (!read_vec3(map, what, "color_linear", light->color_linear, &color_node, error))
		return false;
	for (int i = 0; i < 3; ++i)
		if (light->color_linear[i] < 0.0f || light->color_linear[i] > 16.0f)
			return env_error(error, color_node, "%s: 'color_linear' values must be between 0 and 16", what);

	return read_number(map, what, "intensity", true, 0.0, 16.0, &light->intensity, error) &&
		   read_bool(map, what, "primary", &light->primary, error) &&
		   read_bool(map, what, "casts_shadows", &light->casts_shadows, error) &&
		   read_number(map, what, "angular_radius_degrees", true, 0.01, 5.0, &light->angular_radius_degrees,
					   error) &&
		   read_bool(map, what, "lens_flare", &light->lens_flare, error) &&
		   parse_visual(map, what, light, error);
}

bool AeronEnvironment_ParseLights(const AeronConfigNode* lights, const char* owner,
								  AeronEnvironmentLightDefinition out[AERON_ENV_MAX_LIGHTS],
								  uint32_t* out_count, int32_t* out_primary, AeronConfigError* error) {
	if (!owner)
		owner = "environment";
	if (!lights || !out || !out_count || !out_primary)
		return env_error(error, lights, "%s: missing 'directional_lights'", owner);
	if (AeronConfigNode_Type(lights) != AERON_CONFIG_SEQUENCE)
		return env_error(error, lights, "%s: 'directional_lights' must be a sequence", owner);
	const size_t count = AeronConfigNode_SequenceCount(lights);
	if (count > AERON_ENV_MAX_LIGHTS)
		return env_error(error, lights, "%s: at most %d directional lights are allowed", owner,
						 AERON_ENV_MAX_LIGHTS);

	memset(out, 0, sizeof(AeronEnvironmentLightDefinition) * AERON_ENV_MAX_LIGHTS);
	*out_count   = 0;
	*out_primary = -1;
	for (size_t i = 0; i < count; ++i) {
		const AeronConfigNode*           node  = AeronConfigNode_SequenceGet(lights, i);
		AeronEnvironmentLightDefinition* light = &out[i];
		if (!parse_light(node, owner, i, light, error))
			return false;
		for (size_t j = 0; j < i; ++j)
			if (strcmp(out[j].name, light->name) == 0)
				return env_error(error, node, "%s: duplicate light name '%s'", owner, light->name);
		if (light->casts_shadows && !light->primary)
			return env_error(error, node, "%s light '%s': 'casts_shadows' requires 'primary'", owner,
							 light->name);
		if (light->primary) {
			if (*out_primary >= 0)
				return env_error(error, node, "%s: more than one light sets 'primary'", owner);
			*out_primary = (int32_t)i;
		}
	}
	*out_count = (uint32_t)count;
	return true;
}

static bool parse_lights(const AeronConfigNode* package_map, AeronEnvironmentDefinition* def,
						 AeronConfigError* error) {
	char what[160];
	snprintf(what, sizeof what, "package '%s'", def->name);
	const AeronConfigNode* lights = require_field(package_map, what, "directional_lights", error);
	return lights && AeronEnvironment_ParseLights(lights, what, def->lights, &def->light_count,
												  &def->primary_light_index, error);
}

static bool parse_package(const char* name, const AeronConfigNode* map, AeronEnvironmentDefinition* def,
						  AeronConfigError* error) {
	static const char* const full_fields[] = {
		"kind",
		"radiance",
		"diffuse",
		"specular",
		"intensity",
		"diffuse_strength",
		"specular_strength",
		"ambient_fill",
		"rotation_degrees",
		"directional_lights",
	};
	static const char* const lighting_fields[] = {
		"kind",         "intensity",        "diffuse",  "diffuse_strength",
		"ambient_fill", "rotation_degrees", "specular", "specular_strength",
	};
	static const char* const probes_fields[] = {
		"kind",     "intensity",       "diffuse_strength", "specular_strength", "cubes", "grid",
		"grid_box", "grid_resolution", "probes",
	};

	char what[160];
	snprintf(what, sizeof what, "package '%s'", name);
	memset(def, 0, sizeof *def);
	snprintf(def->name, sizeof def->name, "%s", name);
	def->primary_light_index = -1;

	if (AeronConfigNode_Type(map) != AERON_CONFIG_MAP)
		return env_error(error, map, "%s must be a map", what);
	const AeronConfigNode* kind = require_field(map, what, "kind", error);
	if (!kind)
		return false;
	const char* kind_name = AeronConfigNode_String(kind, NULL);
	if (kind_name && strcmp(kind_name, "full") == 0) {
		def->kind = AERON_ENV_KIND_FULL;
		if (!check_fields(map, what, full_fields, sizeof full_fields / sizeof full_fields[0], error))
			return false;
	} else if (kind_name && strcmp(kind_name, "lighting") == 0) {
		def->kind = AERON_ENV_KIND_LIGHTING;
		if (!check_fields(map, what, lighting_fields, sizeof lighting_fields / sizeof lighting_fields[0],
						  error))
			return false;
	} else if (kind_name && strcmp(kind_name, "probes") == 0) {
		def->kind = AERON_ENV_KIND_PROBES;
		return check_fields(map, what, probes_fields, sizeof probes_fields / sizeof probes_fields[0],
							error) &&
			   read_number(map, what, "intensity", true, 0.0, 16.0, &def->intensity, error) &&
			   read_number(map, what, "diffuse_strength", true, 0.0, 4.0, &def->diffuse_strength, error) &&
			   read_number(map, what, "specular_strength", true, 0.0, 4.0, &def->specular_strength, error) &&
			   read_path(map, what, "cubes", def->cubes_path, sizeof def->cubes_path, error) &&
			   read_path(map, what, "grid", def->grid_path, sizeof def->grid_path, error) &&
			   AeronEnvironment_ParseProbeSet(map, what, def, error);
	} else {
		return env_error(error, kind, "%s: 'kind' must be 'full', 'lighting' or 'probes'", what);
	}

	if (!read_number(map, what, "intensity", true, 0.0, 16.0, &def->intensity, error) ||
		!read_path(map, what, "diffuse", def->diffuse_path, sizeof def->diffuse_path, error) ||
		!read_number(map, what, "diffuse_strength", true, 0.0, 4.0, &def->diffuse_strength, error) ||
		!read_ambient_fill(map, what, def->ambient_fill, error) ||
		!read_vec3(map, what, "rotation_degrees", def->rotation_degrees, NULL, error))
		return false;
	for (int i = 0; i < 3; ++i)
		def->rotation_degrees[i] = AeronEnvironment_NormalizeDegrees(def->rotation_degrees[i]);

	if (def->kind == AERON_ENV_KIND_LIGHTING) {
		/* Reflections are optional: both fields or neither. */
		const bool has_specular = AeronConfigNode_MapGet(map, "specular") != NULL;
		if (has_specular != (AeronConfigNode_MapGet(map, "specular_strength") != NULL))
			return env_error(error, map, "%s: give both 'specular' and 'specular_strength', or neither", what);
		return !has_specular ||
			   (read_path(map, what, "specular", def->specular_path, sizeof def->specular_path, error) &&
				read_number(map, what, "specular_strength", true, 0.0, 4.0, &def->specular_strength, error));
	}
	return read_path(map, what, "radiance", def->radiance_path, sizeof def->radiance_path, error) &&
		   read_path(map, what, "specular", def->specular_path, sizeof def->specular_path, error) &&
		   read_number(map, what, "specular_strength", true, 0.0, 4.0, &def->specular_strength, error) &&
		   parse_lights(map, def, error);
}

bool AeronEnvironment_ParsePackages(const AeronConfigNode* packages, AeronEnvironmentDefinitionSet** out_set,
									AeronConfigError* error) {
	if (!out_set)
		return env_error(error, NULL, "no output set");
	*out_set = NULL;
	if (!packages)
		return env_error(error, NULL, "missing 'packages' map");
	if (AeronConfigNode_Type(packages) != AERON_CONFIG_MAP)
		return env_error(error, packages, "'packages' must be a map");
	const size_t count = AeronConfigNode_MapCount(packages);
	if (count > AERON_ENV_MAX_PACKAGES)
		return env_error(error, packages, "at most %d packages are allowed", AERON_ENV_MAX_PACKAGES);

	AeronEnvironmentDefinitionSet* set = calloc(1, sizeof *set);
	if (!set)
		return env_error(error, packages, "out of memory");
	if (count > 0) {
		set->definitions = calloc(count, sizeof *set->definitions);
		if (!set->definitions) {
			free(set);
			return env_error(error, packages, "out of memory");
		}
	}

	for (size_t i = 0; i < count; ++i) {
		const char*            name = AeronConfigNode_MapKeyAt(packages, i);
		const AeronConfigNode* node = AeronConfigNode_MapValueAt(packages, i);
		bool                   ok   = true;
		if (!AeronEnvironment_IsValidName(name)) {
			ok = env_error(
				error, node,
				"package name '%s' must be 1 to 63 characters of lower-case letters, digits, '.' and '_'",
				name ? name : "");
		} else if (AeronEnvironmentDefinitionSet_Find(set, name)) {
			ok = env_error(error, node, "duplicate package name '%s'", name);
		} else {
			ok = parse_package(name, node, &set->definitions[i], error);
		}
		if (!ok) {
			AeronEnvironmentDefinitionSet_Destroy(set);
			return false;
		}
		set->count = (uint32_t)i + 1;
	}
	*out_set = set;
	return true;
}

void AeronEnvironmentDefinitionSet_Destroy(AeronEnvironmentDefinitionSet* set) {
	if (!set)
		return;
	free(set->definitions);
	free(set);
}

uint32_t AeronEnvironmentDefinitionSet_Count(const AeronEnvironmentDefinitionSet* set) {
	return set ? set->count : 0;
}

const AeronEnvironmentDefinition* AeronEnvironmentDefinitionSet_Get(const AeronEnvironmentDefinitionSet* set,
																	uint32_t index) {
	return set && index < set->count ? &set->definitions[index] : NULL;
}

const AeronEnvironmentDefinition* AeronEnvironmentDefinitionSet_Find(const AeronEnvironmentDefinitionSet* set,
																	 const char* name) {
	if (!set || !name)
		return NULL;
	for (uint32_t i = 0; i < set->count; ++i)
		if (strcmp(set->definitions[i].name, name) == 0)
			return &set->definitions[i];
	return NULL;
}

/* ---- YAML output ------------------------------------------------------ */

typedef struct YamlOut {
	char*  out;
	size_t size;
	size_t length;
} YamlOut;

static void yaml_printf(YamlOut* y, const char* format, ...) {
	va_list args;
	va_start(args, format);
	const size_t remaining = y->length < y->size ? y->size - y->length : 0;
	const int    written   = vsnprintf(remaining ? y->out + y->length : NULL, remaining, format, args);
	va_end(args);
	if (written > 0)
		y->length += (size_t)written;
}

static void yaml_string(YamlOut* y, const char* text) {
	yaml_printf(y, "\"");
	for (const char* p = text; *p; ++p)
		yaml_printf(y, (*p == '"' || *p == '\\') ? "\\%c" : "%c", *p);
	yaml_printf(y, "\"");
}

static void yaml_vec3(YamlOut* y, const float v[3]) {
	yaml_printf(y, "[%.9g, %.9g, %.9g]", (double)v[0], (double)v[1], (double)v[2]);
}

static const char* yaml_shape(AeronEnvironmentProbeShape shape) {
	return shape == AERON_ENV_PROBE_ELLIPSOID ? "ellipsoid" : "box";
}

/* The rest of a probes package, after its kind and intensity. */
static void format_probes(YamlOut* y, const AeronEnvironmentDefinition* d) {
	yaml_printf(y, "    diffuse_strength: %.9g\n    specular_strength: %.9g\n    cubes: ",
				(double)d->diffuse_strength, (double)d->specular_strength);
	yaml_string(y, d->cubes_path);
	yaml_printf(y, "\n    grid: ");
	yaml_string(y, d->grid_path);
	yaml_printf(y, "\n    grid_box: {center: ");
	yaml_vec3(y, d->grid_center);
	yaml_printf(y, ", size: ");
	yaml_vec3(y, d->grid_size);
	yaml_printf(y, "}\n    grid_resolution: [%u, %u, %u]\n    probes:\n", d->grid_resolution[0],
				d->grid_resolution[1], d->grid_resolution[2]);
	for (uint32_t i = 0; i < d->probe_count; ++i) {
		const AeronEnvironmentProbeDefinition* p = &d->probes[i];
		yaml_printf(y, "      - {center: ");
		yaml_vec3(y, p->center);
		yaml_printf(y, ", size: ");
		yaml_vec3(y, p->size);
		yaml_printf(y, ", shape: %s, falloff: %.9g,\n         ", yaml_shape(p->shape), (double)p->falloff);
		if (memcmp(p->parallax_center, p->center, sizeof p->center) != 0) {
			yaml_printf(y, "parallax_center: ");
			yaml_vec3(y, p->parallax_center);
			yaml_printf(y, ", ");
		}
		yaml_printf(y, "parallax_size: ");
		yaml_vec3(y, p->parallax_size);
		yaml_printf(y, ", parallax_shape: %s}\n", yaml_shape(p->parallax_shape));
	}
}

size_t AeronEnvironment_FormatPackageYaml(const AeronEnvironmentDefinition* d, char* out, size_t size) {
	YamlOut y = { out, out ? size : 0, 0 };
	if (out && size)
		out[0] = '\0';
	if (!d)
		return 0;
	static const char* const kinds[] = { "full", "lighting", "probes" };
	yaml_printf(&y, "packages:\n  %s:\n    kind: %s\n    intensity: %.9g\n", d->name, kinds[d->kind],
				(double)d->intensity);
	if (d->kind == AERON_ENV_KIND_PROBES) {
		format_probes(&y, d);
		return y.length;
	}
	if (d->kind == AERON_ENV_KIND_FULL) {
		yaml_printf(&y, "    radiance: ");
		yaml_string(&y, d->radiance_path);
		yaml_printf(&y, "\n");
	}
	yaml_printf(&y, "    diffuse: ");
	yaml_string(&y, d->diffuse_path);
	yaml_printf(&y, "\n");
	if (d->specular_path[0]) {
		yaml_printf(&y, "    specular: ");
		yaml_string(&y, d->specular_path);
		yaml_printf(&y, "\n");
	}
	yaml_printf(&y, "    diffuse_strength: %.9g\n", (double)d->diffuse_strength);
	if (d->specular_path[0])
		yaml_printf(&y, "    specular_strength: %.9g\n", (double)d->specular_strength);
	yaml_printf(&y, "    ambient_fill: ");
	yaml_vec3(&y, d->ambient_fill);
	yaml_printf(&y, "\n    rotation_degrees: ");
	yaml_vec3(&y, d->rotation_degrees);
	yaml_printf(&y, "\n");
	if (d->kind != AERON_ENV_KIND_FULL)
		return y.length;
	if (d->light_count == 0) {
		yaml_printf(&y, "    directional_lights: []\n");
		return y.length;
	}
	yaml_printf(&y, "    directional_lights:\n");
	for (uint32_t i = 0; i < d->light_count; ++i) {
		const AeronEnvironmentLightDefinition* l = &d->lights[i];
		yaml_printf(&y, "      - name: %s\n        direction_cube: ", l->name);
		yaml_vec3(&y, l->direction_cube);
		yaml_printf(&y, "\n        color_linear: ");
		yaml_vec3(&y, l->color_linear);
		yaml_printf(&y,
					"\n        intensity: %.9g\n        primary: %s\n        casts_shadows: %s\n"
					"        angular_radius_degrees: %.9g\n        lens_flare: %s\n        visual:\n",
					(double)l->intensity, l->primary ? "true" : "false", l->casts_shadows ? "true" : "false",
					(double)l->angular_radius_degrees, l->lens_flare ? "true" : "false");
		if (l->visual == AERON_ENV_SUN_DISC) {
			yaml_printf(
				&y,
				"          kind: disc\n          radiance_scale: %.9g\n          glow_radius_degrees: %.9g\n"
				"          glow_strength: %.9g\n",
				(double)l->radiance_scale, (double)l->glow_radius_degrees, (double)l->glow_strength);
		} else {
			yaml_printf(&y, "          kind: sprite\n          sprite: ");
			yaml_string(&y, l->sprite_path);
			yaml_printf(&y, "\n          angular_size_degrees: %.9g\n          sprite_strength: %.9g\n",
						(double)l->angular_size_degrees, (double)l->sprite_strength);
		}
	}
	return y.length;
}

/* ---- probe cells ------------------------------------------------------ */

static float probe_volume(const AeronEnvironmentProbeDefinition* p) {
	const float box = p->size[0] * p->size[1] * p->size[2];
	return p->shape == AERON_ENV_PROBE_ELLIPSOID ? box * 0.5235988f : box; /* pi / 6 */
}

bool AeronEnvironment_BuildProbeCells(const AeronEnvironmentDefinition* d, AeronEnvironmentProbeCells* out) {
	if (!out)
		return false;
	memset(out, 0, sizeof *out);
	if (!d || d->kind != AERON_ENV_KIND_PROBES || d->probe_count == 0 ||
		d->probe_count > AERON_ENV_MAX_PROBES)
		return false;

	/* Influence boxes, their union, and the probes smallest first (ties
	 * by index). */
	float    lo[AERON_ENV_MAX_PROBES][3], hi[AERON_ENV_MAX_PROBES][3], max[3];
	uint32_t order[AERON_ENV_MAX_PROBES];
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
	out->cell_size      = fmaxf(AERON_ENV_PROBE_CELL_MIN_SIZE, largest / AERON_ENV_PROBE_CELL_MAX_AXIS);
	size_t cell_count   = 1;
	for (int a = 0; a < 3; ++a) {
		const float cells = ceilf((max[a] - out->min[a]) / out->cell_size);
		out->dims[a]      = cells < 1.0f                            ? 1u
							: cells > AERON_ENV_PROBE_CELL_MAX_AXIS ? AERON_ENV_PROBE_CELL_MAX_AXIS
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
					if (count < AERON_ENV_PROBE_CELL_PROBES)
						word |= (i + 1u) << (8u * count);
					++count;
				}
				out->words[((size_t)z * out->dims[1] + y) * out->dims[0] + x] = word;
				if (count > out->most_probes)
					out->most_probes = count;
				if (count > AERON_ENV_PROBE_CELL_PROBES)
					++out->overflow_cells;
			}
	return true;
}

void AeronEnvironment_FreeProbeCells(AeronEnvironmentProbeCells* cells) {
	if (!cells)
		return;
	free(cells->words);
	memset(cells, 0, sizeof *cells);
}
