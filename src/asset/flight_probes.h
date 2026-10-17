#ifndef AERON_ASSET_FLIGHT_PROBES_H
#define AERON_ASSET_FLIGHT_PROBES_H

/* Functions from flight_probes.c used by flight_model.c. */

#include "aeron/asset/flight_model.h"

struct cJSON;
struct cgltf_data;

/* Reads the optional `probeData` of the model root extension into a new
 * AeronFlightProbes. Sets *out to NULL when there is no `probeData`.
 * Returns false when it is malformed or its images do not match its
 * layout. */
bool Aeron_FlightProbesFromExtension(const struct cgltf_data* data, const struct cJSON* extension,
									 AeronFlightProbes** out, char* error, size_t error_size);

#endif
