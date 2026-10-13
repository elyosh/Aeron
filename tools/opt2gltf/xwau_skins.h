#ifndef AERON_OPT2GLTF_XWAU_SKINS_H
#define AERON_OPT2GLTF_XWAU_SKINS_H

/*
 * XWAU skins of one OPT (hook_32bpp: ReadOptFunction, ReadFgSkins,
 * GetFlightgroupsColors), as the converter's skin variants (opt2gltf.h).
 *
 * A skin is FlightModels/Skins/<Opt>/<Skin>, a folder with files at its top
 * level (the folders of every root), else <Skin>.zip (the first root's).
 * Each skin context (xwau_missions.h) gives each markings value i a list:
 * the `<Opt> = ...` tokens then the `<Opt>_fgc_<i> = ...` ones; when both
 * are empty, Default_<i> if that skin exists, else Default. The values
 * counted are up to the highest _fgc_ line, the OPT's states and the highest
 * Default_<N> skin. A token is `<skin>[-<opacity>]` (opacity clamped to 0
 * to 100, 100 when it does not parse); a skin that does not exist adds
 * nothing, and a list left empty shows no skin. Unskinned face groups show
 * state i when the OPT has that many states, else 0.
 *
 * Every distinct list over the contexts is a variant, named
 * "<state>|<skin>[-<opacity>],..." (lower case); the global context gives
 * the markings values' default variants.
 *
 * A skin's image for an OPT texture is its file named after the texture
 * (case-insensitive) with .bmp, else .png, else .jpg: in a folder, the first
 * root's; in a zip, the first entry with that file name, whatever its
 * folder. It is decoded at 8 bits per channel and flipped vertically into
 * OPT row order, as the hook flips it; an image of another size than the
 * texture's is skipped and reported.
 */

#include <stdbool.h>
#include <stddef.h>

#include "opt.h"
#include "opt2gltf.h"
#include "xwau_config.h"
#include "xwau_missions.h"

typedef struct XwauSkins {
	OptGltfSkins        skins; /* points into the arrays below */
	char**              skin_names;
	OptGltfImageView*   images; /* skin_count * texture_count; rgba owned */
	size_t              image_count;
	OptGltfSkinVariant* variants;
	char**              variant_names;
	OptGltfSkinLayer*   layers;
	char**              markings;
} XwauSkins;

/* Resolves the skins of `model` (the OPT's name without extension). No
 * variant (skins.variant_count 0) when it shows none. False on an error
 * (reported). */
bool XwauSkins_Resolve(const XwauRoots* roots, const XwauMissions* missions, const char* model,
					   const opt_file_t* opt, const char* label, XwauSkins* out);
void XwauSkins_Free(XwauSkins* skins);

/* The OPT's states: the largest texture count of a LOD 0 face group. */
int XwauSkins_StateCount(const opt_file_t* opt);

/* The skins of a model across the roots. */
typedef struct XwauSkinSet {
	char** names; /* lower case; "" is the skin folder's own files */
	size_t count;
	bool   exists; /* a root has FlightModels/Skins/<Opt> */
} XwauSkinSet;

bool XwauSkins_Names(const XwauRoots* roots, const char* model, XwauSkinSet* out);
void XwauSkins_FreeNames(XwauSkinSet* set);

#define XWAU_SKINS_MAX_MARKINGS 256

/* What a markings value shows in one skin context: its list (the variant
 * "<state>|<list>" when skinned), or no skin and state `state`. */
typedef struct XwauMarkings {
	bool skinned;
	int  state;
	char list[1024];
} XwauMarkings;

/* The markings values of `model`, of `states` states, in the skin context
 * `lines` (*count of them, at most XWAU_SKINS_MAX_MARKINGS; 0 when it shows
 * no skin). False when out of memory. */
bool XwauSkins_Markings(const XwauLines* lines, const char* model, int states, const XwauSkinSet* skins,
						XwauMarkings* out, size_t* count);

#endif /* AERON_OPT2GLTF_XWAU_SKINS_H */
