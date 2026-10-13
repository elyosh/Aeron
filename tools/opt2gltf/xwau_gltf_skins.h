#ifndef AERON_OPT2GLTF_XWAU_GLTF_SKINS_H
#define AERON_OPT2GLTF_XWAU_GLTF_SKINS_H

/*
 * Turns XWAU skins (xwau_skins.h) into material variants of the conversion
 * (opt_gltf.h, OptGltfVariants). This applies to every state-0 texture of
 * LOD 0 that has pixels and is not a screen surface. For each skin variant,
 * the layers that have an image for the texture are blended onto its
 * decoded colors, in order, with the integer straight-alpha blend of the
 * hook. Each distinct result is a composite, shared by all variants with
 * the same layers. A composite is baked like the texture itself
 * (xwau_gltf_material.h) and becomes a separate material. When the
 * emissive, metallic-roughness or transmission of a composite is equal to
 * that of the texture, the composite uses the image of the texture.
 */

#include <stdbool.h>
#include <stdint.h>

#include "opt.h"
#include "opt_gltf.h"
#include "xwau_gltf_material.h"
#include "xwau_skins.h"

typedef struct XwauGltfSkins XwauGltfSkins;

/* Builds the variants of `skins` for `opt`. `materials`, `anims` and
 * `baked` have one entry per texture, and `anims` may be NULL. `textures`
 * holds the given materials of the textures. `screens` has one entry per
 * texture or is NULL. Image files are named after `basename`. Returns NULL
 * when out of memory or when the skins are invalid. Both cases are
 * reported. */
XwauGltfSkins* XwauGltfSkins_Build(const opt_file_t* opt, const char* basename, const XwauSkinVariants* skins,
								   const OptGltfScreen* screens, int base_shade,
								   const XwauGltfMaterial* materials, const XwauGltfAnim* anims,
								   const XwauBaked* baked, const OptGltfMaterial* textures);

/* Returns the variants. They stay valid as long as `skins` does. */
const OptGltfVariants* XwauGltfSkins_Variants(const XwauGltfSkins* skins);

/* Reports the variants and composites to stderr. */
void XwauGltfSkins_Report(const XwauGltfSkins* skins, const char* basename);

void XwauGltfSkins_Free(XwauGltfSkins* skins);

#endif /* AERON_OPT2GLTF_XWAU_GLTF_SKINS_H */
