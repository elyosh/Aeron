/*
 * The canonical written form of the model animation file and the effect
 * library file (animated_materials.md, "Canonical form"), so that the tools
 * write the same values the same way:
 *
 *   - the file's leading comment lines, then its members, one material or
 *     image per line in YAML flow style; strings unquoted when that reads
 *     back the same (letters, digits, '_', '.', '/' and '-', starting with a
 *     letter or '_', not a boolean or null word), else double-quoted;
 *   - materials and images in key order, the members of an entry in the
 *     format's order;
 *   - each number as the shortest decimal that reads back as the same
 *     32-bit float (the game's precision), integers without decimals;
 *   - fields at their default left out (emissiveStrength 1, unlit 0);
 *   - the energy effects in the order shields, tractor, jamming.
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

/* The canonical text of `file` (a model animation file: version, the
 * energy default and materials, or an effect library file: version, library
 * and images),
 * starting with `header` (comment lines, each ending in '\n', or NULL), as
 * a malloc'd string; NULL when out of memory or `file` is neither. */
char* EffectFiles_Format(const struct cJSON* file, const char* header);

/* Writes `file` canonically to `path`. With `header` NULL, the leading
 * comment lines of the file `path` holds are kept. Messages go to stderr,
 * prefixed with `tool`. */
bool EffectFiles_Write(const char* tool, const struct cJSON* file, const char* header, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* AERON_EFFECT_FILES_H */
