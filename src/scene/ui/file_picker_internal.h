#ifndef AERON_SCENE_UI_FILE_PICKER_INTERNAL_H
#define AERON_SCENE_UI_FILE_PICKER_INTERNAL_H

#include "aeron/scene/ui_file_picker.h"

#include <SDL3/SDL.h>

#define PICKER_PATH_CAPACITY 4096
#define PICKER_ERROR_CAPACITY 1024
#define PICKER_HISTORY_CAPACITY 32
#define PICKER_VIEWPORT_MARGIN_REF 32.0f

typedef struct PickerFsEntry {
	uint64_t id;
	char*    name;
	char*    path;
	char     detail[32];
	uint8_t  is_directory;
	uint8_t  hidden;
} PickerFsEntry;

typedef struct PickerFsResult {
	uint64_t       generation;
	char*          path;
	PickerFsEntry* entries;
	size_t         count;
	char           error[PICKER_ERROR_CAPACITY];
} PickerFsResult;

typedef struct PickerOwnedFilter {
	char*  label;
	char** extensions;
	size_t extension_count;
} PickerOwnedFilter;

typedef struct PickerLocation {
	char* label;
	char* path;
} PickerLocation;

struct AeronUiFilePicker {
	SDL_Thread*     worker;
	SDL_Mutex*      mutex;
	SDL_Condition*  condition;
	int             stopping;
	char*           requested_path;
	uint64_t        request_generation;
	PickerFsResult* completed;
	int             worker_failed;

	int                       open;
	AeronUiFilePickerMode     mode;
	char*                     title;
	char*                     instructions;
	char*                     accept_label;
	char*                     cancel_label;
	PickerOwnedFilter*        filters;
	const char**              filter_labels;
	size_t                    filter_count;
	int                       active_filter;
	int                       show_hidden;
	AeronUiFilePickerAcceptFn accept_fn;
	void*                     accept_user;

	PickerFsResult*  current;
	AeronUiListItem* visible;
	size_t*          visible_indices;
	size_t           visible_count;
	size_t           selected_visible;
	char             location_text[PICKER_PATH_CAPACITY];
	char             parent_path[PICKER_PATH_CAPACITY];
	char             last_parent_path[PICKER_PATH_CAPACITY];
	char             inline_error[PICKER_ERROR_CAPACITY];

	PickerLocation*         locations;
	AeronUiListItem*        location_items;
	size_t                  location_count;
	size_t                  selected_location;
	char*                   history[PICKER_HISTORY_CAPACITY];
	size_t                  history_count;
	size_t                  history_index;
	int                     loading;
	AeronUiFilePickerResult terminal;
	int                     layout_output_width;
	int                     layout_output_height;
	AeronUiWindowDesc       window;
};

char*           picker_path_normalize(const char* path, const char* base);
char*           picker_path_parent(const char* path);
int             picker_path_is_directory(const char* path);
int             picker_extension_matches(const char* name, const char* extension);
PickerFsResult* picker_fs_enumerate(const char* path, uint64_t generation);
void            picker_fs_result_free(PickerFsResult* result);

#endif
