#ifndef AERON_OPT2GLTF_XWAU_ACTIVE_COCKPIT_H
#define AERON_OPT2GLTF_XWAU_ACTIVE_COCKPIT_H

/*
 * Reads X-Wing Alliance Upgrade Active Cockpit files
 * (`DynamicCockpit/<Cockpit>.ac`) and turns them into the buttons of the
 * converted model's materials (opt_gltf.h).
 *
 * A file has the sections of a Dynamic Cockpit file: `[<Cockpit>.opt,<Texture>]`
 * binds to the first texture of the converted OPT that has the name. In a
 * section, `texture_size = <width>,<height>` gives the size that the
 * rectangles are measured in, and each `action = <key>, <x0>,<y0>, <x1>,<y1>`
 * line is a rectangle that sends a key. The parsing follows LoadACAction and
 * TranslateACAction in xwa_ddraw_d3d11: the key's name can be written in any
 * case, and its modifiers are found anywhere in it.
 *
 * A button does an input action of the game, not a key. A key becomes the
 * action that it does in the game's default keyboard bindings
 * (XwauNames.key_actions). `HOLOGRAM` becomes `toggle_holograms` and
 * `VRKEYB_TOGGLE` becomes `toggle_vr_keyboard`. A key without an action is
 * left out.
 */

#include <stdbool.h>
#include <stdint.h>

#include "opt.h"
#include "xwau_names.h"

/* The most rectangles of one texture (MAX_AC_COORDS_PER_TEXTURE). */
#define XWAU_ACTIVE_COCKPIT_MAX_BUTTONS 80

typedef struct XwauActiveCockpitButton {
	float       rect[4]; /* u0, v0, u1, v1, sorted */
	const char* action;  /* an action name of `names`, or a fixed one */
} XwauActiveCockpitButton;

typedef struct XwauActiveCockpit {
	/* One entry per OPT texture: the JSON text of the `buttons` list of the
	 * texture's material, or NULL. */
	char**  buttons;
	int32_t texture_count;
	int     converted; /* rectangles that became buttons */
	int     skipped;   /* rectangles whose key has no action */
} XwauActiveCockpit;

/* Reads the value of an `action` line, after its `=`. `width` and `height`
 * are the section's texture size. With `flip_v`, a rectangle's Y is counted
 * from the bottom of the image: XWAU's glove buttons are compared with the
 * texture coordinates of an OBJ file. Returns 1 with the button, 0 when the
 * key has no action (`key` then has the key's normalised name), and -1 when
 * the line can't be read. */
int XwauActiveCockpit_ParseAction(const char* value, float width, float height, bool flip_v,
								  const XwauNames* names, XwauActiveCockpitButton* out, char key[64]);

/* Returns the JSON text of a `buttons` list, owned by the caller, or NULL. */
char* XwauActiveCockpit_ButtonsJson(const XwauActiveCockpitButton* buttons, int count);

/* Reads `ac_path` for the OPT `basename` (its file name without the
 * extension). A section whose texture isn't in the model and a key without
 * an action are reported and left out. Returns false only when the file
 * can't be read or memory runs out. */
bool XwauActiveCockpit_Load(const char* ac_path, const opt_file_t* opt, const char* basename,
							const XwauNames* names, XwauActiveCockpit* out, const char* label);

void XwauActiveCockpit_Free(XwauActiveCockpit* ac);

#endif /* AERON_OPT2GLTF_XWAU_ACTIVE_COCKPIT_H */
