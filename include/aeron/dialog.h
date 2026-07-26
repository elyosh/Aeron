#ifndef AERON_DIALOG_H
#define AERON_DIALOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronFolderDialog AeronFolderDialog;

enum { AERON_MESSAGE_BOX_MAX_BUTTONS = 8 };

typedef enum AeronMessageBoxKind {
	AERON_MESSAGE_BOX_INFORMATION,
	AERON_MESSAGE_BOX_WARNING,
	AERON_MESSAGE_BOX_ERROR
} AeronMessageBoxKind;

typedef struct AeronMessageBoxButton {
	int id;
	const char* label;
	int is_default;
	int is_cancel;
} AeronMessageBoxButton;

typedef struct AeronMessageBoxOptions {
	AeronMessageBoxKind kind;
	const char* title;
	const char* message;
	const AeronMessageBoxButton* buttons;
	size_t button_count;
} AeronMessageBoxOptions;

typedef enum AeronFolderDialogStatus {
	AERON_FOLDER_DIALOG_WAITING,
	AERON_FOLDER_DIALOG_SELECTED,
	AERON_FOLDER_DIALOG_CANCELLED,
	AERON_FOLDER_DIALOG_ERROR
} AeronFolderDialogStatus;

typedef struct AeronFolderDialogOptions {
	const char* title;
	const char* accept_label;
	const char* cancel_label;
	const char* initial_path;
} AeronFolderDialogOptions;

/* Shows a native modal message box and returns the selected caller-defined button id. */
int Aeron_ShowMessageBox(const AeronMessageBoxOptions* options, int* selected_button);
/* Opens a native asynchronous folder picker attached to the Aeron window. */
AeronFolderDialog* Aeron_ShowOpenFolderDialog(const AeronFolderDialogOptions* options);
/* Copies a completed result into main-thread-owned buffers. */
AeronFolderDialogStatus Aeron_PollFolderDialog(AeronFolderDialog* dialog, char* path, size_t path_capacity,
											   char* error, size_t error_capacity);
/* Releases a completed dialog. A waiting dialog remains owned by its callback. */
void Aeron_DestroyFolderDialog(AeronFolderDialog* dialog);

#ifdef __cplusplus
}
#endif

#endif
