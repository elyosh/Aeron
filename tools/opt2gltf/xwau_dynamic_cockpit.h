#ifndef AERON_OPT2GLTF_XWAU_DYNAMIC_COCKPIT_H
#define AERON_OPT2GLTF_XWAU_DYNAMIC_COCKPIT_H

/*
 * X-Wing Alliance Upgrade Dynamic Cockpit files (`DynamicCockpit/<Cockpit>.dc`)
 * resolved into the converter's screen surfaces (opt_gltf.h). Parsing follows
 * xwa_ddraw_d3d11's LoadIndividualDCParams and LoadDCUVCoords, quirks
 * included:
 *
 *   - `cover_texture_size` applies to every later `uv_coords` line, across
 *     sections, until changed;
 *   - a `uv_coords` intensity below 1 selects intensity 4 (unsigned wrap);
 *   - an 11-field `uv_coords` with bloom 0 also clears the graphics layer
 *     (XWAU masks the flags with 0xE7);
 *   - a repeated section resets its rectangles and erase regions, not its
 *     flags or cover;
 *   - a section binds to the first texture of the converted OPT with its
 *     name; a section without rectangles keeps the texture as it is and
 *     only erases its HUD regions.
 *
 * Covers are DAT images (`Effects\<File>.dat-<group>-<image>`), found in
 * the XWAU installation and baked as opt_gltf.h describes. Hologram sections move
 * their faces by FlightModels/<Craft>.ini [CockpitHoloOffsets] (OPT units),
 * read as LoadHoloOffsetFromIniFile does: <Craft> is the cockpit name without
 * its last 7 characters, and [GunnerTurretHoloOffsets] writes the same
 * offset.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "opt.h"
#include "opt_gltf.h"

/* Resolve a game-relative path in the XWAU installation; returns 0 if absent. */
typedef int (*XwauDynamicCockpitFindFile)(void* context, const char* relative, char* out, size_t capacity);

typedef struct XwauDynamicCockpitCover XwauDynamicCockpitCover;

typedef struct XwauDynamicCockpit {
	OptGltfScreen*            screens; /* one per OPT texture */
	int32_t                   texture_count;
	bool                      has_hologram_offset;
	float                     hologram_offset[3]; /* OPT units */
	XwauDynamicCockpitCover** covers;
	size_t                    cover_count;
} XwauDynamicCockpit;

/* Read `dc_path` for the OPT `basename` (its file name without extension).
 * Problems with single lines or sections are reported and skipped; returns
 * false only when the file can't be read or memory runs out. */
bool XwauDynamicCockpit_Load(const char* dc_path, const opt_file_t* opt, const char* basename,
							 XwauDynamicCockpitFindFile find, void* find_context, XwauDynamicCockpit* out,
							 const char* label);

/* Move the LOD 0 faces drawing a hologram texture (any switch state) by the
 * hologram offset. Their corners get their own vertices, so faces of other
 * textures keep theirs. */
bool XwauDynamicCockpit_ApplyHologramOffset(opt_file_t* opt, const XwauDynamicCockpit* dc);

void XwauDynamicCockpit_Free(XwauDynamicCockpit* dc);

#endif /* AERON_OPT2GLTF_XWAU_DYNAMIC_COCKPIT_H */
