#ifndef AERON_XWAU_ANIM_H
#define AERON_XWAU_ANIM_H

/*
 * XWAU image sequences (xwau_mat.h) to the animated-material data of one
 * converted model (docs: xwau_animated_materials.md): per OPT texture, the
 * aeronFlipbooks / aeronDamage / aeronEnergy extras, whether it needs
 * aeronAnimBaseColor, and the static look of an `always` illumination flipbook;
 * per model, the effect-library images it references (the `.effects.json`
 * sidecar the library build reads).
 */

#include <stdbool.h>
#include <stddef.h>

#include "opt.h"
#include "opt2gltf.h"
#include "xwau_mat.h"

/* Resolves a game-relative path (e.g. "Effects\GenericDamage.dat") to a
 * file; returns 0 when absent. */
typedef int (*XwauAnimFindFile)(void* context, const char* rel, char* out, size_t cap);

typedef struct XwauAnim XwauAnim;

/* `model` names the model (seeds and diagnostics); `label` prefixes the
 * report lines. */
XwauAnim* XwauAnim_Begin(const char* model, XwauAnimFindFile find, void* find_context, const char* label);

/* Fills `out` for OPT texture `t` from `mat` (NULL: no material file). The
 * strings and images it points to live until XwauAnim_End. Returns false
 * on OOM. */
bool XwauAnim_Texture(XwauAnim* anim, const XwauMatFile* mat, const opt_texture_t* t, OptGltfXwauAnim* out);

/* Whether any texture has animation data. */
bool XwauAnim_Any(const XwauAnim* anim);

/* Writes the referenced library images as JSON:
 *   {"version":1,"model":"...","images":[{"library":"genericdamage",
 *    "dat":"Effects/GenericDamage.dat","group":8,"image":2,"kind":"multiply",
 *    "tint":[1,1,1],"alphaMode":0,"key":"8-2:multiply"}, ...]} */
bool XwauAnim_WriteSidecar(const XwauAnim* anim, const char* path);

/* Prints the conversion summary and what was skipped. */
void XwauAnim_Report(const XwauAnim* anim);

void XwauAnim_End(XwauAnim* anim);

#endif /* AERON_XWAU_ANIM_H */
