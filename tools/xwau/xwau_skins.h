#ifndef AERON_XWAU_SKINS_H
#define AERON_XWAU_SKINS_H

/*
 * Converts the XWAU skins of one OPT (hook_32bpp: ReadOptFunction,
 * ReadFgSkins, GetFlightgroupsColors) into skin variants for the converter
 * (opt_gltf.h).
 *
 * A skin is a folder FlightModels/Skins/<Opt>/<Skin> with files at its top
 * level, or else a file <Skin>.zip in FlightModels/Skins/<Opt>.
 *
 * Each skin context (xwau_missions.h) gives each markings value i a list of
 * tokens. The list holds the `<Opt> = ...` tokens followed by the
 * `<Opt>_fgc_<i> = ...` tokens. When both are empty, the list is
 * Default_<i> if that skin exists, or else Default. The markings values go
 * up to the highest _fgc_ line, the last state of the OPT or the highest
 * Default_<N> skin, whichever is highest. A token is `<skin>[-<opacity>]`.
 * The opacity is clamped to 0..100, and it is 100 when it does not parse.
 * A skin that does not exist adds nothing, and a list that ends up empty
 * shows no skin. Face groups without a skin show state i if the OPT has
 * more than i states, or else state 0.
 *
 * Each distinct list across the contexts is a variant, named
 * "<state>|<skin>[-<opacity>],..." (lower case). The global context gives
 * the default variant of each markings value.
 *
 * The image of a skin for an OPT texture is the skin file named after the
 * texture (in any case), with .bmp, .png or .jpg tried in this order. In a
 * zip, it is the first entry with that file name, in any folder. The image
 * is decoded at 8 bits per channel and flipped vertically into OPT row
 * order, as the hook does. An image whose size is not the texture's size is
 * skipped and reported.
 */

#include <stdbool.h>
#include <stddef.h>

#include "opt.h"
#include "opt_gltf.h"
#include "xwau_config.h"
#include "xwau_missions.h"

/*
 * The skins as skin variants (xwau_gltf_skins.h): each variant lists its
 * skins in order, and the cook composites them over the textures that have
 * images of them, as the hook blends them. Primitives without any of a
 * variant's skins show the material of the variant's state.
 */
typedef struct XwauSkinLayer {
	uint32_t skin;    /* index into XwauSkinVariants.images rows */
	uint32_t opacity; /* 0..100 */
} XwauSkinLayer;

typedef struct XwauSkinVariant {
	const char*          name;
	uint32_t             state; /* asset state of primitives without any of its skins */
	const XwauSkinLayer* layers;
	size_t               layer_count;
} XwauSkinVariant;

typedef struct XwauSkinVariants {
	/* images[skin * opt->texture_count + texture] is the image of the skin
	 * for that OPT texture. It has the size of the texture and its rows are
	 * in OPT order. rgba is NULL when the skin has no image for it. */
	const OptGltfImageView* images;
	size_t                  skin_count;
	const XwauSkinVariant*  variants;
	size_t                  variant_count;
	/* Variant name for each markings value, or NULL when it has no skin. */
	const char* const* default_markings;
	size_t             markings_count;
} XwauSkinVariants;

typedef struct XwauSkins {
	XwauSkinVariants  skins; /* points into the arrays below */
	char**            skin_names;
	OptGltfImageView* images; /* skin_count * texture_count; rgba owned */
	size_t            image_count;
	XwauSkinVariant*  variants;
	char**            variant_names;
	XwauSkinLayer*    layers;
	char**            markings;
} XwauSkins;

/* Resolves the skins of `model` (the OPT's name without extension). When
 * the model shows no skin, skins.variant_count is 0. Returns false on an
 * error, which is reported. */
bool XwauSkins_Resolve(const char* game, const XwauMissions* missions, const char* model,
					   const opt_file_t* opt, const char* label, XwauSkins* out);
void XwauSkins_Free(XwauSkins* skins);

/* Returns the state count of the OPT, which is the largest texture count of
 * a LOD 0 face group. */
int XwauSkins_StateCount(const opt_file_t* opt);

/* The skins of a model, which are the folders and zips in
 * FlightModels/Skins/<Opt>. */
typedef struct XwauSkinSet {
	char** names; /* lower case; "" is for the files directly in Skins/<Opt> */
	size_t count;
	bool   exists; /* the game has FlightModels/Skins/<Opt> */
} XwauSkinSet;

bool XwauSkins_Names(const char* game, const char* model, XwauSkinSet* out);
void XwauSkins_FreeNames(XwauSkinSet* set);

#define XWAU_SKINS_MAX_MARKINGS 256

/* What a markings value shows in one skin context. When it is skinned, it
 * shows its list as the variant "<state>|<list>". Otherwise it shows no
 * skin, with state `state`. */
typedef struct XwauMarkings {
	bool skinned;
	int  state;
	char list[1024];
} XwauMarkings;

/* Resolves the markings values of `model`, which has `states` states, in
 * the skin context `lines`. Sets *count to their number, at most
 * XWAU_SKINS_MAX_MARKINGS, or 0 when the model shows no skin. Returns false
 * when out of memory. */
bool XwauSkins_Markings(const XwauLines* lines, const char* model, int states, const XwauSkinSet* skins,
						XwauMarkings* out, size_t* count);

#endif /* AERON_XWAU_SKINS_H */
