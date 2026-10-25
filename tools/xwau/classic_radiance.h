#ifndef XWAU_CLASSIC_RADIANCE_H
#define XWAU_CLASSIC_RADIANCE_H

/* The radiance in W/m^2/sr that the imports give a classic image value of
 * 1: a sky texel, and an emission strength of 1 in a converted model. The
 * game takes a classic backdrop texel as the reflectance of a surface lit
 * by a classic light of brightness 1, which it calibrates at 186 W/m^2
 * (classic_lighting.illuminance in the remaster's settings, the sun at the
 * Earth's distance). So the radiance is 186 / pi. Classic emission gets the
 * same radiance, which keeps the classic balance between lights, backdrops
 * and emission. The imports can't read the game's settings and write this
 * value into their files: when the calibration changes, the models and the
 * skyboxes must be imported again. */
#define CLASSIC_RADIANCE 59.2f

#endif
