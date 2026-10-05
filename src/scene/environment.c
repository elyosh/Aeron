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
		"sky_exposure",
		"diffuse_strength",
		"specular_strength",
		"ambient_floor_scale",
		"rotation_degrees",
		"directional_lights",
	};
	static const char* const lighting_fields[] = {
		"kind", "diffuse", "diffuse_strength", "ambient_floor_scale", "rotation_degrees",
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
	} else {
		return env_error(error, kind, "%s: 'kind' must be 'full' or 'lighting'", what);
	}

	if (!read_path(map, what, "diffuse", def->diffuse_path, sizeof def->diffuse_path, error) ||
		!read_number(map, what, "diffuse_strength", true, 0.0, 4.0, &def->diffuse_strength, error) ||
		!read_number(map, what, "ambient_floor_scale", true, 0.0, 1.0, &def->ambient_floor_scale, error) ||
		!read_vec3(map, what, "rotation_degrees", def->rotation_degrees, NULL, error))
		return false;
	for (int i = 0; i < 3; ++i)
		def->rotation_degrees[i] = AeronEnvironment_NormalizeDegrees(def->rotation_degrees[i]);

	if (def->kind == AERON_ENV_KIND_LIGHTING)
		return true;
	return read_path(map, what, "radiance", def->radiance_path, sizeof def->radiance_path, error) &&
		   read_path(map, what, "specular", def->specular_path, sizeof def->specular_path, error) &&
		   read_number(map, what, "sky_exposure", true, 0.0, 16.0, &def->sky_exposure, error) &&
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
