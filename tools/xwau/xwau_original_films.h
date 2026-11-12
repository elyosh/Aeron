#ifndef AERON_XWAU_ORIGINAL_FILMS_H
#define AERON_XWAU_ORIGINAL_FILMS_H

/*
 * Writes the `original_films` section of the game's flight/flight.yaml: the
 * craft record layout of films recorded with this installation.
 *
 * A film saves the state of every object at its start. hook_opt_limit makes
 * each craft record larger (Craft_ExtraSize) and moves its per-mesh,
 * engine and weapon rack arrays to the new end of the record. The game
 * needs the record's size, the arrays' entry counts and their offsets to play
 * these films.
 *
 * The hook changes the records when hook_opt_limit.dll is there, is a build
 * that reads Craft_ExtraSize, and its MeshesCount isn't 0. Its settings are
 * in hook_opt_limit.cfg, or else in the [hook_opt_limit] section of
 * Hooks.ini. The defaults are the hook's:
 * MeshesCount = 255, 255 engines and 128 weapon racks of 14 bytes, in this
 * order after the stock 1017 bytes.
 */

#include <stdbool.h>
#include <stdio.h>

/* Returns whether it wrote the section. */
bool XwauOriginalFilms_Write(FILE* f, const char* game);

#endif /* AERON_XWAU_ORIGINAL_FILMS_H */
