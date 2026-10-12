/*
 * YAML to cJSON (yaml_json.h), over libyaml's document loader.
 */
#include "yaml_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yaml.h>

static cJSON* scalar(const yaml_node_t* node) {
	const char* v = (const char*)node->data.scalar.value;
	if (node->data.scalar.style != YAML_PLAIN_SCALAR_STYLE)
		return cJSON_CreateString(v);
	if (!*v || strcmp(v, "~") == 0 || strcmp(v, "null") == 0)
		return cJSON_CreateNull();
	if (strcmp(v, "true") == 0)
		return cJSON_CreateTrue();
	if (strcmp(v, "false") == 0)
		return cJSON_CreateFalse();
	char*        end;
	const double d = strtod(v, &end);
	if (end != v && !*end)
		return cJSON_CreateNumber(d);
	return cJSON_CreateString(v);
}

static cJSON* convert(yaml_document_t* doc, const yaml_node_t* node, int depth) {
	if (!node || depth > 64)
		return NULL;
	switch (node->type) {
		case YAML_SCALAR_NODE:
			return scalar(node);
		case YAML_SEQUENCE_NODE: {
			cJSON* array = cJSON_CreateArray();
			for (yaml_node_item_t* item = node->data.sequence.items.start;
				 array && item < node->data.sequence.items.top; ++item) {
				cJSON* child = convert(doc, yaml_document_get_node(doc, *item), depth + 1);
				if (!child) {
					cJSON_Delete(array);
					return NULL;
				}
				cJSON_AddItemToArray(array, child);
			}
			return array;
		}
		case YAML_MAPPING_NODE: {
			cJSON* object = cJSON_CreateObject();
			for (yaml_node_pair_t* pair = node->data.mapping.pairs.start;
				 object && pair < node->data.mapping.pairs.top; ++pair) {
				const yaml_node_t* key   = yaml_document_get_node(doc, pair->key);
				cJSON*             child = convert(doc, yaml_document_get_node(doc, pair->value), depth + 1);
				if (!key || key->type != YAML_SCALAR_NODE || !child) {
					cJSON_Delete(child);
					cJSON_Delete(object);
					return NULL;
				}
				cJSON_AddItemToObject(object, (const char*)key->data.scalar.value, child);
			}
			return object;
		}
		default:
			return NULL;
	}
}

cJSON* YamlJson_Load(const char* path, char* err, size_t err_cap) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		snprintf(err, err_cap, "%s: cannot open", path);
		return NULL;
	}
	yaml_parser_t   parser;
	yaml_document_t doc;
	cJSON*          root = NULL;
	if (!yaml_parser_initialize(&parser)) {
		fclose(f);
		snprintf(err, err_cap, "%s: out of memory", path);
		return NULL;
	}
	yaml_parser_set_input_file(&parser, f);
	if (!yaml_parser_load(&parser, &doc)) {
		snprintf(err, err_cap, "%s:%zu: %s", path, parser.problem_mark.line + 1,
				 parser.problem ? parser.problem : "YAML error");
	} else {
		const yaml_node_t* top = yaml_document_get_root_node(&doc);
		root                   = top ? convert(&doc, top, 0) : NULL;
		if (!root)
			snprintf(err, err_cap, "%s: empty, or not plain YAML (no anchors or tags)", path);
		yaml_document_delete(&doc);
	}
	yaml_parser_delete(&parser);
	fclose(f);
	return root;
}
