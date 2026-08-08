/*
 * ui_demo — windowed AeronUi showcase.
 *
 * One centered window exercising every toolkit widget: header, label,
 * wrapped help text, buttons (modal + quit), toggle, sliders, selector,
 * a two-column section, a two-page tab bar, and a scrollable button
 * list. Values are demo-owned, per the immediate-mode contract.
 *
 * Usage: ui_demo <font_atlas_basename>
 *   The basename names a .fnt/.png pair (no extension), e.g. the TIE
 *   repo's tie-remaster/assets/fonts/subtitle/font8, or an atlas baked
 *   with the fontbake tool.
 */

#include "aeron/main.h"
#include "aeron/aeron.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/ui.h"
#include "aeron/scene/ui_file_picker.h"

#include <stdio.h>
#include <string.h>

#define UI_DEMO_LOGICAL_W 1920
#define UI_DEMO_LOGICAL_H 1080

typedef struct DemoState {
	int                modal_open;
	int                vsync;
	int                volume; /* percent */
	float              gamma;
	int                quality;    /* selector index */
	int                tab;        /* active tab page */
	int                last_click; /* last activated list row, -1 = none */
	AeronUiFilePicker* picker;
	char               selected_path[4096];
	char               picker_error[512];
} DemoState;

static void demo_page_settings(AeronUiContext* ui, DemoState* state) {
	static const char* quality_options[] = { "Low", "Medium", "High", "Ultra" };

	AeronUi_Toggle(ui, "VSync", &state->vsync);
	AeronUi_SliderInt(ui, "Volume", &state->volume, 0, 100, 5, "%d%%");
	AeronUi_SliderFloat(ui, "Gamma", &state->gamma, 1.6f, 2.6f, 0.05f, NULL);
	AeronUi_Selector(ui, "Quality", &state->quality, quality_options, 4);
}

static void demo_page_list(AeronUiContext* ui, DemoState* state) {
	AeronUi_Help(ui, "A fixed-height scroll region; focus navigation scrolls rows into view.");
	if (AeronUi_BeginScroll(ui, "demo_list", 320.0f)) {
		for (int i = 0; i < 20; i++) {
			char label[32];
			snprintf(label, sizeof label, "List Entry %d", i + 1);
			AeronUi_PushId(ui, i);
			if (AeronUi_Button(ui, label))
				state->last_click = i;
			AeronUi_PopId(ui);
		}
		AeronUi_EndScroll(ui);
	}
	if (state->last_click >= 0) {
		char text[48];
		snprintf(text, sizeof text, "Last activated: entry %d##last_click", state->last_click + 1);
		AeronUi_Label(ui, text);
	}
}

static void demo_open_picker(DemoState* state, AeronUiFilePickerMode mode) {
	static const char*             image_extensions[] = { "png", "jpg", "jpeg" };
	static const AeronUiFileFilter filters[]          = {
		{ "Images", image_extensions, 3 },
	};
	const AeronUiFilePickerDesc desc = {
		.mode         = mode,
		.title        = mode == AERON_UI_FILE_PICKER_OPEN_FILE ? "OPEN IMAGE" : "SELECT DIRECTORY",
		.instructions = mode == AERON_UI_FILE_PICKER_OPEN_FILE
							? "Choose an image file to exercise file filtering and selection."
							: "Choose a directory. The current directory can be accepted directly.",
		.accept_label = mode == AERON_UI_FILE_PICKER_OPEN_FILE ? "Open File" : "Select Folder",
		.filters      = mode == AERON_UI_FILE_PICKER_OPEN_FILE ? filters : NULL,
		.filter_count = mode == AERON_UI_FILE_PICKER_OPEN_FILE ? 1 : 0,
	};
	state->picker_error[0] = '\0';
	if (!AeronUiFilePicker_Open(state->picker, &desc, state->picker_error, sizeof state->picker_error)) {
		Aeron_LogError("ui_demo", "%s", state->picker_error);
	}
}

static void demo_page_picker(AeronUiContext* ui, DemoState* state) {
	AeronUi_Help(ui, "The same asynchronous AeronUi component supports file and directory selection.");
	AeronUi_BeginColumns(ui, 2, NULL);
	if (AeronUi_Button(ui, "Open Image..."))
		demo_open_picker(state, AERON_UI_FILE_PICKER_OPEN_FILE);
	AeronUi_NextColumn(ui);
	if (AeronUi_Button(ui, "Select Directory..."))
		demo_open_picker(state, AERON_UI_FILE_PICKER_SELECT_DIRECTORY);
	AeronUi_EndColumns(ui);
	if (state->selected_path[0]) {
		char label[sizeof state->selected_path + 16];
		snprintf(label, sizeof label, "Selected: %s", state->selected_path);
		AeronUi_Label(ui, label);
	}
}

static void demo_modal(AeronUiContext* ui, DemoState* state) {
	if (!AeronUi_BeginModal(ui, "ABOUT", &state->modal_open, NULL))
		return;
	AeronUi_Help(ui, "AeronUi is an immediate-mode settings-menu toolkit rendered through "
					 "AeronDrawList2D at native swapchain resolution. This dialog confines focus "
					 "and swallows outside clicks; Esc or the Close button dismisses it.");
	AeronUi_Spacer(ui, 12.0f);
	if (AeronUi_Button(ui, "Close"))
		state->modal_open = 0;
	AeronUi_EndModal(ui);
}

static void demo_window(AeronUiContext* ui, DemoState* state) {
	static const char* tab_titles[] = { "SETTINGS", "LIST", "PICKER" };

	AeronUiWindowDesc desc = { 0 };
	desc.width_ref         = 860.0f;
	desc.centered          = 1;
	if (!AeronUi_BeginWindow(ui, "AERON UI DEMO", &desc))
		return;

	AeronUi_Header(ui, "Widget Showcase");
	AeronUi_Label(ui, "Every AeronUi widget on one screen.");
	AeronUi_Help(ui, "Navigate with Up/Down, adjust with Left/Right, accept with Enter or Space, "
					 "cancel with Esc. Q/E switch tabs. The mouse hovers focus and the wheel "
					 "scrolls lists. Esc at this level quits the demo.");
	AeronUi_Separator(ui);

	if (AeronUi_BeginTabBar(ui, "demo_tabs", tab_titles, 3, &state->tab)) {
		if (state->tab == 0)
			demo_page_settings(ui, state);
		else if (state->tab == 1)
			demo_page_list(ui, state);
		else
			demo_page_picker(ui, state);
		AeronUi_EndTabBar(ui);
	}

	AeronUi_Separator(ui);
	AeronUi_BeginColumns(ui, 2, NULL);
	if (AeronUi_Button(ui, "About..."))
		state->modal_open = 1;
	AeronUi_NextColumn(ui);
	if (AeronUi_Button(ui, "Quit"))
		Aeron_RequestQuit();
	AeronUi_EndColumns(ui);

	demo_modal(ui, state);
	AeronUi_EndWindow(ui);

	AeronUiFilePickerResult picker_result =
		AeronUiFilePicker_Draw(state->picker, ui, state->selected_path, sizeof state->selected_path,
							   state->picker_error, sizeof state->picker_error);
	if (picker_result == AERON_UI_FILE_PICKER_ERROR)
		Aeron_LogError("ui_demo", "%s", state->picker_error);
}

/* Startup upload: the atlas texture reaches the GPU through one acquired
 * command buffer, mirroring how game shells batch their startup loads. */
static int demo_load_font(AeronFontAtlas* atlas, const char* basename) {
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd) {
		fprintf(stderr, "ui_demo: could not acquire the startup command buffer\n");
		return 0;
	}
	if (!AeronFontAtlas_Load(atlas, cmd, basename)) {
		Aeron_CancelCommandBuffer(cmd);
		fprintf(stderr, "ui_demo: could not load font atlas '%s' (.fnt + .png pair)\n", basename);
		return 0;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		AeronFontAtlas_Release(atlas);
		fprintf(stderr, "ui_demo: font atlas upload submission failed\n");
		return 0;
	}
	return 1;
}

int main(int argc, char* argv[]) {
	if (argc != 2) {
		fprintf(stderr, "Usage: ui_demo <font_atlas_basename>\n"
						"  Basename of a .fnt/.png font atlas pair (no extension), e.g. the\n"
						"  TIE repo's tie-remaster/assets/fonts/subtitle/font8, or an atlas\n"
						"  baked with the fontbake tool.\n");
		return 2;
	}

	AeronConfig config;
	memset(&config, 0, sizeof config);
	config.org_name          = "Aeron";
	config.app_name          = "AeronUiDemo";
	config.shader_path       = UI_DEMO_SHADER_RELATIVE_DIR;
	config.window_title      = "Aeron UI Demo";
	config.logical_width     = UI_DEMO_LOGICAL_W;
	config.logical_height    = UI_DEMO_LOGICAL_H;
	config.presentation_mode = AERON_PRESENTATION_ASPECT_FIT;

	if (!Aeron_Init(&config)) {
		fprintf(stderr, "ui_demo: Aeron_Init failed\n");
		return 1;
	}

	AeronFontAtlas atlas;
	if (!demo_load_font(&atlas, argv[1])) {
		Aeron_Shutdown();
		return 1;
	}

	AeronUiContext* ui = AeronUi_Create(NULL);
	if (!ui) {
		fprintf(stderr, "ui_demo: AeronUi_Create failed\n");
		AeronFontAtlas_Release(&atlas);
		Aeron_Shutdown();
		return 1;
	}
	AeronUi_SetFonts(ui, &(AeronUiFontSet) { .regular = &atlas });

	DemoState state = {
		.volume     = 80,
		.gamma      = 2.2f,
		.last_click = -1,
	};
	state.picker = AeronUiFilePicker_Create();
	if (!state.picker) {
		fprintf(stderr, "ui_demo: file picker creation failed\n");
		AeronUi_Destroy(ui);
		AeronFontAtlas_Release(&atlas);
		Aeron_Shutdown();
		return 1;
	}

	while (!Aeron_QuitRequested() && !Aeron_FatalErrorRequested()) {
		int32_t delta_us = Aeron_BeginFrame();
		if (Aeron_FatalErrorRequested())
			break;

		AeronUi_BeginFrame(ui, &(AeronUiFrameDesc) {
								   .input      = Aeron_InputSnapshot(),
								   .dt_seconds = (float)delta_us * 1e-6f,
							   });
		demo_window(ui, &state);
		AeronUiOutput out = AeronUi_EndFrame(ui);
		/* Modal cancels are consumed inside the toolkit; an unconsumed
		 * cancel means Esc at the top level — leave the demo. */
		if (out.cancel_pressed)
			Aeron_RequestQuit();

		AeronUi_Submit(ui);
		if (!Aeron_Present())
			Aeron_RequestFatalRendererError("frame presentation");
	}

	const int exit_status = Aeron_FatalErrorRequested() ? 1 : 0;
	AeronUiFilePicker_Destroy(state.picker);
	AeronUi_Destroy(ui);
	AeronFontAtlas_Release(&atlas);
	Aeron_Shutdown();
	return exit_status;
}
