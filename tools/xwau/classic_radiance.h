#ifndef XWAU_CLASSIC_RADIANCE_H
#define XWAU_CLASSIC_RADIANCE_H

/* The illuminance in W/m^2 of a classic light of brightness 1, the sun at
 * the Earth's distance. It is the default of the game's
 * classic_lighting.illuminance. The importer can't read the game's
 * settings, so it writes values based on the defaults into its files. When
 * a default changes, the mods must be imported again. */
#define CLASSIC_ILLUMINANCE 186.0f

/* The radiance in W/m^2/sr that the imports give an emission strength of 1
 * in a converted model. It is the game's radiance of a white pixel of the
 * classic images (classic_lighting.image_radiance), so the converted models
 * match the classic emission. The default makes a white pixel as bright as
 * a white surface that faces a light of CLASSIC_ILLUMINANCE. */
#define CLASSIC_RADIANCE 59.2056f

#endif
