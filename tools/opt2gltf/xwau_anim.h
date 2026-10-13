#ifndef AERON_XWAU_ANIM_H
#define AERON_XWAU_ANIM_H

/*
 * XWAU image sequences (xwau_mat.h) to the animated-material data of one
 * converted model: per OPT texture, the flipbooks, damage and energy of its
 * material's AERON_flight_model object, whether its illumination frames need
 * the colours before the illumination split, and whether one of them always
 * plays. The library images they name are written by xwau_library.h, which
 * reads them back from the converted models.
 */

#include <stdbool.h>
#include <stddef.h>

#include "opt.h"
#include "opt_gltf.h"
#include "xwau_gltf_material.h"
#include "xwau_mat.h"

/* Resolves a game-relative path (e.g. "Effects\GenericDamage.dat") to a
 * file; returns 0 when absent. */
typedef int (*XwauAnimFindFile)(void* context, const char* rel, char* out, size_t cap);

typedef struct XwauAnim XwauAnim;

/* `model` names the model (seeds and diagnostics); `label` prefixes the
 * report lines. */
XwauAnim* XwauAnim_Begin(const char* model, XwauAnimFindFile find, void* find_context, const char* label);

/* What the texture becomes in the Dynamic Cockpit: no screen, a screen
 * with a cover, one without, or a hologram. */
typedef enum XwauAnimScreen {
	XWAU_ANIM_SCREEN_NONE,
	XWAU_ANIM_SCREEN_COVER,
	XWAU_ANIM_SCREEN_NO_COVER,
	XWAU_ANIM_SCREEN_HOLOGRAM
} XwauAnimScreen;

/* Fills `out` for OPT texture `t` from `mat` (NULL: no material file).
 * `unlit` is the texture's unlit weight (XwauGltf_Unlit), which its
 * colour frames take. On a screen, only what XWAU's screen shader shows is
 * kept: colour frames over a cover; without a cover, cockpit damage frames
 * only, unlit; on a hologram, nothing. The strings and images it points to
 * live until XwauAnim_End. Returns false on OOM. */
bool XwauAnim_Texture(XwauAnim* anim, const XwauMatFile* mat, const opt_texture_t* t, float unlit,
					  XwauAnimScreen screen, XwauGltfAnim* out);

/* Whether any texture has animation data. */
bool XwauAnim_Any(const XwauAnim* anim);

/* Prints the conversion summary and what was skipped. */
void XwauAnim_Report(const XwauAnim* anim);

void XwauAnim_End(XwauAnim* anim);

#endif /* AERON_XWAU_ANIM_H */
