#ifndef AERON_OPT2GLTF_XWAU_SKINS_H
#define AERON_OPT2GLTF_XWAU_SKINS_H

/*
 * XWAU skin inputs of one OPT: the
 * variants file the conversion tools write, and the skin images they
 * extract to <skins dir>/<skin>/<file name>, lower case.
 *
 * The variants file is JSON:
 *   {"variants": [{"name": "0|default_2,ace", "state": 0,
 *                  "layers": [{"skin": "default_2", "opacity": 100}, ...]}, ...],
 *    "default_markings": ["0|default_0", null, ...]}
 *
 * A skin's image for an OPT texture is <skin>/<texture name>.bmp, else .png,
 * else .jpg (the hook's order). It is decoded at 8 bits per channel and
 * flipped vertically into OPT row order, as the hook flips it; an image of
 * another size than the texture's is skipped and reported.
 */

#include <stdbool.h>
#include <stddef.h>

#include "opt.h"
#include "opt2gltf.h"

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

bool XwauSkins_Load(const char* variants_path, const char* skins_dir, const opt_file_t* opt,
					const char* label, XwauSkins* out);
void XwauSkins_Free(XwauSkins* skins);

#endif /* AERON_OPT2GLTF_XWAU_SKINS_H */
