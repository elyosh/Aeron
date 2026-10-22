#ifndef AERON_XWAU_MODEL_H
#define AERON_XWAU_MODEL_H

/*
 * Reads what XWAU's hooks read for one model from the files next to its OPT,
 * and turns it into converter options. Each section is read from
 * FlightModels/<Model><Section>.txt, or else from the [<Section>] section of
 * FlightModels/<Model>.ini, with the C++ rules (xwau_config.h):
 *
 *   - the pilot eye (hook_mission_objects) comes from CockpitPov_Default, or
 *     else from CockpitPov, when the section defines all of CockpitPovX, Y and
 *     Z. The values are read with std::stoi(value, 0, 0) and stored as 16
 *     bits. Otherwise, the eye comes from XWAU's craft table, for the craft
 *     with the model's name (model_tables.h). Missions can select other POV
 *     profiles, but the model is converted with the Default one.
 *
 *   - object profiles (hook_mission_objects): each key of ObjectProfiles,
 *     except TargetableMeshes, is a profile that hides the meshes listed in
 *     its value. Tokens are read with std::stoi. A token that does not parse,
 *     or -1, is the name of another profile, which is resolved recursively.
 *     If the references form a cycle, the profile is skipped. Other negative
 *     numbers are ignored. A profile becomes the component set
 *     hidden_by_profile.<name>, with the name encoded by Xwau_EncodeName.
 *     Meshes above 254 are dropped. A profile that hides nothing creates no
 *     set, and the game then uses the Default profile, as XWAU does.
 *
 *   - mesh collisions (hook_opt_limit): in MeshesCollisions, MeshCollision
 *     is the default of every mesh and MeshCollision_<N> sets mesh N, read
 *     with std::stoi(value, 0, 0). The meshes whose value is 0 have no
 *     collision and become the component set collision.off.
 *
 *   - targetable meshes (hook_mission_objects): the first TargetableMeshes
 *     key of ObjectProfiles lists the meshes that the player can target.
 *     Tokens are split on ',' and ';' and read with std::stoi. The meshes
 *     become the component set targetable. Tokens that aren't numbers, and
 *     meshes outside 0..254, are reported and dropped.
 *
 *   - mesh rotations (hook_sfoils, hook_pilot) become articulations. The
 *     sections SFoils, SFoilsLandingGears, SFoilsHangarDoors, SFoilsHatches
 *     and SFoilsWarheadsBay have lines "mesh, angle, speed, speed back", for
 *     the groups sfoils, landing_gear, hangar_doors, hatches and warheads_bay.
 *     Each SFoilsCargoClamps_<key> has the same lines. It is a section of the
 *     .ini or a <Model>SFoilsCargoClamps_<key>.txt file next to it, and its
 *     group is cargo_clamps.open, cargo_clamps.close or
 *     cargo_clamps.<encoded key>. Pilot has lines "mesh, angle, speed,
 *     behavior" for the group crew, with behaviors 0 to 2 (swing, random,
 *     random_pause). A line with fewer than four tokens is not a mesh line.
 *     Numbers are read with std::stoi(text, 0, 0). A line is skipped and
 *     reported when its mesh is outside 0..254, its angle is outside
 *     -128..255, or a speed is outside 0..255. PilotCockpit is reported and
 *     skipped. The [SFoils] keys CloseSFoilsInHyperspace,
 *     SFoilsAnimationSpeed, AllowFireWhenSFoilsAreClosed,
 *     ParkOrderSFoilsClosed and HangarDoorsDistance become the settings
 *     sfoils.close_in_hyperspace, step_ticks (15 × speed),
 *     sfoils.allow_fire_closed, sfoils.park_closed and hangar_doors.distance,
 *     when they differ from the defaults. A model with crew gets
 *     crew.update_all = 1. The variants from the missions are added last
 *     (xwau_missions.h).
 *
 *   - the Dynamic Cockpit file DynamicCockpit/<Model>.dc.
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
	OptGltfComponentSet          sets[XWAU_MODEL_MAX_SETS];
	size_t                       set_count;
	OptGltfComponentArticulation articulations[XWAU_MODEL_MAX_ARTICULATIONS];
	size_t                       articulation_count;
	OptGltfArticulationSetting   settings[XWAU_MODEL_MAX_SETTINGS];
	size_t                       setting_count;
	char                         dynamic_cockpit[1024]; /* "" without one */
} XwauModel;

/* Reads the files of `model` (the OPT name without the extension). Returns
 * false when out of memory or when a table is full. */
bool XwauModel_Read(const char* game, const XwauMissions* missions, const char* model, XwauModel* out,
					const char* label);

/* Gets the pilot eye of `model` from its POV lines, or else from XWAU's craft table. */
bool XwauModel_Eye(const char* game, const char* model, int eye[3], const char* label);

/* Gets the articulation group of the mesh rotation section `section`, which
 * can be in any case. Returns false when `section` is not a mesh rotation
 * section. The group can be longer than a tag (63 characters), and the callers
 * report that. */
bool XwauModel_SectionGroup(const char* section, char* group, size_t cap);

/* One mesh line of a rotation section. */
typedef struct XwauMeshLine {
	long long values[4]; /* mesh, angle, speed, speed back (or behavior) */
} XwauMeshLine;

/* Collects the mesh lines of `lines`. Lines whose numbers do not parse are
 * skipped, and reported unless `label` is NULL. Returns false when out of
 * memory. */
bool XwauModel_MeshLines(const XwauLines* lines, XwauMeshLine** out, size_t* count, const char* label,
						 const char* where);

#endif /* AERON_XWAU_MODEL_H */
