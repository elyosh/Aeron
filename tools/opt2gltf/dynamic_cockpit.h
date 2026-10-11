#ifndef AERON_OPT2GLTF_DYNAMIC_COCKPIT_H
#define AERON_OPT2GLTF_DYNAMIC_COCKPIT_H

/*
 * X-Wing Alliance Upgrade Dynamic Cockpit files (`DynamicCockpit/<Cockpit>.dc`)
 * resolved into the converter's screen surfaces (opt2gltf.h). Parsing follows
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
 * Covers are DAT images (`Effects\<File>.dat-<group>-<image>`), found under
 * the XWAU roots and baked as opt2gltf.h describes. Hologram sections move
 * their faces by FlightModels/<Craft>.ini [CockpitHoloOffsets] (OPT units),
 * read as LoadHoloOffsetFromIniFile does: <Craft> is the cockpit name without
 * its last 7 characters, and [GunnerTurretHoloOffsets] writes the same
 * offset.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "opt.h"
#include "opt2gltf.h"

/* Resolve a game-relative path under the XWAU roots; returns 0 if absent. */
typedef int (*DynamicCockpitFindFile)(void* context, const char* relative, char* out, size_t capacity);

typedef struct DynamicCockpitCover DynamicCockpitCover;

typedef struct DynamicCockpit {
	OptGltfScreen*        screens; /* one per OPT texture */
	int32_t               texture_count;
	bool                  has_hologram_offset;
	float                 hologram_offset[3]; /* OPT units */
	DynamicCockpitCover** covers;
	size_t                cover_count;
} DynamicCockpit;

/* Read `dc_path` for the OPT `basename` (its file name without extension).
 * Problems with single lines or sections are reported and skipped; returns
 * false only when the file can't be read or memory runs out. */
bool DynamicCockpit_Load(const char* dc_path, const opt_file_t* opt, const char* basename,
						 DynamicCockpitFindFile find, void* find_context, DynamicCockpit* out,
						 const char* label);

/* Move the LOD 0 faces drawing a hologram texture (any switch state) by the
 * hologram offset. Their corners get their own vertices, so faces of other
 * textures keep theirs. */
bool DynamicCockpit_ApplyHologramOffset(opt_file_t* opt, const DynamicCockpit* dc);

void DynamicCockpit_Free(DynamicCockpit* dc);

#endif /* AERON_OPT2GLTF_DYNAMIC_COCKPIT_H */
