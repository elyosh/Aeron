#ifndef AERON_ASSET_FLIGHT_PROBES_H
#define AERON_ASSET_FLIGHT_PROBES_H

/* flight_probes.c for flight_model.c. */

#include "aeron/asset/flight_model.h"

struct cJSON;
struct cgltf_data;

/* Reads the model root extension's optional `probeData` into a new
 * AeronFlightProbes (*out stays NULL without it). False when it is
 * malformed or its images do not match its layout. */
bool Aeron_FlightProbesFromExtension(const struct cgltf_data* data, const struct cJSON* extension,
									 AeronFlightProbes** out, char* error, size_t error_size);

#endif
