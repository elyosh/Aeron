#include "internal.h"

struct AeronFolderDialog {
	SDL_Mutex*              mutex;
	AeronFolderDialogStatus status;
	char                    path[AERON_MAX_PATH];
	char                    error[256];
};

int Aeron_ShowMessageBox(const AeronMessageBoxOptions* options, int* selected_button) {
	SDL_MessageBoxButtonData buttons[AERON_MESSAGE_BOX_MAX_BUTTONS];
	SDL_MessageBoxData data;
	SDL_MessageBoxFlags flags;
	int result = -1;
	size_t i;

	if (!options || !options->title || !options->message || !options->buttons ||
		options->button_count == 0 || options->button_count > sizeof buttons / sizeof buttons[0]) {
		SDL_SetError("invalid Aeron message box options");
		return 0;
	}
	switch (options->kind) {
		case AERON_MESSAGE_BOX_INFORMATION:
			flags = SDL_MESSAGEBOX_INFORMATION;
			break;
		case AERON_MESSAGE_BOX_WARNING:
			flags = SDL_MESSAGEBOX_WARNING;
			break;
		case AERON_MESSAGE_BOX_ERROR:
			flags = SDL_MESSAGEBOX_ERROR;
			break;
		default:
			SDL_SetError("invalid Aeron message box kind");
			return 0;
	}
	for (i = 0; i < options->button_count; ++i) {
		if (!options->buttons[i].label || !options->buttons[i].label[0]) {
			SDL_SetError("invalid Aeron message box button");
			return 0;
		}
		buttons[i].flags = 0;
		if (options->buttons[i].is_default) {
			buttons[i].flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
		}
		if (options->buttons[i].is_cancel) {
			buttons[i].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
		}
		buttons[i].buttonID = options->buttons[i].id;
		buttons[i].text = options->buttons[i].label;
	}
	memset(&data, 0, sizeof data);
	data.flags = flags;
	data.window = g_aeron.window;
	data.title = options->title;
	data.message = options->message;
	data.numbuttons = (int)options->button_count;
	data.buttons = buttons;
	if (!SDL_ShowMessageBox(&data, &result)) {
		Aeron_Log("aeron.dialog", "could not show message box: %s", SDL_GetError());
		return 0;
	}
	if (selected_button) {
		*selected_button = result;
	}
	return 1;
}

static void SDLCALL Aeron_FolderDialogCallback(void* userdata, const char* const* file_list, int filter) {
	AeronFolderDialog* dialog = (AeronFolderDialog*)userdata;

	(void)filter;
	SDL_LockMutex(dialog->mutex);
	if (!file_list) {
		Aeron_CopyString(dialog->error, sizeof dialog->error, SDL_GetError());
		dialog->status = AERON_FOLDER_DIALOG_ERROR;
	} else if (!file_list[0]) {
		dialog->status = AERON_FOLDER_DIALOG_CANCELLED;
	} else if (SDL_strlen(file_list[0]) >= sizeof dialog->path) {
		Aeron_CopyString(dialog->error, sizeof dialog->error, "selected path is too long");
		dialog->status = AERON_FOLDER_DIALOG_ERROR;
	} else {
		Aeron_CopyString(dialog->path, sizeof dialog->path, file_list[0]);
		dialog->status = AERON_FOLDER_DIALOG_SELECTED;
	}
	SDL_UnlockMutex(dialog->mutex);
}

AeronFolderDialog* Aeron_ShowOpenFolderDialog(const AeronFolderDialogOptions* options) {
	AeronFolderDialog* dialog;
	SDL_PropertiesID   properties;

	if (!g_aeron.initialized) {
		return NULL;
	}
	dialog = (AeronFolderDialog*)SDL_calloc(1, sizeof *dialog);
	if (!dialog) {
		return NULL;
	}
	dialog->mutex = SDL_CreateMutex();
	if (!dialog->mutex) {
		SDL_free(dialog);
		return NULL;
	}
	dialog->status = AERON_FOLDER_DIALOG_WAITING;

	properties = SDL_CreateProperties();
	if (!properties ||
		!SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, g_aeron.window) ||
		!SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false) ||
		(options && options->title &&
		 !SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, options->title)) ||
		(options && options->accept_label &&
		 !SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, options->accept_label)) ||
		(options && options->cancel_label &&
		 !SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_CANCEL_STRING, options->cancel_label)) ||
		(options && options->initial_path &&
		 !SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, options->initial_path))) {
		if (properties) {
			SDL_DestroyProperties(properties);
		}
		SDL_DestroyMutex(dialog->mutex);
		SDL_free(dialog);
		return NULL;
	}

	SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, Aeron_FolderDialogCallback, dialog,
									 properties);
	SDL_DestroyProperties(properties);
	return dialog;
}

AeronFolderDialogStatus Aeron_PollFolderDialog(AeronFolderDialog* dialog, char* path, size_t path_capacity,
											   char* error, size_t error_capacity) {
	AeronFolderDialogStatus status;

	if (!dialog) {
		return AERON_FOLDER_DIALOG_ERROR;
	}
	SDL_LockMutex(dialog->mutex);
	status = dialog->status;
	if (status == AERON_FOLDER_DIALOG_SELECTED && path && path_capacity) {
		Aeron_CopyString(path, path_capacity, dialog->path);
	}
	if (status == AERON_FOLDER_DIALOG_ERROR && error && error_capacity) {
		Aeron_CopyString(error, error_capacity, dialog->error);
	}
	SDL_UnlockMutex(dialog->mutex);
	return status;
}

void Aeron_DestroyFolderDialog(AeronFolderDialog* dialog) {
	if (!dialog) {
		return;
	}
	SDL_LockMutex(dialog->mutex);
	if (dialog->status == AERON_FOLDER_DIALOG_WAITING) {
		SDL_UnlockMutex(dialog->mutex);
		return;
	}
	SDL_UnlockMutex(dialog->mutex);
	SDL_DestroyMutex(dialog->mutex);
	SDL_free(dialog);
}
