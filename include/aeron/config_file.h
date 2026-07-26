#ifndef AERON_CONFIG_FILE_H
#define AERON_CONFIG_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "aeron/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque parsed configuration document. */
typedef struct AeronConfigFile AeronConfigFile;
/* Opaque node inside a parsed configuration document. */
typedef struct AeronConfigNode AeronConfigNode;

/* Runtime type assigned to a converted YAML node. */
typedef enum AeronConfigNodeType {
	AERON_CONFIG_NULL,
	AERON_CONFIG_BOOL,
	AERON_CONFIG_INT,
	AERON_CONFIG_FLOAT,
	AERON_CONFIG_STRING,
	AERON_CONFIG_MAP,
	AERON_CONFIG_SEQUENCE
} AeronConfigNodeType;

/* Loads a single-document YAML file from the VFS and converts it to Aeron nodes. */
int AeronConfigFile_LoadYaml(AeronVfs* vfs, AeronVfsRoot root, const char* path,
							 AeronConfigFile** out_config);
/* Destroys a parsed configuration document and all child nodes. */
void AeronConfigFile_Destroy(AeronConfigFile* config);

/* Returns the root node of a parsed document, or NULL for a NULL config. */
const AeronConfigNode* AeronConfigFile_Root(const AeronConfigFile* config);
/* Resolves a dotted/indexed path such as "video.modes[0]" from the root node. */
const AeronConfigNode* AeronConfigFile_GetNode(const AeronConfigFile* config, const char* path);
/* Returns nonzero if a dotted/indexed path resolves to a node. */
int AeronConfigFile_Has(const AeronConfigFile* config, const char* path);

/* Returns a string node value at path, or fallback when missing or not a string. */
const char* AeronConfigFile_GetString(const AeronConfigFile* config, const char* path, const char* fallback);
/* Returns a bool node value at path, or fallback when missing or not a bool. */
int AeronConfigFile_GetBool(const AeronConfigFile* config, const char* path, int fallback);
/* Returns an int node value at path, or fallback when missing or not an int. */
int64_t AeronConfigFile_GetInt(const AeronConfigFile* config, const char* path, int64_t fallback);
/* Returns a float or int node value at path as double, or fallback otherwise. */
double AeronConfigFile_GetFloat(const AeronConfigFile* config, const char* path, double fallback);

/* Returns a node's runtime type; NULL nodes report AERON_CONFIG_NULL. */
AeronConfigNodeType AeronConfigNode_Type(const AeronConfigNode* node);
/* Returns the one-based YAML line number for a node, or zero when unavailable. */
int AeronConfigNode_Line(const AeronConfigNode* node);
/* Returns the one-based YAML column number for a node, or zero when unavailable. */
int AeronConfigNode_Column(const AeronConfigNode* node);

/* Returns a string node value, or fallback when node is NULL or not a string. */
const char* AeronConfigNode_String(const AeronConfigNode* node, const char* fallback);
/* Returns a bool node value, or fallback when node is NULL or not a bool. */
int AeronConfigNode_Bool(const AeronConfigNode* node, int fallback);
/* Returns an int node value, or fallback when node is NULL or not an int. */
int64_t AeronConfigNode_Int(const AeronConfigNode* node, int64_t fallback);
/* Returns a float or int node value as double, or fallback otherwise. */
double AeronConfigNode_Float(const AeronConfigNode* node, double fallback);

/* Returns the number of entries in a map node, or zero for non-map nodes. */
size_t AeronConfigNode_MapCount(const AeronConfigNode* node);
/* Returns the key at index in a map node, or NULL when out of range or not a map. */
const char* AeronConfigNode_MapKeyAt(const AeronConfigNode* node, size_t index);
/* Returns the value at index in a map node, or NULL when out of range or not a map. */
const AeronConfigNode* AeronConfigNode_MapValueAt(const AeronConfigNode* node, size_t index);
/* Returns the map value for key, or NULL when missing or not a map. */
const AeronConfigNode* AeronConfigNode_MapGet(const AeronConfigNode* node, const char* key);

/* Returns the number of items in a sequence node, or zero for non-sequence nodes. */
size_t AeronConfigNode_SequenceCount(const AeronConfigNode* node);
/* Returns the item at index in a sequence node, or NULL when out of range or not a sequence. */
const AeronConfigNode* AeronConfigNode_SequenceGet(const AeronConfigNode* node, size_t index);

#ifdef __cplusplus
}
#endif

#endif
