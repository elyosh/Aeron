#ifndef AERON_XWAU_HANGAR_CRAFTS_H
#define AERON_XWAU_HANGAR_CRAFTS_H

/*
 * Converts the craft lists of hook_hangars into the `hangar_crafts` section
 * of flight/flight.yaml.
 *
 * The hook reads CraftSelectionFighters.txt, CraftSelectionTransports.txt
 * and CraftSelectionCrafts.txt from the game folder when it loads. It has no
 * setting that turns it off, so the files are used whenever
 * hook_hangars.dll is there. Each line is a model index, which is also the
 * object type. The files become the lists `fighters`, `transports` and
 * `all`.
 *
 * A key is only written when its file exists. Without the file, the hook
 * uses the original game's list, and so does OpenXWA without the key. A
 * file with no lines gives an empty list.
 */

#include <stdbool.h>
#include <stdio.h>

#include "xwau_names.h"

/* Writes the `hangar_crafts` section of the installation `game` to `f`.
 * Returns whether it wrote anything. */
bool XwauHangarCrafts_Write(FILE* f, const char* game, const XwauNames* names);

#endif /* AERON_XWAU_HANGAR_CRAFTS_H */
