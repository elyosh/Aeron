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
 * only for a model that has a setting. Today the settings are the HUD's:
 *
 *   hud:
 *     reticle: {5: 135, 6: 136, ...}                 # image position for each slot
 *     colors: {inner: 0xff5b5b5b, border: 0xffe5e5e5} # ARGB
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

/* Reads an icon list file into `*values` (malloc'd) and `*count`. Returns
 * false, with a report, when a line isn't a number. */
bool XwauCraft_ReadIconList(const char* path, uint16_t** values, size_t* count);

/* Writes the craft files and hud.yaml of the installation `xwau_root` into
 * `output_dir`. Returns false when a file can't be written or a list can't
 * be read. */
bool XwauCraft_Write(const char* xwau_root, const char* output_dir);

#endif /* AERON_XWAU_CRAFT_H */
