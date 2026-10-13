#ifndef AERON_OPT2GLTF_XWAU_LIBRARY_H
#define AERON_OPT2GLTF_XWAU_LIBRARY_H

/*
 * The effect library images of converted XWAU models, written into a
 * project: a folder holding models (models/<key>/<key>.gltf) and their
 * effect libraries (effects/<library>/, animated_materials.md).
 *
 * The images are those the models' materials name (the flipbooks and
 * damage of their AERON_flight_model objects). A library is the images of
 * Effects/<library>.dat under the XWAU roots; a library with no such DAT is
 * not an XWAU one and is left alone. An image's key names its DAT image and
 * how it is baked (xwau_effect_bake.h), so the same key always gives the
 * same image: <library>/<library>.yaml lists them, with each image's inputs
 * as PNGs under <library>/frames/ (XWAU's rules applied: an illumination
 * frame as its emissive and its base colour with coverage, the tint and
 * alpha mode baked in), with its size cap and crop.
 *
 * Messages go to stderr.
 */

#include <stdbool.h>
#include <stddef.h>

#include "xwau_config.h"

/* Writes every library the models of `inputs` name (each a model .gltf, or
 * a project whose models are all read) into <project>/effects, replacing
 * the files there, and <project>/effects/effects.yaml: the energy effects
 * and the flipbooks of the DAT images the game draws directly (the
 * DAT-named material files Materials/dat-<G>-<I>.mat; the blast marks of
 * group 3050). */
bool XwauLibrary_Rebuild(const XwauRoots* roots, const char* project, const char* const* inputs,
						 size_t count);

/* Adds the images the model `gltf` names that its project's libraries lack
 * to <project>/effects: images already there are kept as they are. */
bool XwauLibrary_AddModel(const XwauRoots* roots, const char* project, const char* gltf);

#endif /* AERON_OPT2GLTF_XWAU_LIBRARY_H */
