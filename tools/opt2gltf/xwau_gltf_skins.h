#ifndef AERON_OPT2GLTF_XWAU_GLTF_SKINS_H
#define AERON_OPT2GLTF_XWAU_GLTF_SKINS_H

/*
 * Turns XWAU skins (xwau_skins.h) into skin layers and skin variants of the
 * conversion (opt_gltf.h; tools/extensions/AERON_flight_model, "Skins"). A
 * skin is a layer of every state-0 texture of LOD 0 that has pixels, is not
 * a screen surface and has an image of the skin. The layer is the skin's
 * colors baked like the texture itself (xwau_gltf_material.h), with the
 * texture's alpha and illumination; its coverage is the skin's alpha. The
 * cook composites the layers of each variant over the baked texture, which
 * matches baking the hook's blend of the colors wherever a layer is fully
 * opaque or fully transparent. When one skin changes a texture's emissive or
 * metallic-roughness under its coverage, every skin of that texture carries
 * that channel, so that a later layer replaces it as the hook's blend would.
 */

#include <stdbool.h>
#include <stdint.h>

#include "opt.h"
#include "opt_gltf.h"
#include "xwau_gltf_material.h"
#include "xwau_skins.h"

typedef struct XwauGltfSkins XwauGltfSkins;

/* Builds the skin layers and skin variants of `skins` for `opt`. Each skin
 * becomes a layer of the materials whose textures it has an image for: the
 * skin's colors baked with the texture's alpha, illumination and material.
 * Each variant becomes a skin variant, which the cook composites
 * (tools/extensions/AERON_flight_model, "Skins"), or a plain variant that
 * shows its state when none of its skins has an image. `skin_names` names
 * each skin. `materials`, `anims` and `baked` have one entry per texture,
 * and `anims` may be NULL. `screens` has one entry per texture or is NULL.
 * Image files are named after `basename`. Returns NULL when out of memory
 * or when the skins are invalid. Both cases are reported. */
XwauGltfSkins* XwauGltfSkins_Build(const opt_file_t* opt, const char* basename, const XwauSkinVariants* skins,
								   const char* const* skin_names, const OptGltfScreen* screens, int base_shade,
								   const XwauGltfMaterial* materials, const XwauGltfAnim* anims,
								   const XwauBaked* baked);

/* The skin layers of texture `texture` in layer mode, and their count. */
const OptGltfSkinLayer* XwauGltfSkins_Layers(const XwauGltfSkins* skins, int32_t texture, size_t* count);

/* Returns the variants. They stay valid as long as `skins` does. */
const OptGltfVariants* XwauGltfSkins_Variants(const XwauGltfSkins* skins);

/* Reports the variants and layers to stderr. */
void XwauGltfSkins_Report(const XwauGltfSkins* skins, const char* basename);

void XwauGltfSkins_Free(XwauGltfSkins* skins);

#endif /* AERON_OPT2GLTF_XWAU_GLTF_SKINS_H */
