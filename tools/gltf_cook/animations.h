/*
 * The animation merge of aeron_gltf_cook --animations
 * (tools/effect_pack/README.md, "Materials"): the `materials` of an
 * animation file written into a .gltf's material extras, before cooking.
 */
#ifndef AERON_GLTF_COOK_ANIMATIONS_H
#define AERON_GLTF_COOK_ANIMATIONS_H

#include <stdbool.h>

/* Writes `out_gltf`, the .gltf `gltf` with the animation file `anim` merged:
 * per material named there, the `flipbooks`, `damage` and `energy` it gives
 * replace the material's aeronFlipbooks, aeronDamage and aeronEnergy, the
 * file's `library` filling in a flipbook's or damage's own; an
 * `animBaseColor` PNG becomes a new image and texture named by
 * aeronAnimBaseColor. Members the file does not give are kept.
 * `out_gltf` must be in the directory of `gltf` (buffer and image URIs
 * stay relative to it). With `effects_dir`, every key must be in its
 * library there. Messages go to stderr. */
bool aeron_gltf_cook_merge_animations(const char* gltf, const char* anim, const char* effects_dir,
									  const char* out_gltf);

#endif /* AERON_GLTF_COOK_ANIMATIONS_H */
