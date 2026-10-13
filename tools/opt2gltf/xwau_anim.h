#ifndef AERON_XWAU_ANIM_H
#define AERON_XWAU_ANIM_H

/*
 * Converts XWAU image sequences (xwau_mat.h) into the animated-material data of
 * one converted model. For each OPT texture, it gives the flipbooks, damage and
 * energy of the AERON_flight_model object of its material. It also tells
 * whether the illumination frames need the colours from before the
 * illumination split, and whether an illumination flipbook always plays. The
 * library images named by this data are written by xwau_library.h, which reads
 * them back from the converted models.
 */

#include <stdbool.h>
#include <stddef.h>

#include "opt.h"
#include "opt_gltf.h"
#include "xwau_gltf_material.h"
#include "xwau_mat.h"

/* Resolves a game-relative path (e.g. "Effects\GenericDamage.dat") to a
 * file. Returns 0 when the file does not exist. */
typedef int (*XwauAnimFindFile)(void* context, const char* rel, char* out, size_t cap);

typedef struct XwauAnim XwauAnim;

/* `model` is the model name, which seeds the random frame picks. `label` is
 * the prefix of the report lines. */
XwauAnim* XwauAnim_Begin(const char* model, XwauAnimFindFile find, void* find_context, const char* label);

/* What the texture is in the Dynamic Cockpit: not a screen, a screen with a
 * cover, a screen without a cover, or a hologram. */
typedef enum XwauAnimScreen {
	XWAU_ANIM_SCREEN_NONE,
	XWAU_ANIM_SCREEN_COVER,
	XWAU_ANIM_SCREEN_NO_COVER,
	XWAU_ANIM_SCREEN_HOLOGRAM
} XwauAnimScreen;

/* Fills `out` for the OPT texture `t` from `mat`, which is NULL when there is
 * no material file. `unlit` is the unlit weight of the texture
 * (XwauGltf_Unlit), and its colour frames use the same weight. On a screen,
 * only what XWAU's screen shader shows is kept. Over a cover, these are the
 * colour frames. Without a cover, only the cockpit damage frames are kept, and
 * they are unlit. On a hologram, nothing is kept. The strings and images that
 * `out` points to live until XwauAnim_End. Returns false when out of memory. */
bool XwauAnim_Texture(XwauAnim* anim, const XwauMatFile* mat, const opt_texture_t* t, float unlit,
					  XwauAnimScreen screen, XwauGltfAnim* out);

/* Whether any texture has animation data. */
bool XwauAnim_Any(const XwauAnim* anim);

/* Prints the conversion summary and what was skipped. */
void XwauAnim_Report(const XwauAnim* anim);

void XwauAnim_End(XwauAnim* anim);

#endif /* AERON_XWAU_ANIM_H */
