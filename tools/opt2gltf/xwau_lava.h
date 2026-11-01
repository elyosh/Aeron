#ifndef AERON_OPT2GLTF_XWAU_LAVA_H
#define AERON_OPT2GLTF_XWAU_LAVA_H

/*
 * The generated `lava` effect library: the frames of XWAU's procedural lava,
 * baked into two looping emissive flipbooks.
 *
 * XWAU computes the lava for each pixel from a noise texture of rand()
 * values. Here the noise comes from MSVC's rand() with its default seed, laid
 * out as XWAU reads it, so the pattern looks the same but isn't the same
 * texel for texel. Each flipbook has XWAU_LAVA_FRAMES frames of
 * XWAU_LAVA_SIZE² over XWAU_LAVA_SECONDS, and each frame tiles over the
 * texture's UVs:
 *   - with LavaTiling on, a frame is XWAU's tiled lava (noisetile with repeat
 *     2) as it is;
 *   - with LavaTiling off, XWAU's lava (flow of UV × 3) doesn't tile, so the
 *     frame is the tiled lava with repeat 3, which has the same feature size.
 * The loop blends the lava at time t + T into the lava at time t, with the
 * same variance correction that noisetile uses, so the contrast doesn't drop
 * halfway.
 *
 * The frames are made for the default size and colour. XWAU draws the lava
 * unlit, as LavaColor × rz, with rz above 1 in the brightest parts. A frame
 * holds that colour in linear light, divided by XwauLava_Strength(), which is
 * the linear value of the brightest colour kept.
 */

#include <stdbool.h>
#include <stddef.h>

#include "xwau_library.h"

#define XWAU_LAVA_LIBRARY "lava"
#define XWAU_LAVA_FRAMES 48
#define XWAU_LAVA_SECONDS 4.0f
#define XWAU_LAVA_SIZE 256

/* The image key of frame `frame` of the flipbook with LavaTiling on or off. */
void XwauLava_Key(char* out, size_t cap, bool tiling, int frame);

/* The emissive strength of the frames. */
float XwauLava_Strength(void);

/* Writes the library to <effects_dir>/lava: lava.yaml and the frames under
 * frames/. With `merge`, an existing lava.yaml is kept as it is. Returns false
 * after reporting an error. */
bool XwauLava_WriteLibrary(const char* effects_dir, bool merge);

/* Makes the library in memory, for XwauLibrary_Make: its library file content
 * and its frames, rendered on `threads` threads. Returns false after
 * reporting an error. */
bool XwauLava_MakeLibrary(int threads, struct cJSON** library, XwauLibraryInput** inputs, size_t* count);

#endif /* AERON_OPT2GLTF_XWAU_LAVA_H */
