#ifndef AERON_OPT2GLTF_XWAU_MODEL_H
#define AERON_OPT2GLTF_XWAU_MODEL_H

/*
 * What XWAU's hooks read for one model from its files beside the OPT, as
 * the converter's options. Each section is read from
 * FlightModels/<Model><Section>.txt, else from [<Section>] of
 * FlightModels/<Model>.ini, with the C++ rules (xwau_config.h):
 *
 *   - the pilot eye (hook_mission_objects): CockpitPov_Default, else
 *     CockpitPov, when it defines all of CockpitPovX, Y and Z (read as
 *     std::stoi(value, 0, 0), stored as 16 bits); else the eye XWAU's craft
 *     table gives a craft of that name (model_tables.h). Missions select
 *     other POV profiles; the model is converted with the Default one.
 *
 *   - object profiles (hook_mission_objects): each key of ObjectProfiles
 *     (but TargetableMeshes) is a profile hiding the meshes its value lists:
 *     tokens read as std::stoi; a token that does not parse, or -1, names
 *     another profile, resolved recursively (a cycle skips the profile);
 *     other negative numbers are ignored. A profile becomes the component
 *     set hidden_by_profile.<name>, the name encoded (Xwau_EncodeName);
 *     meshes past 254 are dropped, and a profile hiding nothing makes no
 *     set (the game then uses the Default profile, as XWAU).
 *
 *   - mesh rotations (hook_sfoils, hook_pilot): the lines "mesh, angle,
 *     speed, speed back" of SFoils, SFoilsLandingGears, SFoilsHangarDoors,
 *     SFoilsHatches and SFoilsWarheadsBay (groups sfoils, landing_gear,
 *     hangar_doors, hatches, warheads_bay), and of each
 *     SFoilsCargoClamps_<key> (a section of the .ini, or a
 *     <Model>SFoilsCargoClamps_<key>.txt beside it; group
 *     cargo_clamps.open, cargo_clamps.close or cargo_clamps.<encoded key>),
 *     and the "mesh, angle, speed, behavior" lines of Pilot (group crew,
 *     behaviors 0 to 2: swing, random, random_pause), become articulations.
 *     A line with fewer than four tokens is not a mesh line; numbers read
 *     as std::stoi(text, 0, 0). A mesh outside 0..254, an angle outside
 *     -128..255 or a speed outside 0..255 skips the line (all reported).
 *     PilotCockpit is reported and skipped. The [SFoils] keys
 *     CloseSFoilsInHyperspace, SFoilsAnimationSpeed,
 *     AllowFireWhenSFoilsAreClosed, ParkOrderSFoilsClosed and
 *     HangarDoorsDistance become the settings sfoils.close_in_hyperspace,
 *     step_ticks (15 × speed), sfoils.allow_fire_closed, sfoils.park_closed
 *     and hangar_doors.distance where they differ from the defaults; a
 *     model with crew gets crew.update_all = 1. Then the missions' variants
 *     (xwau_missions.h).
 *
 *   - the Dynamic Cockpit file DynamicCockpit/<Model>.dc;
 *
 *   - for a cockpit model (<Craft>Cockpit), its craft's pilot eye, which a
 *     light of the interior is placed at.
 *
 * Problems go to stderr.
 */

#include <stdbool.h>
#include <stddef.h>

#include "opt_gltf.h"
#include "xwau_config.h"
#include "xwau_missions.h"

#define XWAU_MODEL_MAX_ARTICULATIONS 4096
#define XWAU_MODEL_MAX_SETTINGS 64
#define XWAU_MODEL_MAX_SETS 256

typedef struct XwauModel {
	bool                         has_eye;
	int                          eye[3];
	bool                         has_craft_eye; /* a cockpit model's craft's eye */
	int                          craft_eye[3];
	OptGltfComponentSet          sets[XWAU_MODEL_MAX_SETS];
	size_t                       set_count;
	OptGltfComponentArticulation articulations[XWAU_MODEL_MAX_ARTICULATIONS];
	size_t                       articulation_count;
	OptGltfArticulationSetting   settings[XWAU_MODEL_MAX_SETTINGS];
	size_t                       setting_count;
	char                         dynamic_cockpit[1024]; /* "" without one */
} XwauModel;

/* Reads the files of `model` (the OPT's name without extension). False
 * when out of memory or a table is full. */
bool XwauModel_Read(const XwauRoots* roots, const XwauMissions* missions, const char* model, XwauModel* out,
					const char* label);

/* The pilot eye of `model`: its POV lines, else XWAU's craft table. */
bool XwauModel_Eye(const XwauRoots* roots, const char* model, int eye[3], const char* label);

/* The articulation group of mesh rotation section `section` (case
 * insensitive), or false when it is none. The group may be longer than a
 * tag (63 characters), which the callers report. */
bool XwauModel_SectionGroup(const char* section, char* group, size_t cap);

/* One mesh line of a rotation section. */
typedef struct XwauMeshLine {
	long long values[4]; /* mesh, angle, speed, speed back (or behavior) */
} XwauMeshLine;

/* The mesh lines of `lines` (lines whose numbers do not parse are skipped,
 * and reported unless `label` is NULL). False when out of memory. */
bool XwauModel_MeshLines(const XwauLines* lines, XwauMeshLine** out, size_t* count, const char* label,
						 const char* where);

#endif /* AERON_OPT2GLTF_XWAU_MODEL_H */
