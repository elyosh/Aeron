#include "internal.h"

#include <errno.h>
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
};

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
	char*  copy;
	char*  end;
	double parsed;

	copy = AeronConfig_DuplicateBytes((const unsigned char*)value, length);
	if (!copy) {
		return 0;
	}

	errno  = 0;
	parsed = strtod(copy, &end);
	if (errno != 0 || end == copy || *end != '\0') {
		SDL_free(copy);
		return 0;
	}

	SDL_free(copy);
	*out_value = parsed;
	return 1;
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

	SDL_free(node);
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
			Aeron_Log("aeron.config", "YAML config map keys must be scalar values");
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
		Aeron_Log("aeron.config", "YAML config nesting exceeds %d levels", AERON_CONFIG_MAX_DEPTH);
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

static void AeronConfig_LogYamlError(const char* path, const yaml_parser_t* parser) {
	const char* problem;

	problem = parser->problem ? parser->problem : "parse error";
	Aeron_Log("aeron.config", "%s:%zu:%zu: %s", path ? path : "<yaml>", parser->problem_mark.line + 1,
			  parser->problem_mark.column + 1, problem);
}

int AeronConfigFile_LoadYaml(AeronVfs* vfs, AeronVfsRoot root, const char* path,
							 AeronConfigFile** out_config) {
	unsigned char*   data;
	size_t           data_size;
	yaml_parser_t    parser;
	yaml_document_t  document;
	yaml_document_t  extra_document;
	yaml_node_t*     root_node;
	AeronConfigFile* config;
	int              root_id;
	int              result;

	if (!out_config) {
		return 0;
	}

	*out_config = NULL;
	if (!AeronConfig_ReadFile(vfs, root, path, &data, &data_size)) {
		return 0;
	}

	memset(&document, 0, sizeof(document));
	memset(&extra_document, 0, sizeof(extra_document));
	if (!yaml_parser_initialize(&parser)) {
		SDL_free(data);
		return 0;
	}

	result = 0;
	yaml_parser_set_input_string(&parser, data, data_size);
	if (!yaml_parser_load(&parser, &document)) {
		AeronConfig_LogYamlError(path, &parser);
		goto done;
	}

	config = (AeronConfigFile*)SDL_calloc(1, sizeof(*config));
	if (!config) {
		goto done;
	}

	root_node = yaml_document_get_root_node(&document);
	if (root_node) {
		root_id      = AeronConfig_YamlNodeId(&document, root_node);
		config->root = AeronConfig_ConvertYamlNode(&document, root_id, 0);
	} else {
		config->root = AeronConfig_CreateNode(AERON_CONFIG_NULL, NULL);
	}

	if (!config->root) {
		AeronConfigFile_Destroy(config);
		goto done;
	}

	if (!yaml_parser_load(&parser, &extra_document)) {
		AeronConfigFile_Destroy(config);
		AeronConfig_LogYamlError(path, &parser);
		goto done;
	}

	if (yaml_document_get_root_node(&extra_document)) {
		Aeron_Log("aeron.config", "%s: YAML config files must contain a single document",
				  path ? path : "<yaml>");
		AeronConfigFile_Destroy(config);
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

void AeronConfigFile_Destroy(AeronConfigFile* config) {
	if (!config) {
		return;
	}

	AeronConfigNode_Destroy(config->root);
	SDL_free(config);
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
