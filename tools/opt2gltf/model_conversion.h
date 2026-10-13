#ifndef AERON_OPT2GLTF_MODEL_CONVERSION_H
#define AERON_OPT2GLTF_MODEL_CONVERSION_H

/*
 * Converting one OPT in stock or XWAU mode: what the mode gives the model
 * becomes the conversion's build options (opt_gltf.h) and the data they
 * point to.
 *
 * The glTF carries the AERON_flight_model extension, which the cooked GLB
 * keeps with cooked simulation and render data. Conversion keeps the
 * component order and every mesh's type, explosion flags, descriptor,
 * target, rotation frame, hardpoints and engine glows, the LOD 0 geometry
 * and the textures. Components stay one-to-one with the OPT meshes: a mesh
 * with vertices but no LOD 0 faces keeps its vertices as one degenerate
 * triangle (no faces once cooked), and a mesh without LOD 0 triangles
 * otherwise is an error naming the file and mesh. Expected differences from
 * the stock OPT:
 *   - quads become two triangles sharing corner 0 and the face normal;
 *   - only LOD 0 is kept;
 *   - positions round-trip through metres and differ at the ulp level;
 *   - the trailing MESHVERTS box follows the game's replacement policy;
 *   - engine-glow axes round-trip through a quaternion and come back
 *     orthonormalized.
 *
 * Stock mode converts an OPT of the original game as the game draws it,
 * with nothing read beside it:
 *   - normals regenerated with a 90 degree smoothing threshold, or the
 *     given one: connected smoothing fans are built across shared edges,
 *     then averaged with corner-angle weights so triangulation density does
 *     not bias them; wrapped/non-manifold fans split before a normal can
 *     point behind its rendered triangle (e.g. 45 for crisp flat panels);
 *   - self-illumination (lit windows, cockpit lights): textures with
 *     self-lit texels get a second *_emissive.png as the material's
 *     emissiveTexture. For XWA (version 5) OPTs this replicates the
 *     engine's lightmap derivation (ModelTexture_FilterHardwarePalette @
 *     0x44A600 + CreateD3DfromTexture): an entry is self-lit when its
 *     darkest palette row is bright (squared 5-bit magnitude >= 32, green
 *     LSB dropped) and rows 1..6 stay within squared distance 16 of row 0;
 *     glow color from palette row 10; textures named "_*" and palettes
 *     where none/all 256 entries classify are skipped;
 *   - projectiles (model_tables.h) fully self-lit instead, as the classic
 *     renderer draws them: black base color with the texture's alpha kept,
 *     the base texture as emissiveTexture at strength 5;
 *   - the component tags and articulations the game ties to fixed component
 *     numbers, and the pilot eye the game's craft table gives the player's
 *     craft (model_tables.h): the OPT's Cockpit (type 31) hardpoints are
 *     replaced by one at the eye.
 *
 * XWAU mode converts X-Wing Alliance Upgrade models with everything XWAU
 * gives them, read from an XWAU installation (the game folder XWAU is
 * installed into) with case-insensitive paths. Stored normals are used per
 * corner. For each OPT:
 *   - Materials/<basename>.mat (on top of Materials/DefaultGlobalMaterial.mat)
 *     gives each texture's material and normal map, read from the DAT image
 *     it names. The converter then reproduces that renderer's shading in
 *     standard glTF inputs: authored colors, illumination, shadeless and
 *     glass texels, metallic-roughness, KHR_materials_specular, normal maps
 *     with NMIntensity baked in and MikkTSpace tangents;
 *   - its image sequences become material animations (xwau_anim.h);
 *   - its files beside the OPT give the pilot eye, object profiles,
 *     articulations, Dynamic Cockpit and a cockpit model's interior light
 *     (xwau_model.h), and its skins material variants (xwau_skins.h), with
 *     what the missions give it (xwau_missions.h);
 *   - the crew and glass textures model_tables.h lists are lit surfaces
 *     although their material makes them Shadeless;
 *   - more than 120 weapon hardpoints are reported: the game keeps the
 *     first 120 weapon slots, as XWAU does.
 */

#include <stdbool.h>

#include "xwau_config.h"
#include "xwau_mat.h"
#include "xwau_missions.h"

/* The mode models are converted in: stock mode unless initialized with
 * ModelConversion_InitXwau. */
typedef struct ModelConversion {
	float smooth_angle; /* stock mode: degrees, 0 to 180; negative: 90 */
	bool  xwau;
	/* XWAU mode (ModelConversion_InitXwau). */
	const char*   game;   /* the XWAU installation */
	XwauMatParams global; /* Materials/DefaultGlobalMaterial.mat, else code defaults */
	XwauMissions* missions;
} ModelConversion;

/* XWAU mode over the installation `game`: reads the global material and
 * the missions. False when out of memory (reported). */
bool ModelConversion_InitXwau(ModelConversion* mc, const char* game);
void ModelConversion_Free(ModelConversion* mc);

/* Converts the OPT at `opt_path`, named `basename`, into <out_dir> as
 * OptGltf_Convert does; problems are reported under `opt_path`. */
bool ModelConversion_Convert(const ModelConversion* mc, const char* opt_path, const char* basename,
							 const char* out_dir);

#endif /* AERON_OPT2GLTF_MODEL_CONVERSION_H */
