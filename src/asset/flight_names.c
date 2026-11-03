#include "aeron/asset/flight_names.h"

#include <stddef.h>
#include <string.h>

#include "cJSON.h"

static const char* const kHardpointTypes[AERON_FLIGHT_HARDPOINT_TYPE_COUNT] = {
	"none",
	"rebel_laser",
	"turbo_rebel_laser",
	"empire_laser",
	"turbo_empire_laser",
	"ion_cannon",
	"turbo_ion_cannon",
	"torpedo",
	"missile",
	"super_rebel_laser",
	"super_empire_laser",
	"super_ion_cannon",
	"super_torpedo",
	"super_missile",
	"dumb_bomb",
	"fired_bomb",
	"mag_pulse",
	"turbo_mag_pulse",
	"super_mag_pulse",
	"gunner",
	"cockpit_sparks",
	"docking_point",
	"towing",
	"acceleration_start",
	"acceleration_end",
	"inside_hangar",
	"outside_hangar",
	"dock_from_big",
	"dock_from_small",
	"dock_to_big",
	"dock_to_small",
	"pilot_eye",
	"engine_glow",
	"custom_1",
	"custom_2",
	"custom_3",
	"custom_4",
	"custom_5",
	"custom_6",
	"jamming_point",
};

static const char* const kMeshTypes[AERON_FLIGHT_MESH_TYPE_COUNT] = {
	"default",
	"main_hull",
	"wing",
	"fuselage",
	"gun_turret",
	"small_gun",
	"engine",
	"bridge",
	"shield_generator",
	"energy_generator",
	"launcher",
	"communication_system",
	"beam_system",
	"command_system",
	"docking_platform",
	"landing_platform",
	"hangar",
	"cargo_pod",
	"misc_hull",
	"antenna",
	"rotary_wing",
	"rotary_gun_turret",
	"rotary_launcher",
	"rotary_communication_system",
	"rotary_beam_system",
	"rotary_command_system",
	"hatch",
	"custom",
	"weapon_system_1",
	"weapon_system_2",
	"power_regenerator",
	"reactor",
};

static const char* const kExplosionFlags[AERON_FLIGHT_EXPLOSION_FLAG_COUNT] = { "hit_points", "detachable" };

static const char* const kScreenLayers[AERON_FLIGHT_SCREEN_LAYER_COUNT] = { "graphics", "text", "bar" };

static const struct {
	const char* const* names;
	uint32_t           count;
} kLists[AERON_FLIGHT_NAME_LISTS] = {
	{ kHardpointTypes, AERON_FLIGHT_HARDPOINT_TYPE_COUNT },
	{ kMeshTypes, AERON_FLIGHT_MESH_TYPE_COUNT },
	{ kExplosionFlags, AERON_FLIGHT_EXPLOSION_FLAG_COUNT },
	{ kScreenLayers, AERON_FLIGHT_SCREEN_LAYER_COUNT },
};

uint32_t Aeron_FlightNameCount(AeronFlightNameList list) {
	return (unsigned)list < AERON_FLIGHT_NAME_LISTS ? kLists[list].count : 0;
}

const char* Aeron_FlightName(AeronFlightNameList list, uint32_t value) {
	return value < Aeron_FlightNameCount(list) ? kLists[list].names[value] : NULL;
}

int Aeron_FlightNameFind(AeronFlightNameList list, const char* text) {
	if (!text)
		return -1;
	for (uint32_t value = 0; value < Aeron_FlightNameCount(list); ++value)
		if (strcmp(kLists[list].names[value], text) == 0)
			return (int)value;
	return -1;
}

bool Aeron_FlightNameMask(AeronFlightNameList list, const cJSON* names, uint32_t* out) {
	const cJSON* name;
	*out = 0;
	if (!cJSON_IsArray(names))
		return false;
	cJSON_ArrayForEach(name, names) {
		const int bit = cJSON_IsString(name) ? Aeron_FlightNameFind(list, name->valuestring) : -1;
		if (bit < 0)
			return false;
		*out |= 1u << bit;
	}
	return true;
}
