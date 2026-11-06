#include "xwau_skyboxes.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "classic_radiance.h"
#include "util.h"
#include "xwau_config.h"
#include "xwau_cube_faces.h"
#include "xwau_missions.h"

#define SKY_REGIONS 4
/* The game's limit on a hide or show list (XWA_ENV_BACKDROP_LIST_MAX). */
#define SKY_LIST_MAX 64
#define SKY_PI 3.14159265358979323846
/* Radiance in W/m^2/sr of a sky texel of 1, the environment's `intensity`.
 * XWA Upgrade's skyboxes are sRGB images with no unit. It sets how bright
 * the sky looks. */
#define SKY_RADIANCE 40.0f
/* The environment's diffuse_strength and specular_strength: how much the
 * sky lights the models, compared with how bright it is drawn. */
#define SKY_LIGHT_STRENGTH 0.4f
/* The region's backdrop_light_scale. XWAU's renderer dims the lights of
 * backdrops that are not suns (PBRAddCommon.h), and its missions are tuned
 * for that. */
#define SKY_BACKDROP_LIGHT_SCALE 0.25f
/* Radiance in W/m^2/sr of an ambient fill of 1. XWAU compares AmbientMin
 * with a light of brightness 1, so a fill of 1 is as bright as a white
 * surface that faces a classic light of brightness 1. */
#define SKY_FILL_RADIANCE (CLASSIC_ILLUMINANCE / SKY_PI)

static const char* const kMissionDirs[] = { "Missions", "Melee", "Combat" };

#define SKY_AMBIENT 1.0

/* The environment of the Death Star missions without [SkyBoxes]. It has no
 * sky and no directional light, so the light of the mission's backdrops
 * doesn't reach the inside of the Death Star. Its ambient fill is the one
 * the game uses where no environment gives one, so the Death Star keeps its
 * brightness. */
#define SKY_DEATH_STAR_ENVIRONMENT "openxwa.death_star"
#define SKY_DEATH_STAR_AMBIENT 5.9

/* The starfield images that the ddraw wrapper hides under a cube
 * (EffectsRenderer.cpp). */
static const uint16_t kStarfieldImages[][2] = {
	{ 6104, 0 }, { 6104, 5 }, { 6079, 2 }, { 6079, 3 }, { 6079, 4 }, { 6079, 5 }, { 6079, 6 }, { 6034, 3 },
	{ 6034, 4 }, { 6034, 5 }, { 6034, 6 }, { 6042, 1 }, { 6042, 2 }, { 6042, 3 }, { 6042, 4 }, { 6042, 5 },
	{ 6042, 6 }, { 6094, 1 }, { 6094, 2 }, { 6094, 3 }, { 6094, 4 }, { 6094, 5 }, { 6094, 6 }, { 6083, 2 },
	{ 6083, 3 }, { 6083, 5 }, { 6084, 1 }, { 6084, 2 }, { 6084, 4 }, { 6084, 6 },
};

/* ===== Rotation ========================================================= */

static void mat_mul(const double a[9], const double b[9], double out[9]) {
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

/* Right-handed rotations by `degrees`, row-major, for column vectors. */
static void rot_axis(int axis, double degrees, double out[9]) {
	const double a = degrees * SKY_PI / 180.0, c = cos(a), s = sin(a);
	const double x[9] = { 1, 0, 0, 0, c, -s, 0, s, c };
	const double y[9] = { c, 0, s, 0, 1, 0, -s, 0, c };
	const double z[9] = { c, -s, 0, s, c, 0, 0, 0, 1 };
	memcpy(out, axis == 0 ? x : axis == 1 ? y : z, sizeof x);
}

void XwauSkyboxes_Rotation(const float xyz_degrees[3], bool overlay, float out_degrees[3]) {
	double rx[9], ry[9], rz[9], t[9], rot[9];
	rot_axis(0, xyz_degrees[0], rx);
	rot_axis(1, xyz_degrees[1], ry);
	rot_axis(2, xyz_degrees[2], rz);
	if (overlay) {
		mat_mul(rx, ry, t);
		mat_mul(t, rz, rot);
	} else {
		mat_mul(rz, ry, t);
		mat_mul(t, rx, rot);
	}
	/* Q = M Rot^T M, M = diag(1, 1, -1). */
	static const double m[3] = { 1.0, 1.0, -1.0 };
	double              q[9];
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			q[r * 3 + c] = m[r] * m[c] * rot[c * 3 + r];
	/* Q = Ry(yaw) Rx(pitch) Rz(roll): row 1 is (cos p sin r, cos p cos r,
	 * -sin p) and column 2 is (sin y cos p, -sin p, cos y cos p). */
	const double pitch = asin(fmax(-1.0, fmin(1.0, -q[5])));
	double       yaw, roll;
	if (fabs(cos(pitch)) > 1e-6) {
		yaw  = atan2(q[2], q[8]);
		roll = atan2(q[3], q[4]);
	} else {
		/* Pitch +-90: the roll folds into the yaw. */
		yaw  = atan2(-q[6], q[0]);
		roll = 0.0;
	}
	const double angles[3] = { yaw, pitch, roll };
	for (int i = 0; i < 3; ++i) {
		const double d = angles[i] * 180.0 / SKY_PI;
		out_degrees[i] = fabs(d) < 1e-6 ? 0.0f : (float)d;
	}
}

/* ===== Reading [SkyBoxes] ================================================ */

typedef struct SkyFolder {
	char path[1024];   /* the folder found in the game folder */
	char key[128];     /* lower-case encoded name */
	bool sky, overlay; /* how the folder is used */
} SkyFolder;

typedef struct SkyDefinition {
	size_t folder;
	float  ambient_min;
	long   overlay;             /* overlay folder, or -1 */
	float  overlay_rotation[3]; /* XWAU angles */
	char   name[64];
} SkyDefinition;

typedef struct SkyRegion {
	long  definition; /* -1: no cube */
	float rotation[3];
	bool  overlay_without_sky;
} SkyRegion;

typedef struct SkyList {
	bool     all;
	uint32_t images[SKY_LIST_MAX]; /* group << 16 | image */
	size_t   count;
} SkyList;

typedef struct SkyMission {
	char      key[256]; /* "missions/1b0m1fw.tie" */
	bool      death_star; /* a Death Star mission without [SkyBoxes] */
	SkyRegion regions[SKY_REGIONS];
	SkyList   disabled, enabled;
} SkyMission;

typedef struct SkyState {
	const char*    game;
	const char*    stock_game;
	SkyFolder*     folders;
	size_t         folder_count;
	SkyDefinition* definitions;
	size_t         definition_count;
	SkyMission*    missions;
	size_t         mission_count;
	size_t         death_star_count;
} SkyState;

/* The value of `key` read as std::stof reads it, or `fallback`. */
static float sky_float(const XwauLines* lines, const char* key, float fallback) {
	char value[128];
	XwauLines_Value(lines, key, XWAU_RULES_CPP, value, sizeof value);
	if (!value[0])
		return fallback;
	errno           = 0;
	char*       end = NULL;
	const float f   = strtof(value, &end);
	return end == value || errno == ERANGE ? fallback : f;
}

/* Whether the ddraw wrapper loads the cube folder `written`. */
static bool folder_loads(const char* game, const char* written, char* found, size_t cap) {
	char paths[6][XWAU_CUBE_FACE_PATH_MAX];
	return written[0] && Xwau_FindDir(game, written, found, cap) && XwauCubeFaces_Find(found, paths);
}

static bool list_has(const SkyList* list, uint32_t image) {
	for (size_t i = 0; i < list->count; ++i)
		if (list->images[i] == image)
			return true;
	return false;
}

static void list_add(SkyList* list, uint32_t image, const char* what, const char* base) {
	if (list_has(list, image))
		return;
	if (list->count == SKY_LIST_MAX) {
		ReportFatal(base, "[SkyBoxes]: %s has more than %d images, the most that the game takes", what,
					SKY_LIST_MAX);
		return;
	}
	list->images[list->count++] = image;
}

/* Reads DisabledBackdrops or EnabledBackdrops the way PopulateBackdropsMap
 * does. Tokens are separated by ';'. A token with '-' is an image, with each
 * side read by atoi. ALL or all means every image and ends the list. */
static void read_list(const XwauLines* lines, const char* key, const char* base, SkyList* out) {
	char value[2048];
	XwauLines_Value(lines, key, XWAU_RULES_CPP, value, sizeof value);
	for (char* token = value; token && *token;) {
		char* next = strchr(token, ';');
		if (next)
			*next++ = '\0';
		char* dash = strchr(token, '-');
		if (dash) {
			*dash           = '\0';
			const int group = atoi(token);
			const int image = atoi(dash + 1);
			/* A number outside 0 to 65535 names no DAT image. */
			if (group >= 0 && group <= 0xFFFF && image >= 0 && image <= 0xFFFF)
				list_add(out, (uint32_t)group << 16 | (uint32_t)image, key, base);
		} else if (strcmp(token, "ALL") == 0 || strcmp(token, "all") == 0) {
			out->all = true;
			return;
		}
		/* XWAU ignores any other token. */
		token = next;
	}
}

/* Counts the mission keys that the ddraw wrapper reads but the importer
 * doesn't import. Keys that the wrapper doesn't read are ignored. */
static void report_unknown_keys(const XwauLines* lines) {
	static const char* const kPrefixes[] = { "AllRegions", "Region0", "Region1", "Region2", "Region3" };
	static const char* const kOthers[]   = {
		"DisabledBackdrops",      "EnabledBackdrops",       "RenderMode",
		"HangarRotationX",        "HangarRotationY",        "HangarRotationZ",
		"HangarOverlayRotationX", "HangarOverlayRotationY", "HangarOverlayRotationZ"
	};
	for (size_t i = 0; i < lines->count; ++i) {
		const char* equals = strchr(lines->items[i], '=');
		if (!equals)
			continue;
		char name[128];
		snprintf(name, sizeof name, "%.*s", (int)(equals - lines->items[i]), lines->items[i]);
		Xwau_Trim(name, XWAU_RULES_CPP);
		const char* other = NULL;
		for (size_t k = 0; !other && k < sizeof kOthers / sizeof kOthers[0]; ++k)
			if (Xwau_SameName(name, kOthers[k]))
				other = kOthers[k];
		/* The importer bakes its own reflection maps, at a fixed size. */
		for (size_t p = 0; !other && p < sizeof kPrefixes / sizeof kPrefixes[0]; ++p) {
			const size_t length = strlen(kPrefixes[p]);
			char         prefix[16];
			if (strlen(name) < length)
				continue;
			snprintf(prefix, sizeof prefix, "%.*s", (int)length, name);
			if (Xwau_SameName(prefix, kPrefixes[p]) && Xwau_SameName(name + length, "ReflectionRes")) {
				ReportUnsupported("the [SkyBoxes] key %sReflectionRes", p == 0 ? "AllRegions" : "Region<N>");
				break;
			}
		}
		if (other && (strcmp(other, "RenderMode") == 0 || strncmp(other, "Hangar", 6) == 0))
			ReportUnsupported("the [SkyBoxes] key %s", other);
	}
}

/* Return values of folder_index and definition_index that are not an index. */
enum { SKY_LEFT_OUT = -1, SKY_NO_MEMORY = -2 };

/* Returns the index of the cube folder `found`, adds it when it is new, and
 * marks it as used for a sky or an overlay. A folder whose name cannot be
 * encoded, or has the same name as another folder, makes the import fail. */
static long folder_index(SkyState* st, const char* found, bool overlay, const char* base) {
	for (size_t i = 0; i < st->folder_count; ++i) {
		if (strcmp(st->folders[i].path, found) == 0) {
			st->folders[i].sky |= !overlay;
			st->folders[i].overlay |= overlay;
			return (long)i;
		}
	}
	const char* slash = strrchr(found, '/');
	const char* name  = slash ? slash + 1 : found;
	char        key[128];
	if (!Xwau_EncodeName(name, key, sizeof key)) {
		ReportFatal(base, "[SkyBoxes]: the cube folder name %s is too long", name);
		return SKY_LEFT_OUT;
	}
	for (size_t i = 0; i < st->folder_count; ++i) {
		if (strcmp(st->folders[i].key, key) == 0) {
			ReportFatal(base, "[SkyBoxes]: the cube folders %s and %s get the same name", found,
						st->folders[i].path);
			return SKY_LEFT_OUT;
		}
	}
	if (!ArrayGrow((void**)&st->folders, st->folder_count, sizeof *st->folders))
		return SKY_NO_MEMORY;
	SkyFolder* f = &st->folders[st->folder_count];
	memset(f, 0, sizeof *f);
	snprintf(f->path, sizeof f->path, "%s", found);
	snprintf(f->key, sizeof f->key, "%s", key);
	f->sky     = !overlay;
	f->overlay = overlay;
	return (long)st->folder_count++;
}

/* Returns the index of the environment for a cube folder, its values and
 * its overlay, and adds it when it is new. The first one is named
 * xwau.<folder>, or xwau.<folder>.<overlay> with an overlay. The next ones
 * with the same name get .2, .3 and so on. */
static long definition_index(SkyState* st, size_t folder, float ambient_min, long overlay,
							 const float overlay_rotation[3], const char* base) {
	size_t same_name = 0;
	for (size_t i = 0; i < st->definition_count; ++i) {
		const SkyDefinition* d = &st->definitions[i];
		if (d->folder != folder || d->overlay != overlay)
			continue;
		if (d->ambient_min == ambient_min &&
			(overlay < 0 || memcmp(d->overlay_rotation, overlay_rotation, sizeof d->overlay_rotation) == 0))
			return (long)i;
		++same_name;
	}
	char base_name[300], name[64];
	snprintf(base_name, sizeof base_name, overlay < 0 ? "xwau.%s" : "xwau.%s.%s", st->folders[folder].key,
			 overlay < 0 ? "" : st->folders[overlay].key);
	const int length = same_name ? snprintf(name, sizeof name, "%s.%zu", base_name, same_name + 1)
								 : snprintf(name, sizeof name, "%s", base_name);
	if (length >= (int)sizeof name) {
		ReportFatal(base, "[SkyBoxes]: the environment name for %s is too long", st->folders[folder].path);
		return SKY_LEFT_OUT;
	}
	if (!ArrayGrow((void**)&st->definitions, st->definition_count, sizeof *st->definitions))
		return SKY_NO_MEMORY;
	SkyDefinition* d = &st->definitions[st->definition_count];
	d->folder        = folder;
	d->ambient_min   = ambient_min;
	d->overlay       = overlay;
	memcpy(d->overlay_rotation, overlay_rotation, sizeof d->overlay_rotation);
	snprintf(d->name, sizeof d->name, "%s", name);
	return (long)st->definition_count++;
}

/* The AllRegions values that a region without its own cube uses. */
typedef struct SkyAllRegions {
	const char* found;             /* the cube folder, or NULL when it does not load */
	const char* overlay_found;     /* the overlay folder, or NULL */
	float       angles[3];         /* RotationX, Y and Z */
	float       overlay_angles[3]; /* OverlayRotationX, Y and Z */
} SkyAllRegions;

/* Reads region `r`. The region uses its own cube folder when it loads,
 * otherwise the AllRegions one. The overlay follows the same rule. The
 * overlay angles are the region's when the region has its own cube, and the
 * AllRegions ones otherwise, as in PrimarySurface.cpp. Returns false when
 * out of memory. */
static bool read_region(SkyState* st, const XwauLines* lines, int r, const SkyAllRegions* all,
						const char* base, SkyRegion* region) {
	region->definition = -1;
	char key[16], written[512], found[1024], name[64];
	snprintf(key, sizeof key, "Region%d", r);
	XwauLines_Value(lines, key, XWAU_RULES_CPP, written, sizeof written);
	/* The wrapper ignores a folder that doesn't load. */
	const bool own = folder_loads(st->game, written, found, sizeof found);

	/* The wrapper tries the region's overlay only when Region<N> or
	 * Region<N>Illum is set. */
	char overlay[600], illum[600], own_overlay_found[1024];
	snprintf(name, sizeof name, "%sIllum", key);
	XwauLines_Value(lines, name, XWAU_RULES_CPP, illum, sizeof illum);
	snprintf(name, sizeof name, "%sOverlay", key);
	XwauLines_Value(lines, name, XWAU_RULES_CPP, overlay, sizeof overlay);
	if (!overlay[0] && written[0])
		snprintf(overlay, sizeof overlay, "%s_overlay", written);
	const bool  own_overlay   = (written[0] || illum[0]) &&
								folder_loads(st->game, overlay, own_overlay_found, sizeof own_overlay_found);
	const char* overlay_found = own_overlay ? own_overlay_found : all->overlay_found;

	if (!own && !all->found) {
		region->overlay_without_sky = overlay_found != NULL;
		return true;
	}
	const char* prefix = own ? key : "AllRegions";
	/* The importer doesn't convert these. The sky lights the models at full
	 * strength, and the environment has no place for the others. */
	static const char* const kNotImported[] = { "Specular", "AmbientInt", "Illum" };
	for (size_t k = 0; k < sizeof kNotImported / sizeof kNotImported[0]; ++k) {
		char value[600];
		snprintf(name, sizeof name, "%s%s", prefix, kNotImported[k]);
		XwauLines_Value(lines, name, XWAU_RULES_CPP, value, sizeof value);
		if (value[0])
			ReportUnsupported("the [SkyBoxes] key %s%s", own ? "Region<N>" : "AllRegions", kNotImported[k]);
	}
	snprintf(name, sizeof name, "%sAmbientMin", prefix);
	const float ambient_min = sky_float(lines, name, 0.01f);
	for (int axis = 0; axis < 3; ++axis) {
		snprintf(name, sizeof name, "%sRotation%c", key, 'X' + axis);
		region->rotation[axis] = own ? sky_float(lines, name, 0.0f) : all->angles[axis];
	}
	float overlay_rotation[3] = { 0.0f, 0.0f, 0.0f };
	for (int axis = 0; overlay_found && axis < 3; ++axis) {
		snprintf(name, sizeof name, "%sOverlayRotation%c", key, 'X' + axis);
		overlay_rotation[axis] =
			own ? sky_float(lines, name, region->rotation[axis]) : all->overlay_angles[axis];
	}
	const long folder = folder_index(st, own ? found : all->found, false, base);
	const long overlay_folder =
		folder < 0 || !overlay_found ? SKY_LEFT_OUT : folder_index(st, overlay_found, true, base);
	if (folder == SKY_NO_MEMORY || overlay_folder == SKY_NO_MEMORY)
		return false;
	const long definition = folder < 0 ? folder
									   : definition_index(st, (size_t)folder, ambient_min, overlay_folder,
														  overlay_rotation, base);
	if (definition == SKY_NO_MEMORY)
		return false;
	if (definition >= 0)
		region->definition = definition;
	return true;
}

/* Reads the [SkyBoxes] section of mission `dir`/`name`. Returns false when
 * out of memory; *found is false when the mission has no section. */
static bool read_mission(SkyState* st, const char* dir, const char* name, SkyMission* out, bool* found) {
	char base[256], rel[300], ini[1024];
	*found = false;
	if (!PathFormat(base, sizeof base, "%s/%s", dir, name) || !PathFormat(rel, sizeof rel, "%s.ini", base))
		return true;
	XwauLines lines = { 0 };
	if (!XwauLines_Read(Xwau_FindFile(st->game, rel, ini, sizeof ini) ? ini : NULL, "SkyBoxes",
						XWAU_RULES_CPP, &lines))
		return false;
	if (!lines.count) {
		XwauLines_Free(&lines);
		return true;
	}
	/* Leave out a mission that neither the mod nor the game has, before its
	   environments and folders are added. */
	char tie_rel[300], tie[1024];
	if (!PathFormat(tie_rel, sizeof tie_rel, "%s.tie", base) ||
		(!Xwau_FindFile(st->game, tie_rel, tie, sizeof tie) &&
		 !Xwau_FindFile(st->stock_game, tie_rel, tie, sizeof tie))) {
		XwauLines_Free(&lines);
		return true;
	}
	*found = true;
	memset(out, 0, sizeof *out);
	char lower_dir[64];
	LowerCopy(lower_dir, sizeof lower_dir, dir);
	snprintf(out->key, sizeof out->key, "%s/%s.tie", lower_dir, name);
	report_unknown_keys(&lines);
	read_list(&lines, "DisabledBackdrops", base, &out->disabled);
	read_list(&lines, "EnabledBackdrops", base, &out->enabled);

	char all_written[512], all_found[1024], overlay[600], overlay_found[1024];
	XwauLines_Value(&lines, "AllRegions", XWAU_RULES_CPP, all_written, sizeof all_written);
	const bool all_loads = folder_loads(st->game, all_written, all_found, sizeof all_found);
	XwauLines_Value(&lines, "AllRegionsOverlay", XWAU_RULES_CPP, overlay, sizeof overlay);
	if (!overlay[0] && all_written[0])
		snprintf(overlay, sizeof overlay, "%s_overlay", all_written);
	SkyAllRegions all = {
		.found = all_loads ? all_found : NULL,
		.overlay_found =
			folder_loads(st->game, overlay, overlay_found, sizeof overlay_found) ? overlay_found : NULL,
	};
	for (int axis = 0; axis < 3; ++axis) {
		char name[64];
		snprintf(name, sizeof name, "AllRegionsRotation%c", 'X' + axis);
		all.angles[axis] = sky_float(&lines, name, 0.0f);
		snprintf(name, sizeof name, "AllRegionsOverlayRotation%c", 'X' + axis);
		all.overlay_angles[axis] = sky_float(&lines, name, all.angles[axis]);
	}
	bool ok = true;
	for (int r = 0; ok && r < SKY_REGIONS; ++r)
		ok = read_region(st, &lines, r, &all, base, &out->regions[r]);
	XwauLines_Free(&lines);
	return ok;
}

static bool has_mission(const SkyState* st, const char* key) {
	for (size_t m = 0; m < st->mission_count; ++m)
		if (strcmp(st->missions[m].key, key) == 0)
			return true;
	return false;
}

/* Adds the Death Star missions of folder `dir` that the mod or the game has
 * and that have no [SkyBoxes]. The mod's .TIE is read when it has one.
 * Returns false when out of memory. */
static bool add_death_star_missions(SkyState* st, const char* dir) {
	const char* const installs[] = { st->game, st->stock_game };
	bool              ok         = true;
	for (size_t i = 0; ok && i < sizeof installs / sizeof installs[0]; ++i) {
		char folder[1024];
		if (!installs[i] || !Xwau_FindDir(installs[i], dir, folder, sizeof folder))
			continue;
		size_t count = 0;
		char** names = ListDir(folder, true, &count);
		for (size_t n = 0; ok && n < count; ++n) {
			if (!EndsWithNoCase(names[n], ".tie"))
				continue;
			char       rel[300], tie[1024];
			SkyMission mission;
			memset(&mission, 0, sizeof mission);
			mission.death_star = true;
			char lower_dir[64], lower_name[256];
			LowerCopy(lower_dir, sizeof lower_dir, dir);
			LowerCopy(lower_name, sizeof lower_name, names[n]);
			snprintf(mission.key, sizeof mission.key, "%s/%s", lower_dir, lower_name);
			if (has_mission(st, mission.key) || !PathFormat(rel, sizeof rel, "%s/%s", dir, names[n]) ||
				(!Xwau_FindFile(st->game, rel, tie, sizeof tie) &&
				 !Xwau_FindFile(st->stock_game, rel, tie, sizeof tie)) ||
				XwauTie_MissionType(tie) != XWAU_TIE_MISSION_TYPE_DEATH_STAR)
				continue;
			ok = ArrayGrow((void**)&st->missions, st->mission_count, sizeof *st->missions);
			if (ok) {
				st->missions[st->mission_count++] = mission;
				++st->death_star_count;
			}
		}
		FreeList(names, count);
	}
	return ok;
}

/* ===== Writing =========================================================== */

static void write_images(TextBuffer* f, const char* key, const SkyList* const* lists, size_t list_count) {
	TextAppend(f, "          %s: [", key);
	size_t written = 0;
	for (size_t l = 0; l < list_count; ++l) {
		for (size_t i = 0; i < lists[l]->count; ++i) {
			if (written)
				TextAppend(f, "%s", written % 8 ? ", " : ",\n            ");
			TextAppend(f, "\"%u-%u\"", lists[l]->images[i] >> 16, lists[l]->images[i] & 0xFFFFu);
			++written;
		}
	}
	TextAppend(f, "]\n");
}

/* The hide list of a mission: the starfield images, then DisabledBackdrops. */
static void hide_list(const SkyMission* m, SkyList* out, const char* base) {
	memset(out, 0, sizeof *out);
	for (size_t i = 0; i < sizeof kStarfieldImages / sizeof kStarfieldImages[0]; ++i)
		list_add(out, (uint32_t)kStarfieldImages[i][0] << 16 | kStarfieldImages[i][1], "the hide list", base);
	for (size_t i = 0; i < m->disabled.count; ++i)
		list_add(out, m->disabled.images[i], "the hide list", base);
}

static float clamp_value(float v, float max, const char* what, const char* name) {
	if (v >= 0.0f && v <= max)
		return v;
	const float clamped = v < 0.0f || v != v ? 0.0f : max;
	ReportFatal(name, "[SkyBoxes]: %s is %g, and the importer only takes 0 to %g", what, (double)v,
				(double)max);
	return clamped;
}

/* Writes the importer's environments into `f`, as entries of the
 * manifest's `environments` map. */
static void write_environments(const SkyState* st, TextBuffer* f) {
	if (st->death_star_count)
		TextAppend(f,
				   "  " SKY_DEATH_STAR_ENVIRONMENT ":\n"
				   "    intensity: 0\n"
				   "    ambient_fill: [%g, %g, %g]\n"
				   "    directional_lights: []\n",
				   SKY_DEATH_STAR_AMBIENT, SKY_DEATH_STAR_AMBIENT, SKY_DEATH_STAR_AMBIENT);
	for (size_t i = 0; i < st->definition_count; ++i) {
		const SkyDefinition* d       = &st->definitions[i];
		const char*          key     = st->folders[d->folder].key;
		//const float fill = clamp_value(d->ambient_min, 4.0f, "AmbientMin", d->name) * SKY_FILL_RADIANCE;
		const float fill = SKY_AMBIENT;
		TextAppend(f,
				   "  %s:\n"
				   "    intensity: %g\n"
				   "    radiance: \"flight/environments/xwau/%s/sky.ktx2\"\n"
				   "    diffuse: \"flight/environments/xwau/%s/diffuse.ktx2\"\n"
				   "    diffuse_strength: %g\n"
				   "    specular: \"flight/environments/xwau/%s/specular.ktx2\"\n"
				   "    specular_strength: %g\n"
				   "    ambient_fill: [%g, %g, %g]\n"
				   "    rotation_degrees: [0, 0, 0]\n"
				   "    directional_lights: []\n",
				   d->name, (double)SKY_RADIANCE, key, key, (double)SKY_LIGHT_STRENGTH, key,
				   (double)SKY_LIGHT_STRENGTH, (double)fill, (double)fill, (double)fill);
		if (d->overlay < 0)
			continue;
		TextAppend(f, "    overlay:\n      cube: \"flight/environments/xwau/%s/overlay.ktx2\"\n",
				   st->folders[d->overlay].key);
		if (d->overlay_rotation[0] != 0.0f || d->overlay_rotation[1] != 0.0f ||
			d->overlay_rotation[2] != 0.0f) {
			float degrees[3];
			XwauSkyboxes_Rotation(d->overlay_rotation, true, degrees);
			TextAppend(f, "      rotation_degrees: [%g, %g, %g]\n", (double)degrees[0], (double)degrees[1],
					   (double)degrees[2]);
		}
	}
}

/* Writes the importer's mission entries into `f`, as entries of the
 * manifest's `xwa.missions` map. */
static void write_missions(const SkyState* st, TextBuffer* f) {
	for (size_t m = 0; m < st->mission_count; ++m) {
		const SkyMission* mission = &st->missions[m];
		TextAppend(f, "    \"%s\":\n      regions:\n", mission->key);
		if (mission->death_star) {
			for (int r = 0; r < SKY_REGIONS; ++r)
				TextAppend(f, "        \"%d\": " SKY_DEATH_STAR_ENVIRONMENT "\n", r);
			continue;
		}
		SkyList hide;
		hide_list(mission, &hide, mission->key);
		for (int r = 0; r < SKY_REGIONS; ++r) {
			const SkyRegion* region = &mission->regions[r];
			if (region->definition < 0) {
				TextAppend(f, "        \"%d\": none\n", r);
				continue;
			}
			TextAppend(f, "        \"%d\":\n          environment: %s\n", r,
					   st->definitions[region->definition].name);
			if (region->rotation[0] != 0.0f || region->rotation[1] != 0.0f || region->rotation[2] != 0.0f) {
				float degrees[3];
				XwauSkyboxes_Rotation(region->rotation, false, degrees);
				TextAppend(f, "          rotation_degrees: [%g, %g, %g]\n", (double)degrees[0],
						   (double)degrees[1], (double)degrees[2]);
			}
			TextAppend(f, "          backdrops: keep\n          backdrop_light_scale: %g\n",
					   (double)SKY_BACKDROP_LIGHT_SCALE);
			/* EnabledBackdrops = ALL turns hiding off. */
			if (mission->enabled.all) {
				TextAppend(f, "          show_backdrops: all\n");
				continue;
			}
			if (mission->disabled.all) {
				TextAppend(f, "          hide_backdrops: all\n");
			} else {
				const SkyList* lists[] = { &hide };
				write_images(f, "hide_backdrops", lists, 1);
			}
			if (mission->enabled.count) {
				const SkyList* lists[] = { &mission->enabled };
				write_images(f, "show_backdrops", lists, 1);
			}
			TextAppend(f, "          hidden_backdrop_lights: remove\n");
		}
	}
}

/* ===== Updating the manifest ============================================ */

/* Writes the manifest `path` with the importer's `environments` and
 * `xwa.missions` entries. */
static bool write_manifest(const char* path, const char* environments, const char* missions) {
	TextBuffer text = { 0 };
	TextAppend(&text, "# Made by the OpenXWA mod importer.\n");
	TextAppend(&text, environments[0] ? "environments:\n%s" : "environments: {}\n%s", environments);
	TextAppend(&text, missions[0] ? "xwa:\n  missions:\n%s" : "xwa:\n  missions: {}\n%s", missions);
	const bool ok = !text.failed && text.text && WriteWholeFile(path, text.text, text.length);
	if (!ok)
		Report(path, text.failed ? "out of memory" : "cannot write");
	free(text.text);
	return ok;
}

static bool write_folders(const SkyState* st, const char* path) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "cannot write");
		return false;
	}
	for (size_t i = 0; i < st->folder_count; ++i) {
		const SkyFolder* folder = &st->folders[i];
		if (folder->sky)
			fprintf(f, "sky\t%s\txwau.%s\tflight/environments/xwau/%s\n", folder->path, folder->key,
					folder->key);
		if (folder->overlay)
			fprintf(f, "overlay\t%s\t-\tflight/environments/xwau/%s\n", folder->path, folder->key);
	}
	const bool ok = !ferror(f);
	if (fclose(f) != 0 || !ok) {
		Report(path, "cannot write");
		return false;
	}
	return true;
}

bool XwauSkyboxes_Write(const char* game, const char* stock_game, const char* manifest, const char* folders) {
	SkyState st = { .game = game, .stock_game = stock_game };
	bool     ok = true;
	for (size_t d = 0; ok && d < sizeof kMissionDirs / sizeof kMissionDirs[0]; ++d) {
		XwauLines names;
		ok = XwauMissions_Names(game, kMissionDirs[d], &names);
		for (size_t n = 0; ok && n < names.count; ++n) {
			SkyMission mission;
			bool       found = false;
			ok               = read_mission(&st, kMissionDirs[d], names.items[n], &mission, &found);
			if (!ok || !found)
				continue;
			ok = ArrayGrow((void**)&st.missions, st.mission_count, sizeof *st.missions);
			if (ok)
				st.missions[st.mission_count++] = mission;
		}
		XwauLines_Free(&names);
	}
	for (size_t d = 0; ok && d < sizeof kMissionDirs / sizeof kMissionDirs[0]; ++d)
		ok = add_death_star_missions(&st, kMissionDirs[d]);
	size_t with_sky = 0, without = 0, overlays = 0;
	for (size_t m = 0; m < st.mission_count; ++m) {
		if (st.missions[m].death_star)
			continue;
		for (int r = 0; r < SKY_REGIONS; ++r) {
			const SkyRegion* region = &st.missions[m].regions[r];
			with_sky += region->definition >= 0;
			without += region->definition < 0;
			overlays += region->definition >= 0 && st.definitions[region->definition].overlay >= 0;
			if (region->overlay_without_sky)
				ReportUnsupported("a [SkyBoxes] overlay in a region without a cube");
		}
	}
	if (!ok)
		Report(NULL, "out of memory (skyboxes)");
	TextBuffer environments = { 0 }, missions = { 0 };
	if (ok) {
		write_environments(&st, &environments);
		write_missions(&st, &missions);
		if (environments.failed || missions.failed) {
			Report(NULL, "out of memory (skyboxes)");
			ok = false;
		}
	}
	ok = ok &&
		 write_manifest(manifest, environments.text ? environments.text : "",
						missions.text ? missions.text : "") &&
		 write_folders(&st, folders);
	free(environments.text);
	free(missions.text);
	if (ok) {
		Report(NULL,
			   "wrote %s: %zu missions, %zu regions with a sky and %zu without, %zu environments for %zu "
			   "cube folders",
			   manifest, st.mission_count - st.death_star_count, with_sky, without, st.definition_count,
			   st.folder_count);
		if (st.death_star_count)
			Report(NULL,
				   "%zu Death Star missions use " SKY_DEATH_STAR_ENVIRONMENT
				   ", without the light of their backdrops",
				   st.death_star_count);
		if (overlays)
			Report(NULL, "%zu regions have an overlay", overlays);
	}
	free(st.folders);
	free(st.definitions);
	free(st.missions);
	return ok;
}
