#ifndef AERON_OPT2GLTF_MODEL_TABLES_H
#define AERON_OPT2GLTF_MODEL_TABLES_H

/*
 * Data the conversion needs that no file beside an OPT holds: fixed by the
 * game, by an XWA Upgrade (XWAU) release, or chosen for the conversion.
 * Models are named by their OPT's file name without extension, compared
 * without ASCII case.
 */

#include <stdbool.h>
#include <stddef.h>

/* The pilot eye XWAU's craft table gives the craft whose entry has this
 * name (CockpitPovX/Y/Z units), used when the model's files set none. */
bool ModelTables_XwauEye(const char* model, int eye[3]);

/* The pilot eye the original game's craft table gives the player's craft
 * that use this model. */
bool ModelTables_StockEye(const char* model, int eye[3]);

/* The row of the craft list (FlightModels/Spacecraft0.LST) of a flight
 * group's craft type, or -1. */
int ModelTables_CraftListRow(int craft_type);

/* The original game's component tags and articulations of a model, as
 * N=TAG and N=GROUP:ANGLE:SPEED[:SPEED_BACK][:BEHAVIOR] entries (N the
 * component), separated by spaces; NULL when it has none. */
const char* ModelTables_StockTags(const char* model);

/* Whether the original game draws the model fully self-lit (projectiles). */
bool ModelTables_IsProjectile(const char* model);

/* The XWAU textures of a model converted as lit surfaces although their
 * material makes them Shadeless (crew and cockpit glass), separated by
 * spaces; NULL when none. */
const char* ModelTables_LitTextures(const char* model);

#endif /* AERON_OPT2GLTF_MODEL_TABLES_H */
