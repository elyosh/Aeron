/*
 * A YAML file as a cJSON tree, for the tools that read animation files
 * (README.md, "Animation file"). Mappings become
 * objects (keys as strings), sequences arrays. A plain scalar is null
 * (`null`, `~`, empty), a boolean (`true`, `false`), a number when it reads
 * as one, else a string; a quoted scalar is always a string. Anchors and
 * aliases are not supported.
 */
#ifndef AERON_YAML_JSON_H
#define AERON_YAML_JSON_H

#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The first document of `path`; NULL with a message in `err` on failure. */
cJSON* YamlJson_Load(const char* path, char* err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* AERON_YAML_JSON_H */
