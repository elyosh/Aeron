#ifndef AERON_XWAU_SKYBOXES_H
#define AERON_XWAU_SKYBOXES_H

/*
 * Converts the [SkyBoxes] sections of XWAU's missions (the cube maps of the
 * ddraw wrapper, from XWAU UPD 1.1.0) into entries of OpenXWA's environment
 * manifest, flight/environments/environments.yaml, and writes them into it.
 *
 * The section is read from <mission>.ini with the C++ hook rules
 * (xwau_config.h), as the ddraw wrapper reads it:
 *   - AllRegions and Region0 to Region3 name a cube folder in the game
 *     folder. A folder loads when it holds exactly six files (files whose
 *     names start with '.' are not counted). A region uses its own folder
 *     when it loads, with the Region<N>* values and their defaults
 *     (AmbientInt 0.15, AmbientMin 0.01). Otherwise it uses the AllRegions
 *     folder when it loads, with the AllRegions* values (AmbientInt 0.50,
 *     AmbientMin 0.01). Otherwise it has no cube.
 *   - A region uses its Region<N>Overlay folder (default <folder>_overlay)
 *     when it loads, otherwise the AllRegions overlay. With its own cube,
 *     the region's overlay angles are Region<N>OverlayRotation*, which
 *     default to the region's angles. Otherwise they are
 *     AllRegionsOverlayRotation*, which default to the AllRegions angles.
 *   - Numbers are read as std::stof reads them: the leading number of the
 *     value, or the default when there is none.
 *   - DisabledBackdrops and EnabledBackdrops list "group-image" images
 *     separated by ';', each side read with atoi. The token ALL or all
 *     means every image and ends the list.
 *
 * The importer's entries are:
 *   - one environment per cube folder, set of values and overlay. The first
 *     one is named xwau.<folder>, or xwau.<folder>.<overlay folder> with an
 *     overlay; the next ones get .2, .3 and so on. AmbientInt becomes
 *     diffuse_strength and AmbientMin the ambient_fill. Its textures are
 *     flight/environments/xwau/<folder>/{sky,diffuse,specular}.ktx2, which
 *     `environment_bake --faces` writes, and
 *     flight/environments/xwau/<overlay folder>/overlay.ktx2, which
 *     `environment_bake --overlay-faces` writes;
 *   - an `xwa.missions` entry for each original mission with [SkyBoxes],
 *     with its four regions. A region with a cube keeps its backdrops, and
 *     gets its rotation (XwauSkyboxes_Rotation) and its hide and show lists:
 *     the ddraw wrapper's starfield images and DisabledBackdrops are hidden,
 *     and EnabledBackdrops are shown. It also gets `backdrop_light_scale:
 *     0.25`: XWAU's renderer cuts the lights of backdrops that are not suns
 *     to 25% (PBRAddCommon.h), and its missions are tuned for that. A region
 *     without a cube is `none`.
 * A region with an overlay but no cube gets no overlay, and is reported.
 *
 * They go between two comment lines in each of the manifest's maps,
 * `environments` and `xwa.missions`:
 *   # Begin of the entries that xwau_import missions --skyboxes writes. ...
 *   # End of the entries that xwau_import missions --skyboxes writes.
 * Each import replaces what is between them, so hand edits there are lost.
 * The rest of the file is kept as it is, comments included. A file without
 * these lines gets them at the end of each map, and a missing file is
 * created. Entries for the same environments or missions outside these lines
 * must be removed by hand, or the game reports them as duplicates.
 *
 * The folder list has one line per sky folder and one per overlay folder:
 * "sky\t<folder path>\t<environment name>\t<texture folder in the mod folder>"
 * or "overlay\t<folder path>\t-\t<texture folder in the mod folder>".
 *
 * <folder> is the folder name in lower case, encoded with Xwau_EncodeName.
 */

#include <stdbool.h>

/* Turns XWAU rotation angles (degrees about X, Y and Z, applied as
 * Rz Ry Rx, or Rx Ry Rz for an overlay) into OpenXWA's rotation_degrees
 * [yaw, pitch, roll]. XWAU reads its cube at Rot (Xw, Zw, Yw) of a world
 * ray, and OpenXWA reads its converted cube at M Q^T B w with
 * M = diag(1, 1, -1), so Q = M Rot^T M (xwau_skyboxes_plan.md, 4.2). */
void XwauSkyboxes_Rotation(const float xyz_degrees[3], bool overlay, float out_degrees[3]);

/* Writes the importer's entries into the manifest `manifest` and writes the
 * folder list `folders`, for the missions of `game` that the mod or the
 * stock game `stock_game` has. Returns false on an error. */
bool XwauSkyboxes_Write(const char* game, const char* stock_game, const char* manifest, const char* folders);

#endif /* AERON_XWAU_SKYBOXES_H */
