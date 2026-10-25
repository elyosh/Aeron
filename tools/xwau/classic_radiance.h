#ifndef XWAU_CLASSIC_RADIANCE_H
#define XWAU_CLASSIC_RADIANCE_H

/* The illuminance in W/m^2 of a classic light of brightness 1: the game's
 * classic light calibration (classic_lighting.illuminance in the remaster's
 * settings), the sun at the Earth's distance. The imports can't read the
 * game's settings and write values made from it into their files: when the
 * calibration changes, the models and the skyboxes must be imported
 * again. */
#define CLASSIC_ILLUMINANCE 186.0f

/* The radiance in W/m^2/sr that the imports give a classic image value of
 * 1: a sky texel, and an emission strength of 1 in a converted model. The
 * game takes a classic backdrop texel as the reflectance of a surface lit
 * by a classic light of brightness 1, so the radiance is
 * CLASSIC_ILLUMINANCE / pi. Classic emission gets the same radiance, which
 * keeps the classic balance between lights, backdrops and emission. */
#define CLASSIC_RADIANCE (CLASSIC_ILLUMINANCE / 3.14159265f)

#endif
