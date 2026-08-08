#include "file_picker_internal.h"

#include "internal.h"

#include <ctype.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define PICKER_MAX_WIDTH_REF 1120.0f
#define PICKER_SIDEBAR_WIDTH_REF 230.0f
#define PICKER_DESIRED_VISIBLE_ROWS 16.0f

static float picker_help_advance(AeronUiContext* ui, const char* text, float width) {
	const float height = text && text[0] ? AeronUi_MeasureHelpHeight(ui, text, width) : 0.0f;
	return height > 0.0f ? height + ui->theme.item_spacing : 0.0f;
}

static float picker_trailing_height(const AeronUiFilePicker* picker, AeronUiContext* ui, float width) {
	const float row    = ui->theme.row_height;
	const float gap    = ui->theme.item_spacing;
	float       height = 0.0f;
	if (picker->filter_count > 1)
		height += row + gap;
	height += row + gap; /* hidden-files toggle */
	if (picker->loading) {
		height += picker_help_advance(ui, "Loading folder contents...", width);
	} else if (picker->current && picker->visible_count == 0) {
		height += picker_help_advance(ui, "This folder is empty.", width);
	}
	height += row + gap; /* selected path */
	height += picker_help_advance(ui, picker->inline_error, width);
	height += 9.0f + gap;
	height += row;
	return height;
}

static AeronUiWindowDesc picker_window_desc(AeronUiFilePicker* picker, AeronUiContext* ui) {
	/* Loading and validation messages change the list height, not the modal
	 * frame. Recompute the frame only when the output viewport changes. */
	if (picker->layout_output_width == ui->out_w && picker->layout_output_height == ui->out_h &&
		picker->window.width_ref > 0.0f && picker->window.height_ref > 0.0f) {
		return picker->window;
	}
	const float scale               = ui->scale > 0.0f ? ui->scale : 1.0f;
	const float viewport_width_ref  = (float)ui->out_w / scale;
	const float viewport_height_ref = (float)ui->out_h / scale;
	const float horizontal_margin =
		fminf(PICKER_VIEWPORT_MARGIN_REF, fmaxf(0.0f, (viewport_width_ref - 1.0f) * 0.5f));
	const float vertical_margin =
		fminf(PICKER_VIEWPORT_MARGIN_REF, fmaxf(0.0f, (viewport_height_ref - 1.0f) * 0.5f));
	const float width_ref      = fminf(PICKER_MAX_WIDTH_REF, viewport_width_ref - horizontal_margin * 2.0f);
	const float content_width  = fmaxf(1.0f, width_ref - ui->theme.window_pad * 2.0f);
	const float row            = ui->theme.row_height;
	const float gap            = ui->theme.item_spacing;
	const float content_height = picker_help_advance(ui, picker->instructions, content_width) +
								 (row + gap) * 2.0f + row * PICKER_DESIRED_VISIBLE_ROWS + gap +
								 picker_trailing_height(picker, ui, content_width);
	const float desired_height_ref = ui->theme.title_height + ui->theme.window_pad * 2.0f + content_height;
	picker->window                 = (AeronUiWindowDesc) {
		.width_ref  = width_ref,
		.height_ref = fminf(desired_height_ref, viewport_height_ref - vertical_margin * 2.0f),
		.centered   = 1,
	};
	picker->layout_output_width  = ui->out_w;
	picker->layout_output_height = ui->out_h;
	return picker->window;
}

static float picker_list_height_ref(const AeronUiFilePicker* picker, AeronUiContext* ui) {
	const UiLayout* layout = ui_layout_top(ui);
	if (!layout)
		return 1.0f;
	const float width_ref = ui->scale > 0.0f ? layout->w / ui->scale : layout->w;
	const float height =
		AeronUi_AvailableHeight(ui) - picker_trailing_height(picker, ui, width_ref) - ui->theme.item_spacing;
	return fmaxf(height, 1.0f);
}

static void picker_copy_error(char* destination, size_t capacity, const char* message) {
	if (destination && capacity)
		SDL_snprintf(destination, capacity, "%s", message ? message : "");
}

static void picker_clear_visible(AeronUiFilePicker* picker) {
	SDL_free(picker->visible);
	SDL_free(picker->visible_indices);
	picker->visible          = NULL;
	picker->visible_indices  = NULL;
	picker->visible_count    = 0;
	picker->selected_visible = SIZE_MAX;
}

static void picker_clear_descriptor(AeronUiFilePicker* picker) {
	SDL_free(picker->title);
	SDL_free(picker->instructions);
	SDL_free(picker->accept_label);
	SDL_free(picker->cancel_label);
	for (size_t i = 0; i < picker->filter_count; i++) {
		SDL_free(picker->filters[i].label);
		for (size_t n = 0; n < picker->filters[i].extension_count; n++)
			SDL_free(picker->filters[i].extensions[n]);
		SDL_free(picker->filters[i].extensions);
	}
	SDL_free(picker->filters);
	SDL_free(picker->filter_labels);
	picker->title = picker->instructions = picker->accept_label = picker->cancel_label = NULL;
	picker->filters                                                                    = NULL;
	picker->filter_labels                                                              = NULL;
	picker->filter_count                                                               = 0;
}

static void picker_clear_locations(AeronUiFilePicker* picker) {
	for (size_t i = 0; i < picker->location_count; i++) {
		SDL_free(picker->locations[i].label);
		SDL_free(picker->locations[i].path);
	}
	SDL_free(picker->locations);
	SDL_free(picker->location_items);
	picker->locations         = NULL;
	picker->location_items    = NULL;
	picker->location_count    = 0;
	picker->selected_location = SIZE_MAX;
}

static void picker_clear_history(AeronUiFilePicker* picker) {
	for (size_t i = 0; i < picker->history_count; i++)
		SDL_free(picker->history[i]);
	memset(picker->history, 0, sizeof picker->history);
	picker->history_count = 0;
	picker->history_index = 0;
}

static int picker_worker_main(void* user) {
	AeronUiFilePicker* picker = (AeronUiFilePicker*)user;
	for (;;) {
		SDL_LockMutex(picker->mutex);
		while (!picker->stopping && !picker->requested_path)
			SDL_WaitCondition(picker->condition, picker->mutex);
		if (picker->stopping) {
			SDL_UnlockMutex(picker->mutex);
			return 0;
		}
		char*          path       = picker->requested_path;
		const uint64_t generation = picker->request_generation;
		picker->requested_path    = NULL;
		SDL_UnlockMutex(picker->mutex);

		PickerFsResult* result = picker_fs_enumerate(path, generation);
		SDL_free(path);

		SDL_LockMutex(picker->mutex);
		if (result && generation == picker->request_generation && !picker->stopping) {
			picker_fs_result_free(picker->completed);
			picker->completed     = result;
			picker->worker_failed = 0;
		} else if (!result && generation == picker->request_generation && !picker->stopping) {
			picker->worker_failed = 1;
		} else {
			picker_fs_result_free(result);
		}
		SDL_UnlockMutex(picker->mutex);
	}
}

static int picker_request(AeronUiFilePicker* picker, const char* path, int clear_error) {
	char* normalized = picker_path_normalize(path, picker->current ? picker->current->path : NULL);
	if (!normalized) {
		picker_copy_error(picker->inline_error, sizeof picker->inline_error, "The location is invalid.");
		return 0;
	}
	if (strlen(normalized) >= sizeof picker->location_text) {
		SDL_free(normalized);
		picker_copy_error(picker->inline_error, sizeof picker->inline_error,
						  "The location path is too long for the picker.");
		return 0;
	}
	SDL_snprintf(picker->location_text, sizeof picker->location_text, "%s", normalized);
	SDL_LockMutex(picker->mutex);
	SDL_free(picker->requested_path);
	picker->requested_path = normalized;
	picker->request_generation++;
	picker->worker_failed = 0;
	picker->loading       = 1;
	SDL_SignalCondition(picker->condition);
	SDL_UnlockMutex(picker->mutex);
	if (clear_error)
		picker->inline_error[0] = '\0';
	return 1;
}

int picker_extension_matches(const char* name, const char* extension) {
	if (!name || !extension || !extension[0])
		return 0;
	while (*extension == '.' || *extension == '*')
		extension++;
	const size_t name_length = strlen(name);
	const size_t ext_length  = strlen(extension);
	if (!ext_length || name_length <= ext_length || name[name_length - ext_length - 1] != '.')
		return 0;
	return SDL_strcasecmp(name + name_length - ext_length, extension) == 0;
}

static int picker_entry_visible(const AeronUiFilePicker* picker, const PickerFsEntry* entry) {
	if (!picker->show_hidden && entry->hidden)
		return 0;
	if (entry->is_directory || picker->mode == AERON_UI_FILE_PICKER_SELECT_DIRECTORY ||
		picker->filter_count == 0)
		return 1;
	const PickerOwnedFilter* filter = &picker->filters[picker->active_filter];
	for (size_t i = 0; i < filter->extension_count; i++)
		if (picker_extension_matches(entry->name, filter->extensions[i]))
			return 1;
	return 0;
}

static int picker_rebuild_visible(AeronUiFilePicker* picker, uint64_t restore_id) {
	picker_clear_visible(picker);
	if (!picker->current || picker->current->count == 0)
		return 1;
	AeronUiListItem* visible = (AeronUiListItem*)SDL_calloc(picker->current->count, sizeof *visible);
	size_t*          indices = (size_t*)SDL_malloc(picker->current->count * sizeof *indices);
	if (!visible || !indices) {
		SDL_free(visible);
		SDL_free(indices);
		picker_copy_error(picker->inline_error, sizeof picker->inline_error,
						  "Not enough memory to display this folder.");
		return 0;
	}
	for (size_t i = 0; i < picker->current->count; i++) {
		PickerFsEntry* entry = &picker->current->entries[i];
		if (!picker_entry_visible(picker, entry))
			continue;
		const size_t out    = picker->visible_count++;
		visible[out].id     = entry->id;
		visible[out].label  = entry->name;
		visible[out].detail = entry->detail;
		visible[out].flags  = entry->is_directory ? AERON_UI_LIST_ITEM_DIRECTORY : AERON_UI_LIST_ITEM_NONE;
		if (picker->mode == AERON_UI_FILE_PICKER_SELECT_DIRECTORY && !entry->is_directory)
			visible[out].flags |= AERON_UI_LIST_ITEM_DISABLED;
		indices[out] = i;
		if (restore_id && entry->id == restore_id)
			picker->selected_visible = out;
	}
	picker->visible         = visible;
	picker->visible_indices = indices;
	return 1;
}

static uint64_t picker_selected_id(const AeronUiFilePicker* picker) {
	if (!picker->current || picker->selected_visible >= picker->visible_count)
		return 0;
	return picker->current->entries[picker->visible_indices[picker->selected_visible]].id;
}

static void picker_take_completed(AeronUiFilePicker* picker) {
	SDL_LockMutex(picker->mutex);
	PickerFsResult* result  = picker->completed;
	picker->completed       = NULL;
	const int worker_failed = picker->worker_failed;
	picker->worker_failed   = 0;
	SDL_UnlockMutex(picker->mutex);
	if (worker_failed) {
		picker->loading = 0;
		picker_copy_error(picker->inline_error, sizeof picker->inline_error,
						  "Not enough memory to read this folder.");
		if (picker->current)
			SDL_snprintf(picker->location_text, sizeof picker->location_text, "%s", picker->current->path);
	}
	if (!result)
		return;
	picker->loading = 0;
	if (result->error[0]) {
		picker_copy_error(picker->inline_error, sizeof picker->inline_error, result->error);
		if (picker->current)
			SDL_snprintf(picker->location_text, sizeof picker->location_text, "%s", picker->current->path);
		picker_fs_result_free(result);
		return;
	}
	const uint64_t restore_id = picker_selected_id(picker);
	picker_fs_result_free(picker->current);
	picker->current = result;
	SDL_snprintf(picker->location_text, sizeof picker->location_text, "%s", result->path);
	picker->parent_path[0] = '\0';
	char* parent           = picker_path_parent(result->path);
	if (parent && strcmp(parent, result->path) != 0)
		SDL_snprintf(picker->parent_path, sizeof picker->parent_path, "%s", parent);
	SDL_free(parent);
	picker_rebuild_visible(picker, restore_id);
}

static int picker_add_location(AeronUiFilePicker* picker, const char* label, const char* path,
							   int verify_directory) {
	char* normalized = picker_path_normalize(path, NULL);
	if (!normalized || (verify_directory && !picker_path_is_directory(normalized))) {
		SDL_free(normalized);
		return 1;
	}
	for (size_t i = 0; i < picker->location_count; i++) {
		if (SDL_strcasecmp(picker->locations[i].path, normalized) == 0) {
			SDL_free(normalized);
			return 1;
		}
	}
	char* owned_label = SDL_strdup(label);
	if (!owned_label) {
		SDL_free(normalized);
		return 0;
	}
	const size_t     count     = picker->location_count + 1;
	PickerLocation*  locations = (PickerLocation*)SDL_realloc(picker->locations, count * sizeof *locations);
	AeronUiListItem* items     = (AeronUiListItem*)SDL_realloc(picker->location_items, count * sizeof *items);
	if (!locations || !items) {
		if (locations)
			picker->locations = locations;
		if (items)
			picker->location_items = items;
		SDL_free(owned_label);
		SDL_free(normalized);
		return 0;
	}
	picker->locations                              = locations;
	picker->location_items                         = items;
	PickerLocation* location                       = &picker->locations[picker->location_count];
	location->label                                = owned_label;
	location->path                                 = normalized;
	picker->location_items[picker->location_count] = (AeronUiListItem) {
		.id = (uint64_t)count, .label = location->label, .detail = NULL, .flags = AERON_UI_LIST_ITEM_DIRECTORY
	};
	picker->location_count = count;
	return 1;
}

static int picker_build_locations(AeronUiFilePicker* picker) {
	static const struct {
		const char* label;
		SDL_Folder  folder;
	} folders[] = { { "Home", SDL_FOLDER_HOME },
					{ "Desktop", SDL_FOLDER_DESKTOP },
					{ "Documents", SDL_FOLDER_DOCUMENTS },
					{ "Downloads", SDL_FOLDER_DOWNLOADS } };
	for (size_t i = 0; i < sizeof folders / sizeof folders[0]; i++) {
		const char* path = SDL_GetUserFolder(folders[i].folder);
		if (path && !picker_add_location(picker, folders[i].label, path, 1))
			return 0;
	}
	const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);
	if (home) {
		char* root   = picker_path_normalize(home, NULL);
		char* parent = root ? picker_path_parent(root) : NULL;
		while (root && parent && strcmp(root, parent) != 0) {
			SDL_free(root);
			root   = parent;
			parent = picker_path_parent(root);
		}
		SDL_free(parent);
		if (root && !picker_add_location(picker, root, root, 1)) {
			SDL_free(root);
			return 0;
		}
		SDL_free(root);
	}
#if defined(_WIN32)
	const DWORD drives = GetLogicalDrives();
	for (char drive = 'A'; drive <= 'Z'; drive++) {
		if (!(drives & (1u << (drive - 'A'))))
			continue;
		char path[4] = { drive, ':', '/', '\0' };
		if (!picker_add_location(picker, path, path, 0))
			return 0;
	}
#endif
	return 1;
}

static int picker_copy_descriptor(AeronUiFilePicker* picker, const AeronUiFilePickerDesc* desc) {
	picker->title        = SDL_strdup(desc->title && desc->title[0] ? desc->title : "SELECT LOCATION");
	picker->instructions = SDL_strdup(desc->instructions ? desc->instructions : "");
	picker->accept_label =
		SDL_strdup(desc->accept_label && desc->accept_label[0] ? desc->accept_label : "Select");
	picker->cancel_label =
		SDL_strdup(desc->cancel_label && desc->cancel_label[0] ? desc->cancel_label : "Cancel");
	if (!picker->title || !picker->instructions || !picker->accept_label || !picker->cancel_label)
		return 0;
	if (!desc->filter_count)
		return 1;
	picker->filters       = (PickerOwnedFilter*)SDL_calloc(desc->filter_count, sizeof *picker->filters);
	picker->filter_labels = (const char**)SDL_calloc(desc->filter_count, sizeof *picker->filter_labels);
	if (!picker->filters || !picker->filter_labels)
		return 0;
	picker->filter_count = desc->filter_count;
	for (size_t i = 0; i < desc->filter_count; i++) {
		const AeronUiFileFilter* source = &desc->filters[i];
		PickerOwnedFilter*       target = &picker->filters[i];
		target->label           = SDL_strdup(source->label && source->label[0] ? source->label : "Files");
		target->extension_count = source->extension_count;
		if (!target->label || (source->extension_count && !source->extensions))
			return 0;
		if (source->extension_count) {
			target->extensions = (char**)SDL_calloc(source->extension_count, sizeof *target->extensions);
			if (!target->extensions)
				return 0;
			for (size_t n = 0; n < source->extension_count; n++) {
				target->extensions[n] = SDL_strdup(source->extensions[n]);
				if (!target->extensions[n])
					return 0;
			}
		}
		picker->filter_labels[i] = target->label;
	}
	return 1;
}

AeronUiFilePicker* AeronUiFilePicker_Create(void) {
	AeronUiFilePicker* picker = (AeronUiFilePicker*)SDL_calloc(1, sizeof *picker);
	if (!picker)
		return NULL;
	picker->mutex             = SDL_CreateMutex();
	picker->condition         = SDL_CreateCondition();
	picker->selected_visible  = SIZE_MAX;
	picker->selected_location = SIZE_MAX;
	if (!picker->mutex || !picker->condition) {
		AeronUiFilePicker_Destroy(picker);
		return NULL;
	}
	return picker;
}

void AeronUiFilePicker_Destroy(AeronUiFilePicker* picker) {
	if (!picker)
		return;
	if (picker->worker) {
		SDL_LockMutex(picker->mutex);
		picker->stopping = 1;
		SDL_SignalCondition(picker->condition);
		SDL_UnlockMutex(picker->mutex);
		SDL_WaitThread(picker->worker, NULL);
	}
	SDL_free(picker->requested_path);
	picker_fs_result_free(picker->completed);
	picker_fs_result_free(picker->current);
	picker_clear_visible(picker);
	picker_clear_descriptor(picker);
	picker_clear_locations(picker);
	picker_clear_history(picker);
	SDL_DestroyCondition(picker->condition);
	SDL_DestroyMutex(picker->mutex);
	SDL_free(picker);
}

int AeronUiFilePicker_Open(AeronUiFilePicker* picker, const AeronUiFilePickerDesc* desc, char* error,
						   size_t error_capacity) {
	if (!picker || !desc || desc->mode < AERON_UI_FILE_PICKER_OPEN_FILE ||
		desc->mode > AERON_UI_FILE_PICKER_SELECT_DIRECTORY || (desc->filter_count && !desc->filters)) {
		picker_copy_error(error, error_capacity, "Invalid file picker options.");
		return 0;
	}
	picker->open                 = 0;
	picker->loading              = 0;
	picker->terminal             = AERON_UI_FILE_PICKER_NONE;
	picker->layout_output_width  = 0;
	picker->layout_output_height = 0;
	picker->window               = (AeronUiWindowDesc) { 0 };
	SDL_LockMutex(picker->mutex);
	SDL_free(picker->requested_path);
	picker->requested_path = NULL;
	picker_fs_result_free(picker->completed);
	picker->completed     = NULL;
	picker->worker_failed = 0;
	picker->request_generation++;
	SDL_UnlockMutex(picker->mutex);
	picker_clear_descriptor(picker);
	picker_clear_locations(picker);
	picker_clear_history(picker);
	picker_clear_visible(picker);
	picker_fs_result_free(picker->current);
	picker->current         = NULL;
	picker->mode            = desc->mode;
	picker->show_hidden     = desc->show_hidden != 0;
	picker->accept_fn       = desc->accept_fn;
	picker->accept_user     = desc->accept_user;
	picker->active_filter   = 0;
	picker->inline_error[0] = '\0';
	picker->parent_path[0]  = '\0';
	if (!picker_copy_descriptor(picker, desc) || !picker_build_locations(picker)) {
		picker_copy_error(error, error_capacity, "Not enough memory to open the file picker.");
		picker_clear_descriptor(picker);
		picker_clear_locations(picker);
		return 0;
	}
	if (!picker->worker) {
		picker->worker = SDL_CreateThread(picker_worker_main, "aeron-file-picker", picker);
		if (!picker->worker) {
			picker_copy_error(error, error_capacity, SDL_GetError());
			return 0;
		}
	}
	const char* initial =
		desc->initial_path && desc->initial_path[0] ? desc->initial_path : SDL_GetUserFolder(SDL_FOLDER_HOME);
	char* normalized = picker_path_normalize(initial ? initial : ".", NULL);
	if (!normalized || strlen(normalized) >= sizeof picker->location_text) {
		SDL_free(normalized);
		picker_copy_error(error, error_capacity, "The initial file picker path is invalid or too long.");
		return 0;
	}
	picker->history[0]    = SDL_strdup(normalized);
	picker->history_count = picker->history[0] ? 1 : 0;
	picker->history_index = 0;
	if (!picker->history_count) {
		SDL_free(normalized);
		picker_copy_error(error, error_capacity, "Not enough memory to open the file picker.");
		return 0;
	}
	picker_copy_error(picker->inline_error, sizeof picker->inline_error, desc->initial_error);
	const int requested = picker_request(picker, normalized, 0);
	SDL_free(normalized);
	if (requested)
		picker->open = 1;
	return requested;
}

int AeronUiFilePicker_IsOpen(const AeronUiFilePicker* picker) { return picker && picker->open; }

void AeronUiFilePicker_Cancel(AeronUiFilePicker* picker) {
	if (picker && picker->open) {
		picker->open     = 0;
		picker->terminal = AERON_UI_FILE_PICKER_CANCELLED;
	}
}

static int picker_history_push(AeronUiFilePicker* picker, const char* path) {
	char* normalized = picker_path_normalize(path, picker->current ? picker->current->path : NULL);
	if (!normalized)
		return 0;
	if (strlen(normalized) >= sizeof picker->location_text) {
		SDL_free(normalized);
		picker_copy_error(picker->inline_error, sizeof picker->inline_error,
						  "The location path is too long for the picker.");
		return 0;
	}
	while (picker->history_count > picker->history_index + 1)
		SDL_free(picker->history[--picker->history_count]);
	if (picker->history_count == PICKER_HISTORY_CAPACITY) {
		SDL_free(picker->history[0]);
		memmove(picker->history, picker->history + 1,
				(PICKER_HISTORY_CAPACITY - 1) * sizeof picker->history[0]);
		picker->history_count--;
		if (picker->history_index)
			picker->history_index--;
	}
	picker->history[picker->history_count++] = normalized;
	picker->history_index                    = picker->history_count - 1;
	return picker_request(picker, normalized, 1);
}

static const char* picker_candidate(const AeronUiFilePicker* picker) {
	if (picker->loading || !picker->current)
		return NULL;
	if (picker->selected_visible < picker->visible_count) {
		const PickerFsEntry* entry =
			&picker->current->entries[picker->visible_indices[picker->selected_visible]];
		if ((picker->mode == AERON_UI_FILE_PICKER_SELECT_DIRECTORY && entry->is_directory) ||
			(picker->mode == AERON_UI_FILE_PICKER_OPEN_FILE && !entry->is_directory))
			return entry->path;
	}
	return picker->mode == AERON_UI_FILE_PICKER_SELECT_DIRECTORY ? picker->current->path : NULL;
}

static AeronUiFilePickerResult picker_accept(AeronUiFilePicker* picker, const char* path, char* selected,
											 size_t selected_capacity) {
	if (!path)
		return AERON_UI_FILE_PICKER_NONE;
	if (strlen(path) >= selected_capacity) {
		picker_copy_error(picker->inline_error, sizeof picker->inline_error,
						  "The selected path is too long for this application.");
		return AERON_UI_FILE_PICKER_NONE;
	}
	char callback_error[PICKER_ERROR_CAPACITY] = { 0 };
	if (picker->accept_fn &&
		!picker->accept_fn(path, picker->accept_user, callback_error, sizeof callback_error)) {
		picker_copy_error(picker->inline_error, sizeof picker->inline_error,
						  callback_error[0] ? callback_error : "The selected location was rejected.");
		return AERON_UI_FILE_PICKER_NONE;
	}
	SDL_snprintf(selected, selected_capacity, "%s", path);
	picker->open = 0;
	return AERON_UI_FILE_PICKER_SELECTED;
}

AeronUiFilePickerResult AeronUiFilePicker_Draw(AeronUiFilePicker* picker, AeronUiContext* ui,
											   char* selected_path, size_t selected_capacity, char* error,
											   size_t error_capacity) {
	if (!picker || !ui || !selected_path || selected_capacity == 0) {
		picker_copy_error(error, error_capacity, "Invalid file picker draw arguments.");
		return AERON_UI_FILE_PICKER_ERROR;
	}
	if (picker->terminal != AERON_UI_FILE_PICKER_NONE) {
		const AeronUiFilePickerResult terminal = picker->terminal;
		picker->terminal                       = AERON_UI_FILE_PICKER_NONE;
		return terminal;
	}
	if (!picker->open)
		return AERON_UI_FILE_PICKER_NONE;
	picker_take_completed(picker);

	int                     modal_open = picker->open;
	const AeronUiWindowDesc window     = picker_window_desc(picker, ui);
	if (!AeronUi_BeginModal(ui, picker->title, &modal_open, &window)) {
		if (picker->open && !modal_open) {
			picker->open = 0;
			return AERON_UI_FILE_PICKER_CANCELLED;
		}
		return AERON_UI_FILE_PICKER_NONE;
	}
	AeronUiFilePickerResult frame_result = AERON_UI_FILE_PICKER_NONE;
	if (picker->instructions[0])
		AeronUi_Help(ui, picker->instructions);

	AeronUi_BeginColumns(ui, 5, NULL);
	if (AeronUi_ButtonEnabled(ui, "Back##picker", picker->history_index > 0)) {
		picker->history_index--;
		picker_request(picker, picker->history[picker->history_index], 1);
	}
	AeronUi_NextColumn(ui);
	if (AeronUi_ButtonEnabled(ui, "Forward##picker", picker->history_index + 1 < picker->history_count)) {
		picker->history_index++;
		picker_request(picker, picker->history[picker->history_index], 1);
	}
	AeronUi_NextColumn(ui);
	const int can_up = picker->parent_path[0] != '\0';
	if (AeronUi_ButtonEnabled(ui, "Up##picker", can_up))
		picker_history_push(picker, picker->parent_path);
	AeronUi_NextColumn(ui);
	if (AeronUi_Button(ui, "Home##picker")) {
		const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);
		if (home)
			picker_history_push(picker, home);
	}
	AeronUi_NextColumn(ui);
	if (AeronUi_ButtonEnabled(ui, "Refresh##picker", picker->current != NULL))
		picker_request(picker, picker->current->path, 1);
	AeronUi_EndColumns(ui);

	AeronUi_BeginColumns(ui, 2, (const float[]) { 1.0f, -90.0f });
	AeronUi_InputText(ui, "Location##picker", picker->location_text, sizeof picker->location_text,
					  AERON_UI_INPUT_TEXT_NONE);
	AeronUi_NextColumn(ui);
	if (AeronUi_ButtonEnabled(ui, "Go##picker", picker->location_text[0] != '\0'))
		picker_history_push(picker, picker->location_text);
	AeronUi_EndColumns(ui);

	const UiLayout* picker_layout = ui_layout_top(ui);
	const float     sidebar_width_ref =
		picker_layout && ui->scale > 0.0f
			? fminf(PICKER_SIDEBAR_WIDTH_REF, fmaxf(0.0f, picker_layout->w / ui->scale * 0.4f))
			: PICKER_SIDEBAR_WIDTH_REF;
	const float list_height_ref = picker_list_height_ref(picker, ui);
	AeronUi_BeginColumns(ui, 2, (const float[]) { -sidebar_width_ref, 1.0f });
	uint32_t location_result =
		AeronUi_ListBox(ui, "Locations##picker", picker->location_items, picker->location_count,
						&picker->selected_location, list_height_ref);
	if ((location_result & AERON_UI_LIST_ACTIVATED) && picker->selected_location < picker->location_count)
		picker_history_push(picker, picker->locations[picker->selected_location].path);
	AeronUi_NextColumn(ui);
	size_t   loading_selection = SIZE_MAX;
	uint32_t list_result =
		AeronUi_ListBox(ui, "Entries##picker", picker->loading ? NULL : picker->visible,
						picker->loading ? 0 : picker->visible_count,
						picker->loading ? &loading_selection : &picker->selected_visible, list_height_ref);
	if (list_result & AERON_UI_LIST_CHANGED)
		picker->inline_error[0] = '\0';
	if ((list_result & AERON_UI_LIST_ACTIVATED) && picker->selected_visible < picker->visible_count) {
		PickerFsEntry* entry = &picker->current->entries[picker->visible_indices[picker->selected_visible]];
		if (entry->is_directory)
			picker_history_push(picker, entry->path);
		else
			frame_result = picker_accept(picker, entry->path, selected_path, selected_capacity);
	}
	AeronUi_EndColumns(ui);

	if (picker->filter_count > 1) {
		const int previous = picker->active_filter;
		AeronUi_Selector(ui, "File type##picker", &picker->active_filter, picker->filter_labels,
						 (int)picker->filter_count);
		if (previous != picker->active_filter)
			picker_rebuild_visible(picker, picker_selected_id(picker));
	}
	if (AeronUi_Toggle(ui, "Show hidden files##picker", &picker->show_hidden))
		picker_rebuild_visible(picker, picker_selected_id(picker));
	if (picker->loading)
		AeronUi_Help(ui, "Loading folder contents...");
	else if (picker->current && picker->visible_count == 0)
		AeronUi_Help(ui, "This folder is empty.");
	const char* candidate = picker_candidate(picker);
	char        selected_label[PICKER_PATH_CAPACITY + 16];
	SDL_snprintf(selected_label, sizeof selected_label, "Selected: %s", candidate ? candidate : "None");
	AeronUi_Label(ui, selected_label);
	if (picker->inline_error[0])
		AeronUi_Error(ui, picker->inline_error);

	AeronUi_Separator(ui);
	AeronUi_BeginColumns(ui, 2, NULL);
	if (AeronUi_Button(ui, picker->cancel_label)) {
		picker->open = 0;
		frame_result = AERON_UI_FILE_PICKER_CANCELLED;
	}
	AeronUi_NextColumn(ui);
	if (AeronUi_ButtonEnabled(ui, picker->accept_label, candidate != NULL))
		frame_result = picker_accept(picker, candidate, selected_path, selected_capacity);
	AeronUi_EndColumns(ui);
	AeronUi_EndModal(ui);
	return frame_result;
}
