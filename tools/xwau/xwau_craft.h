#ifndef AERON_XWAU_CRAFT_H
#define AERON_XWAU_CRAFT_H

/*
 * Writes the model settings of an XWAU installation (xwau_import craft):
 *   <output_dir>/models/<model key>.yaml  the settings of one model, next to
 *                                         its GLB
 *   <output_dir>/hud.yaml                 the hull and map icon lists
 *
 * A model's key is its INI file's name in lower case, without the extension:
 * FlightModels/Awing.ini gives models/awing.yaml. A settings file is written
 * only for a model that has a setting:
 *
 *   hud:
 *     reticle: {5: 135, 6: 136, ...}                 # image position for each slot
 *     colors: {inner: 0xff5b5b5b, border: 0xffe5e5e5} # ARGB
 *   engine_glows:
 *     0: {min_speed_percent: 1}                      # by glow, in the model's order
 *     6: {min_speed_mglt: 40}
 *
 *   weapons:
 *     switch_by_iff: true                            # fire the weapons of the craft's side
 *     colors:                                        # any markings
 *       variant: {default: 3, 284: 1}                # the bolt's markings, by weapon type
 *       impact: {280: 0xff0000, 282: 0}              # RGB; 0 keeps the stock white mark
 *       energy_bars: {laser_high: 0xffff0000}        # ARGB
 *     by_markings:                                   # the changes for one markings value
 *       1:
 *         variant: {default: 2}
 *   stats_profiles:                                  # xwau_stats_profiles.h
 *     default: {speed: 86, hull: 3045}
 *   shield: {per_generator_rate: 5, player_rate: 40, delay_seconds: 30}
 *
 * A model's laser rates go in its `weapons` group:
 *
 *   weapons:
 *     rates: {decharge: 2, recharge: 3, cooldown: 47}
 *
 * The reticle comes from FlightModels/<model>Reticle.txt, else the [Reticle]
 * section of FlightModels/<model>.ini, read like XWAU's reticle hook: the
 * slots are 5 to 10 and 15 to 22, a key is Reticle_<slot>, and a slot set
 * to -1 is left out. The colours come from the [HUDColor] section of the
 * model's INI, read like XWAU's ddraw wrapper (LoadHUDColorFromIniFile in
 * Effects.cpp): a section counts when its header line contains "[HUDColor]"
 * in this case, the last value of a key wins, and each colour needs three
 * numbers from 0 to 1.
 *
 * The engine glows come from FlightModels/<model>EngineGlows.txt, else the
 * [EngineGlows] section of the model's INI, read like XWAU's hook_opt_limit:
 * each line is "glow, percent, mglt", a line with fewer than 3 values is
 * skipped, and the first line of a glow wins. A glow is hidden while the
 * craft's speed is below `min_speed_percent` of its top speed, or below
 * `min_speed_mglt`. The hook's -1 means no limit, and so does any value of 0
 * or less, since a speed is never below it; such a limit isn't written.
 *
 * The weapon colours come from FlightModels/<model>WeaponColor.txt, else the
 * [WeaponColor] section of the model's INI, read like XWAU's
 * hook_weapon_color:
 *   - WeaponColor and WeaponColor<NNN> give the markings of the bolts of
 *     weapon type NNN (280 to 307). -1 means not set, and 0 is a value.
 *   - WeaponImpactColor_3100 to _3500 give the impact colour of a group of
 *     weapon types, and WeaponImpactColor<NNN> of one type. The importer
 *     expands the groups to weapon types with the hook's table.
 *   - EnergyBarColorLaserHigh, ...LaserLow, ...IonHigh and ...IonLow give
 *     the player's charge bar colours.
 *   - Each key can end with _fg<m>_ (WeaponColor_fg1_, WeaponColor_fg1_280),
 *     which applies to markings m on top of the keys without it.
 * The first line of a key wins. Markings are read with std::stoi(value, 0,
 * 0) and kept as the byte the hook writes; colours are read with
 * std::stoul(value, 0, 16). A colour of 0 means not set, except for a group
 * key without _fg, where it sets the group back to the stock white mark. An
 * impact colour loses its alpha, as in XWAU, and one whose RGB565 value is 0
 * becomes 0, since XWAU then keeps the stock mark. WeaponLightColor keys
 * aren't imported: XWAU players see the light of the bolt's material
 * instead. WeaponSwitchBasedOnIff gives switch_by_iff: -1 isn't set, and
 * any other value is true when it isn't 0.
 *
 * The shield recharge comes from FlightModels/<model>Shield.txt, else the
 * [Shield] section of the model's INI, read like XWAU's hook_shield. With
 * `UseGenerator = 1`, `PerGeneratorRechargeRate` gives `per_generator_rate`;
 * otherwise `RechargeRate` gives `rate`. Both default to 0 when the section
 * exists. `PlayerRechargeRate` gives `player_rate` and `RechargeRateDelay`
 * gives `delay_seconds`; they are written when they aren't 0, since the
 * hook treats 0 as not set. A model without the section gets no `shield`
 * group, and the game uses its default rate (shield_recharge.h).
 *
 * The laser rates come from FlightModels/<model>WeaponRate.txt, or else from
 * the [WeaponRate] section of the model's INI. They are read the way XWAU's
 * hook_weapon_rate reads them, and go into groups of `weapons`:
 *
 *   rates: {decharge: 2, recharge: 3, cooldown: 47}
 *       DechargeRate, RechargeRate, CooldownTimeFactor, DechargeRatePercent
 *       and RechargeRatePercent (decharge_percent, recharge_percent)
 *   energy_transfer: {rate: 32}
 *       EnergyTransferRate, EnergyTransferRatePenalty (penalty),
 *       EnergyTransferWeaponLimit (laser_limit), EnergyTransferShieldLimit
 *       (shield_limit)
 *   ai_torpedoes: {per_pass: 2, per_target: 6}
 *       MaxTorpedoCountPerPass, MaxTorpedoCountPerTarget
 *   impact_spin: {enabled: false, speed_percent: 25, angle_percent: 25}
 *       IsImpactSpinningEnabled, ImpactSpinningSpeedFactorPercent,
 *       ImpactSpinningAngleFactorPercent
 *   system_damage: {max: 1000, 284: 1, 285: 2, 290: 4, 296: 30}
 *       MaxSystemDamages, Weapon284_LaserIon_Damages,
 *       Weapon285_LaserIonTurbo_Damages, Weapon290_LaserIonTurbo_Damages,
 *       Weapon296_MagPulse_Damages
 *
 * -1 means not set; other values below 0 are reported and left out. Any
 * IsImpactSpinningEnabled but 0 turns the spin on.
 *
 * The hook only reads a model's section when the global section has no
 * lines, except for the energy transfer keys, which it reads from the model
 * first. The global section is FlightModels/WeaponRate.txt, or else the
 * [WeaponRate] section of FlightModels/default.ini. The importer writes the
 * model's values anyway, and the game uses them instead of the global ones.
 * The report counts the models whose values XWAU ignores. The global values
 * go in flight/flight.yaml (xwau_flight_settings.h).
 *
 * The component strength comes from FlightModels/<model>
 * SubcomponentsStrength.txt, or else from the [SubcomponentsStrength]
 * section of the model's INI, if the installation has hook_opt_limit.dll
 * with that feature. Each line is the strength of one mesh type, in the
 * order of the game's mesh types, and -1 keeps the game's strength:
 *
 *   component_strength: {gun_turret: 18, small_gun: 18, shield_generator: 36}
 *
 * The hook reads each line as a number, and a line that isn't one would
 * crash XWAU: such a section is reported and left out. Lines past the 32nd
 * aren't used. A strength above 255 doesn't fit in the game's component
 * health, so it is reported and left out.
 *
 * The gunner seats come from FlightModels/<model>Turrets.txt, or else from
 * the [Turrets] section of the model's INI, if the installation has
 * hook_mission_objects.dll (xwau_gunner_seats.h).
 *
 * The sounds come from FlightModels/<model>Sound.txt, or else from the
 * [Sound] section of the model's INI. They are read the way XWAU's
 * hook_engine_sound reads them, and only if the installation has
 * hook_engine_sound.dll:
 *
 *   sound:
 *     interior: awing            # EngineSoundInterior 1 to 7
 *     fly_by: slave              # EngineSoundFlyBy 1 to 8, 11 to 18, 21 to 28
 *     fly_by_mode: 3d            # 2d for 11 to 18, 3d for 21 to 28
 *     engine_wash: starship      # EngineSoundWash: 2 starship, else normal
 *     take_off: falcon           # EngineSoundTakeOff: 2 falcon, else gear
 *     weapons: empire            # WeaponSoundBehavior
 *     weapon_ranges: {283: {first: 2, count: 3}}  # WeaponSoundRange*_283
 *     hyper: rebel               # HyperSoundBehavior 0 to 2
 *     ambient: {offset_m: [0, 300, 0], distance_m: 1000}
 *
 * An engine sound of 0, a HyperSoundBehavior of -1 and an empty
 * WeaponSoundBehavior all mean "use the sound bank", so they aren't written.
 * A WeaponSoundBehavior that isn't a known behaviour, such as 0, becomes
 * `plain`: the hook then plays each weapon type's own sound. A weapon range
 * needs both keys. A range with a count of 0 would crash XWAU, and the hook
 * drops a range that goes past the WeaponRange bank; both are reported and
 * left out. Other values that make XWAU play nothing are also reported and
 * left out. No hook reads EngineSlowing.
 *
 * `ambient` places the starship ambient sound. AmbientSoundOffsetX, Y and Z
 * become `offset_m` (right, up, forward). AmbientSoundDistance, or
 * AmbientSoundDistanceX, Z and Y, become `distance_m` (right, up, forward):
 * this is how the hook reads the distances for a craft that flies level. If
 * an axis distance is 0, it uses AmbientSoundDistance instead, and an
 * AmbientSoundDistance of 0 counts as 1. An axis with no distance is written
 * as null, and the game then uses the craft's size.
 *
 * hud.yaml holds `hull_icons` and `map_icons`, from HullIconList.txt and
 * MapIconList.txt, read like XWAU's hull icon hook: the value at position N
 * is the image id for object type N, counting the lines that the hook reads
 * (empty lines and comment lines are skipped). A missing list leaves out its
 * key. When both are missing, hud.yaml isn't written.
 *
 * Files that a run doesn't write are left as they are.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "xwau_config.h"

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
	bool                  light_colors;  /* the model has WeaponLightColor keys */
	int                   switch_by_iff; /* WeaponSwitchBasedOnIff: 1 or 0, -1 when not set */
} XwauCraftWeapons;

/* The ARGB colour that XWAU's wrapper makes from red, green and blue values
 * from 0 to 1: 0xff << 24 | uint32_t(r * 255) << 16 | uint32_t(g * 255) << 8 |
 * uint32_t(b * 255). The products are truncated, not rounded. */
uint32_t XwauCraft_HudColor(float r, float g, float b);

/* Reads the reticle of a model: `reticle_txt` (<model>Reticle.txt) when it
 * has lines, else the [Reticle] section of `ini`. Either path can be NULL.
 * Problems are reported with the file's name. */
void XwauCraft_ReadReticle(const char* reticle_txt, const char* ini, XwauCraftHud* hud);

/* Reads the [HUDColor] section of `ini`. */
void XwauCraft_ReadHudColors(const char* ini, XwauCraftHud* hud);

/* Reads the engine glows of a model: `glows_txt` (<model>EngineGlows.txt)
 * when it has lines, else the [EngineGlows] section of `ini`. Either path
 * can be NULL. A line with a value that isn't a number, or with a glow
 * outside 0 to 255, is reported and skipped. */
void XwauCraft_ReadEngineGlows(const char* glows_txt, const char* ini, XwauCraftEngineGlows* glows);

/* Reads the weapon colours of a model: `weapons_txt`
 * (<model>WeaponColor.txt) when it has lines, else the [WeaponColor]
 * section of `ini`. Either path can be NULL. A value that isn't a number is
 * reported and ignored. */
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
 * `indent`: "  rates: {decharge: 2, recharge: 3}". */
void XwauCraft_WriteWeaponRates(FILE* f, const char* indent, const XwauCraftWeaponRates* rates);

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
 * `output_dir`, and adds to `cockpits`, when it isn't NULL, the gunner
 * cockpits that the files name (XwauGunnerSeats_WriteCockpits). Returns false when a file can't be
 * written or a list can't be read. */
bool XwauCraft_Write(const char* xwau_root, const char* output_dir, XwauLines* cockpits);

#endif /* AERON_XWAU_CRAFT_H */
