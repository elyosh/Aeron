#include "xwau_model.h"

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

/* Reads the lines of `section` from <Model><Section>.txt, or else from the
 * [<Section>] section of <Model>.ini (hook GetFileLines). */
static bool section_lines(const char* game, const char* model, const char* section, XwauLines* out) {
	char rel[512], path[1024];
	if (!PathFormat(rel, sizeof rel, "FlightModels/%s%s.txt", model, section) ||
		!XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, "", XWAU_RULES_CPP, out))
		return false;
	if (out->count)
		return true;
	if (!PathFormat(rel, sizeof rel, "FlightModels/%s.ini", model))
		return false;
	return XwauLines_Read(Xwau_FindFile(game, rel, path, sizeof path) ? path : NULL, section, XWAU_RULES_CPP,
						  out);
}

/* ---- Pilot eye ------------------------------------------------------------- */

/* Reads the eye from POV lines that define CockpitPovX, Y and Z. */
static bool eye_from_lines(const XwauLines* lines, int eye[3], const char* label, const char* where) {
	static const char* const keys[] = { "CockpitPovX", "CockpitPovY", "CockpitPovZ" };
	char                     values[3][256];
	for (int k = 0; k < 3; ++k) {
		XwauLines_Value(lines, keys[k], XWAU_RULES_CPP, values[k], sizeof values[k]);
		if (!values[k][0])
			return false;
	}
	for (int k = 0; k < 3; ++k) {
		long long value;
		if (!Xwau_StoiAuto(values[k], &value)) {
			Report(label, "%s: %s = %s is not a number; no eye", where, keys[k], values[k]);
			return false;
		}
		eye[k] = (int)(short)(unsigned short)(value & 0xFFFF);
	}
	return true;
}

bool XwauModel_Eye(const char* game, const char* model, int eye[3], const char* label) {
	static const char* const sections[] = { "CockpitPov_Default", "CockpitPov" };
	for (size_t s = 0; s < 2; ++s) {
		XwauLines lines;
		if (!section_lines(game, model, sections[s], &lines))
			return false;
		const bool found = eye_from_lines(&lines, eye, label, sections[s]);
		XwauLines_Free(&lines);
		if (found)
			return true;
	}
	return ModelTables_XwauEye(model, eye);
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
		} else if (index >= 0) {
			int* item = ARRAY_PUSH(out->items, out->count);
			if (!item) {
				*oom = true;
				ok   = false;
				break;
			}
			*item = (int)index;
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
		const char* eq = strchr(lines.items[i], '=');
		char        name[256];
		snprintf(name, sizeof name, "%.*s", eq ? (int)(eq - lines.items[i]) : 0, lines.items[i]);
		Xwau_Trim(name, XWAU_RULES_CPP);
		bool seen = !name[0] || Xwau_SameName(name, "TargetableMeshes");
		for (size_t j = 0; !seen && j < i; ++j) {
			const char* prior_eq = strchr(lines.items[j], '=');
			char        prior[256];
			snprintf(prior, sizeof prior, "%.*s", prior_eq ? (int)(prior_eq - lines.items[j]) : 0,
					 lines.items[j]);
			Xwau_Trim(prior, XWAU_RULES_CPP);
			seen = prior[0] && Xwau_SameName(prior, name);
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
		char set[64] = "hidden_by_profile.";
		if (!found) {
			Report(label, "profile '%s' skipped: reference cycle", name);
		} else if (!Xwau_EncodeName(name, set + strlen(set), sizeof set - strlen(set))) {
			Report(label, "profile '%s' skipped: its set name is longer than %d characters", name, TAG_MAX);
		} else {
			uint32_t members[8] = { 0 };
			bool     any = false, dropped = false;
			for (size_t k = 0; k < indices.count; ++k) {
				if (indices.items[k] > COMPONENT_MAX) {
					dropped = true;
				} else {
					members[indices.items[k] / 32] |= 1u << (indices.items[k] % 32);
					any = true;
				}
			}
			if (dropped)
				Report(label, "profile '%s': mesh indices above %d dropped", name, COMPONENT_MAX);
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
			numbers = Xwau_StoiAuto(tokens.items[k], &line.values[k]);
		if (tokens.count >= 4 && !numbers && label)
			Report(label, "%s: line '%s' skipped: not a number (XWAU would fail)", where, lines->items[i]);
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

/* Adds an articulation if it passes the hook's range checks. Failed checks
 * are reported. */
static bool add_articulation(XwauModel* out, const char* where, const char* group, long long mesh,
							 long long angle, long long speed, long long speed_back, const char* behavior,
							 bool* added, const char* label) {
	*added = false;
	if (mesh < 0 || mesh > COMPONENT_MAX) {
		Report(label, "%s: mesh %lld skipped: outside 0..%d", where, mesh, COMPONENT_MAX);
	} else if (angle < -128 || angle > 255) {
		Report(label, "%s: mesh %lld skipped: angle %lld outside -128..255", where, mesh, angle);
	} else if (speed < 0 || speed > 255 || speed_back < 0 || speed_back > 255) {
		Report(label, "%s: mesh %lld skipped: speeds %lld, %lld outside 0..255", where, mesh, speed,
			   speed_back);
	} else if (strlen(group) > TAG_MAX) {
		Report(label, "%s: skipped: group '%s' is longer than %d characters", where, group, TAG_MAX);
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
			Report(label, "Pilot: mesh %lld skipped: behavior %lld is not converted", mesh_lines[i].values[0],
				   behavior);
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
			Report(label, "PilotCockpit: not converted (OpenXWA has no cockpit list)");
		XwauLines_Free(&lines);
	}

	/* The settings, read from [SFoils] only, as the hook does. */
	if (ok && (ok = section_lines(game, model, "SFoils", &lines))) {
		for (size_t i = 0; ok && i < sizeof kSettings / sizeof kSettings[0]; ++i) {
			char      text[256];
			long long value;
			XwauLines_Value(&lines, kSettings[i].key, XWAU_RULES_CPP, text, sizeof text);
			if (!text[0])
				continue;
			if (!Xwau_StoiAuto(text, &value)) {
				Report(label, "SFoils: %s = %s skipped: not a number (XWAU would fail)", kSettings[i].key,
					   text);
				continue;
			}
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
		ok = add_setting(out, "crew.update_all", 1);
	return ok && XwauMissions_Articulations(missions, model, out->articulations, &out->articulation_count,
											XWAU_MODEL_MAX_ARTICULATIONS, label);
}

bool XwauModel_Read(const char* game, const XwauMissions* missions, const char* model, XwauModel* out,
					const char* label) {
	memset(out, 0, sizeof *out);
	out->has_eye = XwauModel_Eye(game, model, out->eye, label);
	/* A cockpit model uses the same frame as its craft, so it also gets the
	 * eye of its craft. */
	const size_t len = strlen(model);
	if (len > 7 && strcasecmp(model + len - 7, "cockpit") == 0) {
		char craft[256];
		snprintf(craft, sizeof craft, "%.*s", (int)(len - 7), model);
		out->has_craft_eye = XwauModel_Eye(game, craft, out->craft_eye, label);
	}
	char rel[512];
	if (!PathFormat(rel, sizeof rel, "DynamicCockpit/%s.dc", model))
		return false;
	if (!Xwau_FindFile(game, rel, out->dynamic_cockpit, sizeof out->dynamic_cockpit))
		out->dynamic_cockpit[0] = 0;
	return read_profiles(game, model, out, label) && read_rotations(game, missions, model, out, label);
}
