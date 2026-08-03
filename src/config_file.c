#include "internal.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yaml.h>

#define AERON_CONFIG_MAX_DEPTH 128

typedef struct AeronConfigMapEntry {
	char*            key;
	AeronConfigNode* value;
} AeronConfigMapEntry;

struct AeronConfigNode {
	AeronConfigNodeType type;
	AeronVfsRoot        source_root;
	char*               source_path;
	int                 line;
	int                 column;
	union {
		int     bool_value;
		int64_t int_value;
		double  float_value;
		char*   string_value;
		struct {
			AeronConfigMapEntry* entries;
			size_t               count;
		} map;
		struct {
			AeronConfigNode** items;
			size_t            count;
		} sequence;
	} value;
};

struct AeronConfigFile {
	AeronConfigNode* root;
	AeronVfsRoot     root_kind;
	char*            path;
};

typedef struct AeronConfigBuffer {
	char*  data;
	size_t size;
	size_t capacity;
} AeronConfigBuffer;

static char* AeronConfig_DuplicateBytes(const unsigned char* value, size_t length);

static void AeronConfig_ClearError(AeronConfigError* error, AeronVfsRoot root, const char* path) {
	if (!error) return;
	memset(error, 0, sizeof(*error));
	error->root = root;
	if (path) snprintf(error->path, sizeof(error->path), "%s", path);
}

static int AeronConfig_Fail(AeronConfigError* error, AeronConfigErrorCode code, AeronVfsRoot root,
							const char* path, int line, int column, const char* format, ...) {
	if (error) {
		va_list args;
		AeronConfig_ClearError(error, root, path);
		error->code = code;
		error->line = line;
		error->column = column;
		va_start(args, format);
		vsnprintf(error->message, sizeof(error->message), format, args);
		va_end(args);
	}
	return 0;
}

static char* AeronConfig_DuplicateString(const char* value) {
	return value ? AeronConfig_DuplicateBytes((const unsigned char*)value, strlen(value)) : NULL;
}

static char AeronConfig_LowerAscii(char ch) {
	if (ch >= 'A' && ch <= 'Z') {
		return (char)(ch - 'A' + 'a');
	}

	return ch;
}

static int AeronConfig_EqualsIgnoreCase(const char* value, size_t length, const char* expected) {
	size_t i;

	for (i = 0; i < length && expected[i]; ++i) {
		if (AeronConfig_LowerAscii(value[i]) != AeronConfig_LowerAscii(expected[i])) {
			return 0;
		}
	}

	return i == length && expected[i] == '\0';
}

static char* AeronConfig_DuplicateBytes(const unsigned char* value, size_t length) {
	char* copy;

	copy = (char*)SDL_malloc(length + 1);
	if (!copy) {
		return NULL;
	}

	if (length) {
		memcpy(copy, value, length);
	}
	copy[length] = '\0';
	return copy;
}

static AeronConfigNode* AeronConfig_CreateNode(AeronConfigNodeType type, const yaml_node_t* yaml_node) {
	AeronConfigNode* node;

	node = (AeronConfigNode*)SDL_calloc(1, sizeof(*node));
	if (!node) {
		return NULL;
	}

	node->type = type;
	if (yaml_node) {
		node->line   = (int)yaml_node->start_mark.line + 1;
		node->column = (int)yaml_node->start_mark.column + 1;
	}

	return node;
}

static int AeronConfig_ParseInt(const char* value, size_t length, int64_t* out_value) {
	char*     copy;
	char*     end;
	long long parsed;

	copy = AeronConfig_DuplicateBytes((const unsigned char*)value, length);
	if (!copy) {
		return 0;
	}

	errno  = 0;
	parsed = strtoll(copy, &end, 0);
	if (errno != 0 || end == copy || *end != '\0') {
		SDL_free(copy);
		return 0;
	}

	SDL_free(copy);
	*out_value = (int64_t)parsed;
	return 1;
}

static int AeronConfig_ParseFloat(const char* value, size_t length, double* out_value) {
	return Aeron_ParseAsciiDouble(value, length, out_value);
}

static int AeronConfig_TextLooksFloat(const char* value, size_t length) {
	size_t i;

	for (i = 0; i < length; ++i) {
		if (value[i] == '.' || value[i] == 'e' || value[i] == 'E') {
			return 1;
		}
	}

	return 0;
}

static AeronConfigNode* AeronConfig_ConvertScalar(const yaml_node_t* yaml_node) {
	const char*      value;
	size_t           length;
	AeronConfigNode* node;

	value  = (const char*)yaml_node->data.scalar.value;
	length = yaml_node->data.scalar.length;

	if (yaml_node->data.scalar.style == YAML_PLAIN_SCALAR_STYLE) {
		int64_t int_value;
		double  float_value;

		if (length == 0 || AeronConfig_EqualsIgnoreCase(value, length, "null") ||
			AeronConfig_EqualsIgnoreCase(value, length, "~")) {
			return AeronConfig_CreateNode(AERON_CONFIG_NULL, yaml_node);
		}

		if (AeronConfig_EqualsIgnoreCase(value, length, "true") ||
			AeronConfig_EqualsIgnoreCase(value, length, "yes") ||
			AeronConfig_EqualsIgnoreCase(value, length, "on")) {
			node = AeronConfig_CreateNode(AERON_CONFIG_BOOL, yaml_node);
			if (node) {
				node->value.bool_value = 1;
			}
			return node;
		}

		if (AeronConfig_EqualsIgnoreCase(value, length, "false") ||
			AeronConfig_EqualsIgnoreCase(value, length, "no") ||
			AeronConfig_EqualsIgnoreCase(value, length, "off")) {
			node = AeronConfig_CreateNode(AERON_CONFIG_BOOL, yaml_node);
			if (node) {
				node->value.bool_value = 0;
			}
			return node;
		}

		if (AeronConfig_ParseInt(value, length, &int_value)) {
			node = AeronConfig_CreateNode(AERON_CONFIG_INT, yaml_node);
			if (node) {
				node->value.int_value = int_value;
			}
			return node;
		}

		if (AeronConfig_TextLooksFloat(value, length) &&
			AeronConfig_ParseFloat(value, length, &float_value)) {
			node = AeronConfig_CreateNode(AERON_CONFIG_FLOAT, yaml_node);
			if (node) {
				node->value.float_value = float_value;
			}
			return node;
		}
	}

	node = AeronConfig_CreateNode(AERON_CONFIG_STRING, yaml_node);
	if (!node) {
		return NULL;
	}

	node->value.string_value = AeronConfig_DuplicateBytes(yaml_node->data.scalar.value, length);
	if (!node->value.string_value) {
		SDL_free(node);
		return NULL;
	}

	return node;
}

static int AeronConfig_YamlNodeId(const yaml_document_t* document, const yaml_node_t* node) {
	return (int)(node - document->nodes.start) + 1;
}

static void AeronConfigNode_Destroy(AeronConfigNode* node) {
	size_t i;

	if (!node) {
		return;
	}

	switch (node->type) {
		case AERON_CONFIG_STRING:
			SDL_free(node->value.string_value);
			break;
		case AERON_CONFIG_MAP:
			for (i = 0; i < node->value.map.count; ++i) {
				SDL_free(node->value.map.entries[i].key);
				AeronConfigNode_Destroy(node->value.map.entries[i].value);
			}
			SDL_free(node->value.map.entries);
			break;
		case AERON_CONFIG_SEQUENCE:
			for (i = 0; i < node->value.sequence.count; ++i) {
				AeronConfigNode_Destroy(node->value.sequence.items[i]);
			}
			SDL_free(node->value.sequence.items);
			break;
		default:
			break;
	}

	SDL_free(node->source_path);
	SDL_free(node);
}

static int AeronConfigNode_SetSource(AeronConfigNode* node, AeronVfsRoot root, const char* path) {
	size_t index;
	if (!node) return 0;
	node->source_root = root;
	node->source_path = AeronConfig_DuplicateString(path ? path : "");
	if (!node->source_path) return 0;
	if (node->type == AERON_CONFIG_MAP) {
		for (index = 0; index < node->value.map.count; ++index)
			if (!AeronConfigNode_SetSource(node->value.map.entries[index].value, root, path)) return 0;
	} else if (node->type == AERON_CONFIG_SEQUENCE) {
		for (index = 0; index < node->value.sequence.count; ++index)
			if (!AeronConfigNode_SetSource(node->value.sequence.items[index], root, path)) return 0;
	}
	return 1;
}

static AeronConfigNode* AeronConfig_ConvertYamlNode(yaml_document_t* document, int node_id, int depth);

static AeronConfigNode* AeronConfig_ConvertSequence(yaml_document_t* document, const yaml_node_t* yaml_node,
													int depth) {
	size_t            count;
	size_t            i;
	AeronConfigNode*  node;
	yaml_node_item_t* item;

	count = (size_t)(yaml_node->data.sequence.items.top - yaml_node->data.sequence.items.start);
	node  = AeronConfig_CreateNode(AERON_CONFIG_SEQUENCE, yaml_node);
	if (!node) {
		return NULL;
	}

	node->value.sequence.count = count;
	if (count == 0) {
		return node;
	}

	node->value.sequence.items = (AeronConfigNode**)SDL_calloc(count, sizeof(*node->value.sequence.items));
	if (!node->value.sequence.items) {
		AeronConfigNode_Destroy(node);
		return NULL;
	}

	item = yaml_node->data.sequence.items.start;
	for (i = 0; i < count; ++i, ++item) {
		node->value.sequence.items[i] = AeronConfig_ConvertYamlNode(document, *item, depth + 1);
		if (!node->value.sequence.items[i]) {
			AeronConfigNode_Destroy(node);
			return NULL;
		}
	}

	return node;
}

static AeronConfigNode* AeronConfig_ConvertMapping(yaml_document_t* document, const yaml_node_t* yaml_node,
												   int depth) {
	size_t            count;
	size_t            i;
	AeronConfigNode*  node;
	yaml_node_pair_t* pair;

	count = (size_t)(yaml_node->data.mapping.pairs.top - yaml_node->data.mapping.pairs.start);
	node  = AeronConfig_CreateNode(AERON_CONFIG_MAP, yaml_node);
	if (!node) {
		return NULL;
	}

	node->value.map.count = count;
	if (count == 0) {
		return node;
	}

	node->value.map.entries = (AeronConfigMapEntry*)SDL_calloc(count, sizeof(*node->value.map.entries));
	if (!node->value.map.entries) {
		AeronConfigNode_Destroy(node);
		return NULL;
	}

	pair = yaml_node->data.mapping.pairs.start;
	for (i = 0; i < count; ++i, ++pair) {
		yaml_node_t* key_node;

		key_node = yaml_document_get_node(document, pair->key);
		if (!key_node || key_node->type != YAML_SCALAR_NODE) {
			Aeron_LogError("aeron.config", "YAML config map keys must be scalar values");
			AeronConfigNode_Destroy(node);
			return NULL;
		}

		node->value.map.entries[i].key =
			AeronConfig_DuplicateBytes(key_node->data.scalar.value, key_node->data.scalar.length);
		if (!node->value.map.entries[i].key) {
			AeronConfigNode_Destroy(node);
			return NULL;
		}

		node->value.map.entries[i].value = AeronConfig_ConvertYamlNode(document, pair->value, depth + 1);
		if (!node->value.map.entries[i].value) {
			AeronConfigNode_Destroy(node);
			return NULL;
		}
	}

	return node;
}

static AeronConfigNode* AeronConfig_ConvertYamlNode(yaml_document_t* document, int node_id, int depth) {
	yaml_node_t* yaml_node;

	if (depth > AERON_CONFIG_MAX_DEPTH) {
		Aeron_LogError("aeron.config", "YAML config nesting exceeds %d levels", AERON_CONFIG_MAX_DEPTH);
		return NULL;
	}

	yaml_node = yaml_document_get_node(document, node_id);
	if (!yaml_node) {
		return NULL;
	}

	switch (yaml_node->type) {
		case YAML_SCALAR_NODE:
			return AeronConfig_ConvertScalar(yaml_node);
		case YAML_SEQUENCE_NODE:
			return AeronConfig_ConvertSequence(document, yaml_node, depth);
		case YAML_MAPPING_NODE:
			return AeronConfig_ConvertMapping(document, yaml_node, depth);
		default:
			return AeronConfig_CreateNode(AERON_CONFIG_NULL, yaml_node);
	}
}

static int AeronConfig_ReadFile(AeronVfs* vfs, AeronVfsRoot root, const char* path, unsigned char** out_data,
								size_t* out_size) {
	AeronFile* file;
	int64_t    file_size;
	size_t     bytes_read;

	*out_data = NULL;
	*out_size = 0;

	if (!AeronVfs_Open(vfs, root, path, AERON_VFS_READ, &file)) {
		return 0;
	}

	file_size = AeronVfs_GetSize(file);
	if (file_size < 0 || (uint64_t)file_size > (uint64_t)SIZE_MAX - 1u) {
		AeronVfs_Close(file);
		return 0;
	}

	*out_data = (unsigned char*)SDL_malloc((size_t)file_size + 1u);
	if (!*out_data) {
		AeronVfs_Close(file);
		return 0;
	}

	if (!AeronVfs_Read(file, *out_data, (size_t)file_size, &bytes_read) || bytes_read != (size_t)file_size) {
		SDL_free(*out_data);
		*out_data = NULL;
		AeronVfs_Close(file);
		return 0;
	}

	(*out_data)[bytes_read] = '\0';
	*out_size               = bytes_read;
	AeronVfs_Close(file);
	return 1;
}

int AeronConfigFile_LoadYamlEx(AeronVfs* vfs, AeronVfsRoot root, const char* path,
							   AeronConfigFile** out_config, AeronConfigError* error) {
	unsigned char*   data;
	size_t           data_size;
	yaml_parser_t    parser;
	yaml_document_t  document;
	yaml_document_t  extra_document;
	yaml_node_t*     root_node;
	AeronConfigFile* config;
	int              root_id;
	int              result;

	AeronConfig_ClearError(error, root, path);
	if (!vfs || !path || !path[0] || !out_config)
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_INVALID_ARGUMENT, root, path, 0, 0,
								"invalid configuration load arguments");
	*out_config = NULL;
	{
		AeronFileInfo info;
		if (!AeronVfs_Stat(vfs, root, path, &info) || !info.exists)
			return AeronConfig_Fail(error, AERON_CONFIG_ERROR_NOT_FOUND, root, path, 0, 0,
									"configuration file not found");
		if (info.is_directory)
			return AeronConfig_Fail(error, AERON_CONFIG_ERROR_IO, root, path, 0, 0,
									"configuration path is a directory");
	}
	if (!AeronConfig_ReadFile(vfs, root, path, &data, &data_size)) {
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_IO, root, path, 0, 0,
								"could not read configuration file");
	}

	memset(&document, 0, sizeof(document));
	memset(&extra_document, 0, sizeof(extra_document));
	if (!yaml_parser_initialize(&parser)) {
		SDL_free(data);
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, root, path, 0, 0,
								"could not initialize YAML parser");
	}

	result = 0;
	yaml_parser_set_input_string(&parser, data, data_size);
	if (!yaml_parser_load(&parser, &document)) {
		AeronConfig_Fail(error, AERON_CONFIG_ERROR_YAML_SYNTAX, root, path,
						 (int)parser.problem_mark.line + 1, (int)parser.problem_mark.column + 1, "%s",
						 parser.problem ? parser.problem : "YAML syntax error");
		goto done;
	}

	config = (AeronConfigFile*)SDL_calloc(1, sizeof(*config));
	if (!config) {
		AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, root, path, 0, 0,
						 "could not allocate configuration document");
		goto done;
	}
	config->root_kind = root;
	config->path = AeronConfig_DuplicateString(path);
	if (!config->path) {
		AeronConfigFile_Destroy(config);
		config = NULL;
		AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, root, path, 0, 0,
						 "could not allocate configuration path");
		goto done;
	}

	root_node = yaml_document_get_root_node(&document);
	if (root_node) {
		root_id      = AeronConfig_YamlNodeId(&document, root_node);
		config->root = AeronConfig_ConvertYamlNode(&document, root_id, 0);
	} else {
		config->root = AeronConfig_CreateNode(AERON_CONFIG_NULL, NULL);
	}

	if (!config->root || !AeronConfigNode_SetSource(config->root, root, path)) {
		AeronConfigFile_Destroy(config);
		config = NULL;
		AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, root, path, 0, 0,
						 "could not convert YAML document");
		goto done;
	}

	if (!yaml_parser_load(&parser, &extra_document)) {
		AeronConfigFile_Destroy(config);
		config = NULL;
		AeronConfig_Fail(error, AERON_CONFIG_ERROR_YAML_SYNTAX, root, path,
						 (int)parser.problem_mark.line + 1, (int)parser.problem_mark.column + 1, "%s",
						 parser.problem ? parser.problem : "YAML syntax error");
		goto done;
	}

	if (yaml_document_get_root_node(&extra_document)) {
		AeronConfigFile_Destroy(config);
		config = NULL;
		AeronConfig_Fail(error, AERON_CONFIG_ERROR_MULTIPLE_DOCUMENTS, root, path, 0, 0,
						 "configuration must contain one YAML document");
		goto done;
	}

	*out_config = config;
	result      = 1;

done:
	yaml_document_delete(&extra_document);
	yaml_document_delete(&document);
	yaml_parser_delete(&parser);
	SDL_free(data);
	return result;
}

int AeronConfigFile_LoadYaml(AeronVfs* vfs, AeronVfsRoot root, const char* path,
							 AeronConfigFile** out_config) {
	AeronConfigError error;
	int result = AeronConfigFile_LoadYamlEx(vfs, root, path, out_config, &error);
	if (!result && error.code != AERON_CONFIG_ERROR_NOT_FOUND)
		Aeron_LogError("aeron.config", "%s:%d:%d: %s", error.path, error.line, error.column, error.message);
	return result;
}

void AeronConfigFile_Destroy(AeronConfigFile* config) {
	if (!config) {
		return;
	}

	AeronConfigNode_Destroy(config->root);
	SDL_free(config->path);
	SDL_free(config);
}

static AeronConfigFile* AeronConfigFile_Allocate(AeronVfsRoot root, const char* path) {
	AeronConfigFile* config = (AeronConfigFile*)SDL_calloc(1, sizeof(*config));
	if (!config) return NULL;
	config->root_kind = root;
	config->path = AeronConfig_DuplicateString(path ? path : "");
	if (!config->path) {
		SDL_free(config);
		return NULL;
	}
	return config;
}

int AeronConfigFile_CreateMap(AeronVfsRoot root, const char* path, AeronConfigFile** out_config,
							  AeronConfigError* error) {
	AeronConfigFile* config;
	AeronConfig_ClearError(error, root, path);
	if (!path || !out_config)
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_INVALID_ARGUMENT, root, path, 0, 0,
								"invalid configuration creation arguments");
	*out_config = NULL;
	config = AeronConfigFile_Allocate(root, path);
	if (config) {
		config->root = AeronConfig_CreateNode(AERON_CONFIG_MAP, NULL);
		if (config->root && !AeronConfigNode_SetSource(config->root, root, path)) {
			AeronConfigNode_Destroy(config->root);
			config->root = NULL;
		}
	}
	if (!config || !config->root) {
		AeronConfigFile_Destroy(config);
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, root, path, 0, 0,
								"could not allocate configuration document");
	}
	*out_config = config;
	return 1;
}

static AeronConfigNode* AeronConfigNode_Clone(const AeronConfigNode* source) {
	AeronConfigNode* clone;
	size_t index;
	if (!source) return NULL;
	clone = AeronConfig_CreateNode(source->type, NULL);
	if (!clone) return NULL;
	clone->source_root = source->source_root;
	clone->source_path = AeronConfig_DuplicateString(source->source_path ? source->source_path : "");
	clone->line = source->line;
	clone->column = source->column;
	if (!clone->source_path) goto failed;
	if (source->type == AERON_CONFIG_STRING) {
		clone->value.string_value = AeronConfig_DuplicateString(source->value.string_value);
		if (!clone->value.string_value) goto failed;
	} else if (source->type == AERON_CONFIG_MAP) {
		clone->value.map.count = source->value.map.count;
		if (clone->value.map.count) {
			clone->value.map.entries =
				(AeronConfigMapEntry*)SDL_calloc(clone->value.map.count, sizeof(*clone->value.map.entries));
			if (!clone->value.map.entries) goto failed;
		}
		for (index = 0; index < clone->value.map.count; ++index) {
			clone->value.map.entries[index].key = AeronConfig_DuplicateString(source->value.map.entries[index].key);
			clone->value.map.entries[index].value = AeronConfigNode_Clone(source->value.map.entries[index].value);
			if (!clone->value.map.entries[index].key || !clone->value.map.entries[index].value) goto failed;
		}
	} else if (source->type == AERON_CONFIG_SEQUENCE) {
		clone->value.sequence.count = source->value.sequence.count;
		if (clone->value.sequence.count) {
			clone->value.sequence.items =
				(AeronConfigNode**)SDL_calloc(clone->value.sequence.count, sizeof(*clone->value.sequence.items));
			if (!clone->value.sequence.items) goto failed;
		}
		for (index = 0; index < clone->value.sequence.count; ++index) {
			clone->value.sequence.items[index] = AeronConfigNode_Clone(source->value.sequence.items[index]);
			if (!clone->value.sequence.items[index]) goto failed;
		}
	} else {
		clone->value = source->value;
	}
	return clone;
failed:
	AeronConfigNode_Destroy(clone);
	return NULL;
}

int AeronConfigFile_Clone(const AeronConfigFile* source, AeronConfigFile** out_config,
						  AeronConfigError* error) {
	AeronConfigFile* clone;
	if (!source || !out_config)
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_INVALID_ARGUMENT, AERON_VFS_ROOT_RESOURCE, NULL,
								0, 0, "invalid configuration clone arguments");
	AeronConfig_ClearError(error, source->root_kind, source->path);
	*out_config = NULL;
	clone = AeronConfigFile_Allocate(source->root_kind, source->path);
	if (clone) clone->root = AeronConfigNode_Clone(source->root);
	if (!clone || !clone->root) {
		AeronConfigFile_Destroy(clone);
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, source->root_kind, source->path, 0,
								0, "could not clone configuration document");
	}
	*out_config = clone;
	return 1;
}

static size_t AeronConfigNode_MapFind(const AeronConfigNode* map, const char* key, size_t length) {
	size_t index;
	if (!map || map->type != AERON_CONFIG_MAP) return SIZE_MAX;
	for (index = 0; index < map->value.map.count; ++index) {
		const char* current = map->value.map.entries[index].key;
		if (strlen(current) == length && !memcmp(current, key, length)) return index;
	}
	return SIZE_MAX;
}

static int AeronConfigNode_MapAppend(AeronConfigNode* map, const char* key, size_t length,
									AeronConfigNode* value) {
	AeronConfigMapEntry* entries;
	char* copy = AeronConfig_DuplicateBytes((const unsigned char*)key, length);
	if (!copy) return 0;
	entries = (AeronConfigMapEntry*)SDL_realloc(map->value.map.entries,
											   (map->value.map.count + 1u) * sizeof(*entries));
	if (!entries) {
		SDL_free(copy);
		return 0;
	}
	map->value.map.entries = entries;
	entries[map->value.map.count].key = copy;
	entries[map->value.map.count].value = value;
	map->value.map.count++;
	return 1;
}

static AeronConfigNode* AeronConfigNode_Overlay(const AeronConfigNode* base,
											   const AeronConfigNode* overrides) {
	AeronConfigNode* result;
	size_t index;
	if (base->type != AERON_CONFIG_MAP || overrides->type != AERON_CONFIG_MAP)
		return AeronConfigNode_Clone(overrides);
	result = AeronConfigNode_Clone(base);
	if (!result) return NULL;
	for (index = 0; index < overrides->value.map.count; ++index) {
		const AeronConfigMapEntry* entry = &overrides->value.map.entries[index];
		size_t target = AeronConfigNode_MapFind(result, entry->key, strlen(entry->key));
		if (target != SIZE_MAX) {
			AeronConfigNode* merged = AeronConfigNode_Overlay(result->value.map.entries[target].value,
														entry->value);
			if (!merged) goto failed;
			AeronConfigNode_Destroy(result->value.map.entries[target].value);
			result->value.map.entries[target].value = merged;
		} else {
			AeronConfigNode* added = AeronConfigNode_Clone(entry->value);
			if (!added || !AeronConfigNode_MapAppend(result, entry->key, strlen(entry->key), added)) {
				AeronConfigNode_Destroy(added);
				goto failed;
			}
		}
	}
	return result;
failed:
	AeronConfigNode_Destroy(result);
	return NULL;
}

int AeronConfigFile_Overlay(const AeronConfigFile* base, const AeronConfigFile* overrides,
							AeronConfigFile** out_config, AeronConfigError* error) {
	AeronConfigFile* result;
	if (!base || !overrides || !out_config)
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_INVALID_ARGUMENT, AERON_VFS_ROOT_RESOURCE, NULL,
								0, 0, "invalid configuration overlay arguments");
	AeronConfig_ClearError(error, base->root_kind, base->path);
	*out_config = NULL;
	result = AeronConfigFile_Allocate(base->root_kind, base->path);
	if (result) result->root = AeronConfigNode_Overlay(base->root, overrides->root);
	if (!result || !result->root) {
		AeronConfigFile_Destroy(result);
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, base->root_kind, base->path, 0, 0,
								"could not overlay configuration documents");
	}
	*out_config = result;
	return 1;
}

static AeronConfigNode* AeronConfigNode_FromValue(const AeronConfigValue* value, AeronVfsRoot root,
												 const char* path, int depth) {
	AeronConfigNode* node;
	size_t index;
	if (!value || depth > AERON_CONFIG_MAX_DEPTH || value->type < AERON_CONFIG_NULL ||
		value->type > AERON_CONFIG_SEQUENCE ||
		(value->type == AERON_CONFIG_FLOAT && !isfinite(value->value.float_value)) ||
		(value->type == AERON_CONFIG_STRING && !value->value.string_value)) return NULL;
	node = AeronConfig_CreateNode(value->type, NULL);
	if (!node || !AeronConfigNode_SetSource(node, root, path)) goto failed;
	if (value->type == AERON_CONFIG_BOOL) node->value.bool_value = value->value.bool_value != 0;
	else if (value->type == AERON_CONFIG_INT) node->value.int_value = value->value.int_value;
	else if (value->type == AERON_CONFIG_FLOAT) node->value.float_value = value->value.float_value;
	else if (value->type == AERON_CONFIG_STRING) {
		node->value.string_value = AeronConfig_DuplicateString(value->value.string_value);
		if (!node->value.string_value) goto failed;
	} else if (value->type == AERON_CONFIG_MAP) {
		if (value->value.map.count && !value->value.map.entries) goto failed;
		for (index = 0; index < value->value.map.count; ++index) {
			const AeronConfigMapValue* entry = &value->value.map.entries[index];
			AeronConfigNode* child = AeronConfigNode_FromValue(entry->value, root, path, depth + 1);
			if (!entry->key || !child || !AeronConfigNode_MapAppend(node, entry->key, strlen(entry->key), child)) {
				AeronConfigNode_Destroy(child);
				goto failed;
			}
		}
	} else if (value->type == AERON_CONFIG_SEQUENCE) {
		node->value.sequence.count = value->value.sequence.count;
		if (node->value.sequence.count) {
			if (!value->value.sequence.values) goto failed;
			node->value.sequence.items =
				(AeronConfigNode**)SDL_calloc(node->value.sequence.count, sizeof(*node->value.sequence.items));
			if (!node->value.sequence.items) goto failed;
		}
		for (index = 0; index < node->value.sequence.count; ++index) {
			node->value.sequence.items[index] =
				AeronConfigNode_FromValue(&value->value.sequence.values[index], root, path, depth + 1);
			if (!node->value.sequence.items[index]) goto failed;
		}
	}
	return node;
failed:
	AeronConfigNode_Destroy(node);
	return NULL;
}

static int AeronConfig_ValidMutationPath(const char* path) {
	const char* cursor;
	if (!path || !path[0] || path[0] == '.' || strchr(path, '[')) return 0;
	for (cursor = path; *cursor; ++cursor)
		if (*cursor == '.' && (!cursor[1] || cursor[1] == '.')) return 0;
	return 1;
}

static int AeronConfigFile_SetCloned(AeronConfigFile* config, const char* path,
									const AeronConfigValue* value, AeronConfigError* error) {
	AeronConfigNode* map = config->root;
	const char* segment = path;
	if (map->type != AERON_CONFIG_MAP)
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_PATH_TYPE_CONFLICT, config->root_kind,
								config->path, map->line, map->column, "configuration root is not a map");
	for (;;) {
		const char* dot = strchr(segment, '.');
		size_t length = dot ? (size_t)(dot - segment) : strlen(segment);
		size_t index = AeronConfigNode_MapFind(map, segment, length);
		if (!dot) {
			AeronConfigNode* replacement = AeronConfigNode_FromValue(value, config->root_kind, config->path, 0);
			if (!replacement)
				return AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, config->root_kind,
									config->path, 0, 0, "could not allocate configuration value");
			if (index == SIZE_MAX) {
				if (!AeronConfigNode_MapAppend(map, segment, length, replacement)) {
					AeronConfigNode_Destroy(replacement);
					return AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, config->root_kind,
										config->path, 0, 0, "could not extend configuration map");
				}
			} else {
				AeronConfigNode_Destroy(map->value.map.entries[index].value);
				map->value.map.entries[index].value = replacement;
			}
			return 1;
		}
		if (index == SIZE_MAX) {
			AeronConfigNode* child = AeronConfig_CreateNode(AERON_CONFIG_MAP, NULL);
			if (!child || !AeronConfigNode_SetSource(child, config->root_kind, config->path) ||
				!AeronConfigNode_MapAppend(map, segment, length, child)) {
				AeronConfigNode_Destroy(child);
				return AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, config->root_kind,
									config->path, 0, 0, "could not create configuration path");
			}
			map = child;
		} else {
			map = map->value.map.entries[index].value;
			if (map->type != AERON_CONFIG_MAP)
				return AeronConfig_Fail(error, AERON_CONFIG_ERROR_PATH_TYPE_CONFLICT, config->root_kind,
									config->path, map->line, map->column,
									"configuration path crosses a non-map value");
		}
		segment = dot + 1;
	}
}

int AeronConfigFile_SetValue(AeronConfigFile* config, const char* path, const AeronConfigValue* value,
							 AeronConfigError* error) {
	AeronConfigFile* clone = NULL;
	if (!config || !value || !AeronConfig_ValidMutationPath(path))
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_INVALID_ARGUMENT,
								config ? config->root_kind : AERON_VFS_ROOT_RESOURCE,
								config ? config->path : NULL, 0, 0, "invalid configuration mutation arguments");
	if (!AeronConfigFile_Clone(config, &clone, error) || !AeronConfigFile_SetCloned(clone, path, value, error)) {
		AeronConfigFile_Destroy(clone);
		return 0;
	}
	AeronConfigNode_Destroy(config->root);
	config->root = clone->root;
	clone->root = NULL;
	AeronConfigFile_Destroy(clone);
	return 1;
}

int AeronConfigFile_SetNull(AeronConfigFile* c, const char* p, AeronConfigError* e) { AeronConfigValue v = { .type = AERON_CONFIG_NULL }; return AeronConfigFile_SetValue(c, p, &v, e); }
int AeronConfigFile_SetBool(AeronConfigFile* c, const char* p, int x, AeronConfigError* e) { AeronConfigValue v = { .type = AERON_CONFIG_BOOL }; v.value.bool_value = x; return AeronConfigFile_SetValue(c, p, &v, e); }
int AeronConfigFile_SetInt(AeronConfigFile* c, const char* p, int64_t x, AeronConfigError* e) { AeronConfigValue v = { .type = AERON_CONFIG_INT }; v.value.int_value = x; return AeronConfigFile_SetValue(c, p, &v, e); }
int AeronConfigFile_SetFloat(AeronConfigFile* c, const char* p, double x, AeronConfigError* e) {
	AeronConfigValue v = { .type = AERON_CONFIG_FLOAT };
	if (!isfinite(x))
		return AeronConfig_Fail(e, AERON_CONFIG_ERROR_INVALID_ARGUMENT,
								c ? c->root_kind : AERON_VFS_ROOT_RESOURCE,
								c ? c->path : NULL, 0, 0, "configuration float must be finite");
	v.value.float_value = x;
	return AeronConfigFile_SetValue(c, p, &v, e);
}
int AeronConfigFile_SetString(AeronConfigFile* c, const char* p, const char* x, AeronConfigError* e) { AeronConfigValue v = { .type = AERON_CONFIG_STRING }; v.value.string_value = x; return AeronConfigFile_SetValue(c, p, &v, e); }

static int AeronConfigNode_Remove(AeronConfigNode* map, const char* path, int* empty, AeronConfigError* error) {
	const char* dot = strchr(path, '.');
	size_t length = dot ? (size_t)(dot - path) : strlen(path);
	size_t index = AeronConfigNode_MapFind(map, path, length);
	if (index == SIZE_MAX) { *empty = map->value.map.count == 0; return 1; }
	if (dot) {
		AeronConfigNode* child = map->value.map.entries[index].value;
		int child_empty = 0;
		if (child->type != AERON_CONFIG_MAP)
			return AeronConfig_Fail(error, AERON_CONFIG_ERROR_PATH_TYPE_CONFLICT, child->source_root,
								child->source_path, child->line, child->column,
								"configuration path crosses a non-map value");
		if (!AeronConfigNode_Remove(child, dot + 1, &child_empty, error)) return 0;
		if (!child_empty) { *empty = 0; return 1; }
	}
	SDL_free(map->value.map.entries[index].key);
	AeronConfigNode_Destroy(map->value.map.entries[index].value);
	memmove(map->value.map.entries + index, map->value.map.entries + index + 1,
			(map->value.map.count - index - 1u) * sizeof(*map->value.map.entries));
	map->value.map.count--;
	*empty = map->value.map.count == 0;
	return 1;
}

int AeronConfigFile_Remove(AeronConfigFile* config, const char* path, AeronConfigError* error) {
	AeronConfigFile* clone = NULL;
	int empty;
	if (!config || !AeronConfig_ValidMutationPath(path) || config->root->type != AERON_CONFIG_MAP)
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_INVALID_ARGUMENT,
								config ? config->root_kind : AERON_VFS_ROOT_RESOURCE,
								config ? config->path : NULL, 0, 0, "invalid configuration removal arguments");
	if (!AeronConfigFile_Clone(config, &clone, error) || !AeronConfigNode_Remove(clone->root, path, &empty, error)) {
		AeronConfigFile_Destroy(clone);
		return 0;
	}
	AeronConfigNode_Destroy(config->root);
	config->root = clone->root;
	clone->root = NULL;
	AeronConfigFile_Destroy(clone);
	return 1;
}

static int AeronConfigBuffer_Reserve(AeronConfigBuffer* buffer, size_t extra) {
	size_t required;
	size_t capacity;
	char* grown;
	if (extra > SIZE_MAX - buffer->size - 1u) return 0;
	required = buffer->size + extra + 1u;
	if (required <= buffer->capacity) return 1;
	capacity = buffer->capacity ? buffer->capacity : 256u;
	while (capacity < required) {
		if (capacity > SIZE_MAX / 2u) { capacity = required; break; }
		capacity *= 2u;
	}
	grown = (char*)SDL_realloc(buffer->data, capacity);
	if (!grown) return 0;
	buffer->data = grown;
	buffer->capacity = capacity;
	return 1;
}

static int AeronConfigBuffer_Append(AeronConfigBuffer* buffer, const char* text, size_t length) {
	if (!AeronConfigBuffer_Reserve(buffer, length)) return 0;
	memcpy(buffer->data + buffer->size, text, length);
	buffer->size += length;
	buffer->data[buffer->size] = '\0';
	return 1;
}

static int AeronConfigBuffer_Text(AeronConfigBuffer* buffer, const char* text) {
	return AeronConfigBuffer_Append(buffer, text, strlen(text));
}

static int AeronConfigBuffer_Indent(AeronConfigBuffer* buffer, int indent) {
	while (indent-- > 0)
		if (!AeronConfigBuffer_Text(buffer, " ")) return 0;
	return 1;
}

static int AeronConfigBuffer_Quoted(AeronConfigBuffer* buffer, const char* text) {
	const unsigned char* cursor = (const unsigned char*)text;
	if (!AeronConfigBuffer_Text(buffer, "\"")) return 0;
	while (*cursor) {
		const char* escaped = NULL;
		char unicode[7];
		if (*cursor == '\\') escaped = "\\\\";
		else if (*cursor == '"') escaped = "\\\"";
		else if (*cursor == '\n') escaped = "\\n";
		else if (*cursor == '\r') escaped = "\\r";
		else if (*cursor == '\t') escaped = "\\t";
		if (escaped) {
			if (!AeronConfigBuffer_Text(buffer, escaped)) return 0;
		} else if (*cursor < 0x20u || *cursor == 0x7fu) {
			snprintf(unicode, sizeof(unicode), "\\u%04x", (unsigned)*cursor);
			if (!AeronConfigBuffer_Text(buffer, unicode)) return 0;
		} else if (!AeronConfigBuffer_Append(buffer, (const char*)cursor, 1u)) return 0;
		++cursor;
	}
	return AeronConfigBuffer_Text(buffer, "\"");
}

static int AeronConfig_KeyIsPlain(const char* key) {
	const unsigned char* cursor = (const unsigned char*)key;
	if (!(cursor[0] == '_' || (cursor[0] >= 'A' && cursor[0] <= 'Z') ||
		  (cursor[0] >= 'a' && cursor[0] <= 'z'))) return 0;
	for (++cursor; *cursor; ++cursor)
		if (!(*cursor == '_' || *cursor == '-' || (*cursor >= 'A' && *cursor <= 'Z') ||
			  (*cursor >= 'a' && *cursor <= 'z') || (*cursor >= '0' && *cursor <= '9'))) return 0;
	return 1;
}

static int AeronConfig_FormatRoundTripFloat(char* text, size_t capacity, double value) {
	char candidate[64];
	int precision;
	for (precision = 1; precision <= 17; ++precision) {
		double parsed;
		if (Aeron_FormatAsciiDouble(candidate, sizeof(candidate), value, precision) &&
			Aeron_ParseAsciiDouble(candidate, strlen(candidate), &parsed) &&
			memcmp(&parsed, &value, sizeof(value)) == 0) break;
	}
	if (precision > 17) return 0;
	if (!strchr(candidate, '.')) {
		char* exponent = strchr(candidate, 'e');
		size_t length;
		size_t offset;
		if (!exponent) exponent = strchr(candidate, 'E');
		length = strlen(candidate);
		offset = exponent ? (size_t)(exponent - candidate) : length;
		if (length + 2u >= sizeof(candidate)) return 0;
		memmove(candidate + offset + 2u, candidate + offset, length - offset + 1u);
		candidate[offset] = '.';
		candidate[offset + 1u] = '0';
	}
	if (strlen(candidate) >= capacity) return 0;
	strcpy(text, candidate);
	return 1;
}

static int AeronConfig_EmitScalar(AeronConfigBuffer* buffer, const AeronConfigNode* node) {
	char text[64];
	int length;
	switch (node->type) {
		case AERON_CONFIG_NULL: return AeronConfigBuffer_Text(buffer, "null");
		case AERON_CONFIG_BOOL: return AeronConfigBuffer_Text(buffer, node->value.bool_value ? "true" : "false");
		case AERON_CONFIG_INT:
			length = snprintf(text, sizeof(text), "%lld", (long long)node->value.int_value);
			return length > 0 && (size_t)length < sizeof(text) &&
				   AeronConfigBuffer_Append(buffer, text, (size_t)length);
		case AERON_CONFIG_FLOAT:
			return AeronConfig_FormatRoundTripFloat(text, sizeof(text), node->value.float_value) &&
				   AeronConfigBuffer_Text(buffer, text);
		case AERON_CONFIG_STRING: return AeronConfigBuffer_Quoted(buffer, node->value.string_value);
		default: return 0;
	}
}

static int AeronConfig_EmitNode(AeronConfigBuffer* buffer, const AeronConfigNode* node, int indent);

static int AeronConfig_EmitMap(AeronConfigBuffer* buffer, const AeronConfigNode* node, int indent) {
	size_t index;
	if (!node->value.map.count)
		return AeronConfigBuffer_Indent(buffer, indent) && AeronConfigBuffer_Text(buffer, "{}\n");
	for (index = 0; index < node->value.map.count; ++index) {
		const AeronConfigMapEntry* entry = &node->value.map.entries[index];
		int compound = entry->value->type == AERON_CONFIG_MAP || entry->value->type == AERON_CONFIG_SEQUENCE;
		if (!AeronConfigBuffer_Indent(buffer, indent) ||
			!(AeronConfig_KeyIsPlain(entry->key) ? AeronConfigBuffer_Text(buffer, entry->key)
											 : AeronConfigBuffer_Quoted(buffer, entry->key)) ||
			!AeronConfigBuffer_Text(buffer, compound ? ":\n" : ": ")) return 0;
		if (compound) {
			if (!AeronConfig_EmitNode(buffer, entry->value, indent + 2)) return 0;
		} else if (!AeronConfig_EmitScalar(buffer, entry->value) || !AeronConfigBuffer_Text(buffer, "\n")) return 0;
	}
	return 1;
}

static int AeronConfig_EmitSequence(AeronConfigBuffer* buffer, const AeronConfigNode* node, int indent) {
	size_t index;
	if (!node->value.sequence.count)
		return AeronConfigBuffer_Indent(buffer, indent) && AeronConfigBuffer_Text(buffer, "[]\n");
	for (index = 0; index < node->value.sequence.count; ++index) {
		const AeronConfigNode* item = node->value.sequence.items[index];
		int compound = item->type == AERON_CONFIG_MAP || item->type == AERON_CONFIG_SEQUENCE;
		if (!AeronConfigBuffer_Indent(buffer, indent) ||
			!AeronConfigBuffer_Text(buffer, compound ? "-\n" : "- ")) return 0;
		if (compound) {
			if (!AeronConfig_EmitNode(buffer, item, indent + 2)) return 0;
		} else if (!AeronConfig_EmitScalar(buffer, item) || !AeronConfigBuffer_Text(buffer, "\n")) return 0;
	}
	return 1;
}

static int AeronConfig_EmitNode(AeronConfigBuffer* buffer, const AeronConfigNode* node, int indent) {
	if (node->type == AERON_CONFIG_MAP) return AeronConfig_EmitMap(buffer, node, indent);
	if (node->type == AERON_CONFIG_SEQUENCE) return AeronConfig_EmitSequence(buffer, node, indent);
	return AeronConfigBuffer_Indent(buffer, indent) && AeronConfig_EmitScalar(buffer, node) &&
		   AeronConfigBuffer_Text(buffer, "\n");
}

int AeronConfigFile_SerializeYaml(const AeronConfigFile* config, char** out_data,
								  size_t* out_size, AeronConfigError* error) {
	AeronConfigBuffer buffer = { 0 };
	if (!config || !config->root || !out_data || !out_size)
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_INVALID_ARGUMENT,
								config ? config->root_kind : AERON_VFS_ROOT_RESOURCE,
								config ? config->path : NULL, 0, 0,
								"invalid configuration serialization arguments");
	AeronConfig_ClearError(error, config->root_kind, config->path);
	*out_data = NULL;
	*out_size = 0;
	if (!AeronConfig_EmitNode(&buffer, config->root, 0)) {
		SDL_free(buffer.data);
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_OUT_OF_MEMORY, config->root_kind,
								config->path, 0, 0, "could not serialize configuration document");
	}
	*out_data = buffer.data;
	*out_size = buffer.size;
	return 1;
}

void AeronConfigFile_FreeSerialized(char* data) {
	SDL_free(data);
}

int AeronConfigFile_SaveYaml(AeronVfs* vfs, const AeronConfigFile* config, AeronConfigError* error) {
	char* data = NULL;
	size_t size = 0;
	int result;
	if (!vfs || !config || !config->root || config->root_kind == AERON_VFS_ROOT_RESOURCE)
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_INVALID_ARGUMENT,
								config ? config->root_kind : AERON_VFS_ROOT_RESOURCE,
								config ? config->path : NULL, 0, 0, "invalid configuration save arguments");
	if (!AeronConfigFile_SerializeYaml(config, &data, &size, error)) return 0;
	result = AeronVfs_WriteAllAtomic(vfs, config->root_kind, config->path, data, size);
	AeronConfigFile_FreeSerialized(data);
	if (!result)
		return AeronConfig_Fail(error, AERON_CONFIG_ERROR_IO, config->root_kind, config->path, 0, 0,
								"could not atomically save configuration document");
	return 1;
}

const AeronConfigNode* AeronConfigFile_Root(const AeronConfigFile* config) {
	return config ? config->root : NULL;
}

static const AeronConfigNode* AeronConfigNode_MapGetLength(const AeronConfigNode* node, const char* key,
														   size_t key_length) {
	size_t i;

	if (!node || node->type != AERON_CONFIG_MAP || !key) {
		return NULL;
	}

	for (i = 0; i < node->value.map.count; ++i) {
		if (strlen(node->value.map.entries[i].key) == key_length &&
			memcmp(node->value.map.entries[i].key, key, key_length) == 0) {
			return node->value.map.entries[i].value;
		}
	}

	return NULL;
}

static const AeronConfigNode* AeronConfigNode_PathGet(const AeronConfigNode* root, const char* path) {
	const char*            p;
	const AeronConfigNode* node;

	if (!path || path[0] == '\0') {
		return root;
	}

	node = root;
	p    = path;
	while (*p) {
		const char* key_start;
		size_t      key_length;

		if (*p == '.') {
			return NULL;
		}

		key_start = p;
		while (*p && *p != '.' && *p != '[') {
			++p;
		}

		key_length = (size_t)(p - key_start);
		if (key_length) {
			node = AeronConfigNode_MapGetLength(node, key_start, key_length);
			if (!node) {
				return NULL;
			}
		}

		while (*p == '[') {
			size_t index;

			++p;
			if (*p < '0' || *p > '9') {
				return NULL;
			}

			index = 0;
			while (*p >= '0' && *p <= '9') {
				index = index * 10u + (size_t)(*p - '0');
				++p;
			}

			if (*p != ']') {
				return NULL;
			}
			++p;

			node = AeronConfigNode_SequenceGet(node, index);
			if (!node) {
				return NULL;
			}
		}

		if (*p == '.') {
			++p;
			if (*p == '\0') {
				return NULL;
			}
		} else if (*p != '\0') {
			return NULL;
		}
	}

	return node;
}

const AeronConfigNode* AeronConfigFile_GetNode(const AeronConfigFile* config, const char* path) {
	return AeronConfigNode_PathGet(AeronConfigFile_Root(config), path);
}

int AeronConfigFile_Has(const AeronConfigFile* config, const char* path) {
	return AeronConfigFile_GetNode(config, path) != NULL;
}

const char* AeronConfigFile_GetString(const AeronConfigFile* config, const char* path, const char* fallback) {
	return AeronConfigNode_String(AeronConfigFile_GetNode(config, path), fallback);
}

int AeronConfigFile_GetBool(const AeronConfigFile* config, const char* path, int fallback) {
	return AeronConfigNode_Bool(AeronConfigFile_GetNode(config, path), fallback);
}

int64_t AeronConfigFile_GetInt(const AeronConfigFile* config, const char* path, int64_t fallback) {
	return AeronConfigNode_Int(AeronConfigFile_GetNode(config, path), fallback);
}

double AeronConfigFile_GetFloat(const AeronConfigFile* config, const char* path, double fallback) {
	return AeronConfigNode_Float(AeronConfigFile_GetNode(config, path), fallback);
}

AeronConfigNodeType AeronConfigNode_Type(const AeronConfigNode* node) {
	return node ? node->type : AERON_CONFIG_NULL;
}

int AeronConfigNode_Line(const AeronConfigNode* node) { return node ? node->line : 0; }

int AeronConfigNode_Column(const AeronConfigNode* node) { return node ? node->column : 0; }

AeronVfsRoot AeronConfigNode_SourceRoot(const AeronConfigNode* node) {
	return node ? node->source_root : AERON_VFS_ROOT_RESOURCE;
}

const char* AeronConfigNode_SourcePath(const AeronConfigNode* node) {
	return node ? node->source_path : NULL;
}

const char* AeronConfigNode_String(const AeronConfigNode* node, const char* fallback) {
	if (!node || node->type != AERON_CONFIG_STRING) {
		return fallback;
	}

	return node->value.string_value;
}

int AeronConfigNode_Bool(const AeronConfigNode* node, int fallback) {
	if (!node || node->type != AERON_CONFIG_BOOL) {
		return fallback;
	}

	return node->value.bool_value;
}

int64_t AeronConfigNode_Int(const AeronConfigNode* node, int64_t fallback) {
	if (!node || node->type != AERON_CONFIG_INT) {
		return fallback;
	}

	return node->value.int_value;
}

double AeronConfigNode_Float(const AeronConfigNode* node, double fallback) {
	if (!node) {
		return fallback;
	}

	if (node->type == AERON_CONFIG_FLOAT) {
		return node->value.float_value;
	}

	if (node->type == AERON_CONFIG_INT) {
		return (double)node->value.int_value;
	}

	return fallback;
}

size_t AeronConfigNode_MapCount(const AeronConfigNode* node) {
	if (!node || node->type != AERON_CONFIG_MAP) {
		return 0;
	}

	return node->value.map.count;
}

const char* AeronConfigNode_MapKeyAt(const AeronConfigNode* node, size_t index) {
	if (!node || node->type != AERON_CONFIG_MAP || index >= node->value.map.count) {
		return NULL;
	}

	return node->value.map.entries[index].key;
}

const AeronConfigNode* AeronConfigNode_MapValueAt(const AeronConfigNode* node, size_t index) {
	if (!node || node->type != AERON_CONFIG_MAP || index >= node->value.map.count) {
		return NULL;
	}

	return node->value.map.entries[index].value;
}

const AeronConfigNode* AeronConfigNode_MapGet(const AeronConfigNode* node, const char* key) {
	return key ? AeronConfigNode_MapGetLength(node, key, strlen(key)) : NULL;
}

size_t AeronConfigNode_SequenceCount(const AeronConfigNode* node) {
	if (!node || node->type != AERON_CONFIG_SEQUENCE) {
		return 0;
	}

	return node->value.sequence.count;
}

const AeronConfigNode* AeronConfigNode_SequenceGet(const AeronConfigNode* node, size_t index) {
	if (!node || node->type != AERON_CONFIG_SEQUENCE || index >= node->value.sequence.count) {
		return NULL;
	}

	return node->value.sequence.items[index];
}
