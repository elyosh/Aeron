#ifndef AERON_OPT2GLTF_MODEL_TABLES_H
#define AERON_OPT2GLTF_MODEL_TABLES_H

/*
 * Data that the conversion needs but that no file next to an OPT contains.
 * The values are fixed by the game or by an XWA Upgrade (XWAU) release, or
 * chosen for the conversion. A model is named by its OPT file name without the
 * extension, compared in any ASCII case.
 */

#include <stdbool.h>
#include <stddef.h>

/* The pilot eye that XWAU's craft table gives to the craft entry with this
 * name, in CockpitPovX/Y/Z units. It is used when the model's files set no
 * eye. */
bool ModelTables_XwauEye(const char* model, int eye[3]);

/* The pilot eye that the original game's craft table gives to the player's
 * craft that use this model. */
bool ModelTables_StockEye(const char* model, int eye[3]);

/* The row in the craft list (FlightModels/Spacecraft0.LST) for a flight
 * group's craft type, or -1. */
int ModelTables_CraftListRow(int craft_type);

/* The component tags and articulations of a model in the original game, as
 * space-separated entries N=TAG and N=GROUP:ANGLE:SPEED[:SPEED_BACK][:BEHAVIOR],
 * where N is the component. Returns NULL when the model has none. */
const char* ModelTables_StockTags(const char* model);

/* Whether the original game draws the model fully self-lit (projectiles). */
bool ModelTables_IsProjectile(const char* model);

/* The XWAU textures of a model (crew and cockpit glass) that are converted as
 * lit surfaces even though their material makes them Shadeless. The names are
 * separated by spaces. Returns NULL when there are none. */
const char* ModelTables_LitTextures(const char* model);

#endif /* AERON_OPT2GLTF_MODEL_TABLES_H */
