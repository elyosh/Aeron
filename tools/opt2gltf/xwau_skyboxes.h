#ifndef AERON_OPT2GLTF_XWAU_SKYBOXES_H
#define AERON_OPT2GLTF_XWAU_SKYBOXES_H

/*
 * Converts the [SkyBoxes] sections of XWAU's missions (the cube maps of the
 * ddraw wrapper, from XWAU UPD 1.1.0) into an environment manifest for
 * OpenXWA, to merge into flight/environments/environments.yaml.
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
 *   - Numbers are read as std::stof reads them: the leading number of the
 *     value, or the default when there is none.
 *   - DisabledBackdrops and EnabledBackdrops list "group-image" images
 *     separated by ';', each side read with atoi. The token ALL or all
 *     means every image and ends the list.
 *
 * The manifest holds:
 *   - one environment per cube folder and set of values, named
 *     xwau.<folder> for the first set and xwau.<folder>.2, .3 and so on for
 *     the others. AmbientInt becomes diffuse_strength and AmbientMin the
 *     ambient_fill. Its textures are
 *     flight/environments/xwau/<folder>/{sky,diffuse,specular}.ktx2, which
 *     `environment_bake --faces` writes;
 *   - an `xwa.missions` entry for each original mission with [SkyBoxes],
 *     with its four regions. A region with a cube keeps its backdrops, and
 *     gets its rotation (XwauSkyboxes_Rotation) and its hide and show lists:
 *     the ddraw wrapper's starfield images and DisabledBackdrops are hidden,
 *     and EnabledBackdrops are shown. A region without a cube is `none`.
 * Overlays are not written yet: the regions that have one get their sky
 * alone, and are reported.
 *
 * The folder list has one line per cube folder:
 * "<folder path>\t<environment name>\t<texture folder in the mod folder>".
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

/* Writes the manifest `manifest` and the folder list `folders` for the
 * missions of `game` that are also in the original game `stock_game`.
 * Returns false on an error. */
bool XwauSkyboxes_Write(const char* game, const char* stock_game, const char* manifest, const char* folders);

#endif /* AERON_OPT2GLTF_XWAU_SKYBOXES_H */
