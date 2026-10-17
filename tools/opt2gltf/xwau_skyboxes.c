#include "xwau_skyboxes.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "xwau_config.h"
#include "xwau_missions.h"

#define SKY_REGIONS 4
/* The game's limit on a hide or show list (XWA_ENV_BACKDROP_LIST_MAX). */
#define SKY_LIST_MAX 64
#define SKY_PI 3.14159265358979323846

static const char* const kMissionDirs[] = { "Missions", "Melee" };

/* The ddraw wrapper's starfield images, hidden under a cube
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
	char path[1024]; /* the folder found in the game folder */
	char key[128];   /* lower-case encoded name */
	bool sky, overlay; /* how the folder is used */
} SkyFolder;

typedef struct SkyDefinition {
	size_t folder;
	float  ambient_int, ambient_min;
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
	SkyRegion regions[SKY_REGIONS];
	SkyList   disabled, enabled;
} SkyMission;

typedef struct SkyState {
	const char*    game;
	SkyFolder*     folders;
	size_t         folder_count;
	SkyDefinition* definitions;
	size_t         definition_count;
	SkyMission*    missions;
	size_t         mission_count;
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

/* Whether the ddraw wrapper loads the cube folder `written`: it holds
 * exactly six files. */
static bool folder_loads(const char* game, const char* written, char* found, size_t cap) {
	if (!written[0] || !Xwau_FindDir(game, written, found, cap))
		return false;
	size_t count = 0, files = 0;
	char** names = ListDir(found, false, &count);
	for (size_t i = 0; i < count; ++i) {
		char path[1300];
		files +=
			names[i][0] != '.' && PathFormat(path, sizeof path, "%s/%s", found, names[i]) && IsFile(path);
	}
	FreeList(names, count);
	return files == 6;
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
		Report(base, "[SkyBoxes]: %s has more than %d images; image %u-%u is left out", what, SKY_LIST_MAX,
			   image >> 16, image & 0xFFFFu);
		return;
	}
	list->images[list->count++] = image;
}

/* Reads DisabledBackdrops or EnabledBackdrops as PopulateBackdropsMap does:
 * ';'-separated tokens, a token with '-' is an image read with atoi on each
 * side, and ALL or all ends the list with every image. */
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
			if (group < 0 || group > 0xFFFF || image < 0 || image > 0xFFFF)
				Report(base, "[SkyBoxes]: %s: %d-%d is not a DAT image; it is left out", key, group, image);
			else
				list_add(out, (uint32_t)group << 16 | (uint32_t)image, key, base);
		} else if (strcmp(token, "ALL") == 0 || strcmp(token, "all") == 0) {
			out->all = true;
			return;
		} else if (Xwau_Trim(token, XWAU_RULES_CPP)[0]) {
			Report(base, "[SkyBoxes]: %s: '%s' is ignored, as XWAU ignores it", key, token);
		}
		token = next;
	}
}

/* Reports the keys that the ddraw wrapper does not read. */
static void report_unknown_keys(const XwauLines* lines, const char* base) {
	static const char* const kPrefixes[] = { "AllRegions", "Region0", "Region1", "Region2", "Region3" };
	static const char* const kSuffixes[] = { "",
											 "Illum",
											 "Overlay",
											 "Specular",
											 "AmbientInt",
											 "AmbientMin",
											 "RotationX",
											 "RotationY",
											 "RotationZ",
											 "OverlayRotationX",
											 "OverlayRotationY",
											 "OverlayRotationZ",
											 "ReflectionRes" };
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
		bool known = false;
		for (size_t k = 0; !known && k < sizeof kOthers / sizeof kOthers[0]; ++k)
			known = Xwau_SameName(name, kOthers[k]);
		for (size_t p = 0; !known && p < sizeof kPrefixes / sizeof kPrefixes[0]; ++p) {
			const size_t length = strlen(kPrefixes[p]);
			char         prefix[16];
			if (strlen(name) < length)
				continue;
			snprintf(prefix, sizeof prefix, "%.*s", (int)length, name);
			for (size_t f = 0;
				 !known && Xwau_SameName(prefix, kPrefixes[p]) && f < sizeof kSuffixes / sizeof kSuffixes[0];
				 ++f)
				known = Xwau_SameName(name + length, kSuffixes[f]);
		}
		if (!known && name[0])
			Report(base, "[SkyBoxes]: unknown key '%s' is ignored, as XWAU ignores it", name);
	}
}

/* Results of folder_index and definition_index besides an index. */
enum { SKY_LEFT_OUT = -1, SKY_NO_MEMORY = -2 };

/* The index of the cube folder `found`, added when new, and marked as used
 * as a sky or an overlay. A folder whose name cannot be encoded, or that has
 * the name of another folder, is reported and left out. */
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
		Report(base, "[SkyBoxes]: the cube folder name %s is too long; it is left out", name);
		return SKY_LEFT_OUT;
	}
	for (size_t i = 0; i < st->folder_count; ++i) {
		if (strcmp(st->folders[i].key, key) == 0) {
			Report(base, "[SkyBoxes]: %s has the name of %s; it is left out", found, st->folders[i].path);
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

/* The index of the environment for a cube folder, its values and its
 * overlay, added when new: xwau.<folder>, or xwau.<folder>.<overlay> with an
 * overlay, for the first ones, then the same name with .2, .3 and so on. */
static long definition_index(SkyState* st, size_t folder, float ambient_int, float ambient_min, long overlay,
							 const float overlay_rotation[3], const char* base) {
	size_t same_name = 0;
	for (size_t i = 0; i < st->definition_count; ++i) {
		const SkyDefinition* d = &st->definitions[i];
		if (d->folder != folder || d->overlay != overlay)
			continue;
		if (d->ambient_int == ambient_int && d->ambient_min == ambient_min &&
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
		Report(base, "[SkyBoxes]: the environment name for %s is too long; it is left out",
			   st->folders[folder].path);
		return SKY_LEFT_OUT;
	}
	if (!ArrayGrow((void**)&st->definitions, st->definition_count, sizeof *st->definitions))
		return SKY_NO_MEMORY;
	SkyDefinition* d = &st->definitions[st->definition_count];
	d->folder        = folder;
	d->ambient_int   = ambient_int;
	d->ambient_min   = ambient_min;
	d->overlay       = overlay;
	memcpy(d->overlay_rotation, overlay_rotation, sizeof d->overlay_rotation);
	snprintf(d->name, sizeof d->name, "%s", name);
	return (long)st->definition_count++;
}

/* The AllRegions values that a region without its own cube uses. */
typedef struct SkyAllRegions {
	const char* found;               /* the cube folder, or NULL when it does not load */
	const char* overlay_found;       /* the overlay folder, or NULL */
	float       angles[3];           /* RotationX, Y and Z */
	float       overlay_angles[3];   /* OverlayRotationX, Y and Z */
} SkyAllRegions;

/* Reads region `r`: its own cube folder when it loads, else the AllRegions
 * one. The overlay is the region's own when it loads, else the AllRegions
 * one; its angles follow the cube, as in PrimarySurface.cpp. Returns false
 * when out of memory. */
static bool read_region(SkyState* st, const XwauLines* lines, int r, const SkyAllRegions* all, const char* base,
						SkyRegion* region) {
	region->definition = -1;
	char key[16], written[512], found[1024], name[64];
	snprintf(key, sizeof key, "Region%d", r);
	XwauLines_Value(lines, key, XWAU_RULES_CPP, written, sizeof written);
	const bool own = folder_loads(st->game, written, found, sizeof found);
	if (written[0] && !own)
		Report(base, "[SkyBoxes]: %s folder %s does not load (it needs exactly six files)", key, written);

	/* The wrapper tries the region's overlay only when Region<N> or
	 * Region<N>Illum is set. */
	char overlay[600], illum[600], own_overlay_found[1024];
	snprintf(name, sizeof name, "%sIllum", key);
	XwauLines_Value(lines, name, XWAU_RULES_CPP, illum, sizeof illum);
	snprintf(name, sizeof name, "%sOverlay", key);
	XwauLines_Value(lines, name, XWAU_RULES_CPP, overlay, sizeof overlay);
	if (!overlay[0] && written[0])
		snprintf(overlay, sizeof overlay, "%s_overlay", written);
	const bool  own_overlay = (written[0] || illum[0]) &&
							  folder_loads(st->game, overlay, own_overlay_found, sizeof own_overlay_found);
	const char* overlay_found = own_overlay ? own_overlay_found : all->overlay_found;

	if (!own && !all->found) {
		region->overlay_without_sky = overlay_found != NULL;
		return true;
	}
	const char* prefix = own ? key : "AllRegions";
	snprintf(name, sizeof name, "%sAmbientInt", prefix);
	const float ambient_int = sky_float(lines, name, own ? 0.15f : 0.50f);
	snprintf(name, sizeof name, "%sAmbientMin", prefix);
	const float ambient_min = sky_float(lines, name, 0.01f);
	for (int axis = 0; axis < 3; ++axis) {
		snprintf(name, sizeof name, "%sRotation%c", key, 'X' + axis);
		region->rotation[axis] = own ? sky_float(lines, name, 0.0f) : all->angles[axis];
	}
	float overlay_rotation[3] = { 0.0f, 0.0f, 0.0f };
	for (int axis = 0; overlay_found && axis < 3; ++axis) {
		snprintf(name, sizeof name, "%sOverlayRotation%c", key, 'X' + axis);
		overlay_rotation[axis] = own ? sky_float(lines, name, region->rotation[axis]) : all->overlay_angles[axis];
	}
	const long folder = folder_index(st, own ? found : all->found, false, base);
	const long overlay_folder =
		folder < 0 || !overlay_found ? SKY_LEFT_OUT : folder_index(st, overlay_found, true, base);
	if (folder == SKY_NO_MEMORY || overlay_folder == SKY_NO_MEMORY)
		return false;
	const long definition = folder < 0 ? folder
									   : definition_index(st, (size_t)folder, ambient_int, ambient_min,
														  overlay_folder, overlay_rotation, base);
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
	*found = true;
	memset(out, 0, sizeof *out);
	char lower_dir[64];
	LowerCopy(lower_dir, sizeof lower_dir, dir);
	snprintf(out->key, sizeof out->key, "%s/%s.tie", lower_dir, name);
	report_unknown_keys(&lines, base);
	read_list(&lines, "DisabledBackdrops", base, &out->disabled);
	read_list(&lines, "EnabledBackdrops", base, &out->enabled);

	char all_written[512], all_found[1024], overlay[600], overlay_found[1024];
	XwauLines_Value(&lines, "AllRegions", XWAU_RULES_CPP, all_written, sizeof all_written);
	const bool all_loads = folder_loads(st->game, all_written, all_found, sizeof all_found);
	if (all_written[0] && !all_loads)
		Report(base, "[SkyBoxes]: AllRegions folder %s does not load (it needs exactly six files)",
			   all_written);
	XwauLines_Value(&lines, "AllRegionsOverlay", XWAU_RULES_CPP, overlay, sizeof overlay);
	if (!overlay[0] && all_written[0])
		snprintf(overlay, sizeof overlay, "%s_overlay", all_written);
	SkyAllRegions all = {
		.found         = all_loads ? all_found : NULL,
		.overlay_found = folder_loads(st->game, overlay, overlay_found, sizeof overlay_found) ? overlay_found
																							   : NULL,
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

/* ===== Writing =========================================================== */

static void write_images(FILE* f, const char* key, const SkyList* const* lists, size_t list_count) {
	fprintf(f, "          %s: [", key);
	size_t written = 0;
	for (size_t l = 0; l < list_count; ++l) {
		for (size_t i = 0; i < lists[l]->count; ++i) {
			if (written)
				fprintf(f, written % 8 ? ", " : ",\n            ");
			fprintf(f, "\"%u-%u\"", lists[l]->images[i] >> 16, lists[l]->images[i] & 0xFFFFu);
			++written;
		}
	}
	fprintf(f, "]\n");
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
	Report(name, "[SkyBoxes]: %s %g is out of range; %g is used", what, (double)v, (double)clamped);
	return clamped;
}

static bool write_manifest(const SkyState* st, const char* path) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(path, "cannot write");
		return false;
	}
	fprintf(f, "# Written by opt2gltf missions from the [SkyBoxes] sections of XWAU's\n"
			   "# missions. Merge it into environments.yaml.\n"
			   "version: 1\n\nenvironments:\n");
	for (size_t i = 0; i < st->definition_count; ++i) {
		const SkyDefinition* d       = &st->definitions[i];
		const char*          key     = st->folders[d->folder].key;
		const float          diffuse = clamp_value(d->ambient_int, 4.0f, "AmbientInt", d->name);
		const float          fill    = clamp_value(d->ambient_min, 4.0f, "AmbientMin", d->name);
		fprintf(f,
				"  %s:\n"
				"    intensity: 1\n"
				"    radiance: \"flight/environments/xwau/%s/sky.ktx2\"\n"
				"    diffuse: \"flight/environments/xwau/%s/diffuse.ktx2\"\n"
				"    diffuse_strength: %g\n"
				"    specular: \"flight/environments/xwau/%s/specular.ktx2\"\n"
				"    specular_strength: 1\n"
				"    ambient_fill: [%g, %g, %g]\n"
				"    rotation_degrees: [0, 0, 0]\n"
				"    directional_lights: []\n",
				d->name, key, key, (double)diffuse, key, (double)fill, (double)fill, (double)fill);
		if (d->overlay < 0)
			continue;
		fprintf(f, "    overlay:\n      cube: \"flight/environments/xwau/%s/overlay.ktx2\"\n",
				st->folders[d->overlay].key);
		if (d->overlay_rotation[0] != 0.0f || d->overlay_rotation[1] != 0.0f || d->overlay_rotation[2] != 0.0f) {
			float degrees[3];
			XwauSkyboxes_Rotation(d->overlay_rotation, true, degrees);
			fprintf(f, "      rotation_degrees: [%g, %g, %g]\n", (double)degrees[0], (double)degrees[1],
					(double)degrees[2]);
		}
	}
	fprintf(f, "\nxwa:\n  missions:\n");
	for (size_t m = 0; m < st->mission_count; ++m) {
		const SkyMission* mission = &st->missions[m];
		SkyList           hide;
		hide_list(mission, &hide, mission->key);
		fprintf(f, "    \"%s\":\n      regions:\n", mission->key);
		for (int r = 0; r < SKY_REGIONS; ++r) {
			const SkyRegion* region = &mission->regions[r];
			if (region->definition < 0) {
				fprintf(f, "        \"%d\": none\n", r);
				continue;
			}
			fprintf(f, "        \"%d\":\n          environment: %s\n", r,
					st->definitions[region->definition].name);
			if (region->rotation[0] != 0.0f || region->rotation[1] != 0.0f || region->rotation[2] != 0.0f) {
				float degrees[3];
				XwauSkyboxes_Rotation(region->rotation, false, degrees);
				fprintf(f, "          rotation_degrees: [%g, %g, %g]\n", (double)degrees[0],
						(double)degrees[1], (double)degrees[2]);
			}
			fprintf(f, "          backdrops: keep\n");
			/* EnabledBackdrops = ALL turns hiding off. */
			if (mission->enabled.all) {
				fprintf(f, "          show_backdrops: all\n");
				continue;
			}
			if (mission->disabled.all) {
				fprintf(f, "          hide_backdrops: all\n");
			} else {
				const SkyList* lists[] = { &hide };
				write_images(f, "hide_backdrops", lists, 1);
			}
			if (mission->enabled.count) {
				const SkyList* lists[] = { &mission->enabled };
				write_images(f, "show_backdrops", lists, 1);
			}
		}
	}
	const bool ok = !ferror(f);
	if (fclose(f) != 0 || !ok) {
		Report(path, "cannot write");
		return false;
	}
	return true;
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
			fprintf(f, "sky\t%s\txwau.%s\tflight/environments/xwau/%s\n", folder->path, folder->key, folder->key);
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
	SkyState st           = { .game = game };
	bool     ok           = true;
	size_t   not_original = 0;
	for (size_t d = 0; ok && d < sizeof kMissionDirs / sizeof kMissionDirs[0]; ++d) {
		XwauLines names;
		ok = XwauMissions_Names(game, kMissionDirs[d], &names);
		for (size_t n = 0; ok && n < names.count; ++n) {
			SkyMission mission;
			bool       found = false;
			ok               = read_mission(&st, kMissionDirs[d], names.items[n], &mission, &found);
			if (!ok || !found)
				continue;
			char rel[300], stock[1024];
			if (!PathFormat(rel, sizeof rel, "%s/%s.tie", kMissionDirs[d], names.items[n]) ||
				!Xwau_FindFile(stock_game, rel, stock, sizeof stock)) {
				Report(rel, "[SkyBoxes]: the original game has no such mission; it is left out");
				++not_original;
				continue;
			}
			ok = ArrayGrow((void**)&st.missions, st.mission_count, sizeof *st.missions);
			if (ok)
				st.missions[st.mission_count++] = mission;
		}
		XwauLines_Free(&names);
	}
	size_t with_sky = 0, without = 0, overlays = 0;
	for (size_t m = 0; m < st.mission_count; ++m) {
		for (int r = 0; r < SKY_REGIONS; ++r) {
			const SkyRegion* region = &st.missions[m].regions[r];
			with_sky += region->definition >= 0;
			without += region->definition < 0;
			overlays += region->definition >= 0 && st.definitions[region->definition].overlay >= 0;
			if (region->overlay_without_sky)
				Report(st.missions[m].key,
					   "[SkyBoxes]: region %d has an overlay but no cube; an overlay needs a sky, so it is "
					   "left out",
					   r);
		}
	}
	if (!ok)
		Report(NULL, "out of memory (skyboxes)");
	ok = ok && write_manifest(&st, manifest) && write_folders(&st, folders);
	if (ok) {
		Report(NULL,
			   "wrote %s: %zu missions, %zu regions with a sky and %zu without, %zu environments for %zu "
			   "cube folders",
			   manifest, st.mission_count, with_sky, without, st.definition_count, st.folder_count);
		if (overlays)
			Report(NULL, "%zu regions have an overlay", overlays);
		if (not_original)
			Report(NULL, "%zu missions with [SkyBoxes] are not in the original game", not_original);
	}
	free(st.folders);
	free(st.definitions);
	free(st.missions);
	return ok;
}
