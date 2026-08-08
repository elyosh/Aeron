#ifndef AERON_SCENE_UI_FILE_PICKER_H
#define AERON_SCENE_UI_FILE_PICKER_H

#include "aeron/scene/ui.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeronUiFilePicker AeronUiFilePicker;

typedef enum AeronUiFilePickerMode {
	AERON_UI_FILE_PICKER_OPEN_FILE,
	AERON_UI_FILE_PICKER_SELECT_DIRECTORY,
} AeronUiFilePickerMode;

typedef struct AeronUiFileFilter {
	const char*        label;
	const char* const* extensions;
	size_t             extension_count;
} AeronUiFileFilter;

/* Called on the main thread when the user confirms a path. Returning zero
 * leaves the picker open and displays `error`. */
typedef int (*AeronUiFilePickerAcceptFn)(const char* path, void* user, char* error, size_t error_capacity);

typedef struct AeronUiFilePickerDesc {
	AeronUiFilePickerMode     mode;
	const char*               title;
	const char*               instructions;
	const char*               accept_label;
	const char*               cancel_label;
	const char*               initial_path;
	const char*               initial_error;
	const AeronUiFileFilter*  filters;
	size_t                    filter_count;
	int                       show_hidden;
	AeronUiFilePickerAcceptFn accept_fn;
	void*                     accept_user;
} AeronUiFilePickerDesc;

typedef enum AeronUiFilePickerResult {
	AERON_UI_FILE_PICKER_NONE,
	AERON_UI_FILE_PICKER_SELECTED,
	AERON_UI_FILE_PICKER_CANCELLED,
	AERON_UI_FILE_PICKER_ERROR,
} AeronUiFilePickerResult;

AeronUiFilePicker* AeronUiFilePicker_Create(void);
/* Stops any outstanding enumeration before releasing the picker. */
void AeronUiFilePicker_Destroy(AeronUiFilePicker* picker);

/* Opens or replaces the current dialog. Descriptor strings and filters are copied. */
int AeronUiFilePicker_Open(AeronUiFilePicker* picker, const AeronUiFilePickerDesc* desc, char* error,
						   size_t error_capacity);
/* Draw once per AeronUi frame. A terminal result is returned exactly once. */
AeronUiFilePickerResult AeronUiFilePicker_Draw(AeronUiFilePicker* picker, AeronUiContext* ui,
											   char* selected_path, size_t selected_capacity, char* error,
											   size_t error_capacity);
int                     AeronUiFilePicker_IsOpen(const AeronUiFilePicker* picker);
void                    AeronUiFilePicker_Cancel(AeronUiFilePicker* picker);

#ifdef __cplusplus
}
#endif

#endif
