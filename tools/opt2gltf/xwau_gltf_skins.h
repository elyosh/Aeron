#ifndef AERON_OPT2GLTF_XWAU_GLTF_SKINS_H
#define AERON_OPT2GLTF_XWAU_GLTF_SKINS_H

/*
 * XWAU skins (xwau_skins.h) as the conversion's material variants
 * (opt_gltf.h, OptGltfVariants). For every state-0 texture of LOD 0 with
 * pixels that is not a screen surface, a skin variant's layers with an
 * image for the texture are blended, in order and with the hook's integer
 * straight-alpha blend, onto its decoded colors; each distinct result (a
 * composite, shared by the variants with the same layers) is baked as the
 * texture is (xwau_gltf_material.h) and becomes a material of its own. A
 * composite's emissive, metallic-roughness or transmission equal to the
 * texture's own is the texture's.
 */

#include <stdbool.h>
#include <stdint.h>

#include "opt.h"
#include "opt_gltf.h"
#include "xwau_gltf_material.h"
#include "xwau_skins.h"

typedef struct XwauGltfSkins XwauGltfSkins;

/* The variants of `skins` for `opt`: `materials`, `anims` (may be NULL) and
 * `baked` have one entry per texture, `textures` holds the textures' own
 * given materials, `screens` one entry per texture or NULL. Image files are
 * named after `basename`. NULL when out of memory, or when the skins are
 * invalid (reported). */
XwauGltfSkins* XwauGltfSkins_Build(const opt_file_t* opt, const char* basename, const XwauSkinVariants* skins,
								   const OptGltfScreen* screens, int base_shade,
								   const XwauGltfMaterial* materials, const XwauGltfAnim* anims,
								   const XwauBaked* baked, const OptGltfMaterial* textures);

/* The variants (valid while `skins` is). */
const OptGltfVariants* XwauGltfSkins_Variants(const XwauGltfSkins* skins);

/* Reports the variants and composites to stderr. */
void XwauGltfSkins_Report(const XwauGltfSkins* skins, const char* basename);

void XwauGltfSkins_Free(XwauGltfSkins* skins);

#endif /* AERON_OPT2GLTF_XWAU_GLTF_SKINS_H */
