/*
 * Aeron debug UI host — Dear ImGui overlay over the swapchain.
 *
 * Owns the ImGui context + SDL3 / SDL_GPU backends, routes SDL events
 * (with input capture so the overlay doesn't drive the game underneath),
 * renders a top-of-screen menu bar that toggles game-registered tool
 * windows, and records the draw data into Aeron_Present's swapchain
 * render pass after layer composition.
 *
 * Compiled only when the AERON_DEBUG_UI CMake option is ON; the public
 * API in aeron/debug.h has inline no-op stubs otherwise, and the
 * engine call sites go through the no-op macros in internal.h.
 *
 * The dynamic Aeron_DebugRegisterTool registry lets each game supply
 * its own tools.
 */

#include "internal.h"
#include "aeron/debug.h"

#include <imgui.h>
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"

#define AERON_DEBUG_MAX_TOOLS 32

typedef struct AeronDebugTool {
	const char*      menu_label;
	AeronDebugToolFn draw;
	void*            user;
	int              open;
} AeronDebugTool;

static struct {
	bool           initialized = false;
	bool           visible     = false;
	AeronDebugTool tools[AERON_DEBUG_MAX_TOOLS];
	int            tool_count = 0;
} g_debug;

/* ===== Public API ==================================================== */

extern "C" int Aeron_DebugUiAvailable(void) { return g_debug.initialized ? 1 : 0; }

extern "C" void Aeron_DebugRegisterTool(const char* menu_label, AeronDebugToolFn draw, void* user) {
	if (!menu_label || !draw) {
		return;
	}
	if (g_debug.tool_count >= AERON_DEBUG_MAX_TOOLS) {
		Aeron_Log("aeron", "debug UI: tool registry full; '%s' dropped", menu_label);
		return;
	}
	AeronDebugTool* tool = &g_debug.tools[g_debug.tool_count++];
	tool->menu_label     = menu_label;
	tool->draw           = draw;
	tool->user           = user;
	tool->open           = 0;
}

extern "C" void Aeron_DebugUiToggle(void) { g_debug.visible = !g_debug.visible; }

extern "C" void Aeron_DebugUiSetVisible(int visible) { g_debug.visible = visible != 0; }

extern "C" int Aeron_DebugUiVisible(void) { return g_debug.initialized && g_debug.visible; }

/* ===== Engine lifecycle hooks ======================================== */

extern "C" void Aeron_DebugUiInitInternal(void) {
	if (g_debug.initialized || !g_aeron.gpu_device || !g_aeron.window) {
		return;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	/* Games use arrow keys for cockpit / menu navigation; ImGui's
	 * keyboard nav would steal them. Mouse-driven UI only. */
	io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr; /* don't litter imgui.ini next to the binary */

	ImGui::StyleColorsDark();

	/* No manual DPI scaling: the SDL3 backend sets
	 * io.DisplayFramebufferScale from the window's pixel/point ratio, so
	 * widgets land at their authored size on standard and retina displays
	 * alike (see the TIE sdl3 shell's debug_ui.cpp for the knobs that
	 * were tried and dropped). */

	ImGui_ImplSDL3_InitForSDLGPU(g_aeron.window);
	ImGui_ImplSDLGPU3_InitInfo info{};
	info.Device            = g_aeron.gpu_device;
	info.ColorTargetFormat = g_aeron.swapchain_format;
	info.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;
	ImGui_ImplSDLGPU3_Init(&info);

	g_debug.initialized = true;
	Aeron_Log("aeron", "debug UI initialized");
}

extern "C" void Aeron_DebugUiShutdownInternal(void) {
	if (!g_debug.initialized) {
		return;
	}
	ImGui_ImplSDLGPU3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	g_debug.initialized = false;
	g_debug.visible     = false;
}

/* The ImGui SDL_GPU backend caches its pipeline against the
 * ColorTargetFormat fixed at init; on HDR composition flip the
 * swapchain format changes and the cached pipeline becomes incompatible
 * (visible as black splotches over ImGui draws). Rebuild against the
 * fresh format; fonts + the SDL3 platform side stay intact. */
extern "C" void Aeron_DebugUiOnSwapchainFormatChanged(SDL_GPUTextureFormat format) {
	if (!g_debug.initialized) {
		return;
	}
	ImGui_ImplSDLGPU3_Shutdown();
	ImGui_ImplSDLGPU3_InitInfo info{};
	info.Device            = g_aeron.gpu_device;
	info.ColorTargetFormat = format;
	info.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;
	ImGui_ImplSDLGPU3_Init(&info);
}

/* ===== Event + input capture ========================================= */

extern "C" int Aeron_DebugUiHandleEvent(const SDL_Event* event) {
	if (!g_debug.initialized || !event) {
		return 0;
	}

	/* Always feed ImGui so it tracks focus / dpi / mouse position even
	 * while the overlay is hidden. */
	ImGui_ImplSDL3_ProcessEvent(event);

	if (!g_debug.visible) {
		return 0;
	}

	/* While visible, swallow ALL mouse events regardless of whether the
	 * pointer is over a widget — clicks outside the tool windows would
	 * otherwise fire weapons / drag ranges in the game underneath. */
	if (event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
		event->type == SDL_EVENT_MOUSE_BUTTON_UP || event->type == SDL_EVENT_MOUSE_WHEEL) {
		return 1;
	}

	/* Keyboard stays gated on ImGui focus so game hotkeys (including
	 * the game's overlay-toggle binding) keep working when no text
	 * field is active. */
	if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP ||
		event->type == SDL_EVENT_TEXT_INPUT) {
		return ImGui::GetIO().WantCaptureKeyboard ? 1 : 0;
	}

	return 0;
}

extern "C" void Aeron_DebugUiFilterInput(AeronInputSnapshot* input) {
	if (!g_debug.initialized || !g_debug.visible || !input) {
		return;
	}

	/* Aeron's mouse position/motion is polled (not event-driven), so
	 * swallowing SDL mouse events alone doesn't stop the game cursor —
	 * clear the whole per-frame mouse activity while the overlay owns
	 * the pointer. Held-state is cleared too so a button held when the
	 * overlay opens doesn't stay latched in the game. */
	input->mouse.relative_x             = 0.0f;
	input->mouse.relative_y             = 0.0f;
	input->mouse.buttons                = 0;
	input->mouse.pressed_buttons        = 0;
	input->mouse.released_buttons       = 0;
	input->mouse.double_clicked_buttons = 0;

	if (ImGui::GetIO().WantCaptureKeyboard) {
		memset(input->key_pressed, 0, sizeof(input->key_pressed));
		memset(input->key_released, 0, sizeof(input->key_released));
		memset(input->key_typed, 0, sizeof(input->key_typed));
		input->text[0]     = '\0';
		input->text_length = 0;
	}
}

/* ===== Per-frame build + render ====================================== */

static void Aeron_DebugUiDrawMenuBar(void) {
	if (!ImGui::BeginMainMenuBar()) {
		return;
	}

	if (ImGui::BeginMenu("Tools")) {
		for (int i = 0; i < g_debug.tool_count; ++i) {
			bool open = g_debug.tools[i].open != 0;
			ImGui::MenuItem(g_debug.tools[i].menu_label, nullptr, &open);
			g_debug.tools[i].open = open ? 1 : 0;
		}
		ImGui::EndMenu();
	}

	const char* hint = "debug overlay";
	const float w    = ImGui::CalcTextSize(hint).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
	ImGui::SameLine(ImGui::GetWindowWidth() - w);
	ImGui::TextDisabled("%s", hint);

	ImGui::EndMainMenuBar();
}

extern "C" void Aeron_DebugUiBuildFrame(void) {
	if (!g_debug.initialized) {
		return;
	}

	/* Games hide the OS cursor and draw their own; ImGui's default
	 * SDL_SetCursor path would leave an invisible pointer over the
	 * overlay. MouseDrawCursor makes ImGui synthesize the cursor in its
	 * vertex stream — per-frame so it only shows while visible. */
	ImGui::GetIO().MouseDrawCursor = g_debug.visible;

	ImGui_ImplSDLGPU3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();

	if (g_debug.visible) {
		Aeron_DebugUiDrawMenuBar();
		for (int i = 0; i < g_debug.tool_count; ++i) {
			if (g_debug.tools[i].open && g_debug.tools[i].draw) {
				g_debug.tools[i].draw(&g_debug.tools[i].open, g_debug.tools[i].user);
			}
		}
	}

	/* Always Render — even hidden — so the GPU backend's per-frame state
	 * machine stays consistent; empty DrawData makes the render-side
	 * steps no-ops. */
	ImGui::Render();
}

extern "C" void Aeron_DebugUiPrepareRender(SDL_GPUCommandBuffer* command_buffer) {
	if (!g_debug.initialized || !command_buffer) {
		return;
	}
	ImDrawData* draw = ImGui::GetDrawData();
	if (!draw) {
		return;
	}
	/* Runs its own copy pass; must precede any render pass on the CB. */
	ImGui_ImplSDLGPU3_PrepareDrawData(draw, command_buffer);
}

extern "C" void Aeron_DebugUiRecordDraws(SDL_GPUCommandBuffer* command_buffer,
										 SDL_GPURenderPass*    render_pass) {
	if (!g_debug.initialized || !command_buffer || !render_pass) {
		return;
	}
	ImDrawData* draw = ImGui::GetDrawData();
	if (!draw || draw->CmdListsCount == 0) {
		return;
	}
	ImGui_ImplSDLGPU3_RenderDrawData(draw, command_buffer, render_pass);
}
