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
 *     colors:                                        # any markings
 *       variant: {default: 3, 284: 1}                # the bolt's markings, by weapon type
 *       impact: {280: 0xff0000, 282: 0}              # RGB; 0 keeps the stock white mark
 *       energy_bars: {laser_high: 0xffff0000}        # ARGB
 *     by_markings:                                   # the changes for one markings value
 *       1:
 *         variant: {default: 2}
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
 * instead. WeaponSwitchBasedOnIff is step 5 of the model INI plan.
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

typedef struct XwauCraftWeapons {
	XwauCraftWeaponColors colors;
	bool                  has_markings[256];
	XwauCraftWeaponColors by_markings[256];
	bool                  light_colors; /* the model has WeaponLightColor keys */
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

/* Reads an icon list file into `*values` (malloc'd) and `*count`. Returns
 * false, with a report, when a line isn't a number. */
bool XwauCraft_ReadIconList(const char* path, uint16_t** values, size_t* count);

/* Writes the craft files and hud.yaml of the installation `xwau_root` into
 * `output_dir`. Returns false when a file can't be written or a list can't
 * be read. */
bool XwauCraft_Write(const char* xwau_root, const char* output_dir);

#endif /* AERON_XWAU_CRAFT_H */
