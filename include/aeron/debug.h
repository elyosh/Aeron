#ifndef AERON_DEBUG_H
#define AERON_DEBUG_H

/*
 * Aeron debug UI — optional Dear ImGui overlay host.
 *
 * Aeron owns the ImGui context, the SDL3 platform + SDL_GPU render
 * backends, event routing (with input capture so interacting with the
 * overlay doesn't drive the game underneath), and drawing into the
 * swapchain pass after layer composition. Games only register tool
 * windows: plain callbacks that emit ImGui widgets. Tool code is game
 * code — it may include <imgui.h> (exported by the aeron target when
 * the feature is on) but never SDL.
 *
 * Build gating: compiled only when the aeron CMake option
 * AERON_DEBUG_UI is ON (which defines AERON_DEBUG_UI on the aeron
 * target's public interface). Without it, the inline no-op stubs below
 * apply and no ImGui code is linked.
 *
 * Input capture while the overlay is visible:
 *   - ALL mouse input is withheld from the AeronInputSnapshot (clicks
 *     outside tool windows would otherwise fire weapons / move the
 *     game cursor under the overlay).
 *   - Keyboard is withheld only while ImGui wants it (text field
 *     focused), so game hotkeys — including the game's own toggle
 *     binding — keep working.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct AeronTexture;

/* Tool window callback: emit the window's ImGui widgets. `open` is the
 * menu-bar visibility flag for this tool — pass it to ImGui::Begin so
 * the window's close button clears it. `user` is the registration
 * userdata. Called every frame while the tool is toggled on and the
 * overlay is visible. */
typedef void (*AeronDebugToolFn)(int* open, void* user);
typedef void (*AeronDebugApplicationFn)(void* user);

#ifdef AERON_DEBUG_UI

/* Nonzero when the debug UI was compiled in and initialized. */
int Aeron_DebugUiAvailable(void);

/* Register a tool window under the overlay's menu bar. `menu_label` is
 * borrowed and must outlive the registration (string literals in
 * practice). Tools start hidden; the user toggles them from the menu.
 * Safe to call before or after Aeron_Init. */
void Aeron_DebugRegisterTool(const char* menu_label, AeronDebugToolFn draw, void* user);

/* Installs one always-active Dear ImGui application callback. This is intended
 * for standalone developer tools that use Aeron for their window and renderer;
 * unlike registered debug tools it does not add Aeron's Tools menu bar. */
void Aeron_DebugSetApplication(AeronDebugApplicationFn draw, void* user);

/* Emits an ImGui image backed by an Aeron texture. The call must be made from
 * an Aeron-hosted debug/application UI callback. */
void Aeron_DebugImage(struct AeronTexture* texture, float width, float height);

/* Overlay visibility. The game binds its own toggle key (the snapshot
 * keeps delivering it — see the capture rules above). */
void Aeron_DebugUiToggle(void);
void Aeron_DebugUiSetVisible(int visible);
int  Aeron_DebugUiVisible(void);

#else /* !AERON_DEBUG_UI — inline no-op stubs */

static inline int  Aeron_DebugUiAvailable(void) { return 0; }
static inline void Aeron_DebugRegisterTool(const char* menu_label, AeronDebugToolFn draw, void* user) {
	(void)menu_label;
	(void)draw;
	(void)user;
}
static inline void Aeron_DebugSetApplication(AeronDebugApplicationFn draw, void* user) {
	(void)draw;
	(void)user;
}
static inline void Aeron_DebugImage(struct AeronTexture* texture, float width, float height) {
	(void)texture;
	(void)width;
	(void)height;
}
static inline void Aeron_DebugUiToggle(void) {}
static inline void Aeron_DebugUiSetVisible(int visible) { (void)visible; }
static inline int  Aeron_DebugUiVisible(void) { return 0; }

#endif /* AERON_DEBUG_UI */

#ifdef __cplusplus
}
#endif

#endif /* AERON_DEBUG_H */
