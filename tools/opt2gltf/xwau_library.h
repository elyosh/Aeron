#ifndef AERON_OPT2GLTF_XWAU_LIBRARY_H
#define AERON_OPT2GLTF_XWAU_LIBRARY_H

/*
 * Writes the effect library images of converted XWAU models into a project.
 * A project is a folder that holds the models (models/<key>/<key>.gltf) and
 * their effect libraries (effects/<library>/, animated_materials.md).
 *
 * The images are the ones named by the materials of the models, in the
 * flipbooks, damage and details of their AERON_flight_model objects. A
 * library holds images of Effects/<library>.dat in the XWAU installation. A
 * library without this DAT does not come from XWAU and is left alone. An
 * image key identifies a DAT image and how it is baked (xwau_effect_bake.h),
 * so the same key always gives the same image. <library>/<library>.yaml
 * lists the images, with the size cap and crop of each one. The inputs of
 * each image are PNGs under <library>/frames/, with XWAU's rules already
 * applied: an illumination frame becomes an emissive image and a base colour
 * image with coverage, and the tint and alpha mode are baked in. The `lava`
 * library has no DAT: the importer generates it.
 *
 * Messages go to stderr.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xwau_config.h"

struct cJSON;

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

/* ---- Libraries made in memory ----------------------------------------------
 *
 * For a mod import that keeps no sources: each library is made as its library
 * file content and its input images in memory, for the library builder
 * (EffectLibrary_BuildFromInputs), instead of being written as files. */

/* The libraries that the models of an import name. */
typedef struct XwauLibrarySet XwauLibrarySet;

/* An input image of a library: `name` is its path in the library file. */
typedef struct XwauLibraryInput {
	char     name[160];
	uint8_t* rgba; /* tightly packed RGBA8; NULL once taken */
	int      width, height;
} XwauLibraryInput;

/* Reads the libraries that the models in `inputs` name, as
 * XwauLibrary_Rebuild does, and writes <dir>/effects.yaml. Returns NULL after
 * reporting an error. */
XwauLibrarySet* XwauLibrary_Read(const char* game, const char* const* inputs, size_t count, const char* dir);
void            XwauLibrary_FreeSet(XwauLibrarySet* set);

/* The number of libraries, the name of library `index`, and the memory that
 * making its inputs takes: its DAT file and the input images. */
size_t      XwauLibrary_Count(const XwauLibrarySet* set);
const char* XwauLibrary_Name(const XwauLibrarySet* set, size_t index);
size_t      XwauLibrary_InputBytes(const XwauLibrarySet* set, size_t index);

/* Makes library `index`: its library file content `*library` and its input
 * images `*inputs`, which the caller frees with cJSON_Delete and
 * XwauLibrary_FreeInputs. Different libraries can be made at the same time.
 * The lava library renders its frames on `threads` threads. Returns false
 * after reporting an error. */
bool XwauLibrary_Make(const XwauLibrarySet* set, size_t index, int threads, struct cJSON** library,
					  XwauLibraryInput** inputs, size_t* count);
void XwauLibrary_FreeInputs(XwauLibraryInput* inputs, size_t count);

#endif /* AERON_OPT2GLTF_XWAU_LIBRARY_H */
