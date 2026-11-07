#include "xwau_flight_settings.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "classic_radiance.h"
#include "util.h"
#include "xwau_config.h"
#include "xwau_craft.h"
#include "xwau_mission_settings.h"
#include "xwau_object_limits.h"
#include "xwau_weapon_lock.h"

/* XWAU's headlight gives the same light at every distance, and OpenXWA's
 * falls with the square of the distance. They give the same light at this
 * distance: about the distance to the walls of the Death Star's tunnels,
 * which are 55 to 140 m wide. */
#define HEADLIGHTS_MATCH_DISTANCE_M 50.0

/* The headlight settings of ddraw's SSAO.cfg. */
typedef struct Headlights {
	bool   ssao_enabled, auto_turn_on, has_distance, has_ambient;
	double position[3], color[3], angle_degrees;
} Headlights;

/* Reads one setting of SSAO.cfg, as ddraw's LoadSSAOParams does. Keys are
 * found in any case. */
static void read_headlight_setting(const char* key, const char* value, void* data) {
	Headlights* out = data;
	double      number[3];
	if (strcasecmp(key, "ssao_enabled") == 0 && Xwau_ReadSettingNumbers(value, 1, number))
		out->ssao_enabled = number[0] != 0.0;
	else if (strcasecmp(key, "headlights_pos") == 0 || strcasecmp(key, "headlights_col") == 0) {
		/* Without three numbers, ddraw keeps its default. */
		if (Xwau_ReadSettingNumbers(value, 3, number))
			memcpy(strcasecmp(key, "headlights_pos") == 0 ? out->position : out->color, number,
				   sizeof number);
	} else if (strcasecmp(key, "headlights_angle") == 0 && Xwau_ReadSettingNumbers(value, 1, number))
		out->angle_degrees = number[0];
	else if (strcasecmp(key, "headlights_auto_turn_on") == 0 && Xwau_ReadSettingNumbers(value, 1, number))
		out->auto_turn_on = number[0] != 0.0;
	else if (strcasecmp(key, "headlights_distance") == 0)
		out->has_distance = true;
	else if (strcasecmp(key, "headlights_ambient") == 0)
		out->has_ambient = true;
}

/* Reads SSAO.cfg as ddraw's LoadSSAOParams does. Without the file, the
 * headlights are off. */
static void read_headlights(const char* game, Headlights* out) {
	/* ddraw's defaults. Its default angle is given by its cosine, 0.25. */
	*out = (Headlights) { .auto_turn_on  = true,
						  .position      = { 0.0, 0.0, 20.0 },
						  .color         = { 0.85, 0.85, 0.9 },
						  .angle_degrees = acos(0.25) / 0.01745 };
	Xwau_ReadSettings(game, "SSAO.cfg", read_headlight_setting, out);
}

/* Writes the `headlights` section, when the mod's ddraw draws headlights.
 * ddraw's colour holds the light's tint and its brightness, relative to the
 * game's white sun, which gives CLASSIC_ILLUMINANCE. The section has the
 * tint with its brightest channel at 1, and the power of a one-sided light,
 * as a Blender area light, that gives that brightness at
 * HEADLIGHTS_MATCH_DISTANCE_M: E = P / (pi d^2). ddraw's
 * distance (where its light ends) and ambient aren't written: they don't
 * apply to a light that falls with the distance. */
static bool write_headlights(FILE* f, const char* game) {
	char found[1024];
	if (!Xwau_FindFile(game, "ddraw.dll", found, sizeof found))
		return false;
	Headlights h;
	read_headlights(game, &h);
	double brightness = fmax(h.color[0], fmax(h.color[1], h.color[2]));
	if (!h.ssao_enabled || !(brightness > 0.0))
		return false;
	const double power = brightness * CLASSIC_ILLUMINANCE * 3.14159265358979 * HEADLIGHTS_MATCH_DISTANCE_M *
						 HEADLIGHTS_MATCH_DISTANCE_M;
	if (h.has_distance)
		ReportUnsupported("the SSAO.cfg key headlights_distance");
	if (h.has_ambient)
		ReportUnsupported("the SSAO.cfg key headlights_ambient");
	fprintf(f,
			"headlights:\n  position_m: [%g, %g, %g]\n  color: [%.3g, %.3g, %.3g]\n  power_w: %.0f\n"
			"  angle_degrees: %g\n  auto_turn_on: %s\n",
			h.position[0], h.position[1], h.position[2], h.color[0] / brightness, h.color[1] / brightness,
			h.color[2] / brightness, power, h.angle_degrees, h.auto_turn_on ? "true" : "false");
	return true;
}

/* The settings of ddraw's SSAO.cfg and Bloom.cfg that the `suns` section
 * uses. */
typedef struct Suns {
	bool  procedural_suns, ssao_enabled, bloom_enabled, has_mask;
	float flare_intensity;
} Suns;

/* Reads one setting of SSAO.cfg for the `suns` section, as ddraw's
 * LoadSSAOParams does: the value is read with atof, and a switch is on when
 * its value isn't 0. */
static void read_suns_setting(const char* key, const char* value, void* data) {
	Suns*       out    = data;
	const float number = (float)atof(value);
	if (strcasecmp(key, "procedural_suns") == 0)
		out->procedural_suns = number != 0.0f;
	else if (strcasecmp(key, "flare_intensity") == 0)
		out->flare_intensity = number;
	else if (strcasecmp(key, "ssao_enabled") == 0)
		out->ssao_enabled = number != 0.0f;
	else if (strcasecmp(key, "background_suns_strength") == 0 || strcasecmp(key, "lens_flare_strength") == 0)
		out->has_mask = true;
}

/* Reads bloom_enabled from Bloom.cfg, as ddraw's LoadBloomParams does. */
static void read_bloom_setting(const char* key, const char* value, void* data) {
	Suns* out = data;
	if (strcasecmp(key, "bloom_enabled") == 0)
		out->bloom_enabled = (float)atof(value) != 0.0f;
	else if (strcasecmp(key, "background_suns_strength") == 0 || strcasecmp(key, "lens_flare_strength") == 0)
		out->has_mask = true;
}

/* Finds the ddraw build that the game loads. XWAU's ddraw.dll can be a small
 * loader that loads DDraw_<type>.dll, with the type from DDrawType.cfg
 * ("Golden" or "Effects"). The loader holds the text "Golden", which is taken
 * as its default. */
static bool find_ddraw(const char* game, char* out, size_t cap) {
	if (!Xwau_FindFile(game, "ddraw.dll", out, cap))
		return false;
	if (!Xwau_FileHasText(out, "DDrawType.cfg"))
		return true;
	char      path[1024], type[64] = "", name[96];
	XwauLines lines = { 0 };
	if (Xwau_FindFile(game, "DDrawType.cfg", path, sizeof path) &&
		XwauLines_Read(path, "", XWAU_RULES_CPP, &lines))
		XwauLines_Value(&lines, "DDrawType", XWAU_RULES_CPP, type, sizeof type);
	XwauLines_Free(&lines);
	snprintf(name, sizeof name, "DDraw_%s.dll", type[0] ? type : "Golden");
	return Xwau_FindFile(game, name, out, cap);
}

/* Reports the models that ddraw takes for a 3D sun or a skydome, from the
 * words it looks for in their names. In a mission with such a model, ddraw
 * draws neither its procedural suns nor its flare. The game draws them in
 * every mission. */
static void report_sun_models(const char* game) {
	char dir[1024];
	if (!Xwau_FindDir(game, "FlightModels", dir, sizeof dir))
		return;
	static const char* const words[] = { "Sole", "Star3D", "Cielo", "Skydome" };
	size_t                   count   = 0;
	char**                   names   = ListDir(dir, true, &count);
	for (size_t i = 0; i < count; ++i) {
		if (!EndsWithNoCase(names[i], ".opt"))
			continue;
		for (size_t w = 0; w < sizeof words / sizeof words[0]; ++w) {
			if (strstr(names[i], words[w])) {
				ReportUnsupported("ddraw turning off its procedural suns for 3D sun and skydome models");
				break;
			}
		}
	}
	FreeList(names, count);
}

/* Writes the `suns` section, when the mod's ddraw draws procedural suns: its
 * procedural_suns isn't 0. Its flare also needs bloom or SSAO, and a
 * flare_intensity above 0.01. */
static bool write_suns(FILE* f, const char* game) {
	char dll[1024];
	if (!find_ddraw(game, dll, sizeof dll) || !Xwau_FileHasText(dll, "procedural_suns"))
		return false;
	/* ddraw's defaults. */
	Suns s = { .procedural_suns = true, .flare_intensity = 2.0f };
	Xwau_ReadSettings(game, "SSAO.cfg", read_suns_setting, &s);
	Xwau_ReadSettings(game, "Bloom.cfg", read_bloom_setting, &s);
	if (!s.procedural_suns)
		return false;
	report_sun_models(game);
	const bool flare = (s.ssao_enabled || s.bloom_enabled) && s.flare_intensity > 0.01f;
	fprintf(f, "suns:\n  procedural_sun: true\n  procedural_flare: %s\n", flare ? "true" : "false");
	if (flare && s.flare_intensity <= 10.0f)
		fprintf(f, "  flare_intensity: %.6g\n", (double)s.flare_intensity);
	else if (flare)
		ReportFatal("SSAO.cfg", "flare_intensity = %g is above the game's maximum of 10",
					(double)s.flare_intensity);
	if (s.has_mask)
		ReportUnsupported("the Bloom.cfg keys background_suns_strength and lens_flare_strength");
	return true;
}

/* The speed effect settings of ddraw's SSAO.cfg. */
typedef struct SpeedEffect {
	bool  enabled, has_look;
	float intensity, particles;
} SpeedEffect;

/* Reads one setting of SSAO.cfg for the `speed_effect` section, as ddraw's
 * LoadSSAOParams does. */
static void read_speed_effect_setting(const char* key, const char* value, void* data) {
	SpeedEffect* out    = data;
	const float  number = (float)atof(value);
	if (strcasecmp(key, "enable_speed_shader") == 0)
		out->enabled = number != 0.0f;
	else if (strcasecmp(key, "speed_shader_max_intensity") == 0)
		out->intensity = number;
	else if (strcasecmp(key, "speed_shader_max_particles") == 0)
		out->particles = fminf(fmaxf((float)(int)number, 0.0f), 256.0f);
	else if (strcasecmp(key, "speed_shader_scale_factor") == 0 ||
			 strcasecmp(key, "speed_shader_particle_size") == 0 ||
			 strcasecmp(key, "speed_shader_trail_length") == 0 ||
			 strcasecmp(key, "speed_shader_particle_range") == 0)
		out->has_look = true;
}

/* Writes the `speed_effect` section, when the mod's ddraw draws its speed
 * effect. */
static bool write_speed_effect(FILE* f, const char* game) {
	char dll[1024];
	if (!find_ddraw(game, dll, sizeof dll) || !Xwau_FileHasText(dll, "enable_speed_shader"))
		return false;
	/* ddraw's defaults. */
	SpeedEffect e = { .enabled = true, .intensity = 0.6f, .particles = 256.0f };
	Xwau_ReadSettings(game, "SSAO.cfg", read_speed_effect_setting, &e);
	if (!e.enabled)
		return false;
	if (e.has_look)
		ReportUnsupported("the SSAO.cfg keys speed_shader_scale_factor, speed_shader_particle_size, "
						  "speed_shader_trail_length and speed_shader_particle_range");
	/* ddraw adds its streaks to the final image, so its brightness doesn't
	   match OpenXWA's. The mod keeps OpenXWA's default. */
	if (e.intensity != 0.6f)
		ReportUnsupported("the SSAO.cfg key speed_shader_max_intensity");
	fprintf(f, "speed_effect:\n  particles: %.0f\n", (double)e.particles);
	return true;
}

/* Reads `key`, a percent of hook_weapon_rate's settings, for the
 * `weapons` section. */
static long long rate_percent(const XwauSection* settings, const char* key) {
	const long long percent = XwauSection_IntOr(settings, key, 100);
	if (percent < 0 || percent > INT32_MAX) {
		ReportFatal(settings->source, "%s = %lld is a negative percent, which the game can't use", key,
					percent);
		return 100;
	}
	return percent;
}

/* When the DLL `dll` reads the hook's factors `keys`, writes them as the map
 * `name`, with the names in `list` as keys. */
static void write_factors(FILE* f, const char* dll, const XwauSection* settings, const char* name,
						  const XwauNameList* list, const char* const* keys, const float* defaults,
						  int count) {
	if (!Xwau_FileHasText(dll, keys[0]))
		return;
	fprintf(f, "  %s: {", name);
	for (int i = 0; i < count; ++i)
		fprintf(f, "%s%s: %.9g", i ? ", " : "", XwauNames_Name(list, i),
				(double)XwauSection_FloatOr(settings, keys[i], defaults[i]));
	fputs("}\n", f);
}

/* Writes the global weapon profiles of hook_weapon_rate and the hook's settings
   for them. */
static void write_weapon_profile_settings(FILE* f, const XwauNames* names, const XwauStockValues* stock,
										  const char* game, const char* dll, const XwauSection* settings) {
	XwauWeaponStats global;
	if (XwauWeaponStats_Read(game, NULL, &global)) {
		XwauWeaponStats_Write(f, names, stock, "  ", &global);
		XwauWeaponStats_Free(&global);
	}
	static const char* const rank_keys[] = {
		"NoviceRankFireratioMultiplicator",  "OfficerRankFireratioMultiplicator",
		"VeteranRankFireratioMultiplicator", "AceRankFireratioMultiplicator",
		"TopAceRankFireratioMultiplicator",  "SuperAceRankFireratioMultiplicator"
	};
	static const float rank_defaults[] = { 4.0f, 3.0f, 2.0f, 2.0f, 1.0f, 1.0f };
	write_factors(f, dll, settings, "fire_ratio_by_rank", &names->ranks, rank_keys, rank_defaults, 6);
	static const char* const difficulty_keys[]     = { "DamagesMultiplicatorForDifficulty0",
													   "DamagesMultiplicatorForDifficulty1",
													   "DamagesMultiplicatorForDifficulty2" };
	static const float       difficulty_defaults[] = { 0.5f, 0.25f, 1.0f };
	write_factors(f, dll, settings, "damage_by_difficulty", &names->difficulties, difficulty_keys,
				  difficulty_defaults, 3);
	if (Xwau_FileHasText(dll, "LimitLinkedWarheadFireRate") &&
		XwauSection_IntOr(settings, "LimitLinkedWarheadFireRate", 0) != 0)
		fputs("  limit_linked_warhead_fire_rate: true\n", f);
	static const char* const linking_keys[]  = { "PreventAILasersIonWhenDisabling",
												 "PreventAIIonWhenNotDisabling",
												 "PreventPlayerLinkedLasersIon" };
	static const char* const linking_names[] = { "prevent_ai_lasers_ion_when_disabling",
												 "prevent_ai_ion_when_not_disabling",
												 "prevent_player_linked_lasers_ion" };
	bool                     open            = false;
	for (int i = 0; i < 3 && Xwau_FileHasText(dll, linking_keys[i]); ++i) {
		if (XwauSection_IntOr(settings, linking_keys[i], 0) == 0)
			continue;
		fprintf(f, "%s%s: true", open ? ", " : "  linking_rules: {", linking_names[i]);
		open = true;
	}
	if (open)
		fputs("}\n", f);
	if (Xwau_FileHasText(dll, "IsLaserLinksFixEnabled") &&
		XwauSection_IntOr(settings, "IsLaserLinksFixEnabled", 0))
		ReportUnsupported("the hook_weapon_rate key IsLaserLinksFixEnabled");
}

/* Writes the `weapons` section: the impact colours, when the mod installs
 * hook_weapon_color, and the laser rates, when it installs
 * hook_weapon_rate. */
static void write_weapons(FILE* f, const XwauNames* names, const XwauStockValues* stock, const char* game,
						  bool* impact, bool* rates) {
	uint32_t colors[XWAU_CRAFT_WEAPONS];
	char     path[1024];
	*impact = XwauCraft_ReadGlobalImpactColors(game, colors);
	*rates  = Xwau_FindFile(game, "hook_weapon_rate.dll", path, sizeof path);
	if (!*impact && !*rates)
		return;
	fputs("weapons:\n", f);
	if (*impact) {
		fputs("  impact: {", f);
		for (int weapon = 0; weapon < XWAU_CRAFT_WEAPONS; ++weapon)
			fprintf(f, "%s%s: 0x%06x", weapon ? ", " : "",
					XwauNames_WeaponType(names, XWAU_CRAFT_FIRST_WEAPON + weapon), colors[weapon]);
		fputs("}\n", f);
	}
	if (!*rates)
		return;
	XwauCraftWeaponRates defaults;
	XwauCraft_ReadGlobalWeaponRates(game, &defaults);
	XwauSection settings;
	XwauSection_ReadHook(game, "hook_weapon_rate", &settings);
	/* The hook's settings give the AI torpedo limits that the global
	   [WeaponRate] doesn't set. */
	static const char* const torpedo_keys[] = { "MaxTorpedoCountPerPass", "MaxTorpedoCountPerTarget" };
	for (int i = 0; i < 2; ++i) {
		const int key = i ? XWAU_CRAFT_TORPEDOES_PER_TARGET : XWAU_CRAFT_TORPEDOES_PER_PASS;
		if (defaults.value[key] >= 0 || !Xwau_FileHasText(path, torpedo_keys[i]))
			continue;
		const long long value = XwauSection_IntOr(&settings, torpedo_keys[i], -1);
		if (value < -1 || value > INT32_MAX)
			ReportFatal(settings.source, "%s = %lld is below -1, which the game can't use", torpedo_keys[i],
						value);
		else
			defaults.value[key] = (int)value;
	}
	XwauCraft_WriteWeaponRates(f, names, "  ", &defaults);
	if (Xwau_FileHasText(path, "DechargeRatePercent"))
		fprintf(f, "  decharge_percent: %lld\n  recharge_percent: %lld\n",
				rate_percent(&settings, "DechargeRatePercent"),
				rate_percent(&settings, "RechargeRatePercent"));
	write_weapon_profile_settings(f, names, stock, game, path, &settings);
	XwauSection_Free(&settings);
}

/* Writes the `shield_recharge` section, when the mod installs hook_shield:
 * its hook_shield.cfg, else the [hook_shield] section of Hooks.ini. */
static bool write_shield_recharge(FILE* f, const char* game) {
	if (!Xwau_DllHasText(game, "hook_shield.dll", "RechargeRatePercent"))
		return false;
	XwauSection settings;
	XwauSection_ReadHook(game, "hook_shield", &settings);
	long long percent = XwauSection_IntOr(&settings, "RechargeRatePercent", 100);
	if (percent < 0) {
		ReportFatal(settings.source,
					"RechargeRatePercent = %lld is a negative percent, which the game can't use", percent);
		percent = 100;
	}
	fprintf(f,
			"shield_recharge:\n  starships: %s\n  starships_on_easy: %s\n  double_starfighter_shields: %s\n"
			"  rate_percent: %lld\n",
			XwauSection_IntOr(&settings, "IsShieldRechargeForStarshipsEnabled", 1) ? "true" : "false",
			XwauSection_IntOr(&settings, "IsShieldRechargeForStarshipsOnEasyDifficultyEnabled", 0) ? "true"
																								   : "false",
			XwauSection_IntOr(&settings, "IsShieldStrengthForStarfighterDoubled", 0) ? "true" : "false",
			percent);
	XwauSection_Free(&settings);
	return true;
}

/* Writes the `docking` section, when the mod installs hook_dock. The hook's
 * ForceDockElevation (hook_dock.cfg, else Hooks.ini) isn't imported. */
static bool write_docking(FILE* f, const char* game) {
	char path[1024];
	if (!Xwau_FindFile(game, "hook_dock.dll", path, sizeof path))
		return false;
	XwauSection settings;
	XwauSection_ReadHook(game, "hook_dock", &settings);
	const long long force = XwauSection_IntOr(&settings, "ForceDockElevation", -1);
	if (force >= 0)
		ReportUnsupported("the hook_dock key ForceDockElevation");
	XwauSection_Free(&settings);
	fputs("docking:\n  dock_to_small_sets_big: true\n  ai_offsets_own_dock_point: true\n"
		  "  carried_offsets_carrier_dock_point: true\n  player_approach_height: true\n"
		  "  player_snaps_to_target_orientation: true\n",
		  f);
	return true;
}

/* The settings of Hook_CockpitLook that the `camera` section uses. */
typedef struct CockpitLook {
	bool  cockpit_inertia_enabled, external_inertia_enabled, inertia_in_hangar, inertia_in_map;
	float cockpit_inertia, cockpit_max_inertia, cockpit_speed_inertia;
	float external_inertia, external_max_inertia, external_dist_inertia, external_tilt;
	float units_to_meters;
} CockpitLook;

/* Reads one setting of CockpitLook.cfg, as the hook's LoadParams does: keys
 * are found in any case, the value is read with atof, and a switch is on
 * when its value isn't 0. */
static void read_cockpit_look_setting(const char* key, const char* value, void* data) {
	CockpitLook* out    = data;
	const float  number = (float)atof(value);
	if (strcasecmp(key, "cockpit_inertia_enabled") == 0)
		out->cockpit_inertia_enabled = number != 0.0f;
	else if (strcasecmp(key, "cockpit_inertia") == 0)
		out->cockpit_inertia = number;
	else if (strcasecmp(key, "cockpit_max_inertia") == 0)
		out->cockpit_max_inertia = number;
	else if (strcasecmp(key, "cockpit_speed_inertia") == 0)
		out->cockpit_speed_inertia = number;
	else if (strcasecmp(key, "external_inertia_enabled") == 0)
		out->external_inertia_enabled = number != 0.0f;
	else if (strcasecmp(key, "enable_external_inertia_in_hangar") == 0)
		out->inertia_in_hangar = number != 0.0f;
	else if (strcasecmp(key, "external_inertia") == 0)
		out->external_inertia = number;
	else if (strcasecmp(key, "enable_external_inertia_in_map") == 0)
		out->inertia_in_map = number != 0.0f;
	else if (strcasecmp(key, "external_max_inertia") == 0)
		out->external_max_inertia = number;
	else if (strcasecmp(key, "external_dist_inertia") == 0)
		out->external_dist_inertia = number;
	else if (strcasecmp(key, "external_tilt") == 0)
		out->external_tilt = number;
	else if (strcasecmp(key, "xwa_units_to_meters_scale") == 0)
		out->units_to_meters = number;
}

/* Writes the number `key` of the `camera` section. The game ignores a value
 * outside the key's range, so such a value isn't written, and the game's
 * own value applies. That makes the import fail when the hook `uses` the
 * value. */
static void write_camera_number(FILE* f, const char* key, double value, double min, double max, bool used) {
	if (!(value >= min && value <= max)) {
		if (used)
			ReportFatal("CockpitLook.cfg", "camera.%s would be %g, outside the game's range of %g to %g", key,
						value, min, max);
		return;
	}
	fprintf(f, "  %s: %.6g\n", key, value);
}

/* Writes the `camera` section, when the mod installs Hook_CockpitLook.dll or
 * hook_exterior.dll. */
static bool write_camera(FILE* f, const char* game) {
	char       path[1024];
	const bool look     = Xwau_FindFile(game, "Hook_CockpitLook.dll", path, sizeof path);
	const bool exterior = Xwau_FindFile(game, "hook_exterior.dll", path, sizeof path);
	if (!look && !exterior)
		return false;
	fputs("camera:\n", f);
	if (!look) {
		fputs("  exterior_lag: false\n  exterior_tilt: 0\n  cockpit_sway: false\n  exterior_hud: true\n", f);
		return true;
	}
	/* The hook's defaults. */
	CockpitLook c = { .cockpit_inertia       = 0.35f,
					  .cockpit_max_inertia   = 0.2f,
					  .cockpit_speed_inertia = 0.005f,
					  .external_inertia      = 1.0f,
					  .external_max_inertia  = 0.025f,
					  .external_tilt         = -10.0f,
					  .units_to_meters       = 25.0f };
	Xwau_ReadSettings(game, "CockpitLook.cfg", read_cockpit_look_setting, &c);
	/* These only change the camera when its lag is on. */
	if (c.external_inertia_enabled) {
		if (c.external_dist_inertia != 0.0f)
			ReportUnsupported("the CockpitLook.cfg key external_dist_inertia");
		if (c.inertia_in_hangar)
			ReportUnsupported("the CockpitLook.cfg key enable_external_inertia_in_hangar");
		if (c.inertia_in_map)
			ReportUnsupported("the CockpitLook.cfg key enable_external_inertia_in_map");
	}
	/* The hook turns the camera by cockpit_inertia * external_inertia * 16384
	 * Q16 units for each unit of lag, which is pi / 2 radians, and limits
	 * the turn to external_max_inertia * external_inertia * 16384, which is
	 * 90 degrees. Its eye offsets are in its own units, which
	 * xwa_units_to_meters_scale turns into game units, written here in
	 * metres. */
	const double lag = (double)c.cockpit_inertia * c.external_inertia;
	fprintf(f, "  exterior_lag: %s\n", c.external_inertia_enabled ? "true" : "false");
	const bool ext  = c.external_inertia_enabled;
	const bool sway = c.cockpit_inertia_enabled;
	write_camera_number(f, "exterior_lag_strength", lag * 3.14159265358979 / 2.0, 0.0, 2.0, ext);
	write_camera_number(f, "exterior_lag_max", (double)c.external_max_inertia * c.external_inertia * 90.0,
						0.0, 15.0, ext);
	write_camera_number(f, "exterior_tilt", -(double)c.external_tilt, -30.0, 30.0, true);
	fprintf(f, "  cockpit_sway: %s\n", sway ? "true" : "false");
	write_camera_number(f, "cockpit_sway_strength_m",
						Xwau_Metres((double)c.units_to_meters * c.cockpit_inertia), 0.0, 0.5, sway);
	/* The hook's speed sway is for each speed unit, and the key's for each
	   MGLT. */
	write_camera_number(f, "cockpit_speed_sway_m",
						Xwau_Metres((double)c.units_to_meters * c.cockpit_speed_inertia) /
							Xwau_FromGame(XWAU_UNIT_MGLT, 1.0),
						0.0, 0.05, sway);
	write_camera_number(f, "cockpit_sway_max_m",
						Xwau_Metres((double)c.units_to_meters * c.cockpit_max_inertia), 0.0, 0.25, sway);
	fprintf(f, "  exterior_hud: %s\n", exterior ? "true" : "false");
	return true;
}

bool XwauFlightSettings_Write(const char* game, const XwauNames* names, const XwauStockValues* stock,
							  const char* path) {
	char               found[1024];
	const bool         extra_planets = Xwau_FindFile(game, "hook_backdrops.dll", found, sizeof found);
	XwauSettingsConfig config;
	if (!XwauSettingsConfig_Read(game, &config)) {
		XwauSettingsConfig_Free(&config);
		return false;
	}
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(NULL, "cannot write %s", path);
		XwauSettingsConfig_Free(&config);
		return false;
	}
	fputs("# Made by the OpenXWA mod importer.\n", f);
	if (extra_planets)
		fputs("backdrops:\n  extra_planets:\n    - { ids: [104, 255], image_groups: [6304, 6455] }\n", f);
	const bool settings   = XwauMissionSettings_WriteDefaults(f, &config);
	const bool headlights = write_headlights(f, game);
	bool       impact, rates;
	write_weapons(f, names, stock, game, &impact, &rates);
	const bool shields    = write_shield_recharge(f, game);
	const bool docking    = write_docking(f, game);
	const bool limits     = XwauObjectLimits_Write(f, game);
	const bool camera     = write_camera(f, game);
	const bool suns       = write_suns(f, game);
	const bool speed      = write_speed_effect(f, game);
	bool       ai_attack, mag_pulse, warhead_lock;
	const bool weapon_lock_ok =
		XwauWeaponLock_WriteFlightSettings(f, game, names, &ai_attack, &mag_pulse, &warhead_lock);
	XwauSettingsConfig_Free(&config);
	const bool        ok        = !ferror(f) && fclose(f) == 0 && weapon_lock_ok;
	char              what[300] = "";
	const char* const parts[]   = { extra_planets ? "backdrop ids 104 to 255 are planets" : NULL,
									settings ? "the mission rules and defaults" : NULL,
									headlights ? "the headlights" : NULL,
									impact ? "the impact colours" : NULL,
									rates ? "the laser rates" : NULL,
									shields ? "the shield recharge rules" : NULL,
									docking ? "the docking rules" : NULL,
									limits ? "the object counts" : NULL,
									ai_attack ? "the AI attack ranges" : NULL,
									mag_pulse ? "the mag pulse hits" : NULL,
									warhead_lock ? "the AI warhead lock ranges" : NULL,
									camera ? "the camera" : NULL,
									suns ? "the suns" : NULL,
									speed ? "the speed effect" : NULL };
	for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i) {
		if (parts[i])
			snprintf(what + strlen(what), sizeof what - strlen(what), "%s%s", what[0] ? ", " : "", parts[i]);
	}
	if (!ok)
		Report(NULL, "cannot write %s", path);
	else if (what[0])
		Report(NULL, "wrote %s: %s", path, what);
	else
		Report(NULL, "%s has no flight settings", path);
	return ok;
}
