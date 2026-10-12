/*
 * The animation merge of aeron_gltf_cook --animations
 * (animated_materials.md, "Model animations"): the `materials` of a model
 * animation file written into a .gltf's material extras, before cooking.
 * Authored layer names (baseColor, emissive) become the loader's (colour,
 * illumination).
 */
#ifndef AERON_GLTF_COOK_ANIMATIONS_H
#define AERON_GLTF_COOK_ANIMATIONS_H

#include <stdbool.h>

/* Writes `out_gltf`, the .gltf `gltf` with the model animation file `anim`
 * merged: per material named there, the `flipbooks`, `damage` and `energy`
 * it gives replace the material's aeronFlipbooks, aeronDamage and
 * aeronEnergy. Members the file does not give are kept.
 * `out_gltf` must be in the directory of `gltf` (buffer and image URIs
 * stay relative to it). With `effects_dir`, every key must be in its
 * library there; with `library_sources` too, each library named is first
 * built there from <library_sources>/<library>/<library>.yaml when that
 * file exists (an unchanged library is skipped). Messages go to stderr. */
bool aeron_gltf_cook_merge_animations(const char* gltf, const char* anim, const char* effects_dir,
									  const char* library_sources, const char* out_gltf);

/* Prints the model animation file `anim` as JSON on stdout, after checking
 * it as the merge does (libraries aside). Messages go to stderr. */
bool aeron_gltf_cook_print_animations(const char* anim);

struct cJSON;

/* Writes `file`, a model animation file's content (as printed by
 * aeron_gltf_cook_print_animations; read from `label`, for messages), to
 * `out` in the canonical form (effect_files.h), after the same checks. */
bool aeron_gltf_cook_write_animations(const char* label, const struct cJSON* file, const char* out);

#endif /* AERON_GLTF_COOK_ANIMATIONS_H */
