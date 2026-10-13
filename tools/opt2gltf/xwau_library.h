#ifndef AERON_OPT2GLTF_XWAU_LIBRARY_H
#define AERON_OPT2GLTF_XWAU_LIBRARY_H

/*
 * Writes the effect library images of converted XWAU models into a project.
 * A project is a folder that holds the models (models/<key>/<key>.gltf) and
 * their effect libraries (effects/<library>/, animated_materials.md).
 *
 * The images are the ones named by the materials of the models, in the
 * flipbooks and damage of their AERON_flight_model objects. A library holds
 * images of Effects/<library>.dat in the XWAU installation. A library without
 * this DAT does not come from XWAU and is left alone. An image key identifies
 * a DAT image and how it is baked (xwau_effect_bake.h), so the same key always
 * gives the same image. <library>/<library>.yaml lists the images, with the
 * size cap and crop of each one. The inputs of each image are PNGs under
 * <library>/frames/, with XWAU's rules already applied: an illumination frame
 * becomes an emissive image and a base colour image with coverage, and the
 * tint and alpha mode are baked in.
 *
 * Messages go to stderr.
 */

#include <stdbool.h>
#include <stddef.h>

#include "xwau_config.h"

/* Writes every library named by the models in `inputs` into <project>/effects,
 * replacing the files there. Each input is a model .gltf or a project. For a
 * project, all its models are read. Also writes <project>/effects/effects.yaml,
 * which lists the energy effects and the flipbooks of the DAT images that the
 * game draws directly. These flipbooks come from the material files named after
 * DAT images (Materials/dat-<G>-<I>.mat), such as the blast marks of group 3050. */
bool XwauLibrary_Rebuild(const char* game, const char* project, const char* const* inputs, size_t count);

/* Adds to <project>/effects the images that the model `gltf` names and that the
 * libraries of its project do not have yet. Images already there are kept as
 * they are. */
bool XwauLibrary_AddModel(const char* game, const char* project, const char* gltf);

#endif /* AERON_OPT2GLTF_XWAU_LIBRARY_H */
