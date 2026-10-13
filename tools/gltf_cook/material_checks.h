/*
 * The material checks of aeron_gltf_cook: each material's AERON_flight_model
 * object (its flipbooks, hull damage, energy effects and screen) and the
 * model root's energy default hold what the runtime loader
 * (aeron/src/scene/gltf_mesh.c) reads, so that the cooked model loads with
 * them (tools/extensions/AERON_flight_model).
 */
#ifndef AERON_GLTF_COOK_MATERIAL_CHECKS_H
#define AERON_GLTF_COOK_MATERIAL_CHECKS_H

#include <stdbool.h>

/* Checks the materials of the .gltf or .glb `path`. With `effects_dir`,
 * every frame and damage stage must be an image of the right kind in its
 * library there; with `project` too, each library named is first built
 * there from <project>/effects/<library>/<library>.yaml when that file
 * exists (an unchanged library is skipped). Messages go to stderr. */
bool aeron_gltf_cook_check_materials(const char* path, const char* effects_dir, const char* project);

#endif /* AERON_GLTF_COOK_MATERIAL_CHECKS_H */
