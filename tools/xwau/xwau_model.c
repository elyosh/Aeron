#include "xwau_model.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "model_tables.h"
#include "util.h"

#define TAG_MAX 63
#define COMPONENT_MAX 254

static const struct {
	const char* section;
	const char* group;
} kSections[] = {
	{ "SFoils", "sfoils" },
	{ "SFoilsLandingGears", "landing_gear" },
	{ "SFoilsHangarDoors", "hangar_doors" },
	{ "SFoilsHatches", "hatches" },
	{ "SFoilsWarheadsBay", "warheads_bay" },
};

#define SECTION_COUNT (sizeof kSections / sizeof kSections[0])
#define CARGO_CLAMPS "SFoilsCargoClamps_"

static const char* const kBehaviors[] = { "swing", "random", "random_pause" };

/* Reads the lines of a model's section `section`, as the hook's
 * GetFileLines does. */
static bool section_lines(const char* game, const char* model, const char* section, XwauLines* out) {
	XwauSection read;
	if (!XwauSection_ReadModel(game, model, section, XWAU_RULES_CPP, &read))
		return false;
	*out = read.lines;
	return true;
}

/* ---- Pilot eye ------------------------------------------------------------- */

/* Reads the eye from POV lines that define CockpitPovX, Y and Z. A value
 * that isn't a number stops XWAU, and the eye isn't read then. */
static bool eye_from_lines(const XwauLines* lines, int eye[3]) {
	static const char* const keys[] = { "CockpitPovX", "CockpitPovY", "CockpitPovZ" };
	char                     values[3][256];
	for (int k = 0; k < 3; ++k) {
		XwauLines_Value(lines, keys[k], XWAU_RULES_CPP, values[k], sizeof values[k]);
		if (!values[k][0])
			return false;
	}
	for (int k = 0; k < 3; ++k) {
		long long value;
		if (!Xwau_StoiAutoInt(values[k], &value))
			return false;
		eye[k] = (int)(short)(unsigned short)(value & 0xFFFF);
	}
	return true;
}

bool XwauModel_Eye(const char* game, const char* model, int eye[3]) {
	static const char* const sections[] = { "CockpitPov_Default", "CockpitPov" };
	for (size_t s = 0; s < 2; ++s) {
		XwauLines lines;
		if (!section_lines(game, model, sections[s], &lines))
			return false;
		const bool found = eye_from_lines(&lines, eye);
		XwauLines_Free(&lines);
		if (found)
			return true;
	}
	return ModelTables_XwauEye(model, eye);
}

/* ---- Cockpit field of view ------------------------------------------------- */

/* Reads a key of the .dc file as LoadIndividualDCParams does, and clamps it
 * as SetCurrentShipFOV does. */
static void read_fov(const char* param, const char* value, const char* key, bool* has, float* out) {
	if (strcasecmp(param, key) != 0)
		return;
	const float degrees = (float)atof(value);
	*has                = true;
	*out                = degrees < 15.0f ? 15.0f : (degrees > 170.0f ? 170.0f : degrees);
}

bool XwauModel_CockpitFov(const char* game, const char* model, XwauCockpitFov* out) {
	memset(out, 0, sizeof *out);
	char rel[512], path[1024];
	if (!PathFormat(rel, sizeof rel, "DynamicCockpit/%sCockpit.dc", model) ||
		!Xwau_FindFile(game, rel, path, sizeof path))
		return false;
	FILE* f = fopen(path, "rt");
	if (!f)
		return false;
	char buf[256], param[128], value[128];
	while (fgets(buf, sizeof buf, f)) {
		if (buf[0] == ';' || buf[0] == '#')
			continue;
		value[0] = 0;
		if (sscanf(buf, "%127s = %127s", param, value) <= 0)
			continue;
		read_fov(param, value, "xwahacker_fov", &out->has_regular, &out->regular);
		read_fov(param, value, "xwahacker_large_fov", &out->has_large, &out->large);
	}
	fclose(f);
	return out->has_regular || out->has_large;
}

float XwauModel_VerticalFov(float degrees) {
	const float half = degrees * (3.14159265f / 360.0f);
	return atanf(tanf(half) * 0.5f) * (360.0f / 3.14159265f);
}

/* ---- Object profiles ------------------------------------------------------- */

typedef struct Indices {
	int*   items;
	size_t count;
} Indices;

/* Collects the meshes that `profile` hides, as hook
 * GetModelObjectProfileIndices does. `stack` holds the profiles being
 * resolved. Returns false on a reference cycle. */
static bool resolve_profile(const XwauLines* lines, const char* profile, const char** stack, size_t depth,
							Indices* out, bool* oom) {
	for (size_t i = 0; i < depth; ++i)
		if (Xwau_SameName(stack[i], profile))
			return false;
	if (depth == 64)
		return false;
	stack[depth] = profile;
	char value[4096];
	XwauLines_Value(lines, profile, XWAU_RULES_CPP, value, sizeof value);
	XwauLines tokens;
	if (!Xwau_Tokenize(value, XWAU_RULES_CPP, &tokens)) {
		*oom = true;
		return false;
	}
	bool ok = true;
	for (size_t t = 0; ok && t < tokens.count; ++t) {
		long long index;
		if (!Xwau_Stoi(tokens.items[t], &index) || index == -1) {
			ok = resolve_profile(lines, tokens.items[t], stack, depth + 1, out, oom);
		} else {
			/* Negative indices and indices past the meshes are kept, so that
			   read_profiles can skip them or report them. */
			int* item = ARRAY_PUSH(out->items, out->count);
			if (!item) {
				*oom = true;
				ok   = false;
				break;
			}
			*item = index < INT_MIN ? INT_MIN : index > INT_MAX ? INT_MAX : (int)index;
		}
	}
	XwauLines_Free(&tokens);
	return ok;
}

static bool read_profiles(const char* game, const char* model, XwauModel* out, const char* label) {
	XwauLines lines;
	if (!section_lines(game, model, "ObjectProfiles", &lines))
		return false;
	bool ok = true;
	for (size_t i = 0; ok && i < lines.count; ++i) {
		/* The profile names are the keys of the section. Only the first
		 * occurrence of each key counts. */
		char name[256];
		bool seen = !Xwau_LineKey(lines.items[i], XWAU_RULES_CPP, name, sizeof name) ||
					Xwau_SameName(name, "TargetableMeshes");
		for (size_t j = 0; !seen && j < i; ++j) {
			char prior[256];
			seen = Xwau_LineKey(lines.items[j], XWAU_RULES_CPP, prior, sizeof prior) &&
				   Xwau_SameName(prior, name);
		}
		if (seen)
			continue;
		const char* stack[64];
		Indices     indices = { 0 };
		bool        oom     = false;
		const bool  found   = resolve_profile(&lines, name, stack, 0, &indices, &oom);
		if (oom) {
			free(indices.items);
			ok = false;
			break;
		}
		/* XWAU can't resolve a profile that refers back to itself, so it is
		   skipped. */
		if (!found) {
			free(indices.items);
			continue;
		}
		char set[64] = "hidden_by_profile.";
		if (!Xwau_EncodeName(name, set + strlen(set), sizeof set - strlen(set))) {
			ReportFatal(label, "the set name of profile '%s' would be longer than %d characters", name,
						TAG_MAX);
		} else {
			uint32_t members[8] = { 0 };
			bool     any = false, too_high = false;
			for (size_t k = 0; k < indices.count; ++k) {
				/* A negative index names no mesh. */
				if (indices.items[k] < 0)
					continue;
				if (indices.items[k] > COMPONENT_MAX) {
					too_high = true;
				} else {
					members[indices.items[k] / 32] |= 1u << (indices.items[k] % 32);
					any = true;
				}
			}
			if (too_high)
				ReportFatal(label, "profile '%s' has mesh indices above %d, which a model file can't hold",
							name, COMPONENT_MAX);
			if (any) {
				if (out->set_count == XWAU_MODEL_MAX_SETS) {
					ok = false;
				} else {
					OptGltfComponentSet* entry = &out->sets[out->set_count++];
					snprintf(entry->name, sizeof entry->name, "%s", set);
					memcpy(entry->members, members, sizeof members);
				}
			}
		}
		free(indices.items);
	}
	XwauLines_Free(&lines);
	return ok;
}

/* ---- Collisions and targeting ------------------------------------------------ */

static bool add_set(XwauModel* out, const char* name, const uint32_t members[8]) {
	if (out->set_count == XWAU_MODEL_MAX_SETS)
		return false;
	OptGltfComponentSet* entry = &out->sets[out->set_count++];
	snprintf(entry->name, sizeof entry->name, "%s", name);
	memcpy(entry->members, members, sizeof entry->members);
	return true;
}

/* MeshesCollisions (hook_opt_limit): MeshCollision sets the default of every
 * mesh, MeshCollision_<N> sets mesh N, and 0 turns its collision off. The
 * meshes whose collision is off become the set collision.off. A model has at
 * most 255 meshes, so meshes 0 to 254 are read. */
static bool read_collisions(const char* game, const char* model, XwauModel* out, const char* label) {
	XwauSection section;
	if (!XwauSection_ReadModel(game, model, "MeshesCollisions", XWAU_RULES_CPP, &section))
		return false;
	const XwauLines* lines      = &section.lines;
	const long long  fallback   = XwauSection_IntOr(&section, "MeshCollision", 1);
	uint32_t         members[8] = { 0 };
	bool             any        = false;
	for (int mesh = 0; lines->count && mesh <= COMPONENT_MAX; ++mesh) {
		char key[32];
		snprintf(key, sizeof key, "MeshCollision_%d", mesh);
		if (XwauSection_IntOr(&section, key, fallback) == 0) {
			members[mesh / 32] |= 1u << (mesh % 32);
			any = true;
		}
	}
	/* A model file holds meshes 0 to 254 only. */
	for (size_t i = 0; i < lines->count; ++i) {
		const char*  item   = lines->items[i];
		const size_t prefix = strlen("MeshCollision_");
		if (strncasecmp(item, "MeshCollision_", prefix) != 0 || item[prefix] < '0' || item[prefix] > '9')
			continue;
		const long mesh = strtol(item + prefix, NULL, 10);
		if (mesh > COMPONENT_MAX) {
			ReportFatal(label,
						"MeshesCollisions sets mesh %ld, but a model file can only hold meshes 0 to %d", mesh,
						COMPONENT_MAX);
			break;
		}
	}
	XwauSection_Free(&section);
	return !any || add_set(out, "collision.off", members);
}

/* TargetableMeshes (hook_mission_objects): the first TargetableMeshes key of
 * ObjectProfiles lists the meshes that the player can target. Tokens are
 * split on ',' and ';' and read with std::stoi. The meshes become the set
 * targetable. Without the key, or with an empty value, every mesh can be
 * targeted and no set is written. */
static bool read_targetable(const char* game, const char* model, XwauModel* out, const char* label) {
	XwauLines lines;
	char      value[4096];
	if (!section_lines(game, model, "ObjectProfiles", &lines))
		return false;
	XwauLines_Value(&lines, "TargetableMeshes", XWAU_RULES_CPP, value, sizeof value);
	XwauLines_Free(&lines);
	if (!value[0])
		return true;
	XwauLines tokens;
	if (!Xwau_Tokenize(value, XWAU_RULES_CPP, &tokens))
		return false;
	uint32_t members[8] = { 0 };
	bool     any        = false;
	for (size_t t = 0; t < tokens.count; ++t) {
		long long mesh;
		/* XWAU fails on a token that isn't a number, and a negative mesh
		   names no mesh. */
		if (!Xwau_Stoi(tokens.items[t], &mesh) || mesh < 0)
			continue;
		if (mesh > COMPONENT_MAX) {
			ReportFatal(label,
						"TargetableMeshes has mesh %lld, but a model file can only hold meshes 0 to %d", mesh,
						COMPONENT_MAX);
		} else {
			members[mesh / 32] |= 1u << (mesh % 32);
			any = true;
		}
	}
	XwauLines_Free(&tokens);
	if (!any) {
		ReportUnsupported("the ObjectProfiles key TargetableMeshes without a usable mesh");
		return true;
	}
	return add_set(out, "targetable", members);
}

/* ---- Mesh rotations ---------------------------------------------------------- */

bool XwauModel_SectionGroup(const char* section, char* group, size_t cap) {
	for (size_t i = 0; i < SECTION_COUNT; ++i) {
		if (Xwau_SameName(section, kSections[i].section)) {
			snprintf(group, cap, "%s", kSections[i].group);
			return true;
		}
	}
	const size_t prefix = strlen(CARGO_CLAMPS);
	if (strncasecmp(section, CARGO_CLAMPS, prefix) != 0 || !section[prefix])
		return false;
	const char* key = section + prefix;
	if (Xwau_SameName(key, "open") || Xwau_SameName(key, "close")) {
		snprintf(group, cap, "cargo_clamps.%s", Xwau_SameName(key, "open") ? "open" : "close");
		return true;
	}
	const size_t used = (size_t)snprintf(group, cap, "cargo_clamps.");
	if (!Xwau_EncodeName(key, group + used, cap - used))
		group[cap - 1] = 0;
	return true;
}

bool XwauModel_MeshLines(const XwauLines* lines, XwauMeshLine** out, size_t* count, const char* label,
						 const char* where) {
	/* The callers pass a label and a section name, which aren't used. */
	(void)label;
	(void)where;
	*out   = NULL;
	*count = 0;
	for (size_t i = 0; i < lines->count; ++i) {
		XwauLines tokens;
		if (!Xwau_Tokenize(lines->items[i], XWAU_RULES_CPP, &tokens)) {
			free(*out);
			*out = NULL;
			return false;
		}
		XwauMeshLine line;
		bool         numbers = tokens.count >= 4;
		for (int k = 0; numbers && k < 4; ++k)
			numbers = Xwau_StoiAutoInt(tokens.items[k], &line.values[k]);
		/* XWAU fails on a value that isn't a number, and it skips a line
		   with fewer than 4 values. */
		XwauLines_Free(&tokens);
		if (!numbers)
			continue;
		XwauMeshLine* added = ARRAY_PUSH(*out, *count);
		if (!added) {
			free(*out);
			*out = NULL;
			return false;
		}
		*added = line;
	}
	return true;
}

/* Adds an articulation if it passes the hook's range checks. A line that
 * fails them is skipped, as in XWAU. */
static bool add_articulation(XwauModel* out, const char* where, const char* group, long long mesh,
							 long long angle, long long speed, long long speed_back, const char* behavior,
							 bool* added, const char* label) {
	*added = false;
	const bool in_range =
		angle >= -128 && angle <= 255 && speed >= 0 && speed <= 255 && speed_back >= 0 && speed_back <= 255;
	if (mesh < 0 || (mesh <= COMPONENT_MAX && !in_range))
		return true;
	if (mesh > COMPONENT_MAX) {
		ReportFatal(label, "[%s] moves mesh %lld, but a model file can only hold meshes 0 to %d", where, mesh,
					COMPONENT_MAX);
	} else if (strlen(group) > TAG_MAX) {
		ReportFatal(label,
					"[%s] gives group '%s', which is longer than the %d characters that a model file can "
					"hold",
					where, group, TAG_MAX);
	} else if (out->articulation_count == XWAU_MODEL_MAX_ARTICULATIONS) {
		return false;
	} else {
		OptGltfComponentArticulation* a = &out->articulations[out->articulation_count++];
		memset(a, 0, sizeof *a);
		a->component  = (uint32_t)mesh;
		a->angle      = (int32_t)angle;
		a->speed      = (int32_t)speed;
		a->speed_back = (int32_t)speed_back;
		snprintf(a->group, sizeof a->group, "%s", group);
		snprintf(a->behavior, sizeof a->behavior, "%s", behavior ? behavior : "");
		*added = true;
	}
	return true;
}

static bool add_section(const char* game, const char* model, const char* section, const char* group,
						XwauModel* out, const char* label) {
	XwauLines     lines;
	XwauMeshLine* mesh_lines = NULL;
	size_t        count      = 0;
	if (!section_lines(game, model, section, &lines))
		return false;
	bool ok = XwauModel_MeshLines(&lines, &mesh_lines, &count, label, section);
	XwauLines_Free(&lines);
	for (size_t i = 0; ok && i < count; ++i) {
		bool added;
		ok = add_articulation(out, section, group, mesh_lines[i].values[0], mesh_lines[i].values[1],
							  mesh_lines[i].values[2], mesh_lines[i].values[3], NULL, &added, label);
	}
	free(mesh_lines);
	return ok;
}

/* Collects the <key> of each SFoilsCargoClamps_<key> section in <Model>.ini
 * and of each <Model>SFoilsCargoClamps_<key>.txt file next to it. Each key is
 * kept once, compared in any case. */
static bool cargo_clamp_keys(const char* game, const char* model, XwauLines* keys) {
	memset(keys, 0, sizeof *keys);
	char rel[512], ini[1024];
	if (!PathFormat(rel, sizeof rel, "FlightModels/%s.ini", model))
		return false;
	if (!Xwau_FindFile(game, rel, ini, sizeof ini))
		return true;
	XwauLines found = { 0 };
	FILE*     f     = fopen(ini, "rb");
	bool      ok    = true;
	if (f) {
		char line[4096];
		while (ok && fgets(line, sizeof line, f)) {
			Xwau_Trim(line, XWAU_RULES_CPP);
			const size_t len    = strlen(line);
			const size_t prefix = strlen(CARGO_CLAMPS);
			if (len >= 2 && line[0] == '[' && line[len - 1] == ']' && len - 2 >= prefix &&
				strncasecmp(line + 1, CARGO_CLAMPS, prefix) == 0) {
				line[len - 1] = 0;
				ok            = XwauLines_Add(&found, line + 1 + prefix, strlen(line + 1 + prefix));
			}
		}
		fclose(f);
	}
	/* The .txt files next to the .ini, matched by name. */
	char dir[1024];
	snprintf(dir, sizeof dir, "%s", ini);
	*strrchr(dir, '/') = 0;
	char prefix[512];
	snprintf(prefix, sizeof prefix, "%s%s", model, CARGO_CLAMPS);
	size_t       entry_count = 0;
	char** const entries     = ListDir(dir, true, &entry_count);
	const size_t prefix_len  = strlen(prefix);
	for (size_t i = 0; ok && i < entry_count; ++i) {
		const size_t len = strlen(entries[i]);
		if (len >= prefix_len + 4 && strncasecmp(entries[i], prefix, prefix_len) == 0 &&
			strcasecmp(entries[i] + len - 4, ".txt") == 0) {
			ok = XwauLines_Add(&found, entries[i] + prefix_len, len - prefix_len - 4);
		}
	}
	FreeList(entries, entry_count);
	for (size_t i = 0; ok && i < found.count; ++i) {
		bool seen = !found.items[i][0];
		for (size_t j = 0; !seen && j < keys->count; ++j)
			seen = Xwau_SameName(keys->items[j], found.items[i]);
		if (seen)
			continue;
		ok = XwauLines_Add(keys, found.items[i], strlen(found.items[i]));
	}
	XwauLines_Free(&found);
	if (!ok)
		XwauLines_Free(keys);
	return ok;
}

static const struct {
	const char* setting;
	const char* key;
	long long   fallback;
	int         kind; /* 0: flag, 1: 15 × max(v, 1), 2: the value */
} kSettings[] = {
	{ "sfoils.close_in_hyperspace", "CloseSFoilsInHyperspace", 0, 0 },
	{ "step_ticks", "SFoilsAnimationSpeed", 15, 1 },
	{ "sfoils.allow_fire_closed", "AllowFireWhenSFoilsAreClosed", 0, 0 },
	{ "sfoils.park_closed", "ParkOrderSFoilsClosed", 0, 0 },
	{ "hangar_doors.distance", "HangarDoorsDistance", 0x4000, 2 },
};

static bool add_setting(XwauModel* out, const char* name, long long value) {
	if (out->setting_count == XWAU_MODEL_MAX_SETTINGS)
		return false;
	OptGltfArticulationSetting* s = &out->settings[out->setting_count++];
	snprintf(s->name, sizeof s->name, "%s", name);
	s->value = (int32_t)value;
	return true;
}

static bool read_rotations(const char* game, const XwauMissions* missions, const char* model, XwauModel* out,
						   const char* label) {
	bool ok = true;
	for (size_t i = 0; ok && i < SECTION_COUNT; ++i)
		ok = add_section(game, model, kSections[i].section, kSections[i].group, out, label);
	XwauLines keys = { 0 };
	ok             = ok && cargo_clamp_keys(game, model, &keys);
	for (size_t i = 0; ok && i < keys.count; ++i) {
		char section[512], group[512];
		snprintf(section, sizeof section, "%s%s", CARGO_CLAMPS, keys.items[i]);
		XwauModel_SectionGroup(section, group, sizeof group);
		ok = add_section(game, model, section, group, out, label);
	}
	XwauLines_Free(&keys);

	/* The crew: the lines of Pilot, with behaviors 0 to 2. */
	XwauLines     lines;
	XwauMeshLine* mesh_lines = NULL;
	size_t        count      = 0;
	bool          has_crew   = false;
	if (ok && (ok = section_lines(game, model, "Pilot", &lines))) {
		ok = XwauModel_MeshLines(&lines, &mesh_lines, &count, label, "Pilot");
		XwauLines_Free(&lines);
	}
	for (size_t i = 0; ok && i < count; ++i) {
		const long long behavior = mesh_lines[i].values[3];
		if (behavior < 0 || behavior > 2) {
			ReportUnsupported("[Pilot] behaviors other than 0 to 2");
			continue;
		}
		bool added;
		ok = add_articulation(out, "Pilot", "crew", mesh_lines[i].values[0], mesh_lines[i].values[1],
							  mesh_lines[i].values[2], mesh_lines[i].values[2], kBehaviors[behavior], &added,
							  label);
		has_crew |= added;
	}
	free(mesh_lines);
	if (ok && (ok = section_lines(game, model, "PilotCockpit", &lines))) {
		if (lines.count)
			ReportUnsupported("the [PilotCockpit] section");
		XwauLines_Free(&lines);
	}

	/* The settings, read from [SFoils] only, as the hook does. */
	if (ok && (ok = section_lines(game, model, "SFoils", &lines))) {
		for (size_t i = 0; ok && i < sizeof kSettings / sizeof kSettings[0]; ++i) {
			long long value;
			/* XWAU fails on a value that isn't a number. */
			if (XwauLines_Int(&lines, XWAU_RULES_CPP, kSettings[i].key, &value) != XWAU_VALUE_SET)
				continue;
			if (kSettings[i].kind == 0)
				value = value != 0;
			else if (kSettings[i].kind == 1)
				value = 15 * (value > 1 ? value : 1);
			if (value != kSettings[i].fallback)
				ok = add_setting(out, kSettings[i].setting, value);
		}
		XwauLines_Free(&lines);
	}
	if (ok && has_crew)
		ok = add_setting(out, "crew.always_animate", 1);
	return ok && XwauMissions_Articulations(missions, model, out->articulations, &out->articulation_count,
											XWAU_MODEL_MAX_ARTICULATIONS, label);
}

bool XwauModel_Read(const char* game, const XwauMissions* missions, const char* model, XwauModel* out,
					const char* label) {
	memset(out, 0, sizeof *out);
	out->has_eye = XwauModel_Eye(game, model, out->eye);
	XwauModel_CockpitFov(game, model, &out->fov);
	char rel[512];
	if (!PathFormat(rel, sizeof rel, "DynamicCockpit/%s.dc", model))
		return false;
	if (!Xwau_FindFile(game, rel, out->dynamic_cockpit, sizeof out->dynamic_cockpit))
		out->dynamic_cockpit[0] = 0;
	return read_profiles(game, model, out, label) && read_collisions(game, model, out, label) &&
		   read_targetable(game, model, out, label) && read_rotations(game, missions, model, out, label);
}
