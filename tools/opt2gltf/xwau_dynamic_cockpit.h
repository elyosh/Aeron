#ifndef AERON_OPT2GLTF_XWAU_DYNAMIC_COCKPIT_H
#define AERON_OPT2GLTF_XWAU_DYNAMIC_COCKPIT_H

/*
 * Reads X-Wing Alliance Upgrade Dynamic Cockpit files
 * (`DynamicCockpit/<Cockpit>.dc`) and turns them into the converter's screen
 * surfaces (opt_gltf.h). The parsing follows LoadIndividualDCParams and
 * LoadDCUVCoords in xwa_ddraw_d3d11, including their quirks:
 *
 *   - `cover_texture_size` applies to every later `uv_coords` line, also in
 *     later sections, until it changes;
 *   - a `uv_coords` intensity below 1 selects intensity 4 (unsigned wrap);
 *   - an 11-field `uv_coords` line with bloom 0 also clears the graphics layer
 *     (XWAU masks the flags with 0xE7);
 *   - a repeated section resets its rectangles and erase regions, but not its
 *     flags or cover;
 *   - a section binds to the first texture of the converted OPT that has its
 *     name. A section without rectangles leaves the texture as it is and only
 *     erases its HUD regions.
 *
 * Covers are DAT images (`Effects\<File>.dat-<group>-<image>`). They are found
 * in the XWAU installation and baked as described in opt_gltf.h. Hologram
 * sections move their faces by the offset in [CockpitHoloOffsets] of
 * FlightModels/<Craft>.ini (in OPT units), read the way
 * LoadHoloOffsetFromIniFile reads it. <Craft> is the cockpit name without its
 * last 7 characters. [GunnerTurretHoloOffsets] sets the same offset.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "opt.h"
#include "opt_gltf.h"

/* Finds a game-relative path in the XWAU installation. Returns 0 when the file does not exist. */
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

/* Reads `dc_path` for the OPT `basename` (its file name without the
 * extension). A bad line or section is reported and skipped. Returns false
 * only when the file cannot be read or memory runs out. */
bool XwauDynamicCockpit_Load(const char* dc_path, const opt_file_t* opt, const char* basename,
							 XwauDynamicCockpitFindFile find, void* find_context, XwauDynamicCockpit* out,
							 const char* label);

/* Moves the LOD 0 faces that draw a hologram texture, in any switch state,
 * by the hologram offset. Their corners get new vertices, so faces with other
 * textures keep the original ones. */
bool XwauDynamicCockpit_ApplyHologramOffset(opt_file_t* opt, const XwauDynamicCockpit* dc);

void XwauDynamicCockpit_Free(XwauDynamicCockpit* dc);

#endif /* AERON_OPT2GLTF_XWAU_DYNAMIC_COCKPIT_H */
