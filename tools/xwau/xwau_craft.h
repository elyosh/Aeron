#ifndef AERON_XWAU_CRAFT_H
#define AERON_XWAU_CRAFT_H

/*
 * Writes the model settings of an XWAU installation (the mod importer's
 * craft phase):
 *   - <output_dir>/models/<model key>.yaml holds the settings of one model,
 *     next to its GLB. The key is the name of the model's INI file in lower
 *     case, such as awing for FlightModels/Awing.ini. A model without any
 *     setting gets no file.
 *   - <output_dir>/hud.yaml holds the hull and map icons, and how the target
 *     display shows the target.
 *
 *   hud:
 *     reticle: {laser: 135, laser_in_range: 136}
 *     colors: {inner: 0xff5b5b5b, border: 0xffe5e5e5}
 *   engine_glows:
 *     6: {min_speed_mglt: 40}
 *   weapons:
 *     switch_by_iff: all
 *     rates: {decharge: 2, recharge: 3, cooldown: 47}
 *     colors:
 *       variant: {default: 3, ion_cannon: 1}
 *       impact: {rebel_laser: 0xff0000}
 *     by_markings:
 *       1: {variant: {default: 2}}
 *   shield: {per_generator_rate: 5, player_rate: 40, delay_seconds: 30}
 *   component_strength: {gun_turret: 18, shield_generator: 36}
 *   warhead_lock: {lock_ticks: 354, lock_range_m: {small: 2500}}
 *   jamming: {enabled: true, direction: down, range_m: none, rule: hangar_point}
 *   docking: {approach_height_m: 0}
 *   sound: {interior: awing, fly_by: slave, ambient: {offset_m: [0, 300, 0]}}
 *   cockpit_view: {wide_vertical_fov: 61.59}
 *
 * Each group comes from FlightModels/<model><Name>.txt, or else from the
 * [<Name>] section of the model's INI, and is read the way the XWAU hook or
 * ddraw reads it. The names are Reticle, HUDColor, EngineGlows, WeaponColor,
 * WeaponRate, Shield, SubcomponentsStrength, Turrets (xwau_gunner_seats.h),
 * WarheadLock, Jamming, Dock and Sound. A section that needs a hook is only
 * read when the installation has that hook. The weapon profiles of
 * [WeaponStats], [WarheadStats], [EnergyStats], [LinkingStats] and
 * [WeaponHardpointTypes] go into `weapons` too (xwau_weapon_stats.h). The
 * stats profiles are described in xwau_stats_profiles.h.
 *
 * Some values need more than a copy:
 *   - The weapon colour keys with _fg<m>_ go into `by_markings`. The group
 *     keys WeaponImpactColor_3100 to _3500 are expanded to weapon types.
 *   - hook_weapon_rate only reads a model's [WeaponRate] when the global one
 *     is empty, except for the energy transfer keys. So when the global one
 *     has lines, only the model's energy transfer keys are written. The
 *     global values go into flight/flight.yaml (xwau_flight_settings.h).
 *   - `jamming` always has all four keys, because the hook's default for a
 *     missing key isn't the game's stock rule. A jamming distance below -1
 *     never jams, so it gives `enabled: false`.
 *   - The lock ranges, the jamming distance and the docking approach height
 *     are written in metres.
 *   - `docking` gets 22.75 metres (932 game units) when the [Dock] section
 *     has no DockElevation, as in the hook.
 *   - The ambient sound distances are written as right, up and forward in
 *     metres. An axis without a distance is null, and the game then uses the
 *     craft's size.
 *   - `cockpit_view` comes from xwahacker_large_fov in the model's .dc file,
 *     as the vertical field of view that XWAU shows. The regular field of
 *     view goes into the camera of the model's pilot eye instead.
 *
 * hud.yaml gets `hull_icons` and `map_icons` from HullIconList.txt and
 * MapIconList.txt. They only list the icons that differ from stock, as image
 * numbers: XWAU's id 3100 is image 31, and id 0 is `none`. `target_display`
 * comes from the wireframe keys of Dynamic_Cockpit.cfg and from
 * GreenAndRedForIFFColorsOnly of hook_tourmultiplayer. Its colours are
 * written as linear values.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "xwau_config.h"
#include "xwau_game_tables.h"
#include "xwau_names.h"
#include "xwau_weapon_stats.h"

/* The reticle slots of XWAU's reticle hook. */
#define XWAU_CRAFT_RETICLE_SLOTS 23

typedef struct XwauCraftHud {
	bool     has_reticle[XWAU_CRAFT_RETICLE_SLOTS];
	int      reticle[XWAU_CRAFT_RETICLE_SLOTS]; /* the image position, counted from 1 */
	bool     has_inner;
	bool     has_border;
	uint32_t inner; /* ARGB */
	uint32_t border;
} XwauCraftHud;

/* The engine glows of a model: XWAU reads glows 0 to 255. */
#define XWAU_CRAFT_ENGINE_GLOWS 256

typedef struct XwauCraftEngineGlows {
	bool has[XWAU_CRAFT_ENGINE_GLOWS];         /* a line for the glow, even without a limit */
	int  min_percent[XWAU_CRAFT_ENGINE_GLOWS]; /* 0: no limit */
	int  min_mglt[XWAU_CRAFT_ENGINE_GLOWS];    /* 0: no limit */
} XwauCraftEngineGlows;

/* The weapon types that XWAU colours: 280 to 307. */
#define XWAU_CRAFT_FIRST_WEAPON 280
#define XWAU_CRAFT_WEAPONS 28

enum {
	XWAU_CRAFT_LASER_HIGH,
	XWAU_CRAFT_LASER_LOW,
	XWAU_CRAFT_ION_HIGH,
	XWAU_CRAFT_ION_LOW,
	XWAU_CRAFT_BARS
};

/* The weapon colours for any markings, or the changes for one markings
 * value. Index i of the arrays is weapon type 280 + i. */
typedef struct XwauCraftWeaponColors {
	bool     has_default;
	uint8_t  default_variant;
	bool     has_variant[XWAU_CRAFT_WEAPONS];
	uint8_t  variant[XWAU_CRAFT_WEAPONS];
	bool     has_impact[XWAU_CRAFT_WEAPONS];
	uint32_t impact[XWAU_CRAFT_WEAPONS]; /* RGB; 0: the stock white mark */
	bool     has_bar[XWAU_CRAFT_BARS];
	uint32_t bar[XWAU_CRAFT_BARS]; /* ARGB */
} XwauCraftWeaponColors;

/* The [Shield] section of a model. */
typedef struct XwauCraftShield {
	bool      has; /* the model has the section */
	bool      per_generator;
	long long rate; /* per live generator when per_generator */
	long long player_rate;
	long long delay_seconds;
} XwauCraftShield;

/* The laser rates of a [WeaponRate] section, in the order of the keys
 * above. -1 means not set. */
/* The [WeaponRate] keys, in the order of xwau_craft.c's table. */
enum {
	XWAU_CRAFT_TORPEDOES_PER_PASS   = 9,
	XWAU_CRAFT_TORPEDOES_PER_TARGET = 10,
	XWAU_CRAFT_IMPACT_SPIN          = 11,
	XWAU_CRAFT_WEAPON_RATES         = 19
};

typedef struct XwauCraftWeaponRates {
	int value[XWAU_CRAFT_WEAPON_RATES];
} XwauCraftWeaponRates;

typedef struct XwauCraftWeapons {
	XwauCraftWeaponColors colors;
	XwauCraftWeaponRates  rates;
	bool                  has_markings[256];
	XwauCraftWeaponColors by_markings[256];
	int                   switch_by_iff; /* WeaponSwitchBasedOnIff: 1 or 0, -1 when not set */
	XwauWeaponStats       stats;         /* the weapon profiles */
} XwauCraftWeapons;

/* The ARGB colour that XWAU's wrapper makes from red, green and blue values
 * from 0 to 1: 0xff << 24 | uint32_t(r * 255) << 16 | uint32_t(g * 255) << 8 |
 * uint32_t(b * 255). The products are truncated, not rounded. */
uint32_t XwauCraft_HudColor(float r, float g, float b);

/* Reads the reticle of a model: `reticle_txt` (<model>Reticle.txt) when it
 * has lines, else the [Reticle] section of `ini`. Either path can be NULL. */
void XwauCraft_ReadReticle(const char* reticle_txt, const char* ini, XwauCraftHud* hud);

/* Reads the [HUDColor] section of `ini`. */
void XwauCraft_ReadHudColors(const char* ini, XwauCraftHud* hud);

/* Reads the engine glows of a model: `glows_txt` (<model>EngineGlows.txt)
 * when it has lines, else the [EngineGlows] section of `ini`. Either path
 * can be NULL. A line with a value that isn't a number, or with a glow
 * outside 0 to 255, is skipped. */
void XwauCraft_ReadEngineGlows(const char* glows_txt, const char* ini, XwauCraftEngineGlows* glows);

/* Reads the weapon colours of a model: `weapons_txt`
 * (<model>WeaponColor.txt) when it has lines, else the [WeaponColor]
 * section of `ini`. Either path can be NULL. A value that isn't a number is
 * ignored. */
void XwauCraft_ReadWeapons(const char* weapons_txt, const char* ini, XwauCraftWeapons* weapons);

/* Reads the [Shield] section of a model: `shield_txt` (<model>Shield.txt)
 * when it has lines, else the [Shield] section of `ini`. Either path can be
 * NULL. */
void XwauCraft_ReadShield(const char* shield_txt, const char* ini, XwauCraftShield* shield);

enum { XWAU_CRAFT_MESH_TYPES = 32 };

/* The [SubcomponentsStrength] section of a model. -1 keeps the game's
 * strength. */
typedef struct XwauCraftComponents {
	bool has;
	int  strength[XWAU_CRAFT_MESH_TYPES];
} XwauCraftComponents;

/* Reads the [SubcomponentsStrength] section of a model:
 * `strength_txt` (<model>SubcomponentsStrength.txt) when it has lines, else
 * the section of `ini`. Either path can be NULL. */
void XwauCraft_ReadComponents(const char* strength_txt, const char* ini, XwauCraftComponents* components);

/* The keys of [WarheadLock], in this order: LockDistance0 to 2, LockTime,
 * LockStrengthDivisor, AILockDistance0 to 2 and AILockTimeFactor. */
enum { XWAU_CRAFT_WARHEAD_LOCK_KEYS = 9 };

/* The [WarheadLock] section of a model. -1 keeps the game's value. */
typedef struct XwauCraftWarheadLock {
	bool has;
	int  value[XWAU_CRAFT_WARHEAD_LOCK_KEYS];
} XwauCraftWarheadLock;

/* Reads the [WarheadLock] section of a model: `lock_txt`
 * (<model>WarheadLock.txt) when it has lines, else the section of `ini`.
 * Either path can be NULL. When the section has lines, hook_weapon_lock
 * uses 708 for a LockTime that the section doesn't set, even for the
 * Missile Boat, whose stock value is 354. So LockTime is then always set.
 * The keys with a profile name, such as LockTime_<profile>, aren't
 * imported. A value out of range makes the import fail, except a value that
 * would crash XWAU, which is left out. */
void XwauCraft_ReadWarheadLock(const char* lock_txt, const char* ini, XwauCraftWarheadLock* lock);
/* Writes the `warhead_lock` group of `lock`. */
void XwauCraft_WriteWarheadLock(FILE* f, const XwauCraftWarheadLock* lock);

/* The [Jamming] section of a model. `direction` and `rule` index the names
 * of the model file; `range` is -1 for none. */
typedef struct XwauCraftJamming {
	bool has;
	bool enabled;
	int  direction;
	int  range;
	int  rule;
} XwauCraftJamming;

/* Reads the [Jamming] section of a model: `jamming_txt`
 * (<model>Jamming.txt) when it has lines, else the section of `ini`.
 * Either path can be NULL. A HasJamming or JammingDistance that isn't a
 * number would crash XWAU, so the section is then left out. */
void XwauCraft_ReadJamming(const char* jamming_txt, const char* ini, XwauCraftJamming* jamming);
/* Writes the `jamming` group of `jamming`. */
void XwauCraft_WriteJamming(FILE* f, const XwauCraftJamming* jamming);

/* The [Dock] section of a model: the cap of the player's docking approach
 * height above it. */
typedef struct XwauCraftDock {
	bool has;
	int  approach_height;
} XwauCraftDock;

/* Reads the [Dock] section of a model: `dock_txt` (<model>Dock.txt) when it
 * has lines, else the section of `ini`. Either path can be NULL. When the
 * section has lines, a missing DockElevation is 932, as in the hook. A
 * DockElevation that isn't a number would crash XWAU, so the section is
 * then left out. */
void XwauCraft_ReadDock(const char* dock_txt, const char* ini, XwauCraftDock* dock);
/* Writes the `docking` group of `dock`. */
void XwauCraft_WriteDock(FILE* f, const XwauCraftDock* dock);

/* Reads the laser rates of a model: `rate_txt` (<model>WeaponRate.txt)
 * when it has lines, else the [WeaponRate] section of `ini`. Either path
 * can be NULL. */
void XwauCraft_ReadWeaponRates(const char* rate_txt, const char* ini, XwauCraftWeaponRates* rates);

/* Reads the global laser rates of the installation `game`:
 * FlightModels/WeaponRate.txt when it has lines, else the [WeaponRate]
 * section of FlightModels/default.ini. Returns whether they have lines, in
 * which case hook_weapon_rate ignores the models' own sections. */
bool XwauCraft_ReadGlobalWeaponRates(const char* game, XwauCraftWeaponRates* rates);

/* Whether `rates` sets a value. */
bool XwauCraft_HasWeaponRates(const XwauCraftWeaponRates* rates);

/* Whether `rates` sets a value that hook_weapon_rate ignores when the global
 * section has lines: any key but the energy transfer ones. */
bool XwauCraft_HasDefaultFirstRates(const XwauCraftWeaponRates* rates);

/* Writes `rates` as the groups of a `weapons` map, one line each, at
 * `indent`: "  rates: {decharge: 2, recharge: 3}". The system damage keys of
 * weapons use the weapon names in `names`. */
void XwauCraft_WriteWeaponRates(FILE* f, const XwauNames* names, const char* indent,
								const XwauCraftWeaponRates* rates);

/* The [Sound] section of a model. */
typedef struct XwauCraftSound {
	int  interior;    /* 0: not set, else 1 to 7 */
	int  fly_by;      /* 0: not set, else 1 to 8 */
	int  fly_by_mode; /* 0: either, 1: 2D, 2: 3D */
	int  wash;        /* 0: not set, 1: normal, 2: starship */
	int  take_off;    /* 0: not set, 1: gear, 2: falcon */
	int  hyper;       /* -1: not set, else 0 to 2 */
	int  weapons;     /* -1: not set, else a behaviour, 0 for plain */
	int  range_first[XWAU_CRAFT_WEAPONS];
	int  range_count[XWAU_CRAFT_WEAPONS]; /* 0: no range */
	bool engine_slowing;                  /* the section sets EngineSlowing */
	/* The ambient sound, in metres, by axis: right, up and forward. */
	int  ambient_offset[3];
	bool has_ambient_distance[3];
	int  ambient_distance[3];
} XwauCraftSound;

/* Reads the [Sound] section of a model: `sound_txt` (<model>Sound.txt) when
 * it has lines, else the [Sound] section of `ini`. Either path can be NULL.
 * `range_bank` is the number of entries of the WeaponRange bank. */
void XwauCraft_ReadSound(const char* sound_txt, const char* ini, int range_bank, XwauCraftSound* sound);

/* Whether `sound` sets a sound. */
bool XwauCraft_HasSound(const XwauCraftSound* sound);

/* The impact colour that XWAU shows for an ARGB value: its RGB, or 0 (the
 * stock white mark) when its RGB565 value is 0. */
uint32_t XwauCraft_ImpactColor(uint32_t argb);

/* The impact colour group (0 for 3100 to 4 for 3500) of weapon type
 * 280 + `weapon`, from the hook's table. */
int XwauCraft_ImpactGroup(int weapon);

/* Reads the global impact colours of hook_weapon_color: its
 * hook_weapon_color.cfg, else the [hook_weapon_color] section of Hooks.ini,
 * else the hook's defaults (blue, red, green, purple and yellow), as impact
 * colours by weapon type. Returns false when the installation `game` has no
 * hook_weapon_color.dll. */
bool XwauCraft_ReadGlobalImpactColors(const char* game, uint32_t colors[XWAU_CRAFT_WEAPONS]);

/* Reads the global WeaponSwitchBasedOnIff of hook_weapon_color, from the
 * same files as the impact colours, into `on`: false without the key.
 * Returns false when the installation `game` has no hook_weapon_color.dll,
 * or when its DLL doesn't have the setting. */
bool XwauCraft_ReadGlobalSwitchByIff(const char* game, bool* on);

/* Reads an icon list file into `*values` (malloc'd) and `*count`. Returns
 * false, with a report, when a line isn't a number. */
bool XwauCraft_ReadIconList(const char* path, uint16_t** values, size_t* count);

/* Writes the craft files and hud.yaml of the installation `xwau_root` into
 * `output_dir`. When `cockpits` isn't NULL, adds the gunner cockpits that
 * the files use to it. The files use the names in `game_names`. hud.yaml
 * only lists the icons that differ from `stock_values`. Returns false when a
 * file can't be written or a list can't be read. */
bool XwauCraft_Write(const char* xwau_root, const char* output_dir, const XwauNames* game_names,
					 const XwauStockValues* stock_values, XwauLines* cockpits);

#endif /* AERON_XWAU_CRAFT_H */
