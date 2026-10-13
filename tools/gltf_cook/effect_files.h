/*
 * The canonical written form of the effect library file
 * (animated_materials.md, "Canonical form"), so that the tools write the
 * same values the same way:
 *
 *   - the file's leading comment lines, then its members, one image per
 *     line in YAML flow style; strings unquoted when that reads back the
 *     same (letters, digits, '_', '.', '/' and '-', starting with a letter
 *     or '_', not a boolean or null word), else double-quoted;
 *   - images in key order, the members of an entry in the format's order;
 *   - each number as the shortest decimal that reads back as the same
 *     32-bit float (the game's precision), integers without decimals.
 *
 * The data is written as given: callers check it first.
 */
#ifndef AERON_EFFECT_FILES_H
#define AERON_EFFECT_FILES_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;

/* The canonical text of `file`, an effect library file (version, library
 * and images), starting with `header` (comment lines, each ending in '\n',
 * or NULL), as a malloc'd string; NULL when out of memory or `file` is not
 * one. */
char* EffectFiles_Format(const struct cJSON* file, const char* header);

/* Writes `file` canonically to `path`. With `header` NULL, the leading
 * comment lines of the file `path` holds are kept. Messages go to stderr,
 * prefixed with `tool`. */
bool EffectFiles_Write(const char* tool, const struct cJSON* file, const char* header, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* AERON_EFFECT_FILES_H */
