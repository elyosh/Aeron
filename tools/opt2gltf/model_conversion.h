#ifndef AERON_OPT2GLTF_MODEL_CONVERSION_H
#define AERON_OPT2GLTF_MODEL_CONVERSION_H

/*
 * Converts one OPT in stock or XWAU mode. Each mode turns what it knows about
 * the model into build options for the conversion (opt_gltf.h) and the data
 * that these options point to.
 *
 * The glTF carries the AERON_flight_model extension. The cooked GLB keeps it
 * and adds cooked simulation and render data. The conversion keeps the
 * component order and, for every mesh, its type, explosion flags, descriptor,
 * target, rotation frame, hardpoints and engine glows. It also keeps the LOD 0
 * geometry and the textures. Each component matches one OPT mesh. A mesh that
 * has vertices but no LOD 0 faces keeps its vertices as one degenerate
 * triangle, which has no faces once cooked. Any other mesh without LOD 0
 * triangles is an error that names the file and the mesh. Expected
 * differences from the stock OPT:
 *   - quads become two triangles that share corner 0 and the face normal;
 *   - only LOD 0 is kept;
 *   - positions are converted to metres and back, so they differ at the ulp
 *     level;
 *   - the trailing MESHVERTS box follows the game's replacement policy;
 *   - engine-glow axes are converted to a quaternion and back, so they come
 *     back orthonormalized.
 *
 * Stock mode converts an OPT of the original game the way the game draws it.
 * It reads no other file:
 *   - normals are regenerated with a smoothing threshold of 90 degrees, or the
 *     given one (for example 45 for crisp flat panels). Connected smoothing
 *     fans are built across shared edges, then averaged with corner-angle
 *     weights so that dense triangulation does not bias them. Wrapped or
 *     non-manifold fans are split before a normal can point behind its
 *     rendered triangle;
 *   - self-illumination (lit windows, cockpit lights): textures with self-lit
 *     texels get a second image, *_emissive.png, as the material's
 *     emissiveTexture. For XWA OPTs (version 5), this reproduces how the
 *     engine derives its lightmaps (ModelTexture_FilterHardwarePalette @
 *     0x44A600 and CreateD3DfromTexture). A palette entry is self-lit when its
 *     color in row 0, the darkest row, is bright (squared 5-bit magnitude
 *     >= 32, with the green LSB dropped) and rows 1 to 6 stay within a squared
 *     distance of 16 from row 0. The glow color comes from palette row 10.
 *     Textures named "_*" are skipped, and so are palettes where no entry or
 *     all 256 entries are self-lit;
 *   - projectiles (model_tables.h) are fully self-lit instead, as the classic
 *     renderer draws them. The base color is black with the texture's alpha
 *     kept, the specular factor is 0, and the base texture becomes the
 *     emissiveTexture at strength OPT_GLTF_PROJECTILE_STRENGTH. With alpha,
 *     the emission is multiplied by the alpha;
 *   - the component tags and articulations that the game ties to fixed
 *     component numbers are added (model_tables.h). For the player's craft,
 *     the OPT's Cockpit hardpoints (type 31) are replaced by one hardpoint at
 *     the pilot eye from the game's craft table.
 *
 * XWAU mode converts X-Wing Alliance Upgrade models with everything XWAU adds
 * to them. The files are read from an XWAU installation (the game folder that
 * XWAU is installed into), with case-insensitive paths. The stored normal of
 * each corner is used. For each OPT:
 *   - Materials/<basename>.mat, applied on top of
 *     Materials/DefaultGlobalMaterial.mat, gives each texture its material and
 *     its normal map, which is read from the DAT image that the material
 *     names. The converter then reproduces the shading of XWAU's renderer with
 *     standard glTF inputs: authored colors, illumination, shadeless and glass
 *     texels, metallic-roughness, KHR_materials_specular, and normal maps with
 *     NMIntensity baked in and MikkTSpace tangents;
 *   - its image sequences become material animations (xwau_anim.h);
 *   - the files next to the OPT give the pilot eye, object profiles,
 *     articulations and Dynamic Cockpit screens (xwau_model.h). The skins
 *     become material variants (xwau_skins.h). The missions add their own
 *     selections (xwau_missions.h);
 *   - the crew and glass textures listed in model_tables.h are shaded as lit
 *     surfaces, even though their material makes them Shadeless;
 *   - projectiles are baked unlit, as XWAU's renderer draws them, whatever
 *     their materials say (XwauGltfProjectile). A model is a projectile when
 *     an object type of the player's or the AI's projectiles (genus 6 or 7)
 *     uses it in the game's tables, as XWAU's renderer picks them. Without
 *     the tables, the stock projectile models are used. A projectile is a
 *     warhead when its name has Missile, Torpedo, SpaceBomb, Pulse, Rocket or
 *     Flare, but not Boat, in any case, as XWAU's renderer tags them. The
 *     others are lasers;
 *   - more than 120 weapon hardpoints are reported, because the game keeps
 *     only the first 120 weapon slots, as XWAU does.
 */

#include <stdbool.h>

#include "xwau_config.h"
#include "xwau_game_tables.h"
#include "xwau_mat.h"
#include "xwau_missions.h"

/* The conversion mode. It is stock mode unless ModelConversion_InitXwau
 * initialized it. */
typedef struct ModelConversion {
	float smooth_angle; /* stock mode, in degrees (0..180); negative means 90 */
	bool  xwau;
	/* XWAU mode (ModelConversion_InitXwau). */
	const char*   game;   /* the XWAU installation */
	XwauMatParams   global; /* Materials/DefaultGlobalMaterial.mat, or the built-in defaults */
	XwauMissions*   missions;
	XwauGameTables* tables; /* the game's object types and model lists, or NULL when they can't be read */
} ModelConversion;

/* Sets XWAU mode for the installation `game`, and reads the global material,
 * the missions and the game's tables. Returns false when out of memory
 * (reported). */
bool ModelConversion_InitXwau(ModelConversion* mc, const char* game);
void ModelConversion_Free(ModelConversion* mc);

/* Converts the OPT at `opt_path`, named `basename`, into <out_dir> with
 * OptGltf_Convert. Problems are reported with `opt_path` as the label. */
bool ModelConversion_Convert(const ModelConversion* mc, const char* opt_path, const char* basename,
							 const char* out_dir);

#endif /* AERON_OPT2GLTF_MODEL_CONVERSION_H */
