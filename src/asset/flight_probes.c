/*
 * A flight model's baked environment probes: the layout's JSON form (the
 * model root's `probeData`, the probes bake's bake.json and its job) and the
 * checks that its two KTX2 images hold what it says.
 */

#include "flight_probes.h"

#include "cJSON.h"
#include "cgltf.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool probes_error(char* error, size_t error_size, const char* format, ...) {
	if (error && error_size) {
		va_list args;
		va_start(args, format);
		vsnprintf(error, error_size, format, args);
		va_end(args);
	}
	return false;
}

static bool json_finite(const cJSON* value, double min, double max, float* out) {
	if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < min ||
		value->valuedouble > max)
		return false;
	*out = (float)value->valuedouble;
	return true;
}

/* An optional number in [min, max]; `fallback` when absent. */
static bool json_optional(const cJSON* object, const char* field, double min, double max, float fallback,
						  float* out) {
	const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, field);
	*out               = fallback;
	return !value || json_finite(value, min, max, out);
}

static bool json_floats(const cJSON* value, float* out, int count, bool positive) {
	if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) != count)
		return false;
	for (int index = 0; index < count; ++index)
		if (!json_finite(cJSON_GetArrayItem(value, index), -FLT_MAX, FLT_MAX, &out[index]) ||
			(positive && !(out[index] > 0.0f)))
			return false;
	return true;
}

static bool json_shape(const cJSON* value, AeronFlightProbeShape* out) {
	if (!cJSON_IsString(value) || !value->valuestring)
		return false;
	if (strcmp(value->valuestring, "box") == 0)
		*out = AERON_FLIGHT_PROBE_BOX;
	else if (strcmp(value->valuestring, "ellipsoid") == 0)
		*out = AERON_FLIGHT_PROBE_ELLIPSOID;
	else
		return false;
	return true;
}

static bool parse_probe(const cJSON* item, uint32_t index, AeronFlightProbe* probe, char* error,
						size_t error_size) {
	const cJSON* parallax_center = cJSON_GetObjectItemCaseSensitive(item, "parallaxCenter");
	if (!cJSON_IsObject(item) ||
		!json_floats(cJSON_GetObjectItemCaseSensitive(item, "center"), probe->center, 3, false) ||
		!json_floats(cJSON_GetObjectItemCaseSensitive(item, "size"), probe->size, 3, true) ||
		!json_shape(cJSON_GetObjectItemCaseSensitive(item, "shape"), &probe->shape) ||
		!json_finite(cJSON_GetObjectItemCaseSensitive(item, "falloff"), 0.0, 1.0, &probe->falloff) ||
		!(probe->falloff > 0.0f) ||
		!json_floats(cJSON_GetObjectItemCaseSensitive(item, "parallaxSize"), probe->parallax_size, 3, true) ||
		!json_shape(cJSON_GetObjectItemCaseSensitive(item, "parallaxShape"), &probe->parallax_shape))
		return probes_error(
			error, error_size,
			"probe %u needs center, size (above 0), shape (box or ellipsoid), falloff (above 0, "
			"at most 1), parallaxSize and parallaxShape",
			index);
	memcpy(probe->parallax_center, probe->center, sizeof probe->parallax_center);
	if (parallax_center && !json_floats(parallax_center, probe->parallax_center, 3, false))
		return probes_error(error, error_size, "probe %u: parallaxCenter must be three numbers", index);
	return true;
}

static bool parse_layout(const cJSON* object, AeronFlightProbeLayout* out, char* error, size_t error_size) {
	memset(out, 0, sizeof *out);
	if (!cJSON_IsObject(object))
		return probes_error(error, error_size, "the probe layout must be a JSON object");
	if (!json_optional(object, "intensity", 0.0, 16.0, 1.0f, &out->intensity) ||
		!json_optional(object, "diffuseStrength", 0.0, 4.0, 1.0f, &out->diffuse_strength) ||
		!json_optional(object, "specularStrength", 0.0, 4.0, 1.0f, &out->specular_strength) ||
		!json_optional(object, "keyLightShadow", 0.0, 1.0, AERON_FLIGHT_KEY_LIGHT_SHADOW,
					   &out->key_light_shadow))
		return probes_error(error, error_size,
							"intensity (0 to 16), diffuseStrength and specularStrength (0 to 4) and "
							"keyLightShadow (0 to 1) are optional numbers");

	const cJSON* box = cJSON_GetObjectItemCaseSensitive(object, "gridBox");
	if (!cJSON_IsObject(box) ||
		!json_floats(cJSON_GetObjectItemCaseSensitive(box, "center"), out->grid_center, 3, false) ||
		!json_floats(cJSON_GetObjectItemCaseSensitive(box, "size"), out->grid_size, 3, true))
		return probes_error(error, error_size, "gridBox needs center and size (above 0)");
	const cJSON* resolution = cJSON_GetObjectItemCaseSensitive(object, "gridResolution");
	if (!cJSON_IsArray(resolution) || cJSON_GetArraySize(resolution) != 3)
		return probes_error(error, error_size, "gridResolution must be three integers");
	for (int axis = 0; axis < 3; ++axis) {
		const cJSON* value = cJSON_GetArrayItem(resolution, axis);
		if (!cJSON_IsNumber(value) || trunc(value->valuedouble) != value->valuedouble ||
			value->valuedouble < AERON_FLIGHT_PROBE_GRID_MIN ||
			value->valuedouble > AERON_FLIGHT_PROBE_GRID_MAX)
			return probes_error(error, error_size, "gridResolution values must be integers from %d to %d",
								AERON_FLIGHT_PROBE_GRID_MIN, AERON_FLIGHT_PROBE_GRID_MAX);
		out->grid_resolution[axis] = (uint32_t)value->valuedouble;
	}

	const cJSON* probes = cJSON_GetObjectItemCaseSensitive(object, "probes");
	const int    count  = cJSON_GetArraySize(probes);
	if (!cJSON_IsArray(probes) || count < 1 || count > AERON_FLIGHT_MAX_PROBES)
		return probes_error(error, error_size, "probes must hold 1 to %d probes", AERON_FLIGHT_MAX_PROBES);
	for (int index = 0; index < count; ++index)
		if (!parse_probe(cJSON_GetArrayItem(probes, index), (uint32_t)index, &out->probes[index], error,
						 error_size))
			return false;
	out->probe_count = (uint32_t)count;

	const cJSON* plane        = cJSON_GetObjectItemCaseSensitive(object, "reflectionPlane");
	out->has_reflection_plane = plane != NULL;
	if (plane &&
		(!cJSON_IsObject(plane) ||
		 !json_floats(cJSON_GetObjectItemCaseSensitive(plane, "center"), out->reflection_plane_center, 3,
					  false) ||
		 !json_floats(cJSON_GetObjectItemCaseSensitive(plane, "size"), out->reflection_plane_size, 2, true) ||
		 !json_finite(cJSON_GetObjectItemCaseSensitive(plane, "distance"), 0.0, 10.0,
					  &out->reflection_plane_distance)))
		return probes_error(error, error_size,
							"reflectionPlane needs center, size (two numbers above 0) and "
							"distance (0 to 10)");
	return true;
}

bool Aeron_FlightProbeLayoutParse(const char* json, size_t size, AeronFlightProbeLayout* out, char* error,
								  size_t error_size) {
	if (!json || !out)
		return probes_error(error, error_size, "no probe layout");
	cJSON*     object = cJSON_ParseWithLength(json, size);
	const bool ok     = object ? parse_layout(object, out, error, error_size)
							   : probes_error(error, error_size, "the probe layout is not valid JSON");
	cJSON_Delete(object);
	return ok;
}

typedef struct JsonOut {
	char*  out;
	size_t size;
	size_t length;
} JsonOut;

static void json_printf(JsonOut* j, const char* format, ...) {
	va_list args;
	va_start(args, format);
	const size_t remaining = j->length < j->size ? j->size - j->length : 0;
	const int    written   = vsnprintf(remaining ? j->out + j->length : NULL, remaining, format, args);
	va_end(args);
	if (written > 0)
		j->length += (size_t)written;
}

/* The shortest decimal that reads back as `value`: 0.7 for 0.7f, not
 * 0.699999988; with an exponent only when the value needs one (120, not
 * 1.2e+02). */
static void json_float(JsonOut* j, float value) {
	char full[32], text[32];
	snprintf(full, sizeof full, "%.9g", (double)value);
	const bool exponent = strchr(full, 'e') != NULL;
	snprintf(text, sizeof text, "%s", full);
	for (int digits = 1; digits < 9; ++digits) {
		char shorter[32];
		snprintf(shorter, sizeof shorter, "%.*g", digits, (double)value);
		if (strtof(shorter, NULL) == value && (exponent || !strchr(shorter, 'e'))) {
			snprintf(text, sizeof text, "%s", shorter);
			break;
		}
	}
	json_printf(j, "%s", text);
}

/* Floats from `values`, comma-separated in brackets. */
static void json_floats_out(JsonOut* j, const float* values, int count) {
	json_printf(j, "[");
	for (int i = 0; i < count; ++i) {
		json_printf(j, i ? ", " : "");
		json_float(j, values[i]);
	}
	json_printf(j, "]");
}

/* `"name": value` and a separator. */
static void json_field(JsonOut* j, const char* name, float value, const char* after) {
	json_printf(j, "\"%s\": ", name);
	json_float(j, value);
	json_printf(j, "%s", after);
}

static const char* shape_name(AeronFlightProbeShape shape) {
	return shape == AERON_FLIGHT_PROBE_ELLIPSOID ? "ellipsoid" : "box";
}

size_t Aeron_FlightProbeLayoutFormat(const AeronFlightProbeLayout* l, char* out, size_t size) {
	JsonOut j = { out, out ? size : 0, 0 };
	if (out && size)
		out[0] = '\0';
	if (!l)
		return 0;
	json_printf(&j, "{\n  ");
	json_field(&j, "intensity", l->intensity, ",\n  ");
	json_field(&j, "diffuseStrength", l->diffuse_strength, ",\n  ");
	json_field(&j, "specularStrength", l->specular_strength, ",\n  ");
	json_printf(&j, "\"gridBox\": {\"center\": ");
	json_floats_out(&j, l->grid_center, 3);
	json_printf(&j, ", \"size\": ");
	json_floats_out(&j, l->grid_size, 3);
	json_printf(&j, "},\n  \"gridResolution\": [%u, %u, %u],\n  ", l->grid_resolution[0],
				l->grid_resolution[1], l->grid_resolution[2]);
	json_field(&j, "keyLightShadow", l->key_light_shadow, ",\n  \"probes\": [\n");
	for (uint32_t k = 0; k < l->probe_count; ++k) {
		const AeronFlightProbe* p = &l->probes[k];
		json_printf(&j, "    {\"center\": ");
		json_floats_out(&j, p->center, 3);
		json_printf(&j, ", \"size\": ");
		json_floats_out(&j, p->size, 3);
		json_printf(&j, ", \"shape\": \"%s\", ", shape_name(p->shape));
		json_field(&j, "falloff", p->falloff, ",\n     \"parallaxCenter\": ");
		json_floats_out(&j, p->parallax_center, 3);
		json_printf(&j, ", \"parallaxSize\": ");
		json_floats_out(&j, p->parallax_size, 3);
		json_printf(&j, ", \"parallaxShape\": \"%s\"}%s\n", shape_name(p->parallax_shape),
					k + 1 < l->probe_count ? "," : "");
	}
	json_printf(&j, "  ]");
	if (l->has_reflection_plane) {
		json_printf(&j, ",\n  \"reflectionPlane\": {\"center\": ");
		json_floats_out(&j, l->reflection_plane_center, 3);
		json_printf(&j, ", \"size\": ");
		json_floats_out(&j, l->reflection_plane_size, 2);
		json_printf(&j, ", ");
		json_field(&j, "distance", l->reflection_plane_distance, "}");
	}
	json_printf(&j, "\n}\n");
	return j.length;
}

/* ---- KTX2 headers ---------------------------------------------------------- */

enum {
	KTX2_HEADER_SIZE      = 48,
	VK_FORMAT_RGBA16F     = 97,
	VK_FORMAT_BC6H_UFLOAT = 143,
	VK_FORMAT_BC6H_SFLOAT = 144,
};

typedef struct Ktx2Header {
	uint32_t format, width, height, depth, layers, faces, levels;
} Ktx2Header;

static uint32_t read_u32(const uint8_t* bytes) {
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static bool ktx2_header(const uint8_t* bytes, size_t size, Ktx2Header* out) {
	static const uint8_t identifier[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
											0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
	if (!bytes || size < KTX2_HEADER_SIZE || memcmp(bytes, identifier, sizeof identifier) != 0)
		return false;
	out->format = read_u32(bytes + 12);
	out->width  = read_u32(bytes + 20);
	out->height = read_u32(bytes + 24);
	out->depth  = read_u32(bytes + 28);
	out->layers = read_u32(bytes + 32);
	out->faces  = read_u32(bytes + 36);
	out->levels = read_u32(bytes + 40);
	return true;
}

/* The cube array holds the layout's probes, square HDR faces; the grid one
 * RGBA16F level of 6 rx by ry rz texels. */
static bool check_images(const AeronFlightProbeLayout* l, const uint8_t* cubes, size_t cubes_size,
						 const uint8_t* grid, size_t grid_size, char* error, size_t error_size) {
	Ktx2Header c, g;
	if (!ktx2_header(cubes, cubes_size, &c) || c.faces != 6 || c.layers != l->probe_count ||
		c.width != c.height || c.depth != 0 ||
		(c.format != VK_FORMAT_BC6H_UFLOAT && c.format != VK_FORMAT_BC6H_SFLOAT &&
		 c.format != VK_FORMAT_RGBA16F))
		return probes_error(error, error_size,
							"the probe cubes must be a KTX2 array of %u square BC6H or RGBA16F cube maps",
							l->probe_count);
	const uint32_t* r = l->grid_resolution;
	if (!ktx2_header(grid, grid_size, &g) || g.format != VK_FORMAT_RGBA16F || g.faces != 1 || g.layers != 0 ||
		g.depth != 0 || g.levels != 1 || g.width != 6u * r[0] || g.height != r[1] * r[2])
		return probes_error(
			error, error_size,
			"the probe grid must be a KTX2 image of one RGBA16F level of %ux%u for a %ux%ux%u grid",
			6u * r[0], r[1] * r[2], r[0], r[1], r[2]);
	return true;
}

static bool copy_images(AeronFlightProbes* out, const uint8_t* cubes, size_t cubes_size, const uint8_t* grid,
						size_t grid_size) {
	out->cubes = malloc(cubes_size);
	out->grid  = malloc(grid_size);
	if (!out->cubes || !out->grid)
		return false;
	memcpy(out->cubes, cubes, cubes_size);
	memcpy(out->grid, grid, grid_size);
	out->cubes_size = cubes_size;
	out->grid_size  = grid_size;
	return true;
}

bool Aeron_FlightProbesBuild(const char* layout_json, size_t layout_size, const uint8_t* cubes,
							 size_t cubes_size, const uint8_t* grid, size_t grid_size, AeronFlightProbes* out,
							 char* error, size_t error_size) {
	if (!out)
		return probes_error(error, error_size, "no output");
	memset(out, 0, sizeof *out);
	if (!Aeron_FlightProbeLayoutParse(layout_json, layout_size, &out->layout, error, error_size) ||
		!check_images(&out->layout, cubes, cubes_size, grid, grid_size, error, error_size))
		return false;
	if (!copy_images(out, cubes, cubes_size, grid, grid_size)) {
		Aeron_FlightProbesFree(out);
		return probes_error(error, error_size, "out of memory");
	}
	return true;
}

void Aeron_FlightProbesFree(AeronFlightProbes* probes) {
	if (!probes)
		return;
	free(probes->cubes);
	free(probes->grid);
	memset(probes, 0, sizeof *probes);
}

/* ---- probeData --------------------------------------------------------------- */

/* A buffer view's bytes, or NULL when it is out of its buffer. */
static const uint8_t* view_bytes(const cgltf_data* data, const cJSON* index, size_t* size) {
	if (!cJSON_IsNumber(index) || index->valuedouble < 0 || trunc(index->valuedouble) != index->valuedouble ||
		index->valuedouble >= (double)data->buffer_views_count)
		return NULL;
	const cgltf_buffer_view* view = &data->buffer_views[(size_t)index->valuedouble];
	if (!view->buffer || !view->buffer->data || view->size == 0 || view->offset > view->buffer->size ||
		view->size > view->buffer->size - view->offset)
		return NULL;
	*size = view->size;
	return (const uint8_t*)view->buffer->data + view->offset;
}

bool Aeron_FlightProbesFromExtension(const cgltf_data* data, const cJSON* extension, AeronFlightProbes** out,
									 char* error, size_t error_size) {
	*out                    = NULL;
	const cJSON* probe_data = cJSON_GetObjectItemCaseSensitive(extension, "probeData");
	const cJSON* version    = cJSON_GetObjectItemCaseSensitive(probe_data, "version");
	size_t       cubes_size = 0;
	size_t       grid_size  = 0;
	if (!probe_data)
		return true;
	if (!cJSON_IsObject(probe_data) || !cJSON_IsNumber(version) || version->valuedouble != 1.0)
		return probes_error(error, error_size, "probeData must be an object of version 1");
	const uint8_t* cubes =
		view_bytes(data, cJSON_GetObjectItemCaseSensitive(probe_data, "cubes"), &cubes_size);
	const uint8_t* grid = view_bytes(data, cJSON_GetObjectItemCaseSensitive(probe_data, "grid"), &grid_size);
	if (!cubes || !grid)
		return probes_error(error, error_size, "probeData's cubes and grid must be buffer views");
	AeronFlightProbes* probes = calloc(1, sizeof *probes);
	if (!probes)
		return probes_error(error, error_size, "out of memory");
	if (!parse_layout(probe_data, &probes->layout, error, error_size) ||
		!check_images(&probes->layout, cubes, cubes_size, grid, grid_size, error, error_size) ||
		!copy_images(probes, cubes, cubes_size, grid, grid_size)) {
		Aeron_FlightProbesFree(probes);
		free(probes);
		return false;
	}
	*out = probes;
	return true;
}
